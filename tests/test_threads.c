/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the use of the core from several threads
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The string helpers that worker threads and the UI thread call at the same
 * time, and the cancellation of operations from another thread: the worker
 * stops at its next cancellation point. The worker's log or progress callback
 * hands over to the main thread, which cancels, so that the cancellation
 * happens in the middle of the operation every time. This test is also meant
 * to run under ThreadSanitizer.
 */

#include <dirent.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rufus.h"
#include "options.h"
#include "resource.h"
#include "ui_iface.h"
#include "vhd.h"
#include "localization.h"
#include "wimlib.h"
#include "test.h"
#include "test_files.h"

#define NB_THREADS                  8
#define NB_ITERATIONS               20000
#define NB_FILES                    40

/* The log is written by several threads */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t started, cancelled;
static _Atomic(int) cancel_on_log = 0, cancel_on_progress = 0, hash_completed = 0;

static void thread_log(const char* text)
{
	pthread_mutex_lock(&log_mutex);
	test_log(text);
	pthread_mutex_unlock(&log_mutex);
	// Hand over to the main thread on the first file that gets extracted
	if ((strncmp(text, "Extracting: ", 12) == 0) && (cancel_on_log-- == 1)) {
		sem_post(&started);
		sem_wait(&cancelled);
	}
}

static void thread_progress(void* dialog, uint32_t pos, BOOL taskbar)
{
	if (cancel_on_progress-- == 1) {
		sem_post(&started);
		sem_wait(&cancelled);
	}
}

static void thread_event(enum ui_event_type type, intptr_t param)
{
	if (type == UM_HASH_COMPLETED)
		hash_completed = 1;
}

// Wait for the worker to reach the point where it hands over, and cancel
static void cancel_when_started(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 30;
	CHECK(sem_timedwait(&started, &ts) == 0);
	CancelOperation();
	sem_post(&cancelled);
}

/* The helpers that return a static buffer have one per thread */
static void* string_thread(void* param)
{
	int t = (int)(intptr_t)param, i, errors = 0;
	char expected[64], size[32], *p;

	static_strcpy(size, SizeToHumanReadable((uint64_t)(t + 1) * MB, FALSE, FALSE));
	for (i = 0; i < NB_ITERATIONS; i++) {
		snprintf(expected, sizeof(expected), "MSG_%03d UNTRANSLATED", t * 10 + i % 10);
		p = lmprintf(MSG_000 + t * 10 + i % 10);
		if (strcmp(p, expected) != 0)
			errors++;
		p = SizeToHumanReadable((uint64_t)(t + 1) * MB, FALSE, FALSE);
		if (strcmp(p, size) != 0)
			errors++;
		if (i % 1000 == 0)
			uprintf("Thread %d: %d", t, i);
	}
	return (void*)(intptr_t)errors;
}

static void test_strings(void)
{
	pthread_t thread[NB_THREADS];
	void* errors;
	int i;

	for (i = 0; i < NB_THREADS; i++)
		CHECK(pthread_create(&thread[i], NULL, string_thread, (void*)(intptr_t)i) == 0);
	for (i = 0; i < NB_THREADS; i++) {
		CHECK(pthread_join(thread[i], &errors) == 0);
		CHECK_EQ_INT((intptr_t)errors, 0);
	}
	CHECK_CONTAINS(log_buf, "Thread 7: 19000\n");
}

/* An ISO extraction, cancelled after its first file */
static void* extract_thread(void* param)
{
	return (void*)(intptr_t)ExtractISO(image_path, (const char*)param, FALSE);
}

static int count_files(const char* path)
{
	DIR* dir = opendir(path);
	struct dirent* entry;
	int n = 0;

	if (dir == NULL)
		return -1;
	while ((entry = readdir(dir)) != NULL)
		n += (entry->d_name[0] != '.');
	closedir(dir);
	return n;
}

static void test_cancel_extraction(void)
{
	const char* xorriso[] = { "xorriso", "-as", "mkisofs", "-quiet", "-o", tmp_path("files.iso"), "-V", "FILES",
		"-R", "-J", tmp_path("files"), NULL };
	char name[64], *dest = tmp_path("dest");
	pthread_t thread;
	void* r;
	int i;

	for (i = 0; i < NB_FILES; i++) {
		snprintf(name, sizeof(name), "files/file%02d.txt", i);
		make_file(name, "Some content");
	}
	CHECK(run(xorriso));
	image_path = tmp_path("files.iso");
	ErrorStatus = 0;
	CHECK(ExtractISO(image_path, "", TRUE));
	CHECK(mkdir_p(dest, 0755) == 0);

	cancel_on_log = 1;
	CHECK(pthread_create(&thread, NULL, extract_thread, dest) == 0);
	cancel_when_started();
	CHECK(pthread_join(thread, &r) == 0);
	CHECK(!(BOOL)(intptr_t)r);
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_CANCELLED));
	// The file that was being extracted when the operation was cancelled, at most
	CHECK(count_files(dest) <= 1);

	// Without a cancellation, everything gets extracted
	ErrorStatus = 0;
	CHECK(ExtractISO(image_path, dest, FALSE));
	CHECK_EQ_INT(count_files(dest), NB_FILES);
	image_path = NULL;
}

/* A hash, cancelled after its first block */
static void test_cancel_hash(void)
{
	uint8_t* buf = calloc(1, 16 * MB);
	pthread_t thread;
	void* r;

	CHECK(buf != NULL);
	CHECK(write_file(tmp_path("hash.img"), buf, 16 * MB) == 16 * MB);
	free(buf);
	image_path = tmp_path("hash.img");
	memset(&img_report, 0, sizeof(img_report));
	img_report.image_size = 16 * MB;
	ErrorStatus = 0;
	hash_completed = 0;

	cancel_on_progress = 1;
	CHECK(pthread_create(&thread, NULL, HashThread, NULL) == 0);
	cancel_when_started();
	CHECK(pthread_join(thread, &r) == 0);
	CHECK((intptr_t)r != 0);
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_CANCELLED));
	CHECK(!hash_completed);

	ErrorStatus = 0;
	CHECK(HashThread(NULL) == NULL);
	CHECK(hash_completed);
	image_path = NULL;
}

/* An external command, which gets terminated */
static void* command_thread(void* param)
{
	const char* sleep_cmd[] = { "sleep", "30", NULL };
	return (void*)(intptr_t)RunCommand(sleep_cmd, NULL, FALSE);
}

static void test_cancel_command(void)
{
	struct timespec start, end;
	pthread_t thread;
	void* r;

	ErrorStatus = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	CHECK(pthread_create(&thread, NULL, command_thread, NULL) == 0);
	usleep(200 * 1000);
	CancelOperation();
	CHECK(pthread_join(thread, &r) == 0);
	clock_gettime(CLOCK_MONOTONIC, &end);
	CHECK_EQ_INT((intptr_t)r, ERROR_CANCELLED);
	CHECK(end.tv_sec - start.tv_sec < 10);
	CHECK_CONTAINS(log_buf, "Command was terminated by user\n");
}

/* A WIM split (of a >4 GB install.wim for FAT32), through the wimlib progress function */
static void test_cancel_wim(void)
{
	WIMStruct* wim = NULL;
	struct stat st;

	make_file("wimsrc/Windows/Boot/EFI/bootmgr.efi", "bootmgr.efi");
	CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) == 0);
	CHECK(wimlib_add_image(wim, tmp_path("wimsrc"), "Windows", NULL, 0) == 0);
	CHECK(wimlib_write(wim, tmp_path("install.wim"), WIMLIB_ALL_IMAGES, 0, 1) == 0);
	wimlib_free(wim);

	CancelOperation();
	CHECK(!WimSplitFile(tmp_path("install.wim"), tmp_path("cancelled.swm"), strlen(tmp_dir) + 1));
	CHECK(stat(tmp_path("cancelled.swm"), &st) != 0);
	ErrorStatus = 0;
	CHECK(WimSplitFile(tmp_path("install.wim"), tmp_path("install.swm"), strlen(tmp_dir) + 1));
	CHECK(stat(tmp_path("install.swm"), &st) == 0);
}

int main(void)
{
	const ui_iface_t iface = { .log = thread_log, .progress = thread_progress, .event = thread_event };
	const char* tools[] = { "xorriso", "sleep" };

	ui_set_iface(&iface);
	if (!have_tools(tools, ARRAYSIZE(tools)))
		return 77;
	if (!test_files_init())
		return 1;
	CHECK(sem_init(&started, 0, 0) == 0);
	CHECK(sem_init(&cancelled, 0, 0) == 0);
	ResetOptions(&opts);
	opts.fs_type = FS_NTFS;

	test_strings();
	test_cancel_extraction();
	test_cancel_hash();
	test_cancel_command();
	test_cancel_wim();

	sem_destroy(&started);
	sem_destroy(&cancelled);
	test_files_cleanup();
	return TEST_RESULT();
}

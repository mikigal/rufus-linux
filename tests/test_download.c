/*
 * Rufus: The Reliable USB Formatting Utility
 * Download tests
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
 * The downloads of download.c, from an HTTP server of the test on 127.0.0.1:
 * files and buffers, redirections, errors, truncated transfers, a cancellation,
 * and signed files (a real file of rufus.ie, with its signature, in
 * data/download, served through $RUFUS_TEST_URL_PREFIX).
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "rufus.h"
#include "resource.h"
#include "settings.h"
#include "ui_iface.h"
#include "download.h"
#include "test.h"
#include "test_files.h"
#include "test_ui.h"
#include "test_http.h"

#define FILE_SIZE       100000
#define NOLENGTH_SIZE   (70 * 1024)
#define SLOW_SIZE       (10 * 1024 * 1024)

static char data_dir[MAX_PATH];
static char last_agent[256], last_accept[256];
static int events[64], nb_events = 0, progress_calls = 0;
static uint32_t last_progress = 0;

static void test_event(enum ui_event_type type, intptr_t param)
{
	if (nb_events < (int)ARRAYSIZE(events))
		events[nb_events++] = type;
}

static void test_progress_cb(void* dialog, uint32_t pos, BOOL taskbar)
{
	progress_calls++;
	last_progress = pos;
}

static uint8_t pattern(size_t i)
{
	return (uint8_t)((i * 7) ^ (i >> 8));
}

static void send_pattern(int fd, size_t len, BOOL slow)
{
	uint8_t buf[1024];
	size_t i, j, n;

	for (i = 0; i < len; i += n) {
		n = (len - i < sizeof(buf)) ? len - i : sizeof(buf);
		for (j = 0; j < n; j++)
			buf[j] = pattern(i + j);
		if (send(fd, buf, n, MSG_NOSIGNAL) <= 0)
			return;
		if (slow)
			usleep(20000);
	}
}

static void serve(int fd, const char* method, const char* path, const char* req)
{
	char file[MAX_PATH * 2];
	uint8_t* buf = NULL;
	uint32_t size;
	BOOL head = (strcmp(method, "HEAD") == 0);

	http_get_header(req, "User-Agent:", last_agent, sizeof(last_agent));
	http_get_header(req, "Accept:", last_accept, sizeof(last_accept));
	if (strcmp(path, "/ok/file.bin") == 0 || strncmp(path, "/ok/file.bin?", 13) == 0) {
		http_send_headers(fd, 200, "OK", FILE_SIZE, NULL);
		if (!head)
			send_pattern(fd, FILE_SIZE, FALSE);
	} else if (strcmp(path, "/redirect") == 0) {
		http_send_headers(fd, 302, "Found", 0, "Location: /ok/file.bin");
	} else if (strcmp(path, "/short") == 0) {
		http_send_headers(fd, 200, "OK", 1000, NULL);
		send_pattern(fd, 500, FALSE);
	} else if (strcmp(path, "/nolength") == 0) {
		http_send_headers(fd, 200, "OK", -1, NULL);
		if (!head)
			send_pattern(fd, NOLENGTH_SIZE, FALSE);
	} else if (strcmp(path, "/empty") == 0) {
		http_send_headers(fd, 200, "OK", 0, NULL);
	} else if (strcmp(path, "/slow") == 0) {
		http_send_headers(fd, 200, "OK", SLOW_SIZE, NULL);
		send_pattern(fd, SLOW_SIZE, TRUE);
	} else if (strcmp(path, "/forbidden") == 0) {
		http_send_headers(fd, 403, "Forbidden", 9, NULL);
		http_send_all(fd, "Forbidden", 9);
	} else {
		// The files of data/download, which $RUFUS_TEST_URL_PREFIX maps the URLs to
		snprintf(file, sizeof(file), "%s%s", data_dir, path);
		size = (strstr(path, "..") == NULL) ? read_file(file, &buf) : 0;
		if (size == 0) {
			http_send_headers(fd, 404, "Not Found", 9, NULL);
			http_send_all(fd, "Not Found", 9);
		} else {
			http_send_headers(fd, 200, "OK", size, NULL);
			if (!head)
				http_send_all(fd, buf, size);
		}
		free(buf);
	}
}

static BOOL check_pattern(const uint8_t* buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (buf[i] != pattern(i))
			return FALSE;
	return TRUE;
}

static void test_buffers(void)
{
	BYTE* buf = NULL;
	uint64_t size;

	size = DownloadToFileOrBuffer(http_url("/ok/file.bin"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, FILE_SIZE);
	CHECK_EQ_INT(DownloadStatus, 200);
	CHECK(buf != NULL && check_pattern(buf, FILE_SIZE));
	// Two NUL bytes past the end
	CHECK(buf != NULL && buf[FILE_SIZE] == 0 && buf[FILE_SIZE + 1] == 0);
	CHECK_CONTAINS(last_agent, APPLICATION_NAME "/");
	CHECK_CONTAINS(last_agent, "(Linux; ");
	free(buf);

	// A redirection is followed
	size = DownloadToFileOrBuffer(http_url("/redirect"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, FILE_SIZE);
	CHECK(buf != NULL && check_pattern(buf, FILE_SIZE));
	free(buf);

	// A file without a length (upstream refuses those), larger than the initial buffer
	size = DownloadToFileOrBuffer(http_url("/nolength"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, NOLENGTH_SIZE);
	CHECK(buf != NULL && check_pattern(buf, NOLENGTH_SIZE));
	CHECK(buf != NULL && buf[NOLENGTH_SIZE] == 0 && buf[NOLENGTH_SIZE + 1] == 0);
	free(buf);

	// Errors: the buffer is NULL, and DownloadStatus is the HTTP status
	buf = (BYTE*)"not NULL";
	size = DownloadToFileOrBuffer(http_url("/missing"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK(buf == NULL);
	CHECK_EQ_INT(DownloadStatus, 404);
	CHECK_EQ_INT(errno, ENOENT);
	CHECK_CONTAINS(log_buf, "File not found '");
	size = DownloadToFileOrBuffer(http_url("/forbidden"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK_EQ_INT(DownloadStatus, 403);
	CHECK_EQ_INT(errno, EACCES);
	CHECK_CONTAINS(log_buf, "Unable to access file '");
	// A truncated transfer
	size = DownloadToFileOrBuffer(http_url("/short"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK(buf == NULL);
	// An empty file is a failure, as upstream
	size = DownloadToFileOrBuffer(http_url("/empty"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	// Nothing listens there
	size = DownloadToFileOrBuffer("http://127.0.0.1:9/file", NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK_EQ_INT(errno, ECONNREFUSED);
	CHECK(buf == NULL);

	// The GitHub API gets raw content, and the user agent can be set
	setenv("RUFUS_TEST_URL_PREFIX", http_url(""), 1);
	DownloadToFileOrBufferEx("https://api.github.com/ok/file.bin", NULL, SYMBOL_SERVER_USER_AGENT, &buf, NULL, FALSE);
	CHECK_EQ_STR(last_accept, "application/vnd.github.v3.raw");
	CHECK_EQ_STR(last_agent, SYMBOL_SERVER_USER_AGENT);
	free(buf);
	unsetenv("RUFUS_TEST_URL_PREFIX");
}

static void test_files(void)
{
	char path[MAX_PATH], bak[MAX_PATH];
	uint8_t* buf = NULL;
	uint32_t size;

	snprintf(path, sizeof(path), "%s/file.bin", tmp_dir);
	snprintf(bak, sizeof(bak), "%s.bak", path);
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/ok/file.bin"), path, NULL, NULL, FALSE), FILE_SIZE);
	size = read_file(path, &buf);
	CHECK_EQ_INT(size, FILE_SIZE);
	CHECK(buf != NULL && check_pattern(buf, FILE_SIZE));
	safe_free(buf);
	CHECK(access(bak, F_OK) != 0);

	// A failed download restores the previous file
	write_file(path, (const uint8_t*)"previous", 8);
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/short"), path, NULL, NULL, FALSE), 0);
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_WRITE_FAULT));
	size = read_file(path, &buf);
	CHECK(size == 8 && memcmp(buf, "previous", 8) == 0);
	safe_free(buf);
	CHECK(access(bak, F_OK) != 0);
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/missing"), path, NULL, NULL, FALSE), 0);
	size = read_file(path, &buf);
	CHECK(size == 8 && memcmp(buf, "previous", 8) == 0);
	safe_free(buf);
	// And a successful one replaces it
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/ok/file.bin"), path, NULL, NULL, FALSE), FILE_SIZE);
	CHECK(access(bak, F_OK) != 0);
	unlink(path);

	// No file is left by a failure without a previous file
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/missing"), path, NULL, NULL, FALSE), 0);
	CHECK(access(path, F_OK) != 0);
	// A directory that does not exist
	snprintf(path, sizeof(path), "%s/none/file.bin", tmp_dir);
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/ok/file.bin"), path, NULL, NULL, FALSE), 0);
	CHECK_EQ_INT(errno, ENOENT);
}

static void test_progress(void)
{
	BYTE* buf = NULL;

	// The progress of a download on the main window
	nb_events = 0;
	progress_calls = 0;
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/ok/file.bin"), NULL, &buf, MAIN_WINDOW_PROGRESS, FALSE), FILE_SIZE);
	CHECK(nb_events > 0 && events[0] == UM_PROGRESS_INIT);
	CHECK(progress_calls > 0);
	CHECK_EQ_INT(last_progress, MAX_PROGRESS);
	CHECK_CONTAINS(log_buf, "Downloading http://127.0.0.1:");
	CHECK_CONTAINS(log_buf, "Successfully downloaded 'file.bin'");
	free(buf);
}

static void* cancel_thread(void* param)
{
	usleep(300000);
	CancelOperation();
	return NULL;
}

static void test_cancel(void)
{
	char path[MAX_PATH];
	pthread_t thread;
	time_t start = time(NULL);

	snprintf(path, sizeof(path), "%s/slow.bin", tmp_dir);
	pthread_create(&thread, NULL, cancel_thread, NULL);
	CHECK_EQ_INT(DownloadToFileOrBuffer(http_url("/slow"), path, NULL, NULL, FALSE), 0);
	pthread_join(thread, NULL);
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_CANCELLED));
	CHECK_EQ_INT(errno, ECANCELED);
	CHECK(time(NULL) - start < 5);
	CHECK(access(path, F_OK) != 0);
}

static void test_downloadable(void)
{
	CHECK(IsDownloadable(http_url("/ok/file.bin")));
	CHECK(IsDownloadable(http_url("/redirect")));
	CHECK(!IsDownloadable(http_url("/missing")));
	CHECK_EQ_INT(DownloadStatus, 404);
	// No length
	CHECK(!IsDownloadable(http_url("/nolength")));
	CHECK(!IsDownloadable("http://127.0.0.1:9/file"));
	CHECK(!IsDownloadable(NULL));
}

static void test_signed(void)
{
	const char* bss_url = FILES_URL "/syslinux-6.03/ldlinux.bss";
	char path[MAX_PATH], ref[MAX_PATH], tampered[MAX_PATH * 2];
	uint8_t *buf = NULL, *ref_buf = NULL;
	uint32_t size;

	setenv("RUFUS_TEST_URL_PREFIX", http_url(""), 1);
	snprintf(path, sizeof(path), "%s/ldlinux.bss", tmp_dir);
	snprintf(ref, sizeof(ref), "%s/rufus.ie/files/syslinux-6.03/ldlinux.bss", data_dir);
	CHECK_EQ_INT(DownloadSignedFile(bss_url, path, NULL, TRUE), 512);
	CHECK_EQ_INT(DownloadStatus, 200);
	CHECK_CONTAINS(log_buf, "Download signature is valid ✓");
	size = read_file(path, &buf);
	CHECK(size == 512 && read_file(ref, &ref_buf) == 512 && memcmp(buf, ref_buf, 512) == 0);
	safe_free(buf);
	unlink(path);

	// A file that does not match its signature: nothing gets written
	snprintf(tampered, sizeof(tampered), "%s/rufus.ie/files/tampered", data_dir);
	CHECK(mkdir_p(tampered, 0755) == 0);
	snprintf(tampered, sizeof(tampered), "%s/rufus.ie/files/tampered/ldlinux.bss", data_dir);
	ref_buf[0x100] ^= 1;
	write_file(tampered, ref_buf, 512);
	snprintf(ref, sizeof(ref), "%s/rufus.ie/files/syslinux-6.03/ldlinux.bss.sig", data_dir);
	snprintf(tampered, sizeof(tampered), "%s/rufus.ie/files/tampered/ldlinux.bss.sig", data_dir);
	CHECK(copy_file(ref, tampered, FALSE));
	answer(1, IDOK);
	CHECK_EQ_INT(DownloadSignedFile(FILES_URL "/tampered/ldlinux.bss", path, NULL, TRUE), 0);
	CHECK_EQ_INT(DownloadStatus, 403);
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(APPERR(ERROR_BAD_SIGNATURE)));
	CHECK_CONTAINS(log_buf, "FATAL: Download signature is invalid ✗");
	CHECK(asked_msg(0, MSG_044));
	CHECK(access(path, F_OK) != 0);
	unlink(tampered);
	snprintf(tampered, sizeof(tampered), "%s/rufus.ie/files/tampered/ldlinux.bss", data_dir);
	unlink(tampered);
	snprintf(tampered, sizeof(tampered), "%s/rufus.ie/files/tampered", data_dir);
	rmdir(tampered);
	free(ref_buf);

	// A missing file, with and without a prompt
	answer(0);
	CHECK_EQ_INT(DownloadSignedFile(FILES_URL "/syslinux-6.03/none", path, NULL, FALSE), 0);
	CHECK_EQ_INT(DownloadStatus, 404);
	CHECK_EQ_INT(nb_asked, 0);
	answer(1, IDOK);
	CHECK_EQ_INT(DownloadSignedFile(FILES_URL "/syslinux-6.03/none", path, NULL, TRUE), 0);
	CHECK(asked_msg(0, MSG_044));
	// The progress bar of the main window gets restored
	answer(0);
	nb_events = 0;
	CHECK_EQ_INT(DownloadSignedFile(bss_url, path, MAIN_WINDOW_PROGRESS, FALSE), 512);
	CHECK(nb_events >= 2 && events[0] == UM_PROGRESS_INIT && events[nb_events - 1] == UM_PROGRESS_EXIT);
	unlink(path);
	unsetenv("RUFUS_TEST_URL_PREFIX");
}

static void test_short_names(void)
{
	CHECK_EQ_STR(GetDownloadShortName("https://rufus.ie/files/grub-2.12/core.img"), "core.img");
	CHECK_EQ_STR(GetDownloadShortName("https://example.com/Win11.iso?t=1&e=2"), "Win11.iso");
	CHECK_EQ_STR(GetDownloadShortName("https://example.com/Win11.iso%3Ft=1"), "Win11.iso");
	CHECK_EQ_STR(GetDownloadShortName("https://example.com/a.iso#frag"), "a.iso");
	CHECK_EQ_STR(GetDownloadShortName("/tmp/dir/file.bin"), "file.bin");
	CHECK_EQ_STR(GetDownloadShortName("file.bin"), "file.bin");
	CHECK_EQ_STR(GetDownloadShortName(NULL), "");
}

int main(void)
{
	const char* data = getenv("RUFUS_TEST_DATA");

	if (data == NULL) {
		fprintf(stderr, "RUFUS_TEST_DATA is not set\n");
		return 1;
	}
	const ui_iface_t iface = { .log = test_log, .progress = test_progress_cb, .ask = test_ask, .event = test_event };

	snprintf(data_dir, sizeof(data_dir), "%s/download", data);
	test_files_init();
	if (!http_start(serve)) {
		fprintf(stderr, "Could not start the server: %s\n", strerror(errno));
		return 1;
	}
	unsetenv("RUFUS_TEST_URL_PREFIX");
	ui_set_iface(&iface);

	test_short_names();
	test_buffers();
	test_files();
	test_progress();
	test_cancel();
	test_downloadable();
	test_signed();

	test_files_cleanup();
	return TEST_RESULT();
}

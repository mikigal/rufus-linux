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

#define FILE_SIZE       100000
#define NOLENGTH_SIZE   (70 * 1024)
#define SLOW_SIZE       (10 * 1024 * 1024)

static int server_fd = -1, port = 0;
static char data_dir[MAX_PATH];
static volatile int requests = 0;
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

static void send_all(int fd, const void* buf, size_t len)
{
	const char* p = buf;
	ssize_t r;

	while (len > 0) {
		r = send(fd, p, len, MSG_NOSIGNAL);
		if (r <= 0)
			return;
		p += r;
		len -= (size_t)r;
	}
}

static void send_headers(int fd, int status, const char* reason, long long length, const char* extra)
{
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\nConnection: close\r\n", status, reason);

	if (length >= 0)
		n += snprintf(&buf[n], sizeof(buf) - n, "Content-Length: %lld\r\n", length);
	if (extra != NULL)
		n += snprintf(&buf[n], sizeof(buf) - n, "%s\r\n", extra);
	n += snprintf(&buf[n], sizeof(buf) - n, "\r\n");
	send_all(fd, buf, n);
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

// A header of the request, if present
static void get_header(const char* req, const char* name, char* value, size_t len)
{
	const char* p = strcasestr(req, name);
	size_t n;

	value[0] = 0;
	if (p == NULL)
		return;
	p += strlen(name);
	while (*p == ' ')
		p++;
	n = strcspn(p, "\r\n");
	snprintf(value, len, "%.*s", (int)n, p);
}

static void serve(int fd)
{
	char req[4096], method[8], path[1024], file[MAX_PATH * 2];
	uint8_t* buf = NULL;
	ssize_t r, len = 0;
	uint32_t size;
	BOOL head;

	// The request ends with an empty line
	while (len < (ssize_t)sizeof(req) - 1) {
		r = recv(fd, &req[len], sizeof(req) - 1 - len, 0);
		if (r <= 0)
			return;
		len += r;
		req[len] = 0;
		if (strstr(req, "\r\n\r\n") != NULL)
			break;
	}
	if (sscanf(req, "%7s %1023s", method, path) != 2)
		return;
	requests++;
	get_header(req, "User-Agent:", last_agent, sizeof(last_agent));
	get_header(req, "Accept:", last_accept, sizeof(last_accept));
	head = (strcmp(method, "HEAD") == 0);

	if (strcmp(path, "/ok/file.bin") == 0 || strncmp(path, "/ok/file.bin?", 13) == 0) {
		send_headers(fd, 200, "OK", FILE_SIZE, NULL);
		if (!head)
			send_pattern(fd, FILE_SIZE, FALSE);
	} else if (strcmp(path, "/redirect") == 0) {
		send_headers(fd, 302, "Found", 0, "Location: /ok/file.bin");
	} else if (strcmp(path, "/short") == 0) {
		send_headers(fd, 200, "OK", 1000, NULL);
		send_pattern(fd, 500, FALSE);
	} else if (strcmp(path, "/nolength") == 0) {
		send_headers(fd, 200, "OK", -1, NULL);
		if (!head)
			send_pattern(fd, NOLENGTH_SIZE, FALSE);
	} else if (strcmp(path, "/empty") == 0) {
		send_headers(fd, 200, "OK", 0, NULL);
	} else if (strcmp(path, "/slow") == 0) {
		send_headers(fd, 200, "OK", SLOW_SIZE, NULL);
		send_pattern(fd, SLOW_SIZE, TRUE);
	} else if (strcmp(path, "/forbidden") == 0) {
		send_headers(fd, 403, "Forbidden", 9, NULL);
		send_all(fd, "Forbidden", 9);
	} else {
		// The files of data/download, which $RUFUS_TEST_URL_PREFIX maps the URLs to
		snprintf(file, sizeof(file), "%s%s", data_dir, path);
		size = (strstr(path, "..") == NULL) ? read_file(file, &buf) : 0;
		if (size == 0) {
			send_headers(fd, 404, "Not Found", 9, NULL);
			send_all(fd, "Not Found", 9);
		} else {
			send_headers(fd, 200, "OK", size, NULL);
			if (!head)
				send_all(fd, buf, size);
		}
		free(buf);
	}
}

static void* server_thread(void* param)
{
	int fd;

	while ((fd = accept(server_fd, NULL, NULL)) >= 0) {
		serve(fd);
		shutdown(fd, SHUT_WR);
		close(fd);
	}
	return NULL;
}

static BOOL start_server(void)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t len = sizeof(addr);
	pthread_t thread;

	server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (server_fd < 0 || bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		listen(server_fd, 16) != 0 || getsockname(server_fd, (struct sockaddr*)&addr, &len) != 0)
		return FALSE;
	port = ntohs(addr.sin_port);
	return pthread_create(&thread, NULL, server_thread, NULL) == 0;
}

static char* url(const char* path)
{
	static char buf[4][256];
	static int i = 0;

	i = (i + 1) % 4;
	snprintf(buf[i], sizeof(buf[i]), "http://127.0.0.1:%d%s", port, path);
	return buf[i];
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

	size = DownloadToFileOrBuffer(url("/ok/file.bin"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, FILE_SIZE);
	CHECK_EQ_INT(DownloadStatus, 200);
	CHECK(buf != NULL && check_pattern(buf, FILE_SIZE));
	// Two NUL bytes past the end
	CHECK(buf != NULL && buf[FILE_SIZE] == 0 && buf[FILE_SIZE + 1] == 0);
	CHECK_CONTAINS(last_agent, APPLICATION_NAME "/");
	CHECK_CONTAINS(last_agent, "(Linux; ");
	free(buf);

	// A redirection is followed
	size = DownloadToFileOrBuffer(url("/redirect"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, FILE_SIZE);
	CHECK(buf != NULL && check_pattern(buf, FILE_SIZE));
	free(buf);

	// A file without a length (upstream refuses those), larger than the initial buffer
	size = DownloadToFileOrBuffer(url("/nolength"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, NOLENGTH_SIZE);
	CHECK(buf != NULL && check_pattern(buf, NOLENGTH_SIZE));
	CHECK(buf != NULL && buf[NOLENGTH_SIZE] == 0 && buf[NOLENGTH_SIZE + 1] == 0);
	free(buf);

	// Errors: the buffer is NULL, and DownloadStatus is the HTTP status
	buf = (BYTE*)"not NULL";
	size = DownloadToFileOrBuffer(url("/missing"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK(buf == NULL);
	CHECK_EQ_INT(DownloadStatus, 404);
	CHECK_EQ_INT(errno, ENOENT);
	CHECK_CONTAINS(log_buf, "File not found '");
	size = DownloadToFileOrBuffer(url("/forbidden"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK_EQ_INT(DownloadStatus, 403);
	CHECK_EQ_INT(errno, EACCES);
	CHECK_CONTAINS(log_buf, "Unable to access file '");
	// A truncated transfer
	size = DownloadToFileOrBuffer(url("/short"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK(buf == NULL);
	// An empty file is a failure, as upstream
	size = DownloadToFileOrBuffer(url("/empty"), NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	// Nothing listens there
	size = DownloadToFileOrBuffer("http://127.0.0.1:9/file", NULL, &buf, NULL, FALSE);
	CHECK_EQ_INT(size, 0);
	CHECK_EQ_INT(errno, ECONNREFUSED);
	CHECK(buf == NULL);

	// The GitHub API gets raw content, and the user agent can be set
	setenv("RUFUS_TEST_URL_PREFIX", url(""), 1);
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
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/ok/file.bin"), path, NULL, NULL, FALSE), FILE_SIZE);
	size = read_file(path, &buf);
	CHECK_EQ_INT(size, FILE_SIZE);
	CHECK(buf != NULL && check_pattern(buf, FILE_SIZE));
	safe_free(buf);
	CHECK(access(bak, F_OK) != 0);

	// A failed download restores the previous file
	write_file(path, (const uint8_t*)"previous", 8);
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/short"), path, NULL, NULL, FALSE), 0);
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_WRITE_FAULT));
	size = read_file(path, &buf);
	CHECK(size == 8 && memcmp(buf, "previous", 8) == 0);
	safe_free(buf);
	CHECK(access(bak, F_OK) != 0);
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/missing"), path, NULL, NULL, FALSE), 0);
	size = read_file(path, &buf);
	CHECK(size == 8 && memcmp(buf, "previous", 8) == 0);
	safe_free(buf);
	// And a successful one replaces it
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/ok/file.bin"), path, NULL, NULL, FALSE), FILE_SIZE);
	CHECK(access(bak, F_OK) != 0);
	unlink(path);

	// No file is left by a failure without a previous file
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/missing"), path, NULL, NULL, FALSE), 0);
	CHECK(access(path, F_OK) != 0);
	// A directory that does not exist
	snprintf(path, sizeof(path), "%s/none/file.bin", tmp_dir);
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/ok/file.bin"), path, NULL, NULL, FALSE), 0);
	CHECK_EQ_INT(errno, ENOENT);
}

static void test_progress(void)
{
	BYTE* buf = NULL;

	// The progress of a download on the main window
	nb_events = 0;
	progress_calls = 0;
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/ok/file.bin"), NULL, &buf, MAIN_WINDOW_PROGRESS, FALSE), FILE_SIZE);
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
	CHECK_EQ_INT(DownloadToFileOrBuffer(url("/slow"), path, NULL, NULL, FALSE), 0);
	pthread_join(thread, NULL);
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_CANCELLED));
	CHECK_EQ_INT(errno, ECANCELED);
	CHECK(time(NULL) - start < 5);
	CHECK(access(path, F_OK) != 0);
}

static void test_downloadable(void)
{
	CHECK(IsDownloadable(url("/ok/file.bin")));
	CHECK(IsDownloadable(url("/redirect")));
	CHECK(!IsDownloadable(url("/missing")));
	CHECK_EQ_INT(DownloadStatus, 404);
	// No length
	CHECK(!IsDownloadable(url("/nolength")));
	CHECK(!IsDownloadable("http://127.0.0.1:9/file"));
	CHECK(!IsDownloadable(NULL));
}

static void test_signed(void)
{
	const char* bss_url = FILES_URL "/syslinux-6.03/ldlinux.bss";
	char path[MAX_PATH], ref[MAX_PATH], tampered[MAX_PATH * 2];
	uint8_t *buf = NULL, *ref_buf = NULL;
	uint32_t size;

	setenv("RUFUS_TEST_URL_PREFIX", url(""), 1);
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
	if (!start_server()) {
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

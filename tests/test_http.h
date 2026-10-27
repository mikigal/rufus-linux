/*
 * Rufus: The Reliable USB Formatting Utility
 * An HTTP server for the tests
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
 * A server on 127.0.0.1, which gives each request (one per connection) to the
 * handler of the test, for the downloads of download.c and fido.c, which
 * $RUFUS_TEST_URL_PREFIX sends there ("https://rufus.ie/x" is "/rufus.ie/x").
 */

#pragma once

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "rufus.h"

// The request: its method ("GET", "HEAD"), path, and whole text (for the headers)
typedef void (*http_handler_t)(int fd, const char* method, const char* path, const char* request);

static int http_server_fd = -1, http_port = 0;
static http_handler_t http_handler = NULL;
static volatile int http_requests = 0;

static inline void http_send_all(int fd, const void* buf, size_t len)
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

// The headers of a reply, with a Content-Length unless length is negative
static inline void http_send_headers(int fd, int status, const char* reason, long long length, const char* extra)
{
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\nConnection: close\r\n", status, reason);

	if (length >= 0)
		n += snprintf(&buf[n], sizeof(buf) - n, "Content-Length: %lld\r\n", length);
	if (extra != NULL)
		n += snprintf(&buf[n], sizeof(buf) - n, "%s\r\n", extra);
	n += snprintf(&buf[n], sizeof(buf) - n, "\r\n");
	http_send_all(fd, buf, n);
}

// A whole reply, with a body
static inline void http_reply(int fd, int status, const char* reason, const char* type, const char* body)
{
	char extra[128];

	snprintf(extra, sizeof(extra), "Content-Type: %s", type);
	http_send_headers(fd, status, reason, (long long)strlen(body), extra);
	http_send_all(fd, body, strlen(body));
}

// A header of the request, if present
static inline void http_get_header(const char* req, const char* name, char* value, size_t len)
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

static void* http_server_thread(void* param)
{
	char req[8192], method[8], path[2048];
	ssize_t r, len;
	int fd;

	while ((fd = accept(http_server_fd, NULL, NULL)) >= 0) {
		// The request ends with an empty line
		for (len = 0; len < (ssize_t)sizeof(req) - 1; ) {
			r = recv(fd, &req[len], sizeof(req) - 1 - len, 0);
			if (r <= 0)
				break;
			len += r;
			req[len] = 0;
			if (strstr(req, "\r\n\r\n") != NULL)
				break;
		}
		req[len] = 0;
		if (sscanf(req, "%7s %2047s", method, path) == 2) {
			http_requests++;
			http_handler(fd, method, path, req);
		}
		shutdown(fd, SHUT_WR);
		close(fd);
	}
	return NULL;
}

static inline BOOL http_start(http_handler_t handler)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t len = sizeof(addr);
	pthread_t thread;

	http_handler = handler;
	http_server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (http_server_fd < 0 || bind(http_server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		listen(http_server_fd, 16) != 0 || getsockname(http_server_fd, (struct sockaddr*)&addr, &len) != 0)
		return FALSE;
	http_port = ntohs(addr.sin_port);
	return pthread_create(&thread, NULL, http_server_thread, NULL) == 0;
}

// The URL of a path of the server
static inline char* http_url(const char* path)
{
	static char buf[8][256];
	static int i = 0;

	i = (i + 1) % 8;
	snprintf(buf[i], sizeof(buf[i]), "http://127.0.0.1:%d%s", http_port, path);
	return buf[i];
}

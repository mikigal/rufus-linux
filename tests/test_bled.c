/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the bled decompression library
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

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bled.h"
#include "test.h"

/* Must match payload() in data/bled/generate.py */
#define PAYLOAD_SIZE (600 * 1024)

static char data_dir[4096];
static _Atomic(uint32_t) cancel_request;

static void test_printf(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static char* payload(size_t size)
{
	char* buf = malloc(size + 128);
	size_t len = 0;
	int i = 0;

	while (len < size)
		len += (size_t)sprintf(&buf[len], "Rufus bled test line %08d: the quick brown fox jumps over the lazy dog\n", i++);
	return buf;
}

static const char* fixture(const char* name)
{
	static char path[4096 + 64];

	snprintf(path, sizeof(path), "%s/bled/%s", data_dir, name);
	return path;
}

static char* read_file(const char* path, size_t* size)
{
	struct stat st;
	char* buf;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	buf = malloc((size_t)st.st_size + 1);
	*size = (size_t)read(fd, buf, (size_t)st.st_size);
	close(fd);
	return buf;
}

static void test_to_buffer(const char* expected)
{
	static const struct { const char* name; int type; } cases[] = {
		{ "payload.gz", BLED_COMPRESSION_GZIP },
		{ "payload.xz", BLED_COMPRESSION_XZ },
		{ "payload.lzma", BLED_COMPRESSION_LZMA },
		{ "payload.bz2", BLED_COMPRESSION_BZIP2 },
		{ "payload.zst", BLED_COMPRESSION_ZSTD },
		{ "archive.zip", BLED_COMPRESSION_ZIP },	/* first member is the payload */
	};
	char* buf = malloc(PAYLOAD_SIZE);
	size_t i;

	for (i = 0; i < ARRAYSIZE(cases); i++) {
		int64_t r;

		memset(buf, 0, PAYLOAD_SIZE);
		r = bled_uncompress_to_buffer(fixture(cases[i].name), buf, PAYLOAD_SIZE, cases[i].type);
		if (r != PAYLOAD_SIZE)
			fprintf(stderr, "  while decompressing %s\n", cases[i].name);
		CHECK_EQ_INT(r, PAYLOAD_SIZE);
		CHECK_EQ_MEM(buf, expected, PAYLOAD_SIZE);
	}
	free(buf);
}

static void test_uncompressed_size(void)
{
	/* Formats that store their uncompressed size must report it exactly */
	CHECK_EQ_INT(bled_get_uncompressed_size(fixture("payload.gz"), BLED_COMPRESSION_GZIP), PAYLOAD_SIZE);
	CHECK_EQ_INT(bled_get_uncompressed_size(fixture("payload.xz"), BLED_COMPRESSION_XZ), PAYLOAD_SIZE);
	CHECK_EQ_INT(bled_get_uncompressed_size(fixture("payload.zst"), BLED_COMPRESSION_ZSTD), PAYLOAD_SIZE);
}

static void test_with_fds(const char* tmp_dir, const char* expected)
{
	char dst_path[4096 + 32];
	char* out;
	size_t size = 0;
	int src_fd, dst_fd;

	snprintf(dst_path, sizeof(dst_path), "%s/fds.out", tmp_dir);
	src_fd = open(fixture("payload.xz"), O_RDONLY);
	dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(src_fd >= 0 && dst_fd >= 0);
	CHECK_EQ_INT(bled_uncompress_with_fds(src_fd, dst_fd, BLED_COMPRESSION_XZ), PAYLOAD_SIZE);
	/* The descriptors must still belong to (and be closable by) the caller */
	CHECK_EQ_INT(close(src_fd), 0);
	CHECK_EQ_INT(close(dst_fd), 0);

	out = read_file(dst_path, &size);
	CHECK(out != NULL);
	CHECK_EQ_INT(size, PAYLOAD_SIZE);
	if (out != NULL && size == PAYLOAD_SIZE)
		CHECK_EQ_MEM(out, expected, PAYLOAD_SIZE);
	free(out);
	unlink(dst_path);
}

static void test_to_dir(const char* tmp_dir, const char* expected)
{
	char path[4096 + 64];
	char* out;
	size_t size = 0;

	CHECK(bled_uncompress_to_dir(fixture("archive.zip"), tmp_dir, BLED_COMPRESSION_ZIP) > 0);

	snprintf(path, sizeof(path), "%s/payload.txt", tmp_dir);
	out = read_file(path, &size);
	CHECK_EQ_INT(size, PAYLOAD_SIZE);
	if (out != NULL && size == PAYLOAD_SIZE)
		CHECK_EQ_MEM(out, expected, PAYLOAD_SIZE);
	free(out);
	unlink(path);

	snprintf(path, sizeof(path), "%s/dir/sub/hello.txt", tmp_dir);
	out = read_file(path, &size);
	CHECK(out != NULL);
	if (out != NULL) {
		out[size] = 0;
		CHECK_EQ_STR(out, "hello from a subdirectory\n");
	}
	free(out);
	unlink(path);
	snprintf(path, sizeof(path), "%s/dir/sub", tmp_dir);
	rmdir(path);
	snprintf(path, sizeof(path), "%s/dir", tmp_dir);
	rmdir(path);
}

static void test_zip_slip(const char* tmp_dir)
{
	char sandbox[4096 + 32], escaped[4096 + 32];

	snprintf(sandbox, sizeof(sandbox), "%s/sandbox", tmp_dir);
	snprintf(escaped, sizeof(escaped), "%s/escaped.txt", tmp_dir);
	CHECK_EQ_INT(mkdir(sandbox, 0755), 0);
	CHECK(bled_uncompress_to_dir(fixture("zipslip.zip"), sandbox, BLED_COMPRESSION_ZIP) < 0);
	CHECK(access(escaped, F_OK) != 0);
	unlink(escaped);
	rmdir(sandbox);
}

static void test_cancel(void)
{
	char* buf = malloc(PAYLOAD_SIZE);

	cancel_request = 1;
	CHECK(bled_uncompress_to_buffer(fixture("payload.gz"), buf, PAYLOAD_SIZE, BLED_COMPRESSION_GZIP) < 0);
	cancel_request = 0;
	free(buf);
}

int main(void)
{
	char tmp_dir[] = "/tmp/rufus-test-bled-XXXXXX";
	const char* env = getenv("RUFUS_TEST_DATA");
	char* expected;

	if (env == NULL) {
		fprintf(stderr, "RUFUS_TEST_DATA is not set\n");
		return 1;
	}
	snprintf(data_dir, sizeof(data_dir), "%s", env);
	if (mkdtemp(tmp_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	expected = payload(PAYLOAD_SIZE);
	CHECK_EQ_INT(bled_init(0, test_printf, NULL, NULL, NULL, NULL, &cancel_request), 0);

	test_to_buffer(expected);
	test_uncompressed_size();
	test_with_fds(tmp_dir, expected);
	test_to_dir(tmp_dir, expected);
	test_zip_slip(tmp_dir);
	test_cancel();

	bled_exit();
	free(expected);
	rmdir(tmp_dir);
	return TEST_RESULT();
}

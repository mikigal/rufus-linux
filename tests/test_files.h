/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit test helpers for the files and images that the tests create
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
 * The tests work in a temporary directory, which test_files_init() creates
 * and test_files_cleanup() removes, and they build their images there with
 * external tools (see have() and run()). The core's log goes to log_buf,
 * which test_files_cleanup() prints if a check failed.
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "rufus.h"
#include "test.h"

static char tmp_dir[] = "/tmp/rufus-test-XXXXXX";
static char log_buf[256 * 1024];

static void test_log(const char* text)
{
	safe_strcat(log_buf, sizeof(log_buf), text);
}

// The paths and file contents stay valid until the end of the test, as they end up in
// argv arrays, globals and expressions that use more than one of them
static void** buffers = NULL;
static size_t nb_buffers = 0;

static void* keep(void* buf)
{
	buffers = realloc(buffers, (nb_buffers + 1) * sizeof(void*));
	CHECK(buffers != NULL);
	buffers[nb_buffers++] = buf;
	return buf;
}

static char* tmp_path(const char* name)
{
	char* p = keep(malloc(MAX_PATH));

	CHECK(p != NULL);
	snprintf(p, MAX_PATH, "%s/%s", tmp_dir, name);
	return p;
}

// Create a file (and its directories) in the temporary directory
static void make_file(const char* name, const char* content)
{
	char path[MAX_PATH], *p;

	static_strcpy(path, tmp_path(name));
	p = strrchr(path, '/');
	*p = 0;
	CHECK(mkdir_p(path, 0755) == 0);
	*p = '/';
	CHECK(write_file(path, (const uint8_t*)content, (uint32_t)strlen(content)) == strlen(content));
}

// The content of a file, or "" if it can't be read
static char* file_content(const char* path)
{
	char* buf = NULL;

	if (read_file(path, (uint8_t**)&buf) == 0) {
		free(buf);
		return "";
	}
	return keep(buf);
}

static BOOL run(const char* const* argv)
{
	DWORD r = RunCommand(argv, NULL, TRUE);
	if (r != 0)
		fprintf(stderr, "Command '%s' failed with %u:\n%s\n", argv[0], r, log_buf);
	return (r == 0);
}

static BOOL have(const char* tool)
{
	const char* argv[] = { "sh", "-c", "command -v \"$0\" >/dev/null", tool, NULL };
	return (RunCommand(argv, NULL, FALSE) == 0);
}

// Returns FALSE (after printing which one) if one of the tools is missing
static BOOL have_tools(const char* const* tools, size_t nb_tools)
{
	size_t i;

	for (i = 0; i < nb_tools; i++) {
		if (!have(tools[i])) {
			fprintf(stderr, "'%s' is not installed - skipping\n", tools[i]);
			return FALSE;
		}
	}
	return TRUE;
}

static BOOL test_files_init(void)
{
	if (mkdtemp(tmp_dir) == NULL) {
		perror("mkdtemp");
		return FALSE;
	}
	return TRUE;
}

static void test_files_cleanup(void)
{
	char cmd[64];
	size_t i;

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_dir);
	IGNORE_RETVAL(system(cmd));
	for (i = 0; i < nb_buffers; i++)
		free(buffers[i]);
	free(buffers);
	buffers = NULL;
	nb_buffers = 0;
	if (test_failures != 0)
		fprintf(stderr, "Log:\n%s\n", log_buf);
}

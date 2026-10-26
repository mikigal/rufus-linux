/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for wimlib and its Rufus patches
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
 * Exercises the wimlib operations Rufus relies on: open, extract a single
 * path (e.g. bootmgfw.efi or the SYSTEM hive), add a file to an image and
 * rewrite in place (boot.wim customization), split into .swm parts (FAT32
 * targets), and the Rufus specific "image.iso|path/in/image.wim" open.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wimlib.h"
#include "test.h"

static char tmp_dir[] = "/tmp/rufus-test-wimlib-XXXXXX";

void uprintf(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static const char* tmp_path(const char* name)
{
	static char path[4][4096 + 64];
	static int i = 0;

	i = (i + 1) % 4;
	snprintf(path[i], sizeof(path[i]), "%s/%s", tmp_dir, name);
	return path[i];
}

static void write_file(const char* path, const char* content)
{
	FILE* f = fopen(path, "w");

	CHECK(f != NULL);
	if (f != NULL) {
		fputs(content, f);
		fclose(f);
	}
}

static char* read_text(const char* path)
{
	static char buf[4096];
	size_t n;
	FILE* f = fopen(path, "r");

	if (f == NULL)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

/* Create a WIM with a Windows-like tree, the way an install.wim looks */
static void create_wim(const char* wim_path)
{
	WIMStruct* wim = NULL;

	CHECK_EQ_INT(mkdir(tmp_path("tree"), 0755), 0);
	CHECK_EQ_INT(mkdir(tmp_path("tree/Windows"), 0755), 0);
	CHECK_EQ_INT(mkdir(tmp_path("tree/Windows/Boot"), 0755), 0);
	CHECK_EQ_INT(mkdir(tmp_path("tree/Windows/Boot/EFI"), 0755), 0);
	write_file(tmp_path("tree/Windows/Boot/EFI/bootmgfw.efi"), "MZ fake boot manager\n");
	write_file(tmp_path("tree/readme.txt"), "image one\n");

	CHECK_EQ_INT(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &wim), 0);
	CHECK_EQ_INT(wimlib_add_image(wim, tmp_path("tree"), "Windows 11 Pro", NULL, 0), 0);
	CHECK_EQ_INT(wimlib_write(wim, wim_path, WIMLIB_ALL_IMAGES, 0, 0), 0);
	wimlib_free(wim);
}

static void test_extract_and_update(const char* wim_path)
{
	const char* path = "/Windows/Boot/EFI/bootmgfw.efi";
	struct wimlib_update_command cmd = { 0 };
	WIMStruct* wim = NULL;
	char* text;

	CHECK_EQ_INT(wimlib_open_wim(wim_path, 0, &wim), 0);
	if (wim == NULL)
		return;
	CHECK_EQ_INT(wimlib_resolve_image(wim, "Windows 11 Pro"), 1);
	CHECK_EQ_INT(mkdir(tmp_path("out"), 0755), 0);
	CHECK_EQ_INT(wimlib_extract_paths(wim, 1, tmp_path("out"), &path, 1,
		WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE), 0);
	text = read_text(tmp_path("out/bootmgfw.efi"));
	CHECK(text != NULL);
	if (text != NULL)
		CHECK_EQ_STR(text, "MZ fake boot manager\n");
	wimlib_free(wim);

	/* Add a file to the image and rewrite the WIM in place */
	write_file(tmp_path("autounattend.xml"), "<unattend/>\n");
	CHECK_EQ_INT(wimlib_open_wim(wim_path, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim), 0);
	if (wim == NULL)
		return;
	cmd.op = WIMLIB_UPDATE_OP_ADD;
	cmd.add.fs_source_path = (char*)tmp_path("autounattend.xml");
	cmd.add.wim_target_path = "/autounattend.xml";
	CHECK_EQ_INT(wimlib_update_image(wim, 1, &cmd, 1, 0), 0);
	CHECK_EQ_INT(wimlib_overwrite(wim, 0, 0), 0);
	wimlib_free(wim);

	path = "/autounattend.xml";
	CHECK_EQ_INT(wimlib_open_wim(wim_path, 0, &wim), 0);
	if (wim == NULL)
		return;
	CHECK_EQ_INT(wimlib_extract_paths(wim, 1, tmp_path("out"), &path, 1,
		WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE), 0);
	text = read_text(tmp_path("out/autounattend.xml"));
	CHECK(text != NULL);
	if (text != NULL)
		CHECK_EQ_STR(text, "<unattend/>\n");
	wimlib_free(wim);
}

static void test_split(const char* wim_path)
{
	WIMStruct* wim = NULL;
	struct stat st;

	CHECK_EQ_INT(wimlib_open_wim(wim_path, 0, &wim), 0);
	if (wim == NULL)
		return;
	/* Rufus splits at 4094 MB; any part size larger than the WIM works here */
	CHECK_EQ_INT(wimlib_split(wim, tmp_path("install.swm"), 4094ULL * 1024 * 1024, 0), 0);
	wimlib_free(wim);
	CHECK_EQ_INT(stat(tmp_path("install.swm"), &st), 0);
}

static void test_open_from_iso(const char* wim_path)
{
	char cmd[16384], iso_wim[4096 + 64];
	const char* path = "/readme.txt";
	WIMStruct* wim = NULL;
	char* text;

	if (system("command -v xorriso >/dev/null 2>&1") != 0) {
		fprintf(stderr, "xorriso not found, skipping the ISO test\n");
		return;
	}
	CHECK_EQ_INT(mkdir(tmp_path("iso"), 0755), 0);
	CHECK_EQ_INT(mkdir(tmp_path("iso/sources"), 0755), 0);
	snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", wim_path, tmp_path("iso/sources/install.wim"));
	CHECK_EQ_INT(system(cmd), 0);

	/* ISO-9660 + Joliet only, then UDF + ISO-9660 like Windows media */
	snprintf(cmd, sizeof(cmd), "xorriso -as mkisofs -quiet -J -o '%s' '%s'",
		tmp_path("plain.iso"), tmp_path("iso"));
	CHECK_EQ_INT(system(cmd), 0);
	snprintf(iso_wim, sizeof(iso_wim), "%s|sources/install.wim", tmp_path("plain.iso"));
	CHECK_EQ_INT(wimlib_open_wim(iso_wim, 0, &wim), 0);
	if (wim != NULL) {
		CHECK_EQ_INT(mkdir(tmp_path("out_iso"), 0755), 0);
		CHECK_EQ_INT(wimlib_extract_paths(wim, 1, tmp_path("out_iso"), &path, 1,
			WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE), 0);
		text = read_text(tmp_path("out_iso/readme.txt"));
		CHECK(text != NULL);
		if (text != NULL)
			CHECK_EQ_STR(text, "image one\n");
		wimlib_free(wim);
	}
	/* Files inside an image are read-only */
	wim = NULL;
	CHECK_EQ_INT(wimlib_open_wim(iso_wim, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim), WIMLIB_ERR_WIM_IS_READONLY);
	/* Missing file inside an existing image */
	snprintf(iso_wim, sizeof(iso_wim), "%s|sources/boot.wim", tmp_path("plain.iso"));
	CHECK(wimlib_open_wim(iso_wim, 0, &wim) != 0);
}

int main(void)
{
	char cmd[4096 + 16];
	const char* wim_path;

	if (mkdtemp(tmp_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	CHECK_EQ_INT(wimlib_global_init(0), 0);
	wimlib_set_print_errors(true);

	wim_path = strdup(tmp_path("install.wim"));
	create_wim(wim_path);
	test_extract_and_update(wim_path);
	test_split(wim_path);
	test_open_from_iso(wim_path);

	wimlib_global_cleanup();
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_dir);
	(void)!system(cmd);
	free((void*)wim_path);
	return TEST_RESULT();
}

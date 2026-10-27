/*
 * Rufus: The Reliable USB Formatting Utility
 * The Windows images of the end-to-end tests
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
 * rufus-test-mkwim <tree>: add the sources\boot.wim and sources\install.wim of
 * a Windows 11 24H2 media to a directory, which the media builder of the tests
 * (harness/media.py) makes an ISO of. The SYSTEM hive of boot.wim has the
 * HKLM\SYSTEM\Setup key, as the one of Windows does.
 *
 * rufus-test-mkwim --check <boot.wim>: print what the Windows User Experience
 * did to boot.wim: the values of HKLM\SYSTEM\Setup\LabConfig (with hivex), and
 * whether it has an Autounattend.xml.
 */

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rufus.h"
#include "wimlib.h"
#include "test.h"
#include "test_hive.h"

static char tmp_dir[] = "/tmp/rufus-mkwim-XXXXXX";

static BOOL make(const char* tree, const char* name, const char* content)
{
	char path[MAX_PATH], *p;

	snprintf(path, sizeof(path), "%s/%s", tree, name);
	p = strrchr(path, '/');
	*p = 0;
	if (mkdir_p(path, 0755) != 0)
		return FALSE;
	*p = '/';
	return write_file(path, (const uint8_t*)content, (uint32_t)strlen(content)) == strlen(content);
}

static int build(const char* tree)
{
	char src[MAX_PATH], path[MAX_PATH];
	WIMStruct* wim = NULL;
	static const char* properties[][2] = {
		{ "DISPLAYNAME", "Windows 11 Pro" },
		{ "WINDOWS/VERSION/MAJOR", "10" },
		{ "WINDOWS/VERSION/MINOR", "0" },
		{ "WINDOWS/VERSION/BUILD", "26100" },
		{ "WINDOWS/VERSION/SPBUILD", "1742" },
		{ "WINDOWS/EDITIONID", "Professional" },
		{ "WINDOWS/ARCH", "9" },
		{ "WINDOWS/LANGUAGES/LANGUAGE", "en-US" },
	};
	size_t i;
	int r = 1;

	// boot.wim: Windows PE and Windows Setup, with a SYSTEM hive
	snprintf(src, sizeof(src), "%s/boot", tmp_dir);
	if (!make(src, "Windows/System32/ntoskrnl.exe", "kernel") || !make(src, "Windows/System32/config/.keep", ""))
		goto out;
	snprintf(path, sizeof(path), "%s/Windows/System32/config/SYSTEM", src);
	make_hive(path);
#if defined(RUFUS_HAVE_HIVEX)
	add_hive_key(path, "Setup");
#endif
	if (wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) != 0 ||
		wimlib_add_image(wim, src, "Microsoft Windows PE (amd64)", NULL, 0) != 0 ||
		wimlib_add_image(wim, src, "Microsoft Windows Setup (amd64)", NULL, 0) != 0)
		goto out;
	snprintf(path, sizeof(path), "%s/sources", tree);
	if (mkdir_p(path, 0755) != 0)
		goto out;
	snprintf(path, sizeof(path), "%s/sources/boot.wim", tree);
	if (wimlib_write(wim, path, WIMLIB_ALL_IMAGES, 0, 1) != 0)
		goto out;
	wimlib_free(wim);
	wim = NULL;

	// install.wim: Windows 11 Pro 24H2, x64, with the EFI boot manager whose architecture Rufus checks
	snprintf(src, sizeof(src), "%s/install", tmp_dir);
	if (!make(src, "Windows/System32/ntoskrnl.exe", "kernel") ||
		!make(src, "Windows/Boot/EFI/bootmgfw.efi", "bootmgfw"))
		goto out;
	if (wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) != 0 ||
		wimlib_add_image(wim, src, "Windows 11 Pro", NULL, 0) != 0)
		goto out;
	for (i = 0; i < ARRAYSIZE(properties); i++) {
		if (wimlib_set_image_property(wim, 1, properties[i][0], properties[i][1]) != 0)
			goto out;
	}
	snprintf(path, sizeof(path), "%s/sources/install.wim", tree);
	if (wimlib_write(wim, path, WIMLIB_ALL_IMAGES, 0, 1) != 0)
		goto out;
	// The files that the Windows User Experience modifies
	if (!make(tree, "sources/appraiserres.dll", "appraiser"))
		goto out;
	r = 0;

out:
	if (wim != NULL)
		wimlib_free(wim);
	return r;
}

static int check(const char* boot_wim)
{
	char dir[MAX_PATH], path[MAX_PATH];
	const char* paths[] = { "Windows/System32/config/SYSTEM", "Autounattend.xml" };
	WIMStruct* wim = NULL;
	struct stat st;
	int r = 1;

	snprintf(dir, sizeof(dir), "%s/check", tmp_dir);
	if (mkdir_p(dir, 0755) != 0 || wimlib_open_wim(boot_wim, 0, &wim) != 0)
		return 1;
	if (wimlib_extract_paths(wim, 2, dir, &paths[0], 1, WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0)
		goto out;
	snprintf(path, sizeof(path), "%s/SYSTEM", dir);
#if defined(RUFUS_HAVE_HIVEX)
	printf("BypassTPMCheck=%d\n", lab_config_value(path, "BypassTPMCheck"));
	printf("BypassSecureBootCheck=%d\n", lab_config_value(path, "BypassSecureBootCheck"));
	printf("BypassRAMCheck=%d\n", lab_config_value(path, "BypassRAMCheck"));
#else
	printf("No hivex\n");
#endif
	wimlib_extract_paths(wim, 2, dir, &paths[1], 1, WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE);
	snprintf(path, sizeof(path), "%s/Autounattend.xml", dir);
	printf("Autounattend.xml=%s\n", (stat(path, &st) == 0) ? "yes" : "no");
	r = 0;

out:
	wimlib_free(wim);
	return r;
}

static int remove_entry(const char* path, const struct stat* st, int type, struct FTW* ftw)
{
	return remove(path);
}

int main(int argc, char** argv)
{
	int r;

	if (argc < 2 || (strcmp(argv[1], "--check") == 0 && argc < 3)) {
		fprintf(stderr, "Usage: %s <tree> | --check <boot.wim>\n", argv[0]);
		return 2;
	}
	if (mkdtemp(tmp_dir) == NULL)
		return 1;
	wimlib_global_init(0);
	r = (strcmp(argv[1], "--check") == 0) ? check(argv[2]) : build(argv[1]);
	wimlib_global_cleanup();
	nftw(tmp_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	return r;
}

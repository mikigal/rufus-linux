/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the Rufus fork of libcdio
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
 * Builds a hybrid BIOS/UEFI El Torito ISO with xorriso and checks what Rufus'
 * ISO scan depends on: volume label, Joliet/Rock Ridge long names, file
 * contents, and the fork-specific virtual "/[BOOT]/" El Torito images.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cdio/iso9660.h>

#include "test.h"

#define LONG_NAME   "Long File Name With Spaces.txt"
#define BIOS_SIZE   (4 * ISO_BLOCKSIZE)
/* El Torito entries are sized by their load size, in 512-byte virtual sectors */
#define BIOS_LOAD_SECTORS 4
#define EFI_SIZE    (32 * ISO_BLOCKSIZE)

static char tmp_dir[] = "/tmp/rufus-test-libcdio-XXXXXX";

static const char* tmp_path(const char* name)
{
	static char path[4][4096 + 64];
	static int i = 0;

	i = (i + 1) % 4;
	snprintf(path[i], sizeof(path[i]), "%s/%s", tmp_dir, name);
	return path[i];
}

static void write_pattern(const char* path, char c, size_t size)
{
	char* buf = malloc(size);
	FILE* f = fopen(path, "wb");

	memset(buf, c, size);
	CHECK(f != NULL && fwrite(buf, 1, size, f) == size);
	if (f != NULL)
		fclose(f);
	free(buf);
}

/* Read the first block of 'path' in the ISO and check that it is filled with 'c' */
static void check_content(iso9660_t* iso, const char* path, char c, uint64_t size)
{
	char block[ISO_BLOCKSIZE], expected[ISO_BLOCKSIZE];
	iso9660_stat_t* st = iso9660_ifs_stat_translate(iso, path);

	CHECK(st != NULL);
	if (st == NULL) {
		fprintf(stderr, "  missing: %s\n", path);
		return;
	}
	CHECK_EQ_INT(st->total_size, size);
	memset(expected, c, sizeof(expected));
	CHECK_EQ_INT(iso9660_iso_seek_read(iso, block, st->lsn, 1), ISO_BLOCKSIZE);
	CHECK_EQ_MEM(block, expected, sizeof(block));
	iso9660_stat_free(st);
}

static void test_scan(const char* iso_path)
{
	iso9660_t* iso = iso9660_open_ext(iso_path, ISO_EXTENSION_ALL);
	iso9660_stat_t* st;
	CdioISO9660FileList_t* list;
	CdioListNode_t* node;
	char* label = NULL;
	int boot_images = 0;

	CHECK(iso != NULL);
	if (iso == NULL)
		return;

	CHECK(iso9660_ifs_get_volume_id(iso, &label));
	CHECK_EQ_STR(label, "RUFUS_TEST");
	free(label);

	/* Long names come from Joliet / Rock Ridge */
	st = iso9660_ifs_stat_translate(iso, "/" LONG_NAME);
	CHECK(st != NULL);
	iso9660_stat_free(st);
	check_content(iso, "/boot/bios.img", 'B', BIOS_SIZE);

	/* El Torito images exposed by the fork as virtual files */
	list = iso9660_ifs_readdir(iso, "/[BOOT]");
	CHECK(list != NULL);
	if (list != NULL) {
		_CDIO_LIST_FOREACH(node, list) {
			st = (iso9660_stat_t*)_cdio_list_node_data(node);
			if (strstr(st->filename, "-Boot-") != NULL)
				boot_images++;
		}
		iso9660_filelist_free(list);
	}
	CHECK_EQ_INT(boot_images, 2);
	/* Entry 0 is the BIOS image, of which the firmware only loads the first
	 * BIOS_LOAD_SECTORS, and entry 1 the EFI one that Rufus looks for */
	check_content(iso, "/[BOOT]/0-Boot-NoEmul.img", 'B', BIOS_LOAD_SECTORS * 512);
	check_content(iso, "/[BOOT]/1-Boot-NoEmul.img", 'E', EFI_SIZE);
	iso9660_close(iso);

	/* Without the Joliet and Rock Ridge extensions only 8.3 names remain */
	iso = iso9660_open_ext(iso_path, ISO_EXTENSION_NONE);
	CHECK(iso != NULL);
	if (iso != NULL) {
		st = iso9660_ifs_stat_translate(iso, "/" LONG_NAME);
		CHECK(st == NULL);
		iso9660_stat_free(st);
		iso9660_close(iso);
	}
}

int main(void)
{
	char cmd[16384];

	if (system("command -v xorriso >/dev/null 2>&1") != 0) {
		fprintf(stderr, "xorriso not found, skipping\n");
		return 77;
	}
	if (mkdtemp(tmp_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	CHECK_EQ_INT(mkdir(tmp_path("iso"), 0755), 0);
	CHECK_EQ_INT(mkdir(tmp_path("iso/boot"), 0755), 0);
	write_pattern(tmp_path("iso/boot/bios.img"), 'B', BIOS_SIZE);
	write_pattern(tmp_path("iso/efi.img"), 'E', EFI_SIZE);
	write_pattern(tmp_path("iso/" LONG_NAME), 'L', 100);
	snprintf(cmd, sizeof(cmd), "xorriso -as mkisofs -quiet -J -R -V RUFUS_TEST "
		"-b boot/bios.img -no-emul-boot -boot-load-size %d "
		"-eltorito-alt-boot -e efi.img -no-emul-boot -o '%s' '%s'",
		BIOS_LOAD_SECTORS, tmp_path("test.iso"), tmp_path("iso"));
	CHECK_EQ_INT(system(cmd), 0);

	test_scan(tmp_path("test.iso"));

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_dir);
	(void)!system(cmd);
	return TEST_RESULT();
}

/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the ms-sys boot record library
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
#include <unistd.h>

#include "br.h"
#include "fat32.h"
#include "file.h"
#include "ntfs.h"
#include "test.h"

#define DISK_SIZE      (8 * 1024 * 1024)
#define PART_OFFSET    (1024 * 1024)

/* Defined in br.c, set by the application to the drive's sector size */
extern unsigned long ulBytesPerSector;

/* Provided by the application */
void uprintf(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

static void pwrite_all(int fd, const void* buf, size_t len, off_t offset)
{
	CHECK_EQ_INT(pwrite(fd, buf, len, offset), (ssize_t)len);
}

static void test_mbr(int fd)
{
	FAKE_FD fake = { fd, 0 };
	FILE* fp = (FILE*)&fake;

	CHECK(is_zero_mbr(fp));
	CHECK(!is_br(fp));

	CHECK(write_win7_mbr(fp));
	CHECK(is_br(fp));
	CHECK(is_win7_mbr(fp));
	CHECK(!is_rufus_mbr(fp));

	CHECK(write_windows_disk_signature(fp, 0x49464555));
	CHECK_EQ_INT(read_windows_disk_signature(fp), 0x49464555);
	/* The disk signature lives outside of the boot code */
	CHECK(is_win7_mbr(fp));

	CHECK(write_rufus_mbr(fp));
	CHECK(is_rufus_mbr(fp));
	CHECK(!is_win7_mbr(fp));
	CHECK_EQ_INT(read_windows_disk_signature(fp), 0x49464555);
}

static void test_fat32_pbr(int fd)
{
	/* Minimal FAT32 boot sector: just enough for ms-sys to recognize it */
	uint8_t bs[512] = { 0 };
	uint8_t check[512];
	FAKE_FD fake = { fd, PART_OFFSET };
	FILE* fp = (FILE*)&fake;

	memcpy(&bs[0x03], "MSDOS5.0", 8);
	bs[0x0B] = 0x00; bs[0x0C] = 0x02;	/* 512 bytes per sector */
	bs[0x0D] = 8;				/* sectors per cluster */
	memcpy(&bs[0x47], "RUFUS TEST ", 11);
	memcpy(&bs[0x52], "FAT32   ", 8);
	bs[0x1FE] = 0x55; bs[0x1FF] = 0xAA;
	pwrite_all(fd, bs, sizeof(bs), PART_OFFSET);

	CHECK(is_fat_32_fs(fp));
	CHECK(write_fat_32_br(fp, 1));
	CHECK(entire_fat_32_br_matches(fp));

	/* The BPB and the label must have been preserved */
	CHECK_EQ_INT(pread(fd, check, sizeof(check), PART_OFFSET), (ssize_t)sizeof(check));
	CHECK_EQ_MEM(&check[0x0B], &bs[0x0B], 0x47 - 0x0B);
	CHECK_EQ_MEM(&check[0x47], "RUFUS TEST ", 11);

	/* Writing through a partition offset must not touch the MBR */
	fake._offset = 0;
	CHECK(is_rufus_mbr(fp));
}

static void test_ntfs_pbr(int fd)
{
	uint8_t bs[512] = { 0 };
	FAKE_FD fake = { fd, 2 * PART_OFFSET };
	FILE* fp = (FILE*)&fake;

	memcpy(&bs[0x03], "NTFS    ", 8);
	bs[0x0B] = 0x00; bs[0x0C] = 0x02;
	bs[0x1FE] = 0x55; bs[0x1FF] = 0xAA;
	pwrite_all(fd, bs, sizeof(bs), 2 * PART_OFFSET);

	CHECK(is_ntfs_fs(fp));
	CHECK(!is_fat_32_fs(fp));
	CHECK(write_ntfs_br(fp));
	CHECK(entire_ntfs_br_matches(fp));
}

static void test_errors(int fd)
{
	uint8_t buf[512];
	/* Positioned past the end of the image: reads must fail cleanly */
	FAKE_FD fake = { fd, DISK_SIZE };
	FILE* fp = (FILE*)&fake;

	CHECK(!is_fat_32_fs(fp));
	CHECK(!read_data(fp, 0, buf, sizeof(buf)));
	CHECK(read_sectors(-1, 512, 0, 1, buf) < 0);
	CHECK(write_sectors(-1, 512, 0, 1, buf) < 0);
}

int main(void)
{
	char path[] = "/tmp/rufus-test-mssys-XXXXXX";
	int fd = mkstemp(path);

	if (fd < 0 || ftruncate(fd, DISK_SIZE) != 0) {
		perror("test image");
		return 1;
	}
	unlink(path);

	ulBytesPerSector = 512;
	test_mbr(fd);
	test_fat32_pbr(fd);
	test_ntfs_pbr(fd);
	test_errors(fd);

	close(fd);
	return TEST_RESULT();
}

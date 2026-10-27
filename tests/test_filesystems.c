/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the file systems
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
 * FormatPartition() on image files, checked with blkid, the fsck of each file
 * system and the fields of the boot sectors. The file systems whose mkfs tool
 * is missing are skipped (mkntfs: ntfsprogs, mkfs.exfat: exfatprogs, mkudffs:
 * udftools).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "filesystems.h"
#include "test.h"
#include "test_files.h"

#define PART_OFFSET                 (1 * MB)

static char* image(const char* name, uint64_t size)
{
	char* path = tmp_path(name);
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);

	CHECK((fd >= 0) && (ftruncate(fd, (off_t)size) == 0));
	close(fd);
	return path;
}

// What blkid finds in an image (TYPE, LABEL, VERSION...)
static char* blkid(const char* path, const char* tag)
{
	const char* argv[] = { "blkid", "-p", "-o", "value", "-s", tag, path, NULL };
	char* r;

	log_buf[0] = 0;
	if (RunCommand(argv, NULL, TRUE) != 0)
		return "";
	r = keep(strdup(log_buf));
	trim(r);
	return r;
}

static BOOL check(const char* tool, const char* path)
{
	const char* argv[] = { tool, "-n", path, NULL };
	const char* e2fsck[] = { tool, "-fn", path, NULL };

	return (RunCommand((strcmp(tool, "e2fsck") == 0) ? e2fsck : argv, NULL, TRUE) == 0);
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void boot_sector(const char* path, uint8_t* bs)
{
	int fd = open(path, O_RDONLY);

	CHECK((fd >= 0) && (pread(fd, bs, 512, 0) == 512));
	close(fd);
}

static void reset(void)
{
	ResetOptions(&opts);
	opts.disk_size = 8 * GB;
	ErrorStatus = 0;
}

static void test_fat(void)
{
	uint8_t bs[512], pattern[16];
	char* path;
	int fd;

	// FAT32, with the BPB of a partition at 1 MB
	reset();
	path = image("fat32.img", 256 * MB);
	CHECK(FormatPartition(path, PART_OFFSET, 256 * MB, 512, 4096, FS_FAT32, "MY_USB", FP_QUICK));
	CHECK_EQ_STR(blkid(path, "TYPE"), "vfat");
	CHECK_EQ_STR(blkid(path, "VERSION"), "FAT32");
	CHECK_EQ_STR(blkid(path, "LABEL"), "MY_USB");
	CHECK(check("fsck.fat", path));
	boot_sector(path, bs);
	CHECK_EQ_INT(le16(&bs[11]), 512);			// Bytes per sector
	CHECK_EQ_INT(bs[13], 8);					// Sectors per cluster
	CHECK_EQ_INT(le16(&bs[24]), 63);			// Sectors per track
	CHECK_EQ_INT(le16(&bs[26]), 255);			// Heads
	CHECK_EQ_INT(le32(&bs[28]), PART_OFFSET / 512);	// Hidden sectors
	CHECK((bs[510] == 0x55) && (bs[511] == 0xaa));

	// FAT16, slow: the volume gets zeroed first
	reset();
	path = image("fat16.img", 64 * MB);
	fd = open(path, O_WRONLY);
	memset(pattern, 0xaa, sizeof(pattern));
	CHECK(pwrite(fd, pattern, sizeof(pattern), 60 * MB) == sizeof(pattern));
	close(fd);
	CHECK(FormatPartition(path, 0, 64 * MB, 512, 0, FS_FAT16, "FAT16", 0));
	CHECK_EQ_STR(blkid(path, "VERSION"), "FAT16");
	CHECK_EQ_STR(blkid(path, "LABEL"), "FAT16");
	CHECK(check("fsck.fat", path));
	fd = open(path, O_RDONLY);
	CHECK(pread(fd, pattern, sizeof(pattern), 60 * MB) == sizeof(pattern));
	close(fd);
	CHECK_EQ_MEM(pattern, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);

	// Large FAT32 (as for drives over 32 GB), written by Rufus itself
	reset();
	path = image("large.img", 512 * MB);
	CHECK(FormatPartition(path, PART_OFFSET, 512 * MB, 512, 0, FS_FAT32, "LARGE FAT", FP_QUICK | FP_LARGE_FAT32));
	CHECK_EQ_STR(blkid(path, "VERSION"), "FAT32");
	CHECK_EQ_STR(blkid(path, "LABEL"), "LARGE FAT");
	CHECK(check("fsck.fat", path));
	boot_sector(path, bs);
	CHECK_EQ_MEM(&bs[3], "MSWIN4.1", 8);
	CHECK_EQ_INT(le32(&bs[28]), PART_OFFSET / 512);
	CHECK_EQ_MEM(&bs[71], "LARGE FAT  ", 11);
	// The data area starts on a MB boundary
	CHECK_EQ_INT(((le16(&bs[14]) + bs[16] * le32(&bs[36])) * 512) % MB, 0);
	// Which Alt-L also selects, and which a cancellation stops
	reset();
	opts.force_large_fat32 = TRUE;
	CancelOperation();
	CHECK(!FormatPartition(path, PART_OFFSET, 512 * MB, 512, 0, FS_FAT32, "LARGE FAT", FP_QUICK));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_CANCELLED));
	ErrorStatus = 0;
}

static void test_ext(void)
{
	const char* dumpe2fs[] = { "dumpe2fs", "-h", tmp_path("ext4.img"), NULL };
	const char* debugfs[] = { "debugfs", "-R", "cat /persistence.conf", tmp_path("ext3.img"), NULL };
	char* path;

	reset();
	path = image("ext2.img", 64 * MB);
	CHECK(FormatPartition(path, 0, 64 * MB, 512, 0, FS_EXT2, "EXT2", FP_QUICK));
	CHECK_EQ_STR(blkid(path, "TYPE"), "ext2");
	CHECK_EQ_STR(blkid(path, "LABEL"), "EXT2");
	CHECK(check("e2fsck", path));

	// With the persistence.conf of Debian live media
	path = image("ext3.img", 64 * MB);
	CHECK(FormatPartition(path, 0, 64 * MB, 512, 0, FS_EXT3, "persistence", FP_QUICK | FP_CREATE_PERSISTENCE_CONF));
	CHECK_EQ_STR(blkid(path, "TYPE"), "ext3");
	CHECK(check("e2fsck", path));
	log_buf[0] = 0;
	CHECK(RunCommand(debugfs, NULL, TRUE) == 0);
	CHECK_CONTAINS(log_buf, "/ union");

	// ext4, which upstream can't create
	path = image("ext4.img", 128 * MB);
	CHECK(FormatPartition(path, 0, 128 * MB, 512, 4096, FS_EXT4, "casper-rw", 0));
	CHECK_EQ_STR(blkid(path, "TYPE"), "ext4");
	CHECK_EQ_STR(blkid(path, "LABEL"), "casper-rw");
	CHECK(check("e2fsck", path));
	log_buf[0] = 0;
	CHECK(RunCommand(dumpe2fs, NULL, TRUE) == 0);
	CHECK_CONTAINS(log_buf, "metadata_csum");
	CHECK_CONTAINS(log_buf, "extent");
	CHECK_CONTAINS(log_buf, "Block size:               4096");
}

static void test_other(void)
{
	uint8_t bs[512];
	char* path;

	if (have("mkntfs")) {
		reset();
		path = image("ntfs.img", 256 * MB);
		CHECK(FormatPartition(path, PART_OFFSET, 256 * MB, 512, 4096, FS_NTFS, "NTFS Volume", FP_QUICK));
		CHECK_EQ_STR(blkid(path, "TYPE"), "ntfs");
		CHECK_EQ_STR(blkid(path, "LABEL"), "NTFS Volume");
		boot_sector(path, bs);
		CHECK_EQ_MEM(&bs[3], "NTFS    ", 8);
		CHECK_EQ_INT(bs[13], 8);
		CHECK_EQ_INT(le32(&bs[28]), PART_OFFSET / 512);
		if (have("ntfsfix")) {
			const char* argv[] = { "ntfsfix", "-n", path, NULL };
			CHECK(RunCommand(argv, NULL, TRUE) == 0);
		}
	} else {
		fprintf(stderr, "mkntfs is not installed - skipping NTFS\n");
	}

	if (have("mkfs.exfat")) {
		reset();
		path = image("exfat.img", 256 * MB);
		CHECK(FormatPartition(path, PART_OFFSET, 256 * MB, 512, 32768, FS_EXFAT, "EXFAT", FP_QUICK));
		CHECK_EQ_STR(blkid(path, "TYPE"), "exfat");
		CHECK_EQ_STR(blkid(path, "LABEL"), "EXFAT");
		if (have("fsck.exfat"))
			CHECK(check("fsck.exfat", path));
	} else {
		fprintf(stderr, "mkfs.exfat is not installed - skipping exFAT\n");
	}

	if (have("mkudffs")) {
		reset();
		path = image("udf.img", 256 * MB);
		CHECK(FormatPartition(path, PART_OFFSET, 256 * MB, 512, 0, FS_UDF, "UDF Volume", FP_QUICK));
		CHECK_EQ_STR(blkid(path, "TYPE"), "udf");
		CHECK_EQ_STR(blkid(path, "LABEL"), "UDF Volume");
	} else {
		fprintf(stderr, "mkudffs is not installed - skipping UDF\n");
	}

	// No ReFS
	reset();
	path = image("refs.img", 64 * MB);
	CHECK(!FormatPartition(path, 0, 64 * MB, 512, 0, FS_REFS, "ReFS", FP_QUICK));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_NOT_SUPPORTED));
	ErrorStatus = 0;
	CHECK(!FormatPartition(path, 0, 64 * MB, 512, 3000, FS_FAT32, "BAD", FP_QUICK));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_INVALID_PARAMETER));
	ErrorStatus = 0;
}

static void test_labels(void)
{
	char label[64];

	static_strcpy(label, "my usb:stick*");
	ToValidLabel(label, TRUE, 8 * GB);
	CHECK_EQ_STR(label, "MY USBSTICK");
	CHECK_EQ_STR(img_report.usb_label, "MY USBSTICK");
	static_strcpy(label, "v1.2\tbeta");
	ToValidLabel(label, TRUE, 8 * GB);
	CHECK_EQ_STR(label, "V1_2_BETA");
	// Too long
	static_strcpy(label, "ABCDEFGHIJKLMNOP");
	ToValidLabel(label, TRUE, 8 * GB);
	CHECK_EQ_STR(label, "ABCDEFGHIJK");
	// Mostly non-ASCII: the size of the drive instead
	static_strcpy(label, "Zażółć");
	ToValidLabel(label, TRUE, 8 * GB);
	CHECK_EQ_STR(label, "8 GB");
	static_strcpy(label, "Zażółć");
	ToValidLabel(label, TRUE, 7800 * MB);
	CHECK_EQ_STR(label, "7_6 GB");
	// NTFS keeps it all, up to 32 characters
	static_strcpy(label, "Zażółć gęślą jaźń: v1.0");
	ToValidLabel(label, FALSE, 8 * GB);
	CHECK_EQ_STR(label, "Zażółć gęślą jaźń: v1_0");
	static_strcpy(label, "0123456789abcdefghijklmnopqrstuvwxyz");
	ToValidLabel(label, FALSE, 8 * GB);
	CHECK_EQ_STR(label, "0123456789abcdefghijklmnopqrstuv");
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };
	const char* tools[] = { "mkfs.fat", "fsck.fat", "blkid", "e2fsck", "dumpe2fs", "debugfs" };

	ui_set_iface(&iface);
	test_labels();
	if (!have_tools(tools, ARRAYSIZE(tools)))
		return (test_failures != 0) ? 1 : 77;
	if (!test_files_init())
		return 1;
	test_fat();
	test_ext();
	test_other();
	test_files_cleanup();
	return TEST_RESULT();
}

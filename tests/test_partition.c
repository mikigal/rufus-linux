/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the partitioning
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
 * The layouts of PlanPartitions() for each kind of drive upstream creates
 * (the expected offsets and sizes were computed separately, with the formulas
 * of CreatePartition()), and the tables of WritePartitionTable() on image
 * files, read back by sfdisk.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "resource.h"
#include "ui_iface.h"
#include "partition.h"
#include "test.h"
#include "test_files.h"

#define DISK_SIZE                   (8ULL * GB)

static partition_layout_t layout;

static void reset(void)
{
	ResetOptions(&opts);
	opts.cluster_size = 4096;
	opts.boot_type = BT_IMAGE;
}

#define CHECK_PART(i, name_, offset_, size_) do { \
	CHECK_EQ_STR(layout.part[i].name, name_); \
	CHECK_EQ_INT(layout.part[i].offset, offset_); \
	CHECK_EQ_INT(layout.part[i].size, size_); \
} while (0)

static void test_layouts(void)
{
	// A single FAT32 partition
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, FALSE, 0, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.count, 1);
	CHECK_PART(0, "Main Data Partition", 1 * MB, 8588869632ULL);
	CHECK_EQ_INT(layout.part[0].mbr_type, 0x0c);
	CHECK(layout.part[0].bootable);
	CHECK(layout.mbr_signature != MBR_UEFI_MARKER);
	opts.boot_type = BT_NON_BOOTABLE;
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, TRUE, 0, DISK_SIZE, 512));
	CHECK(!layout.part[0].bootable);
	CHECK_EQ_INT(layout.mbr_signature, MBR_UEFI_MARKER);

	// The types of the other file systems
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT16, FALSE, 0, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.part[0].mbr_type, 0x0e);
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_EXFAT, FALSE, 0, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.part[0].mbr_type, 0x07);
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_EXT4, FALSE, 0, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.part[0].mbr_type, 0x83);
	CHECK(!PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_UNKNOWN, FALSE, 0, DISK_SIZE, 512));

	// GPT, with the ESP and MSR first (as Windows To Go has them)
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_GPT, FS_NTFS, FALSE, XP_ESP | XP_MSR, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.count, 3);
	CHECK_PART(0, "EFI System Partition", 1 * MB, 260 * MB);
	CHECK_PART(1, "Microsoft Reserved Partition", 273690624ULL, 128 * MB);
	CHECK_PART(2, "Main Data Partition", 407908352ULL, 8181989376ULL);
	CHECK_EQ_INT(layout.index[PI_ESP], 0);
	CHECK_EQ_INT(layout.index[PI_MAIN], 2);

	// NTFS with UEFI:NTFS at the end
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_NTFS, FALSE, XP_UEFI_NTFS, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.count, 2);
	CHECK_PART(0, "Main Data Partition", 1 * MB, 8587804672ULL);
	CHECK_PART(1, "UEFI:NTFS", 8588869632ULL, 1064448ULL);
	CHECK_EQ_INT(layout.part[1].mbr_type, 0xef);
	CHECK_EQ_INT(layout.index[PI_UEFI_NTFS], 1);
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_GPT, FS_NTFS, FALSE, XP_UEFI_NTFS, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.part[1].gpt_attributes, GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER);

	// Persistence
	reset();
	opts.persistence_size = 1 * GB;
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, FALSE, XP_PERSISTENCE, DISK_SIZE, 512));
	CHECK_PART(0, "Main Data Partition", 1 * MB, 7515099136ULL);
	CHECK_PART(1, "Linux Persistence", 7516164096ULL, 1073769984ULL);
	CHECK_EQ_INT(layout.part[1].mbr_type, 0x83);
	CHECK_EQ_INT(layout.index[PI_CASPER], 1);

	// Fixes for old BIOSes: aligned to a cylinder, and with a BIOS compatibility partition
	reset();
	opts.old_bios_fixes = TRUE;
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, FALSE, 0, DISK_SIZE, 512));
	CHECK_PART(0, "Main Data Partition", 65536, 8589836288ULL);
	opts.old_bios_fixes = FALSE;
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, FALSE, XP_COMPAT, DISK_SIZE, 512));
	CHECK_PART(0, "Main Data Partition", 1 * MB, 8588836864ULL);
	CHECK_PART(1, "BIOS Compatibility", 8589901824ULL, 32256);
	CHECK_EQ_INT(layout.part[1].mbr_type, RUFUS_EXTRA_PARTITION_TYPE);

	// ISO → ESP: at most 1 GB
	reset();
	opts.write_as_esp = TRUE;
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, FALSE, 0, DISK_SIZE, 512));
	CHECK_PART(0, "EFI System Partition", 1 * MB, 1073737728ULL);
	CHECK_EQ_INT(layout.part[0].mbr_type, 0xef);

	// 4K sectors
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_GPT, FS_FAT32, FALSE, 0, DISK_SIZE, 4096));
	CHECK_PART(0, "Main Data Partition", 1 * MB, 8588611584ULL);

	// No partition table, and drives that are too small
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_SFD, FS_FAT32, FALSE, 0, DISK_SIZE, 512));
	CHECK_EQ_INT(layout.count, 0);
	opts.persistence_size = 16 * GB;
	CHECK(!PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, FALSE, XP_PERSISTENCE, DISK_SIZE, 512));
}

// sfdisk indents its JSON, so the comparisons are made without any white space
static char* squeeze(const char* str)
{
	char* r = keep(strdup(str));
	char* d = r;

	for (; *str != 0; str++) {
		if ((*str != ' ') && (*str != '\n') && (*str != '\t'))
			*d++ = *str;
	}
	*d = 0;
	return r;
}
#define CHECK_JSON(json, needle) CHECK_CONTAINS(json, squeeze(needle))

// Write a layout to a new image file, and read it back with sfdisk
static char* write_and_dump(const char* name, uint32_t sector_size)
{
	char sector_opt[16];
	const char* argv[] = { "sfdisk", "--sector-size", sector_opt, "--json", tmp_path(name), NULL };
	int fd = open(tmp_path(name), O_RDWR | O_CREAT | O_TRUNC, 0644);

	CHECK(fd >= 0);
	CHECK(ftruncate(fd, (off_t)layout.disk_size) == 0);
	// Some leftovers, which the partitions must not have
	CHECK(pwrite(fd, "garbage", 7, (off_t)layout.part[0].offset) == 7);
	CHECK(WritePartitionTable(fd, &layout));
	close(fd);
	snprintf(sector_opt, sizeof(sector_opt), "%u", sector_size);
	log_buf[0] = 0;
	CHECK(RunCommand(argv, NULL, TRUE) == 0);
	CHECK(strstr(log_buf, "corrupt") == NULL);
	CHECK(strstr(log_buf, "mismatch") == NULL);
	return squeeze(log_buf);
}

static void test_tables(void)
{
	char *json, expected[256];
	uint8_t* buf = NULL, *uefi_ntfs;
	uint32_t size;

	// MBR, with the UEFI marker
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, TRUE, 0, DISK_SIZE, 512));
	json = write_and_dump("mbr.img", 512);
	CHECK_JSON(json, "\"label\": \"dos\"");
	CHECK_JSON(json, "\"id\": \"0x49464555\"");
	CHECK_JSON(json, "\"start\": 2048, \"size\": 16775136, \"type\": \"c\", \"bootable\": true}");
	{
		int fd = open(tmp_path("mbr.img"), O_RDONLY);
		uint8_t mbr[512], part[16];
		CHECK(pread(fd, mbr, sizeof(mbr), 0) == sizeof(mbr));
		// CHS (0, 32, 33) for LBA 2048, and the maximum for the end, past 8 GB
		CHECK_EQ_MEM(&mbr[446], "\x80\x20\x21\x00\x0c\xfe\xff\xff", 8);
		CHECK((mbr[510] == 0x55) && (mbr[511] == 0xaa));
		// The first sectors of the partition were zeroed
		CHECK(pread(fd, part, sizeof(part), 1 * MB) == sizeof(part));
		CHECK_EQ_MEM(part, "\0\0\0\0\0\0\0", 7);
		close(fd);
	}

	// MBR, with UEFI:NTFS
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_NTFS, FALSE, XP_UEFI_NTFS, DISK_SIZE, 512));
	json = write_and_dump("uefi_ntfs.img", 512);
	snprintf(expected, sizeof(expected), "\"start\": %llu, \"size\": %llu, \"type\": \"ef\"}",
		8588869632ULL / 512, 1064448ULL / 512);
	CHECK_JSON(json, expected);
	CHECK_JSON(json, "\"type\": \"7\", \"bootable\": true}");
	// With the UEFI:NTFS image in its partition
	uefi_ntfs = GetResource(IDR_UEFI_NTFS, "uefi-ntfs.img", &size, FALSE);
	CHECK(uefi_ntfs != NULL);
	{
		int fd = open(tmp_path("uefi_ntfs.img"), O_RDONLY);
		buf = malloc(size);
		CHECK((buf != NULL) && (pread(fd, buf, size, (off_t)8588869632ULL) == (ssize_t)size));
		CHECK((buf != NULL) && (uefi_ntfs != NULL) && (memcmp(buf, uefi_ntfs, size) == 0));
		free(buf);
		close(fd);
	}

	// GPT
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_GPT, FS_NTFS, FALSE, XP_ESP | XP_MSR | XP_UEFI_NTFS, DISK_SIZE, 512));
	json = write_and_dump("gpt.img", 512);
	CHECK_JSON(json, "\"label\": \"gpt\"");
	CHECK_JSON(json, "\"firstlba\": 34");
	snprintf(expected, sizeof(expected), "\"lastlba\": %llu", DISK_SIZE / 512 - 34);
	CHECK_JSON(json, expected);
	CHECK_JSON(json, "\"start\": 2048, \"size\": 532480, \"type\": \"C12A7328-F81F-11D2-BA4B-00A0C93EC93B\"");
	CHECK_JSON(json, "\"name\": \"EFI System Partition\"");
	CHECK_JSON(json, "\"type\": \"E3C9E316-0B5C-4DB8-817D-F92DF00215AE\"");
	CHECK_JSON(json, "\"name\": \"Microsoft Reserved Partition\"");
	CHECK_JSON(json, "\"type\": \"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\"");
	CHECK_JSON(json, "\"name\": \"UEFI:NTFS\", \"attrs\": \"GUID:63\"");
	{
		// The protective MBR, and the backup header at the end of the disk
		int fd = open(tmp_path("gpt.img"), O_RDONLY);
		uint8_t mbr[512], hdr[512];
		CHECK(pread(fd, mbr, sizeof(mbr), 0) == sizeof(mbr));
		CHECK_EQ_MEM(&mbr[446], "\x00\x00\x02\x00\xee\xfe\xff\xff\x01\x00\x00\x00\xff\xff\xff\x00", 16);
		CHECK(pread(fd, hdr, sizeof(hdr), (off_t)(DISK_SIZE - 512)) == sizeof(hdr));
		CHECK_EQ_MEM(hdr, "EFI PART", 8);
		close(fd);
	}

	// GPT on 4K sectors
	reset();
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_GPT, FS_FAT32, FALSE, 0, DISK_SIZE, 4096));
	json = write_and_dump("gpt4k.img", 4096);
	CHECK_JSON(json, "\"sectorsize\": 4096");
	CHECK_JSON(json, "\"firstlba\": 6");
	snprintf(expected, sizeof(expected), "\"start\": 256, \"size\": %llu", 8588611584ULL / 4096);
	CHECK_JSON(json, expected);

	// Clearing the tables
	{
		int fd = open(tmp_path("gpt.img"), O_RDWR);
		const char* argv[] = { "sfdisk", "--json", tmp_path("gpt.img"), NULL };
		uint8_t hdr[8];
		CHECK(ClearMBRGPT(fd, DISK_SIZE, 512));
		CHECK(pread(fd, hdr, sizeof(hdr), (off_t)(DISK_SIZE - 512)) == sizeof(hdr));
		CHECK_EQ_MEM(hdr, "\0\0\0\0\0\0\0\0", 8);
		close(fd);
		CHECK(RunCommand(argv, NULL, FALSE) != 0);
	}

	// The CRC32 of GPTs
	CHECK_EQ_INT(GptCrc32((const uint8_t*)"123456789", 9), 0xcbf43926);
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };
	const char* tools[] = { "sfdisk" };

	ui_set_iface(&iface);
	test_layouts();
	if (have_tools(tools, ARRAYSIZE(tools))) {
		if (!test_files_init())
			return 1;
		test_tables();
		test_files_cleanup();
	} else if (test_failures != 0) {
		fprintf(stderr, "Log:\n%s\n", log_buf);
	}
	return TEST_RESULT();
}

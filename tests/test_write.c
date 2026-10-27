/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the disk image writing and the boot records
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
 * WriteDrive() and WriteMBR()/WriteSBR()/WritePBR() on image files that
 * stand for the drive; the boot records are identified with ms-sys.
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
#include "boot_records.h"
#include "write_drive.h"
#include "filesystems.h"
#include "bled/bled.h"
#include "file.h"
#include "br.h"
#include "fat16.h"
#include "fat32.h"
#include "test.h"
#include "test_files.h"

#define DRIVE_SIZE                  (16 * MB)

static uint8_t* source = NULL;
static uint32_t source_size = 3 * MB + 100;

// A drive full of 0x55, which no write produces
static int drive(const char* name, uint64_t size)
{
	uint8_t* buf = malloc(1 * MB);
	uint64_t i;
	int fd = open(tmp_path(name), O_RDWR | O_CREAT | O_TRUNC, 0644);

	CHECK((fd >= 0) && (buf != NULL));
	memset(buf, 0x55, 1 * MB);
	for (i = 0; i < size; i += 1 * MB)
		CHECK(pwrite(fd, buf, 1 * MB, (off_t)i) == 1 * MB);
	free(buf);
	return fd;
}

static BOOL drive_is(int fd, uint64_t offset, const uint8_t* data, uint32_t len)
{
	uint8_t* buf = malloc(len);
	BOOL r = (buf != NULL) && (pread(fd, buf, len, (off_t)offset) == (ssize_t)len) && (memcmp(buf, data, len) == 0);

	free(buf);
	return r;
}

static BOOL drive_filled(int fd, uint64_t offset, uint8_t val, uint32_t len)
{
	uint8_t* buf = malloc(len);
	uint32_t i;
	BOOL r = (buf != NULL) && (pread(fd, buf, len, (off_t)offset) == (ssize_t)len);

	for (i = 0; r && (i < len); i++)
		r = (buf[i] == val);
	free(buf);
	return r;
}

static int image(const char* name, int compression)
{
	int fd = open(tmp_path(name), O_RDONLY);

	CHECK(fd >= 0);
	memset(&img_report, 0, sizeof(img_report));
	img_report.compression_type = (uint8_t)compression;
	img_report.image_size = (uint64_t)lseek(fd, 0, SEEK_END);
	ResetOptions(&opts);
	ErrorStatus = 0;
	return fd;
}

static void test_write_drive(void)
{
	const char* gzip[] = { "gzip", "-k", "-f", tmp_path("source.img"), NULL };
	const char* xz[] = { "xz", "-k", "-f", tmp_path("source.img"), NULL };
	const char* vhd_fixed[] = { "qemu-img", "convert", "-q", "-f", "raw", "-O", "vpc", "-o", "subformat=fixed",
		tmp_path("source.img"), tmp_path("fixed.vhd"), NULL };
	const char* vhd_dynamic[] = { "qemu-img", "convert", "-q", "-f", "raw", "-O", "vpc", "-o", "subformat=dynamic",
		tmp_path("source.img"), tmp_path("dynamic.vhd"), NULL };
	const char* vhdx[] = { "qemu-img", "convert", "-q", "-f", "raw", "-O", "vhdx", "-o", "subformat=dynamic,block_size=1M",
		tmp_path("source.img"), tmp_path("dynamic.vhdx"), NULL };
	uint8_t zero[512] = { 0 };
	uint32_t i;
	int fd, src;

	source = malloc(source_size);
	CHECK(source != NULL);
	for (i = 0; i < source_size; i++)
		source[i] = (uint8_t)(i * 7 + (i >> 9));
	CHECK(write_file(tmp_path("source.img"), source, source_size) == source_size);

	// A raw image, which doesn't end on a sector: the end of its last sector is zeroed
	fd = drive("raw.drive", DRIVE_SIZE);
	src = image("source.img", BLED_COMPRESSION_NONE);
	CHECK(WriteDrive(fd, DRIVE_SIZE, 512, src, FALSE));
	CHECK(drive_is(fd, 0, source, source_size));
	CHECK(drive_filled(fd, source_size, 0, 512 - 100));
	CHECK(drive_filled(fd, source_size + 412, 0x55, 512));
	close(src);
	close(fd);

	// Compressed images
	CHECK(run(gzip));
	fd = drive("gz.drive", DRIVE_SIZE);
	src = image("source.img.gz", BLED_COMPRESSION_GZIP);
	CHECK(WriteDrive(fd, DRIVE_SIZE, 512, src, FALSE));
	CHECK(drive_is(fd, 0, source, source_size));
	CHECK(drive_filled(fd, source_size, 0, 512 - 100));
	CHECK_CONTAINS(log_buf, "Notice: Compressed image data didn't end on block boundary.");
	close(src);
	close(fd);
	if (have("xz")) {
		CHECK(run(xz));
		fd = drive("xz.drive", DRIVE_SIZE);
		src = image("source.img.xz", BLED_COMPRESSION_XZ);
		CHECK(WriteDrive(fd, DRIVE_SIZE, 512, src, FALSE));
		CHECK(drive_is(fd, 0, source, source_size));
		close(src);
		close(fd);
	}

	// VHD and VHDX images: the disk they hold (see test_vdisk.c for the formats)
	if (have("qemu-img")) {
		CHECK(run(vhd_fixed));
		CHECK(run(vhd_dynamic));
		CHECK(run(vhdx));
		fd = drive("vhd.drive", DRIVE_SIZE);
		src = image("fixed.vhd", BLED_COMPRESSION_MAX + 1);
		CHECK(WriteDrive(fd, DRIVE_SIZE, 512, src, FALSE));
		CHECK(drive_is(fd, 0, source, source_size));
		close(src);
		close(fd);
		fd = drive("dynamic-vhd.drive", DRIVE_SIZE);
		src = image("dynamic.vhd", BLED_COMPRESSION_MAX + 1);
		CHECK(WriteDrive(fd, DRIVE_SIZE, 512, src, FALSE));
		CHECK(drive_is(fd, 0, source, source_size));
		close(src);
		close(fd);
		fd = drive("vhdx.drive", DRIVE_SIZE);
		src = image("dynamic.vhdx", BLED_COMPRESSION_MAX + 2);
		CHECK(WriteDrive(fd, DRIVE_SIZE, 512, src, FALSE));
		CHECK(drive_is(fd, 0, source, source_size));
		close(src);
		// Not a VHDX
		src = image("source.img", BLED_COMPRESSION_MAX + 2);
		CHECK(!WriteDrive(fd, DRIVE_SIZE, 512, src, FALSE));
		CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_NOT_SUPPORTED));
		close(src);
		close(fd);
	}

	// Larger than the drive: the drive is filled
	fd = drive("small.drive", 1 * MB);
	src = image("source.img", BLED_COMPRESSION_NONE);
	CHECK(WriteDrive(fd, 1 * MB, 512, src, FALSE));
	CHECK(drive_is(fd, 0, source, 1 * MB));
	CHECK_EQ_INT(lseek(fd, 0, SEEK_END), 1 * MB);
	close(src);
	close(fd);

	// Zeroing
	fd = drive("zero.drive", DRIVE_SIZE);
	ResetOptions(&opts);
	CHECK(WriteDrive(fd, DRIVE_SIZE, 512, -1, TRUE));
	CHECK(drive_filled(fd, 0, 0, DRIVE_SIZE));
	close(fd);

	// Fast zeroing skips the empty blocks (of DD_BUFFER_SIZE), and writes 0xff elsewhere
	fd = drive("fast.drive", 2 * DD_BUFFER_SIZE);
	{
		uint8_t* buf = calloc(1, DD_BUFFER_SIZE);
		CHECK((buf != NULL) && (pwrite(fd, buf, DD_BUFFER_SIZE, 0) == DD_BUFFER_SIZE));
		free(buf);
	}
	opts.fast_zeroing = TRUE;
	CHECK(WriteDrive(fd, 2 * DD_BUFFER_SIZE, 512, -1, TRUE));
	CHECK(drive_filled(fd, 0, 0x00, DD_BUFFER_SIZE));
	CHECK(drive_filled(fd, DD_BUFFER_SIZE, 0xff, DD_BUFFER_SIZE));
	opts.fast_zeroing = FALSE;
	close(fd);

	// Cancelled
	fd = drive("cancel.drive", DRIVE_SIZE);
	CancelOperation();
	CHECK(!WriteDrive(fd, DRIVE_SIZE, 512, -1, TRUE));
	CHECK(drive_filled(fd, 0, 0x55, 512));
	ErrorStatus = 0;
	close(fd);
	(void)zero;
	free(source);
}

// A drive with a FAT32 partition, and the FAKE_FD that ms-sys identifies the boot records with
static int partitioned_drive(int partition_style, partition_layout_t* layout)
{
	int fd = drive("br.drive", 64 * MB);

	CHECK(PlanPartitions(layout, partition_style, FS_FAT32, FALSE, 0, 64 * MB, 512));
	CHECK(WritePartitionTable(fd, layout));
	return fd;
}

static void test_mbr(void)
{
	partition_layout_t layout;
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	uint8_t mbr[512], *core_img, *buf;
	uint32_t size;
	int fd;

	set_bytes_per_sector(512);

	// Non bootable: a zeroed MBR, with the partition table
	ResetOptions(&opts);
	memset(&img_report, 0, sizeof(img_report));
	opts.boot_type = BT_NON_BOOTABLE;
	opts.fs_type = FS_FAT32;
	fd = partitioned_drive(PARTITION_STYLE_MBR, &layout);
	fake_fd._fd = fd;
	CHECK(WriteMBR(fd, 512));
	CHECK(is_zero_mbr_not_including_disk_signature_or_copy_protect(fp));
	CHECK((pread(fd, mbr, sizeof(mbr), 0) == sizeof(mbr)) && (mbr[0x1be] == 0x00) && (mbr[0x1c2] == 0x0c));
	CHECK((mbr[510] == 0x55) && (mbr[511] == 0xaa));

	// Syslinux, with the boot flag for BIOS
	opts.boot_type = BT_IMAGE;
	opts.target_type = TT_BIOS;
	img_report.sl_version = 0x0604;
	CHECK(WriteMBR(fd, 512));
	CHECK(is_syslinux_mbr(fp));
	CHECK((pread(fd, mbr, sizeof(mbr), 0) == sizeof(mbr)) && (mbr[0x1be] == 0x80));
	CHECK(WriteSBR(fd, 512, layout.part[0].offset));

	// GRUB 2, with its core.img right after the MBR
	img_report.sl_version = 0;
	img_report.has_grub2 = 1;
	CHECK(WriteMBR(fd, 512));
	CHECK(is_grub2_mbr(fp));
	CHECK(WriteSBR(fd, 512, layout.part[0].offset));
	core_img = GetResource(IDR_GR_GRUB2_CORE_IMG, "core.img", &size, FALSE);
	CHECK((core_img != NULL) && drive_is(fd, 512, core_img, size));
	// Which must fit before the first partition
	CHECK(!WriteSBR(fd, 512, 16 * KB));

	// Windows, with the Rufus MBR unless it is disabled (Alt-A)
	memset(&img_report, 0, sizeof(img_report));
	img_report.has_bootmgr = TRUE;
	CHECK(WriteMBR(fd, 512));
	CHECK(is_rufus_mbr(fp));
	opts.use_rufus_mbr = FALSE;
	CHECK(WriteMBR(fd, 512));
	CHECK(is_win7_mbr(fp));
	close(fd);

	// GPT: the protective MBR message, and the message SBR
	memset(&img_report, 0, sizeof(img_report));
	opts.partition_type = PARTITION_STYLE_GPT;
	img_report.has_grub2 = 1;
	fd = partitioned_drive(PARTITION_STYLE_GPT, &layout);
	fake_fd._fd = fd;
	CHECK(WriteMBR(fd, 512));
	CHECK(is_rufus_msg_mbr(fp));
	CHECK(WriteSBR(fd, 512, layout.part[0].offset));
	buf = GetResource(IDR_SBR_MSG, "msg.txt", &size, FALSE);
	CHECK((buf != NULL) && drive_is(fd, 17 * KB, buf, size));
	close(fd);
}

static void test_pbr(void)
{
	const char* mkfs32[] = { "mkfs.fat", "-F32", "-I", "--mbr=n", tmp_path("fat32.vol"), NULL };
	const char* mkfs16[] = { "mkfs.fat", "-F16", "-I", "--mbr=n", tmp_path("fat16.vol"), NULL };
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	int fd;

	set_bytes_per_sector(512);
	ResetOptions(&opts);
	memset(&img_report, 0, sizeof(img_report));

	// FAT32, at an offset of the drive (both the primary and the backup boot sectors)
	fd = drive("fat32.vol", 128 * MB);
	CHECK(run(mkfs32));
	opts.boot_type = BT_IMAGE;
	img_report.has_bootmgr = TRUE;
	CHECK(WritePBR(fd, 0, 512, FS_FAT32));
	fake_fd._fd = fd;
	CHECK(entire_fat_32_pe_br_matches(fp));
	fake_fd._offset = 6 * 512;
	CHECK(entire_fat_32_pe_br_matches(fp));
	fake_fd._offset = 0;
	opts.boot_type = BT_FREEDOS;
	CHECK(WritePBR(fd, 0, 512, FS_FAT32));
	CHECK(entire_fat_32_fd_br_matches(fp));
	opts.boot_type = BT_IMAGE;
	memset(&img_report, 0, sizeof(img_report));
	CHECK(WritePBR(fd, 0, 512, FS_FAT32));
	CHECK(entire_fat_32_br_matches(fp));
	// Not FAT16
	CHECK(!WritePBR(fd, 0, 512, FS_FAT16));
	close(fd);

	// FAT16
	fd = drive("fat16.vol", 32 * MB);
	CHECK(run(mkfs16));
	fake_fd._fd = fd;
	CHECK(WritePBR(fd, 0, 512, FS_FAT16));
	CHECK(entire_fat_16_br_matches(fp));
	// ext has none
	CHECK(WritePBR(fd, 0, 512, FS_EXT4));
	close(fd);
	ErrorStatus = 0;
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };
	const char* tools[] = { "gzip", "mkfs.fat" };

	ui_set_iface(&iface);
	if (!have_tools(tools, ARRAYSIZE(tools)))
		return 77;
	if (!test_files_init())
		return 1;
	test_write_drive();
	test_mbr();
	test_pbr();
	test_files_cleanup();
	return TEST_RESULT();
}

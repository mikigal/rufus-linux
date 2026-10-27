/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the image analysis and ISO extraction
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
 * The test images are built from scratch with xorriso, mtools, mkfs.fat,
 * qemu-img and wimlib, so no download is needed. The test is skipped (exit
 * code 77) if one of these tools is missing.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "rufus.h"
#include "resource.h"
#include "options.h"
#include "ui_iface.h"
#include "vhd.h"
#include "wimlib.h"
#include "file.h"
#include "br.h"
#include "bled/bled.h"
#include "test.h"
#include "test_files.h"

extern StrArray modified_files;
extern uint64_t total_blocks;

// A fake isolinux.bin, with the version string that GetSyslinuxVersion() looks for
static void make_isolinux_bin(const char* name, const char* version)
{
	char buf[1024];

	memset(buf, 0x90, sizeof(buf));
	snprintf(&buf[512], 128, "ISOLINUX %s 2019-02-04  Copyright (C) 1994-2015 H. Peter Anvin et al", version);
	buf[sizeof(buf) - 1] = 0;
	make_file(name, buf);
}

static void reset_scan(void)
{
	memset(&img_report, 0, sizeof(img_report));
	total_blocks = 0;
	ErrorStatus = 0;
}

/*
 * A Linux live ISO: Syslinux, GRUB (BIOS and UEFI), casper persistence, md5sum.txt,
 * Rock Ridge long names and symbolic links.
 */
static void test_linux_iso(void)
{
	const char* xorriso[] = { "xorriso", "-as", "mkisofs", "-quiet", "-o", tmp_path("linux.iso"), "-V", "TEST LINUX 24.04",
		"-R", "-J", "-joliet-long", tmp_path("linux"), NULL };
	char dest[MAX_PATH], path[MAX_PATH], long_name[128];
	uint8_t* buf = NULL;
	uint32_t size;

	make_isolinux_bin("linux/isolinux/isolinux.bin", "6.04");
	make_file("linux/isolinux/isolinux.cfg", "default live\nlabel live\n  kernel /casper/vmlinuz\n"
		"  append  file=/cdrom/preseed/ubuntu.seed boot=casper initrd=/casper/initrd quiet splash ---\n");
	make_file("linux/isolinux/ldlinux.c32", "ldlinux.c32");
	make_file("linux/boot/grub/grub.cfg", "search --set=root --label TEST\\x20LINUX\\x2024.04\n"
		"menuentry \"Try\" {\n\tlinux\t/casper/vmlinuz  boot=casper quiet splash ---\n\tinitrd\t/casper/initrd\n}\n");
	{
		// As in the real module: the format string, followed by the version string
		char mod[256];
		memset(mod, '.', sizeof(mod));
		memcpy(mod, "\x7f" "ELF", 4);
		memcpy(&mod[32], "GRUB  version %s\0" "2.12", 22);
		CHECK(mkdir_p(tmp_path("linux/boot/grub/i386-pc"), 0755) == 0);
		CHECK(write_file(tmp_path("linux/boot/grub/i386-pc/normal.mod"), (uint8_t*)mod, sizeof(mod)) == sizeof(mod));
	}
	make_file("linux/EFI/BOOT/BOOTX64.EFI", "MZ - not a real PE, but large enough to not be a broken symlink to a "
		"file that does not exist on the media, which is what Linux Mint did at some point. "
		"This is just padding to get past the 256 bytes threshold that the scan uses for that. "
		"..............................................................................");
	make_file("linux/casper/vmlinuz", "kernel");
	make_file("linux/casper/initrd", "initrd");
	make_file("linux/md5sum.txt", "00000000000000000000000000000000  ./boot/grub/grub.cfg\n"
		"11111111111111111111111111111111  ./isolinux/isolinux.cfg\n"
		"22222222222222222222222222222222  ./casper/vmlinuz\n");
	memset(long_name, 'n', 80);
	strcpy(&long_name[80], ".txt");
	snprintf(path, sizeof(path), "linux/docs/%s", long_name);
	make_file(path, "Rock Ridge allows names longer than 64 characters");
	// As Mint LMDE does, which needs working symbolic links
	IGNORE_RETVAL(symlink("casper", tmp_path("linux/live")));
	CHECK(run(xorriso));

	/* Scan */
	reset_scan();
	image_path = tmp_path("linux.iso");
	CHECK(ExtractISO(image_path, "", TRUE));
	CHECK_EQ_STR(img_report.label, "TEST LINUX 24.04");
	CHECK_EQ_INT(img_report.sl_version, 0x0604);
	CHECK_EQ_STR(img_report.sl_version_str, "6.04");
	CHECK_EQ_STR(img_report.cfg_path, "/isolinux/isolinux.cfg");
	CHECK_EQ_INT(img_report.has_grub2, 1);
	CHECK_EQ_STR(img_report.grub2_version, "2.12");
	CHECK_EQ_INT(img_report.has_efi, 2 << ARCH_X86_64);
	CHECK_EQ_STR(img_report.efi_boot_entry[0].path, "/EFI/BOOT/BOOTX64.EFI");
	CHECK(img_report.uses_casper);
	CHECK_EQ_INT(img_report.has_md5sum, 1);
	CHECK(img_report.has_long_filename);
	CHECK_EQ_INT(img_report.has_symlinks, SYMLINKS_RR);
	CHECK(img_report.needs_ntfs);
	CHECK(!HAS_WINDOWS(img_report));
	CHECK(HAS_PERSISTENCE(img_report));
	CHECK(IS_BIOS_BOOTABLE(img_report) && IS_EFI_BOOTABLE(img_report));
	CHECK(img_report.projected_size > 0);
	CHECK(img_report.mismatch_size <= 0);	// xorriso pads the image

	/* Files in the image */
	size = ReadISOFileToBuffer(image_path, "/casper/vmlinuz", &buf);
	CHECK_EQ_INT(size, 6);
	if (buf != NULL)
		CHECK(memcmp(buf, "kernel", 6) == 0);
	safe_free(buf);
	CHECK_EQ_INT(ReadISOFileToBuffer(image_path, "/nonexistent", &buf), 0);
	CHECK_EQ_INT(ExtractISOFile(image_path, "/casper/initrd", tmp_path("initrd"), FILE_ATTRIBUTE_HIDDEN), 6);
	CHECK_EQ_STR(file_content(tmp_path("initrd")), "initrd");

	/* Extraction, with persistence and a label that differs from the ISO's */
	ResetOptions(&opts);
	opts.boot_type = BT_IMAGE;
	opts.fs_type = FS_FAT32;
	opts.persistence_size = 1 * GB;
	static_strcpy(img_report.usb_label, "MY USB");
	static_strcpy(dest, tmp_path("dest"));
	CHECK(mkdir(dest, 0755) == 0);
	StrArrayCreate(&modified_files, 8);
	log_buf[0] = 0;
	CHECK(ExtractISO(image_path, dest, FALSE));
	CHECK_EQ_INT(ErrorStatus, 0);
	snprintf(path, sizeof(path), "%s/docs/%s", dest, long_name);
	CHECK_EQ_STR(file_content(path), "Rock Ridge allows names longer than 64 characters");
	CHECK_EQ_STR(file_content(tmp_path("dest/EFI/BOOT/BOOTX64.EFI")), file_content(tmp_path("linux/EFI/BOOT/BOOTX64.EFI")));
	// No symbolic links on FAT: the link becomes a text file with its target
	CHECK_EQ_STR(file_content(tmp_path("dest/live")), "casper");
	// Config files patched for persistence and the label (with dos2unix, so LF line endings)
	CHECK_CONTAINS(file_content(tmp_path("dest/isolinux/isolinux.cfg")),
		"  append  persistent file=/cdrom/preseed/ubuntu.seed boot=casper initrd=/casper/initrd quiet splash ---\n");
	CHECK(strchr(file_content(tmp_path("dest/isolinux/isolinux.cfg")), '\r') == NULL);
	CHECK_CONTAINS(file_content(tmp_path("dest/boot/grub/grub.cfg")),
		"\tlinux\t/casper/vmlinuz  boot=casper persistent quiet splash ---\n");
	CHECK_CONTAINS(file_content(tmp_path("dest/boot/grub/grub.cfg")), "--label MY\\x20USB\n");
	CHECK_EQ_INT(modified_files.Index, 2);
	// A syslinux.cfg that points to the isolinux one
	CHECK_EQ_STR(file_content(tmp_path("dest/syslinux.cfg")),
		"DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG /isolinux/isolinux.cfg\n  APPEND /isolinux/\n");
	// And the md5sum.txt entries of the modified files
	UpdateMD5Sum(dest, "md5sum.txt");
	CHECK(strstr(file_content(tmp_path("dest/md5sum.txt")), "00000000000000000000000000000000") == NULL);
	CHECK(strstr(file_content(tmp_path("dest/md5sum.txt")), "11111111111111111111111111111111") == NULL);
	CHECK(strstr(file_content(tmp_path("dest/md5sum.txt")), "22222222222222222222222222222222  ./casper/vmlinuz") != NULL);
	StrArrayDestroy(&modified_files);

	/* On NTFS, symbolic links are replicated */
	opts.fs_type = FS_NTFS;
	opts.persistence_size = 0;
	opts.preserve_timestamps = TRUE;
	static_strcpy(dest, tmp_path("dest_ntfs"));
	CHECK(mkdir(dest, 0755) == 0);
	CHECK(ExtractISO(image_path, dest, FALSE));
	snprintf(path, sizeof(path), "%s/live", dest);
	memset(long_name, 0, sizeof(long_name));
	CHECK(readlink(path, long_name, sizeof(long_name)) > 0);
	CHECK_EQ_STR(long_name, "casper");
	snprintf(path, sizeof(path), "%s/live/vmlinuz", dest);
	CHECK_EQ_STR(file_content(path), "kernel");
	image_path = NULL;
}

/*
 * A Windows-like ISO: bootmgr, an install.wim (that wimlib creates) and EFI boot files that
 * are only in an efi.img FAT image.
 */
static void test_windows_iso(void)
{
	const char* mkfs[] = { "mkfs.fat", "-C", "-n", "EFISYS", tmp_path("windows/efi.img"), "1440", NULL };
	const char* mmd[] = { "mmd", "-i", tmp_path("windows/efi.img"), "::/EFI", "::/EFI/BOOT", NULL };
	const char* mcopy[] = { "mcopy", "-i", tmp_path("windows/efi.img"), tmp_path("bootx64.efi"), "::/EFI/BOOT/BOOTX64.EFI", NULL };
	// NB: xorriso does not create UDF file systems, so this is ISO-9660 + Joliet
	const char* xorriso[] = { "xorriso", "-as", "mkisofs", "-quiet", "-o", tmp_path("windows.iso"), "-V", "CCCOMA_X64FRE_EN-US_DV9",
		"-J", "-joliet-long", tmp_path("windows"), NULL };
	char dest[MAX_PATH], wim_path[2 * MAX_PATH];
	WIMStruct* wim = NULL;

	make_file("windows/bootmgr", "bootmgr");
	make_file("windows/bootmgr.efi", "bootmgr.efi");
	make_file("wimsrc/Windows/Boot/EFI/bootmgr.efi", "the bootmgr.efi of the image");
	make_file("bootx64.efi", "EFI bootloader from efi.img");
	CHECK(mkdir_p(tmp_path("windows/sources"), 0755) == 0);
	CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) == 0);
	CHECK(wimlib_add_image(wim, tmp_path("wimsrc"), "Windows 11 Pro", NULL, 0) == 0);
	CHECK(wimlib_write(wim, tmp_path("windows/sources/install.wim"), WIMLIB_ALL_IMAGES, 0, 1) == 0);
	wimlib_free(wim);
	CHECK(run(mkfs));
	CHECK(run(mmd));
	CHECK(run(mcopy));
	CHECK(run(xorriso));

	reset_scan();
	image_path = tmp_path("windows.iso");
	CHECK(ExtractISO(image_path, "", TRUE));
	CHECK_EQ_STR(img_report.label, "CCCOMA_X64FRE_EN-US_DV9");
	CHECK(img_report.has_bootmgr);
	CHECK(img_report.has_bootmgr_efi);
	CHECK(HAS_WINDOWS(img_report));
	CHECK_EQ_INT(img_report.wininst_index, 1);
	CHECK_EQ_STR(img_report.wininst_path[0], "/sources/install.wim");
	CHECK_EQ_INT(img_report.wininst_version, 0x10d00);	// WIM version, read from inside the ISO
	CHECK_EQ_STR(img_report.efi_img_path, "/efi.img");
	CHECK(HAS_WINTOGO(img_report));
	// bootmgr.efi only sets bit 0: the EFI loaders are in efi.img
	CHECK_EQ_INT(img_report.has_efi, 1);

	// A file from the WIM inside the ISO
	static_sprintf(wim_path, "%s|%s", image_path, img_report.wininst_path[0]);
	CHECK(WimExtractFile(wim_path, 1, "Windows\\Boot\\EFI\\bootmgr.efi", tmp_path("bootmgr_from_wim.efi")));
	CHECK_EQ_STR(file_content(tmp_path("bootmgr_from_wim.efi")), "the bootmgr.efi of the image");
	CHECK(WimExtractFile(wim_path, 1, "Windows/Boot/EFI/bootmgr.efi", tmp_path("bootmgr_from_wim2.efi")));

	// An image with EFI boot files in efi.img only: they get extracted from it
	img_report.has_efi = 0x8000;
	ResetOptions(&opts);
	opts.boot_type = BT_IMAGE;
	opts.fs_type = FS_NTFS;
	static_strcpy(dest, tmp_path("dest_windows"));
	CHECK(mkdir(dest, 0755) == 0);
	CHECK(ExtractISO(image_path, dest, FALSE));
	CHECK_EQ_STR(file_content(tmp_path("dest_windows/bootmgr")), "bootmgr");
	CHECK_EQ_STR(file_content(tmp_path("dest_windows/EFI/BOOT/BOOTX64.EFI")), "EFI bootloader from efi.img");
	image_path = NULL;
}

/*
 * Disk images (DD), compressed or VHD
 */
static void test_disk_images(void)
{
	const char* gzip[] = { "gzip", "-k", "-9", tmp_path("disk.img"), NULL };
	const char* vhd[] = { "qemu-img", "convert", "-q", "-f", "raw", "-O", "vpc", "-o", "subformat=dynamic",
		tmp_path("disk.img"), tmp_path("disk.vhd"), NULL };
	const char* vhd_fixed[] = { "qemu-img", "convert", "-q", "-f", "raw", "-O", "vpc", "-o", "subformat=fixed",
		tmp_path("disk.img"), tmp_path("fixed.vhd"), NULL };
	FAKE_FD fake_fd = { 0 };
	uint8_t* disk;
	int fd;

	// A 4 MB disk with the Rufus MBR
	disk = calloc(1, 4 * MB);
	CHECK(disk != NULL);
	if (disk == NULL)
		return;
	write_file(tmp_path("disk.img"), disk, 4 * MB);
	fd = open(tmp_path("disk.img"), O_RDWR);
	fake_fd._fd = fd;
	set_bytes_per_sector(512);
	CHECK(write_rufus_mbr((FILE*)&fake_fd));
	close(fd);
	write_file(tmp_path("blank.img"), disk, 1 * MB);
	free(disk);

	reset_scan();
	CHECK_EQ_INT(IsBootableImage(tmp_path("disk.img")), 1);
	CHECK_EQ_INT(img_report.compression_type, BLED_COMPRESSION_NONE);
	CHECK_EQ_INT(img_report.image_size, 4 * MB);
	CHECK(!img_report.is_windows_img);
	CHECK(strstr(log_buf, "has a Rufus Master Boot Record") != NULL);

	reset_scan();
	CHECK_EQ_INT(IsBootableImage(tmp_path("blank.img")), 0);
	CHECK_EQ_INT(IsBootableImage(tmp_path("nonexistent.img")), -1);

	CHECK(run(gzip));
	reset_scan();
	CHECK_EQ_INT(IsBootableImage(tmp_path("disk.img.gz")), 1);
	CHECK_EQ_INT(img_report.compression_type, BLED_COMPRESSION_GZIP);
	CHECK_EQ_INT(img_report.projected_size, 4 * MB);

	if (have("qemu-img")) {
		CHECK(run(vhd));
		CHECK(run(vhd_fixed));
		reset_scan();
		CHECK_EQ_INT(IsBootableImage(tmp_path("disk.vhd")), 1);
		CHECK(img_report.is_vhd);
		CHECK(img_report.projected_size >= 4 * MB);
		reset_scan();
		CHECK_EQ_INT(IsBootableImage(tmp_path("fixed.vhd")), 1);
		CHECK(img_report.is_vhd);
	}

	// A WIM
	{
		WIMStruct* wim = NULL;
		CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) == 0);
		CHECK(wimlib_add_image(wim, tmp_path("wimsrc"), "Windows 11 Pro", NULL, 0) == 0);
		CHECK(wimlib_write(wim, tmp_path("install.wim"), WIMLIB_ALL_IMAGES, 0, 1) == 0);
		wimlib_free(wim);
		reset_scan();
		CHECK_EQ_INT(IsBootableImage(tmp_path("install.wim")), 0);
		CHECK(img_report.is_windows_img);
		CHECK_EQ_INT(GetWimVersion(tmp_path("install.wim")), 0x10d00);
	}
}

static void test_syslinux_versions(void)
{
	char buf[1024], *ext;

	SetEmbeddedSyslinuxVersions();
	CHECK_EQ_INT(embedded_sl_version[0], 0x0407);
	CHECK_EQ_INT(embedded_sl_version[1], 0x0604);
	CHECK_EQ_STR(embedded_sl_version_str[1], "6.04");

	memset(buf, ' ', sizeof(buf));
	snprintf(&buf[100], 200, "SYSLINUX 6.04 6.04-pre1* 2019-02-04");
	buf[sizeof(buf) - 1] = 0;
	CHECK_EQ_INT(GetSyslinuxVersion(buf, sizeof(buf), &ext), 0x0604);
	CHECK(ext[0] == '/');
	CHECK_EQ_INT(GetSyslinuxVersion(buf, 100, &ext), 0);
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };
	const char* tools[] = { "xorriso", "mkfs.fat", "mmd", "mcopy", "gzip" };

	ui_set_iface(&iface);
	if (!have_tools(tools, ARRAYSIZE(tools)))
		return 77;
	if (!test_files_init())
		return 1;
	ResetOptions(&opts);

	test_syslinux_versions();
	test_linux_iso();
	test_windows_iso();
	test_disk_images();

	test_files_cleanup();
	return TEST_RESULT();
}

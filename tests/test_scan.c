/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the image scan
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
 * ImageScan() on ISOs (xorriso), Windows images (wimlib) and disk images,
 * with the notifications it displays answered by test_ui.h.
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "resource.h"
#include "ui_iface.h"
#include "image_scan.h"
#include "wimlib.h"
#include "test.h"
#include "test_files.h"
#include "test_ui.h"

extern uint64_t total_blocks;

static char status_text[256], info_text[256];

static void test_status(BOOL info, unsigned int duration, const char* text)
{
	if (info)
		static_strcpy(info_text, text != NULL ? text : "");
	else
		static_strcpy(status_text, text != NULL ? text : "");
}

static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cond = PTHREAD_COND_INITIALIZER;
static int scan_result = -1;

static void test_event(enum ui_event_type type, intptr_t param)
{
	if (type != UM_IMAGE_SCAN_COMPLETED)
		return;
	pthread_mutex_lock(&event_mutex);
	scan_result = (int)param;
	pthread_cond_signal(&event_cond);
	pthread_mutex_unlock(&event_mutex);
}

static void make_iso(const char* iso, const char* label, const char* dir)
{
	const char* xorriso[] = { "xorriso", "-as", "mkisofs", "-quiet", "-o", tmp_path(iso), "-V", label,
		"-R", "-J", tmp_path(dir), NULL };

	CHECK(run(xorriso));
}

// Add a boot marker to an image, as an ISOHybrid or a bootable disk image has
static void set_boot_marker(const char* path)
{
	const uint8_t marker[2] = { 0x55, 0xaa };
	int fd = open(path, O_WRONLY);

	CHECK(fd >= 0);
	CHECK(pwrite(fd, marker, sizeof(marker), 510) == sizeof(marker));
	close(fd);
}

// A PE with nothing but the headers that FindArch() reads
static void make_pe(const char* name, uint16_t machine)
{
	uint8_t pe[0x60] = { 'M', 'Z' };

	pe[0x3c] = 0x40;		// e_lfanew
	memcpy(&pe[0x40], "PE\0\0", 4);
	pe[0x44] = machine & 0xff;
	pe[0x45] = machine >> 8;
	make_file(name, "");
	CHECK(write_file(tmp_path(name), pe, sizeof(pe)) == sizeof(pe));
}

static enum rules_scan_result scan(const char* name)
{
	free(image_path);
	image_path = strdup(tmp_path(name));
	ErrorStatus = 0;
	log_buf[0] = 0;
	return ImageScan(FALSE);
}

static void make_linux_tree(void)
{
	char buf[1024];

	memset(buf, 0x90, sizeof(buf));
	snprintf(&buf[512], 128, "ISOLINUX 6.04 2019-02-04  Copyright (C) 1994-2015 H. Peter Anvin et al");
	buf[sizeof(buf) - 1] = 0;
	make_file("linux/isolinux/isolinux.bin", buf);
	make_file("linux/isolinux/isolinux.cfg", "default live\nlabel live\n  kernel /vmlinuz\n");
	memset(buf, 'x', 400);
	buf[400] = 0;
	make_file("linux/EFI/BOOT/BOOTX64.EFI", buf);
	make_file("linux/vmlinuz", "kernel");
}

/* Linux ISOs, and the labels that change how they get written */
static void test_linux_isos(void)
{
	make_linux_tree();

	// A Red Hat 8 derivative
	make_iso("fedora.iso", "Fedora-WS-Live-38-1-6", "linux");
	answer(0);
	CHECK_EQ_INT(scan("fedora.iso"), RULES_SCAN_OK);
	CHECK_ALL_ANSWERED();
	CHECK(img_report.is_iso);
	CHECK(img_report.rh8_derivative);
	CHECK(!img_report.disable_iso);
	CHECK(IS_EFI_BOOTABLE(img_report) && IS_BIOS_BOOTABLE(img_report));
	CHECK_EQ_STR(short_image_path, "fedora.iso");
	CHECK_CONTAINS(log_buf, "Using image: fedora.iso (");
	CHECK_CONTAINS(log_buf, "ISO label: 'Fedora-WS-Live-38-1-6'\n");
	CHECK_CONTAINS(log_buf, "  Uses: Syslinux/Isolinux v6.04\n");
	CHECK_CONTAINS(log_buf, "UEFI bootloaders analysis:\n  • /EFI/BOOT/BOOTX64.EFI\n");
	CHECK_EQ_INT(img_report.has_secureboot_bootloader, 0);
	CHECK(strncmp(status_text, "MSG_205 ", 8) == 0);
	CHECK(strncmp(info_text, "MSG_210 ", 8) == 0);

	// SUSE: ISO mode is disabled...
	make_iso("suse.iso", "openSUSE-Tumbleweed-DVD-x86_64", "linux");
	answer(0);
	CHECK_EQ_INT(scan("suse.iso"), RULES_SCAN_OK);
	CHECK_ALL_ANSWERED();
	CHECK(!img_report.rh8_derivative);
	CHECK(img_report.disable_iso);
	CHECK(img_report.is_iso);

	// ...which, for an ISOHybrid, enforces DD mode
	set_boot_marker(tmp_path("suse.iso"));
	answer(1, IDOK);
	CHECK_EQ_INT(scan("suse.iso"), RULES_SCAN_OK);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(0, MSG_274));
	CHECK(!img_report.is_iso);
	CHECK(IS_DD_BOOTABLE(img_report));
	CHECK_CONTAINS(log_buf, "Note: DD image mode enforced since this ISOHybrid is not ISO mode compatible.");

	// A Fedora ISOHybrid keeps both modes
	set_boot_marker(tmp_path("fedora.iso"));
	answer(0);
	CHECK_EQ_INT(scan("fedora.iso"), RULES_SCAN_OK);
	CHECK_ALL_ANSWERED();
	CHECK(img_report.is_iso && IS_DD_BOOTABLE(img_report));

	// A truncated ISO
	{
		uint8_t* buf = NULL;
		uint32_t size = read_file(tmp_path("fedora.iso"), &buf);
		CHECK(size > 64 * KB);
		CHECK(write_file(tmp_path("truncated.iso"), buf, size - 32 * KB) == size - 32 * KB);
		free(buf);
	}
	answer(1, IDOK);
	CHECK_EQ_INT(scan("truncated.iso"), RULES_SCAN_OK);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(0, MSG_297));
	CHECK_EQ_INT(img_report.mismatch_size, 32 * KB);
	CHECK_CONTAINS(log_buf, "  ERROR: Detected that file on disk has been truncated by 32 KB!");

	// An ISO without any boot method we support
	make_file("data/readme.txt", "Just data");
	make_iso("data.iso", "DATA", "data");
	answer(1, IDOK);
	CHECK_EQ_INT(scan("data.iso"), RULES_SCAN_UNSUPPORTED);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(0, MSG_081));
	CHECK(image_path == NULL);
	CHECK(short_image_path == NULL);
	CHECK(strncmp(status_text, "MSG_086 ", 8) == 0);
}

/* A Windows install image */
static void make_windows_wim(const char* name, uint16_t machine)
{
	WIMStruct* wim = NULL;

	if (machine != 0)
		make_pe("wimsrc/Windows/Boot/EFI/bootmgr.efi", machine);
	else
		make_file("wimsrc/Windows/Boot/EFI/bootmgr.efi", "Not a PE, but large enough to have the size of a DOS header, "
			"which is what FindArch() looks at first.");
	CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) == 0);
	CHECK(wimlib_add_image(wim, tmp_path("wimsrc"), "Windows 11 Pro", NULL, 0) == 0);
	CHECK(wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/MAJOR", "10") == 0);
	CHECK(wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/MINOR", "0") == 0);
	CHECK(wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/BUILD", "22631") == 0);
	CHECK(wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/SPBUILD", "2861") == 0);
	CHECK(wimlib_write(wim, tmp_path(name), WIMLIB_ALL_IMAGES, 0, 1) == 0);
	wimlib_free(wim);
}

static void test_windows_images(void)
{
	answer(0);
	make_windows_wim("x64.wim", IMAGE_FILE_MACHINE_AMD64);
	CHECK_EQ_INT(scan("x64.wim"), RULES_SCAN_OK);
	CHECK(img_report.is_windows_img);
	CHECK_EQ_INT(img_report.has_efi, 1 | (2 << ARCH_X86_64));
	CHECK(img_report.has_bootmgr_efi);
	CHECK_EQ_INT(img_report.wininst_index, 1);
	CHECK_EQ_INT(img_report.win_version.major, 11);
	CHECK_CONTAINS(log_buf, "  Image contains an x86_64 EFI boot manager\n");
	CHECK_CONTAINS(log_buf, "  Image is a UEFI bootable Windows 11 (Build 22631.2861) installation image\n");

	make_windows_wim("riscv.wim", IMAGE_FILE_MACHINE_RISCV64);
	CHECK_EQ_INT(scan("riscv.wim"), RULES_SCAN_OK);
	CHECK_EQ_INT(img_report.has_efi, 1 | (2 << ARCH_RISCV_64));
	CHECK_CONTAINS(log_buf, "  Image contains a RISC-V 64 EFI boot manager\n");

	// Without an EFI boot manager, there is nothing to boot
	make_windows_wim("nonpe.wim", 0);
	answer(1, IDOK);
	CHECK_EQ_INT(scan("nonpe.wim"), RULES_SCAN_UNSUPPORTED);
	CHECK_ALL_ANSWERED();
	CHECK_CONTAINS(log_buf, "FindArch: DOS header not found\n");
	CHECK_CONTAINS(log_buf, "  Image does not contain an EFI boot manager\n");
	CHECK_CONTAINS(log_buf, "  Image is a NON-UEFI bootable Windows 11 (Build 22631.2861) installation image\n");
	CHECK(image_path == NULL);
}

/* Disk images, and files that are not images */
static void test_disk_images(void)
{
	const char* gzip[] = { "gzip", "-k", "-f", tmp_path("disk.img"), NULL };
	uint8_t* zero = calloc(1, 1 * MB);

	CHECK(zero != NULL);
	CHECK(write_file(tmp_path("disk.img"), zero, 1 * MB) == 1 * MB);
	CHECK(write_file(tmp_path("blank.img"), zero, 1 * MB) == 1 * MB);
	free(zero);
	set_boot_marker(tmp_path("disk.img"));
	CHECK(run(gzip));

	answer(0);
	CHECK_EQ_INT(scan("disk.img"), RULES_SCAN_OK);
	CHECK(!img_report.is_iso);
	CHECK(IS_DD_BOOTABLE(img_report));
	CHECK_CONTAINS(log_buf, "  Image is a bootable disk image\n");
	CHECK_EQ_STR(short_image_path, "disk.img");

	CHECK_EQ_INT(scan("disk.img.gz"), RULES_SCAN_OK);
	CHECK_CONTAINS(log_buf, "  Image is a compressed bootable disk image\n");
	CHECK_CONTAINS(log_buf, "  Size: 1 MB (Projected)\n");
	CHECK_ALL_ANSWERED();

	// Not an image
	answer(1, IDOK);
	CHECK_EQ_INT(scan("blank.img"), RULES_SCAN_FAILED);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(0, MSG_081));
	CHECK_EQ_INT(asked[0].flags, MB_OK | MB_ICONINFORMATION);
	CHECK(image_path == NULL);
	CHECK(strncmp(status_text, "MSG_203 ", 8) == 0);

	// Can't be read
	answer(1, IDOK);
	CHECK_EQ_INT(scan("nonexistent.iso"), RULES_SCAN_FAILED);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(0, MSG_042));
	CHECK_EQ_INT(asked[0].flags, MB_OK | MB_ICONERROR);

	// Nothing to scan
	answer(0);
	image_path = NULL;
	CHECK_EQ_INT(ImageScan(FALSE), RULES_SCAN_FAILED);
	CHECK_ALL_ANSWERED();
}

/* The scan thread, which reports its result as an event */
static void test_thread(void)
{
	answer(0);
	free(image_path);
	image_path = strdup(tmp_path("fedora.iso"));
	short_image_path = "kept";
	scan_result = -1;
	ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);	// From an earlier operation
	CHECK(StartImageScan(TRUE));
	pthread_mutex_lock(&event_mutex);
	while (scan_result < 0)
		pthread_cond_wait(&event_cond, &event_mutex);
	pthread_mutex_unlock(&event_mutex);
	CHECK_EQ_INT(scan_result, RULES_SCAN_OK);
	CHECK_EQ_STR(short_image_path, "kept");
	CHECK(img_report.rh8_derivative);
	CHECK_ALL_ANSWERED();
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log, .ask = test_ask, .status = test_status, .event = test_event };
	const char* tools[] = { "xorriso", "gzip" };

	ui_set_iface(&iface);
	if (!have_tools(tools, ARRAYSIZE(tools)))
		return 77;
	if (!test_files_init())
		return 1;
	static_strcpy(temp_dir, tmp_dir);
	ResetOptions(&opts);

	test_linux_isos();
	test_windows_images();
	test_disk_images();
	test_thread();

	safe_free(image_path);
	test_files_cleanup();
	return TEST_RESULT();
}

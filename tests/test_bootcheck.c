/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the boot check
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
 * BootCheck() on image reports set up by the tests, with scripted answers
 * (test_ui.h). The bootloader files are either placed in app_data_dir by the
 * test, or downloaded, which fails as nothing listens where the downloads go
 * ($RUFUS_TEST_URL_PREFIX, see tests/CMakeLists.txt).
 */

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "resource.h"
#include "settings.h"
#include "ui_iface.h"
#include "boot_check.h"
#include "wue.h"
#include "wimlib.h"
#include "test.h"
#include "test_files.h"
#include "test_ui.h"
#include "../res/grub2/grub2_version.h"
#include "../res/grub/grub_version.h"

extern char embedded_sl_version_str[2][12], embedded_sl_version_ext[2][32];
static const bootcheck_drive_t drive = { "/dev/rufus-test-drive", FALSE };
static char files_dir[MAX_PATH];

static void reset(int boot_type)
{
	ResetOptions(&opts);
	opts.boot_type = boot_type;
	opts.partition_type = PARTITION_STYLE_MBR;
	opts.target_type = TT_BIOS;
	opts.fs_type = FS_FAT32;
	opts.cluster_size = 4096;
	opts.disk_size = 8 * GB;
	memset(&img_report, 0, sizeof(img_report));
	// NB: This is the static buffer of CreateUnattendXml()
	unattend_xml_path = NULL;
	size_check = TRUE;
}

// A Linux ISO with Syslinux 6.04 (as the embedded one) and EFI
static void linux_iso(void)
{
	img_report.is_iso = TRUE;
	img_report.sl_version = 0x0604;
	static_strcpy(img_report.sl_version_str, "6.04");
	static_strcpy(img_report.sl_version_ext, embedded_sl_version_ext[1]);
	img_report.has_efi = 2 << ARCH_X86_64;
	img_report.projected_size = 2 * GB;
	static_strcpy(img_report.label, "LINUX");
}

// A Windows 11 ISO
static void windows_iso(int build)
{
	img_report.is_iso = TRUE;
	img_report.has_bootmgr = TRUE;
	img_report.has_bootmgr_efi = TRUE;
	img_report.has_efi = 1 | (2 << ARCH_X86_64);
	img_report.wininst_index = 1;
	static_strcpy(img_report.wininst_path[0], "/sources/install.wim");
	img_report.win_version.major = (build > 20000) ? 11 : 10;
	img_report.win_version.build = (uint16_t)build;
	img_report.projected_size = 5 * GB;
}

static void cache_file(const char* name, const char* content)
{
	char path[MAX_PATH];

	snprintf(path, sizeof(path), "appdata/%s/%s", FILES_DIR, name);
	make_file(path, content);
}

static void uncache(void)
{
	char cmd[MAX_PATH + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", files_dir);
	IGNORE_RETVAL(system(cmd));
}

#define CHECK_BOOTCHECK(expected) do { \
	CHECK_EQ_INT(BootCheck(&drive), expected); \
	CHECK_ALL_ANSWERED(); \
} while (0)

/* What doesn't need a check */
static void test_nothing_to_check(void)
{
	reset(BT_NON_BOOTABLE);
	answer(0);
	CHECK_EQ_INT(BootCheck(NULL), BOOTCHECK_CANCEL);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	reset(BT_IMAGE);
	opts.zero_drive = TRUE;
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	reset(BT_FREEDOS);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
}

/* The checks of the image against the drive */
static void test_image(void)
{
	// Too large for the drive, unless the size check is disabled (Alt-S)
	reset(BT_IMAGE);
	img_report.is_bootable_img = 1;
	img_report.projected_size = 8 * GB + 8 * KB;
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_088));
	CHECK_EQ_INT(asked[0].flags, MB_OK | MB_ICONERROR);
	size_check = FALSE;
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(opts.write_as_image);

	// A DD image with a revoked bootloader
	reset(BT_IMAGE);
	img_report.is_bootable_img = 1;
	img_report.has_secureboot_bootloader = 1 | (1 << 2);
	answer(1, IDCANCEL);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_338));
	CHECK_EQ_INT(asked[0].flags, MB_OKCANCEL | MB_ICONWARNING);
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(opts.write_as_image);

	// ISOHybrid: ISO or DD mode...
	reset(BT_IMAGE);
	linux_iso();
	img_report.is_bootable_img = 1;
	answer(1, 0x02);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(asked_msg(0, MSG_274));
	CHECK_EQ_INT(asked[0].nb_choices, 2);
	CHECK(opts.write_as_image);
	opts.write_as_image = FALSE;
	answer(1, 0x01);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(!opts.write_as_image && !opts.write_as_esp);
	answer(1, -1);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	// ...but not with persistence
	opts.persistence_size = 1 * GB;
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	opts.persistence_size = 0;

	// ...or ISO → ESP, on GPT, for a small enough image
	opts.partition_type = PARTITION_STYLE_GPT;
	opts.target_type = TT_UEFI;
	img_report.projected_size = 512 * MB;
	answer(1, 0x02);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK_EQ_INT(asked[0].nb_choices, 3);
	CHECK(opts.write_as_esp && !opts.write_as_image);
	opts.write_as_esp = FALSE;
	answer(1, 0x04);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(opts.write_as_image && !opts.write_as_esp);

	// Which a regular ISO also offers
	reset(BT_IMAGE);
	linux_iso();
	opts.partition_type = PARTITION_STYLE_GPT;
	opts.target_type = TT_UEFI;
	img_report.projected_size = 512 * MB;
	answer(1, 0x02);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(asked_msg(0, MSG_274));
	CHECK_EQ_INT(asked[0].nb_choices, 2);
	CHECK(opts.write_as_esp);
}

/* The file systems and targets that an ISO can't use */
static void test_incompatible(void)
{
	// UEFI, without EFI bootloaders
	reset(BT_IMAGE);
	linux_iso();
	img_report.has_efi = 0;
	opts.target_type = TT_UEFI;
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_090));

	// NTFS with Syslinux 6
	reset(BT_IMAGE);
	linux_iso();
	opts.fs_type = FS_NTFS;
	opts.target_type = TT_UEFI;
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	opts.target_type = TT_BIOS;
	img_report.sl_version = 0x0503;
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_092));

	// Windows on FAT for BIOS, unless dual UEFI/BIOS is enabled (Alt-E)
	reset(BT_IMAGE);
	windows_iso(19045);
	img_report.has_panther_unattend = TRUE;
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_092));
	opts.allow_dual_uefi_bios = TRUE;
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);

	// KolibriOS on FAT16
	reset(BT_IMAGE);
	img_report.is_iso = TRUE;
	img_report.has_kolibrios = TRUE;
	opts.fs_type = FS_FAT16;
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_099));

	// A file over 4 GB on FAT, unless the WIM can be split
	reset(BT_IMAGE);
	linux_iso();
	img_report.has_4GB_file = 1;
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_099));
	img_report.has_4GB_file = 0x11;
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);

	// UEFI:NTFS needs NTFS or exFAT
	reset(BT_UEFI_NTFS);
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_092));
	opts.fs_type = FS_EXFAT;
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
}

/* The Windows User Experience dialog */
static void test_windows_user_experience(void)
{
	uint16_t default_mask = UNATTEND_DEFAULT_SELECTION_MASK;

	reset(BT_IMAGE);
	windows_iso(26200);
	opts.partition_type = PARTITION_STYLE_GPT;
	opts.target_type = TT_UEFI;
	unattend_xml_mask = default_mask;
	answer(1, 0x01 | 0x04 | 0x20);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(asked_msg(0, MSG_327));
	CHECK_EQ_INT(asked[0].style, BS_AUTOCHECKBOX);
	// Bypass, online account, user, locale, privacy, silent, BitLocker, QoL, 2023 CA and SkuSiPolicy
	CHECK_EQ_INT(asked[0].nb_choices, 10);
	CHECK_EQ_INT(asked[0].mask, 0x03);
	CHECK_EQ_INT(opts.unattend_xml_flags, UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SET_USER | UNATTEND_SILENT_INSTALL);
	CHECK(opts.append_silent);
	CHECK(unattend_xml_path != NULL);
	CHECK_CONTAINS(file_content(unattend_xml_path), "processorArchitecture=\"amd64\"");
	// The options that the dialog doesn't offer are kept
	CHECK_EQ_INT(unattend_xml_mask, UNATTEND_OFFLINE_INTERNAL_DRIVES | opts.unattend_xml_flags);
	CHECK_CONTAINS(file_content(ini_file), "WindowsUserExperienceOptions = ");

	// Expert mode adds S Mode
	expert_mode = TRUE;
	answer(1, -1);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK_EQ_INT(asked[0].nb_choices, 11);
	expert_mode = FALSE;

	// Windows 10 has fewer options
	reset(BT_IMAGE);
	windows_iso(19045);
	opts.target_type = TT_UEFI;
	answer(1, 0x04);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK_EQ_INT(asked[0].nb_choices, 3);
	CHECK_EQ_INT(opts.unattend_xml_flags, UNATTEND_NO_DATA_COLLECTION);
	CHECK(!opts.append_silent);

	// Not when the ISO has its own unattend.xml
	reset(BT_IMAGE);
	windows_iso(26200);
	opts.target_type = TT_UEFI;
	img_report.has_panther_unattend = TRUE;
	answer(0);
	log_buf[0] = 0;
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(unattend_xml_path == NULL);
	CHECK_CONTAINS(log_buf, "the 'Windows User Experience dialog' will not be displayed");
	unattend_xml_mask = default_mask;
}

/* Windows To Go, from an install.wim */
static void test_windows_to_go(void)
{
	WIMStruct* wim = NULL;
	char* saved_image_path = image_path;

	make_file("wimsrc/Windows/System32/ntoskrnl.exe", "kernel");
	CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) == 0);
	CHECK(wimlib_add_image(wim, tmp_path("wimsrc"), "Windows 11 Pro", NULL, 0) == 0);
	CHECK(wimlib_set_image_property(wim, 1, "DISPLAYNAME", "Windows 11 Pro") == 0);
	CHECK(wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/MAJOR", "10") == 0);
	CHECK(wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/BUILD", "26200") == 0);
	CHECK(wimlib_write(wim, tmp_path("install.wim"), WIMLIB_ALL_IMAGES, 0, 1) == 0);
	wimlib_free(wim);

	reset(BT_IMAGE);
	windows_iso(26200);
	img_report.is_iso = FALSE;
	img_report.is_windows_img = TRUE;
	opts.image_options = IMOP_WINTOGO;
	opts.image_option = IMOP_WIN_TO_GO;
	opts.partition_type = PARTITION_STYLE_GPT;
	opts.target_type = TT_UEFI;

	// NTFS only
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_092));

	// An image that can't be opened
	opts.fs_type = FS_NTFS;
	image_path = tmp_path("nonexistent.wim");
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_291));

	// A single edition: no selection, just the Windows User Experience dialog
	image_path = tmp_path("install.wim");
	answer(1, 0x01);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK_EQ_INT(wintogo_index, 1);
	CHECK(asked_msg(0, MSG_327));
	// Offline drives, online account, user, locale, privacy and QoL (no 2023 CA without bcdboot)
	CHECK_EQ_INT(asked[0].nb_choices, 6);
	CHECK_EQ_INT(opts.unattend_xml_flags, UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_WINDOWS_TO_GO);
	CHECK(unattend_xml_path != NULL);
	// The architecture of an install.wim is the one of its EFI boot manager
	CHECK_CONTAINS(file_content(unattend_xml_path), "processorArchitecture=\"amd64\"");
	unattend_xml_mask = UNATTEND_DEFAULT_SELECTION_MASK;
	image_path = saved_image_path;
}

/* The bootloader files of the BIOS boot of an ISO */
static void test_bootloader_files(void)
{
	char name[MAX_PATH];

	// Another GRUB version: core.img from app_data_dir, or the embedded one
	reset(BT_IMAGE);
	linux_iso();
	img_report.sl_version = 0;
	img_report.has_grub2 = 1;
	static_strcpy(img_report.grub2_version, "2.06");
	answer(1, IDCANCEL);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_115));
	CHECK_EQ_INT(asked[0].flags, MB_YESNOCANCEL | MB_ICONWARNING);
	answer(1, IDNO);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(grub2_buf == NULL);
	log_buf[0] = 0;
	answer(1, IDYES);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(grub2_buf == NULL);
	CHECK_CONTAINS(log_buf, "Downloading " FILES_URL "/grub-2.06/core.img\n");
	CHECK_CONTAINS(log_buf, "Downloading " FILES_URL "/grub-2/core.img\n");
	CHECK_CONTAINS(log_buf, "was not found - will use embedded version");
	cache_file("grub-2.06/core.img", "GRUB 2.06 core.img");
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK_EQ_INT(grub2_len, 18);
	CHECK(grub2_buf != NULL && memcmp(grub2_buf, "GRUB 2.06 core.img", 18) == 0);
	// Not for the version we embed, nor for GPT
	static_strcpy(img_report.grub2_version, GRUB2_PACKAGE_VERSION);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(grub2_buf == NULL);
	static_strcpy(img_report.grub2_version, "2.06");
	opts.partition_type = PARTITION_STYLE_GPT;
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(grub2_buf == NULL);
	uncache();

	// Another Syslinux v5+: ldlinux.sys and ldlinux.bss from app_data_dir
	reset(BT_IMAGE);
	linux_iso();
	img_report.sl_version = 0x0603;
	static_strcpy(img_report.sl_version_str, "6.03");
	img_report.sl_version_ext[0] = 0;
	answer(1, IDNO);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_115));
	// The failed download gets reported
	answer(2, IDYES, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_DOWNLOAD_ERROR);
	CHECK(asked_msg(1, MSG_044));
	cache_file("syslinux-6.03/ldlinux.sys", "ldlinux.sys");
	cache_file("syslinux-6.03/ldlinux.bss", "ldlinux.bss!");
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK_EQ_INT(syslinux_ldlinux_len[0], 11);
	CHECK_EQ_INT(syslinux_ldlinux_len[1], 12);
	// With an extra version, the main version is tried too
	static_strcpy(img_report.sl_version_ext, "/2014-10-06");
	log_buf[0] = 0;
	// No error message for an extra version (as the embedded version may be used instead)
	answer(1, IDYES);
	CHECK_BOOTCHECK(BOOTCHECK_DOWNLOAD_ERROR);
	CHECK_CONTAINS(log_buf, "Downloading " FILES_URL "/syslinux-6.03/2014-10-06/ldlinux.sys\n");
	CHECK_CONTAINS(log_buf, "Extended version was not found, trying main version...\n");
	CHECK_CONTAINS(log_buf, "Downloading " FILES_URL "/syslinux-6.03/ldlinux.sys\n");
	// The embedded version is the last resort for another extra version
	img_report.sl_version = 0x0604;
	static_strcpy(img_report.sl_version_str, "6.04");
	answer(1, IDYES);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	uncache();

	// Syslinux v4: the obsolete .c32 files of the ISO can be replaced
	reset(BT_IMAGE);
	linux_iso();
	img_report.sl_version = 0x0403;
	static_strcpy(img_report.sl_version_str, "4.03");
	img_report.has_old_c32[1] = TRUE;
	answer(1, IDNO);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(asked_msg(0, MSG_083));
	CHECK(!opts.use_own_c32[1]);
	// The failed download gets reported
	answer(2, IDYES, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_DOWNLOAD_ERROR);
	CHECK(asked_msg(1, MSG_044));
	snprintf(name, sizeof(name), "syslinux-%s/vesamenu.c32", embedded_sl_version_str[0]);
	cache_file(name, "vesamenu.c32");
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK(opts.use_own_c32[1]);
	CHECK(!opts.use_own_c32[0]);
	uncache();

	// No BIOS bootloader for UEFI
	reset(BT_IMAGE);
	linux_iso();
	img_report.sl_version = 0x0603;
	opts.target_type = TT_UEFI;
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
}

/* The files that the helper gets from the front end, and what it loads from them */
static void test_shared_boot_files(void)
{
	char names[MAX_BOOT_FILES][MAX_PATH], name[MAX_PATH];

	// GRUB
	reset(BT_IMAGE);
	linux_iso();
	img_report.sl_version = 0;
	img_report.has_grub2 = 1;
	static_strcpy(img_report.grub2_version, "2.06");
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 1);
	CHECK_EQ_STR(names[0], "grub-2.06/core.img");
	CHECK_EQ_INT(GetBootFiles(names, TRUE), 0);
	cache_file("grub-2.06/core.img", "GRUB 2.06 core.img");
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	CHECK_EQ_INT(GetBootFiles(names, TRUE), 1);
	safe_free(grub2_buf);
	LoadBootFiles();
	CHECK_EQ_INT(grub2_len, 18);
	CHECK(grub2_buf != NULL && memcmp(grub2_buf, "GRUB 2.06 core.img", 18) == 0);
	opts.partition_type = PARTITION_STYLE_GPT;
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 0);
	LoadBootFiles();
	CHECK(grub2_buf == NULL);
	uncache();

	// Syslinux v5+, which needs both files
	reset(BT_IMAGE);
	linux_iso();
	img_report.sl_version = 0x0603;
	static_strcpy(img_report.sl_version_str, "6.03");
	static_strcpy(img_report.sl_version_ext, "/2014-10-06");
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 2);
	CHECK_EQ_STR(names[0], "syslinux-6.03/2014-10-06/ldlinux.sys");
	CHECK_EQ_STR(names[1], "syslinux-6.03/2014-10-06/ldlinux.bss");
	cache_file("syslinux-6.03/2014-10-06/ldlinux.sys", "ldlinux.sys");
	LoadBootFiles();
	CHECK(syslinux_ldlinux_len[0] == 0 && syslinux_ldlinux_len[1] == 0);
	cache_file("syslinux-6.03/2014-10-06/ldlinux.bss", "ldlinux.bss!");
	CHECK_EQ_INT(GetBootFiles(names, TRUE), 2);
	LoadBootFiles();
	CHECK_EQ_INT(syslinux_ldlinux_len[0], 11);
	CHECK_EQ_INT(syslinux_ldlinux_len[1], 12);
	// None for UEFI
	opts.target_type = TT_UEFI;
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 0);
	LoadBootFiles();
	CHECK(syslinux_ldlinux_len[0] == 0);
	uncache();

	// Syslinux v4: the .c32 files that replace the ones of the ISO
	reset(BT_IMAGE);
	linux_iso();
	img_report.sl_version = 0x0403;
	img_report.has_old_c32[1] = TRUE;
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 0);
	opts.use_own_c32[1] = TRUE;
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 1);
	snprintf(name, sizeof(name), "syslinux-%s/vesamenu.c32", embedded_sl_version_str[0]);
	CHECK_EQ_STR(names[0], name);

	// The other boot selections
	reset(BT_SYSLINUX_V6);
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 1);
	snprintf(name, sizeof(name), "syslinux-%s/ldlinux.c32", embedded_sl_version_str[1]);
	CHECK_EQ_STR(names[0], name);
	reset(BT_MSDOS);
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 1);
	CHECK_EQ_STR(names[0], "diskcopy.dll");
	reset(BT_GRUB4DOS);
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 1);
	CHECK_EQ_STR(names[0], "grub4dos-" GRUB4DOS_VERSION "/grldr");
	reset(BT_FREEDOS);
	CHECK_EQ_INT(GetBootFiles(names, FALSE), 0);
}

/* The files of the other boot selections */
static void test_boot_selections(void)
{
	char name[MAX_PATH];

	// Syslinux 6
	reset(BT_SYSLINUX_V6);
	answer(1, IDCANCEL);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_103));
	answer(1, IDNO);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	// The failed download gets reported
	answer(2, IDYES, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_DOWNLOAD_ERROR);
	CHECK(asked_msg(1, MSG_044));
	snprintf(name, sizeof(name), "syslinux-%s/ldlinux.c32", embedded_sl_version_str[1]);
	cache_file(name, "ldlinux.c32");
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);

	// Grub4DOS
	reset(BT_GRUB4DOS);
	answer(1, IDCANCEL);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_103));
	// The failed download gets reported
	answer(2, IDYES, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_DOWNLOAD_ERROR);
	CHECK(asked_msg(1, MSG_044));
	cache_file("grub4dos-" GRUB4DOS_VERSION "/grldr", "grldr");
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);

	// MS-DOS, which can't use 64 KB clusters
	reset(BT_MSDOS);
	opts.cluster_size = 64 * KB;
	answer(1, IDOK);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_111));
	opts.cluster_size = 4096;
	answer(1, IDNO);
	CHECK_BOOTCHECK(BOOTCHECK_CANCEL);
	CHECK(asked_msg(0, MSG_115));
	answer(1, IDYES);
	CHECK_BOOTCHECK(BOOTCHECK_DOWNLOAD_ERROR);
	cache_file("diskcopy.dll", "diskcopy.dll");
	answer(0);
	CHECK_BOOTCHECK(BOOTCHECK_PROCEED);
	uncache();
}

/* An image that is stored on the target drive */
static void test_source_on_target(void)
{
	char path[PATH_MAX], real[PATH_MAX], dev[PATH_MAX + 8], *p;
	struct stat st;

	reset(BT_IMAGE);
	CHECK(!IsSourceImageLocatedOnTargetDrive(NULL));
	CHECK(!IsSourceImageLocatedOnTargetDrive("/dev/null"));
	CHECK(!IsSourceImageLocatedOnTargetDrive("/dev/rufus-test-drive"));

	// The root file system, if it is on a block device that the test can see
	image_path = "/";
	if ((stat("/", &st) != 0) || (snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", major(st.st_dev),
		minor(st.st_dev)), realpath(path, real) == NULL)) {
		fprintf(stderr, "The root file system is not on a block device - skipping its check\n");
		goto out;
	}
	p = strrchr(real, '/');
	snprintf(dev, sizeof(dev), "/dev%s", p);
	if ((stat(dev, &st) != 0) || !S_ISBLK(st.st_mode)) {
		fprintf(stderr, "No '%s' - skipping the root file system check\n", dev);
		goto out;
	}
	CHECK(IsSourceImageLocatedOnTargetDrive(dev));
	opts.boot_type = BT_NON_BOOTABLE;
	CHECK(!IsSourceImageLocatedOnTargetDrive(dev));
	opts.boot_type = BT_IMAGE;
	// And the whole disk, for a partition
	snprintf(path, sizeof(path), "%s/partition", real);
	if (access(path, F_OK) == 0) {
		*p = 0;
		p = strrchr(real, '/');
		snprintf(dev, sizeof(dev), "/dev%s", p);
		if ((stat(dev, &st) == 0) && S_ISBLK(st.st_mode))
			CHECK(IsSourceImageLocatedOnTargetDrive(dev));
		// The boot check refuses it
		answer(1, IDOK);
		img_report.is_bootable_img = 1;
		bootcheck_drive_t target = { dev, TRUE };
		CHECK_EQ_INT(BootCheck(&target), BOOTCHECK_CANCEL);
		CHECK_ALL_ANSWERED();
		CHECK(asked_msg(0, MSG_358));
	}
out:
	image_path = NULL;
}

/* The boot check thread, which reports its result as an event */
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cond = PTHREAD_COND_INITIALIZER;
static int bootcheck_result = 1;

static void test_event(enum ui_event_type type, intptr_t param)
{
	if (type != UM_FORMAT_START)
		return;
	pthread_mutex_lock(&event_mutex);
	bootcheck_result = (int)param;
	pthread_cond_signal(&event_cond);
	pthread_mutex_unlock(&event_mutex);
}

static void test_thread(void)
{
	bootcheck_drive_t target = { "/dev/rufus-test-drive", FALSE };

	reset(BT_UEFI_NTFS);
	answer(1, IDOK);
	bootcheck_result = 1;
	CHECK(StartBootCheck(&target));
	target.path = NULL;	// The thread has its own copy
	pthread_mutex_lock(&event_mutex);
	while (bootcheck_result == 1)
		pthread_cond_wait(&event_cond, &event_mutex);
	pthread_mutex_unlock(&event_mutex);
	CHECK_EQ_INT(bootcheck_result, BOOTCHECK_CANCEL);
	CHECK_ALL_ANSWERED();
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log, .ask = test_ask, .event = test_event };

	ui_set_iface(&iface);
	if (!test_files_init())
		return 1;
	static_strcpy(temp_dir, tmp_dir);
	static_strcpy(app_data_dir, tmp_path("appdata"));
	snprintf(files_dir, sizeof(files_dir), "%s/%s", app_data_dir, FILES_DIR);
	make_file("rufus.ini", "; Rufus settings\n");
	ini_file = tmp_path("rufus.ini");
	SetEmbeddedSyslinuxVersions();
	image_path = tmp_path("rufus.ini");	// Any existing file

	test_nothing_to_check();
	test_image();
	test_incompatible();
	test_windows_user_experience();
	test_windows_to_go();
	test_bootloader_files();
	test_shared_boot_files();
	test_boot_selections();
	test_source_on_target();
	test_thread();

	safe_free(grub2_buf);
	ini_file = NULL;
	test_files_cleanup();
	return TEST_RESULT();
}

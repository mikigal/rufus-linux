/*
 * Rufus: The Reliable USB Formatting Utility
 * Boot check
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
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
 * BootCheckThread() from rufus.c, on opts instead of the main window controls.
 * Upstream changes the current directory to app_data_dir\FILES_DIR to look for
 * and download the bootloader files. Here, the paths are absolute, as the
 * current directory is shared by all the threads.
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "boot_check.h"
#include "wue.h"
#include "missing.h"
#include "settings.h"
#include "resource.h"
#include "localization.h"
#include "../res/grub/grub_version.h"
#include "../res/grub2/grub2_version.h"

extern const char* old_c32_name[NB_OLD_C32];
extern uint16_t embedded_sl_version[2];
extern char embedded_sl_version_str[2][12], embedded_sl_version_ext[2][32];

/*
 * The whole disk that a block device belongs to, as its device number,
 * or 0 if it isn't a block device. Partitions are resolved through sysfs.
 * NB: A file system on a device mapper (LVM, LUKS) or a btrfs volume is not
 * traced back to its disk, as upstream doesn't for an image without a drive
 * letter.
 */
static dev_t GetWholeDisk(dev_t dev)
{
	char path[64], real[PATH_MAX], *p;
	unsigned int maj, min;
	FILE* fd;

	static_sprintf(path, "/sys/dev/block/%u:%u", major(dev), minor(dev));
	if (realpath(path, real) == NULL)
		return 0;
	// A partition is a subdirectory of its disk, and has a 'partition' attribute
	static_sprintf(path, "/sys/dev/block/%u:%u/partition", major(dev), minor(dev));
	if (access(path, F_OK) != 0)
		return dev;
	p = strrchr(real, '/');
	if (p == NULL)
		return 0;
	*p = 0;
	safe_strcat(real, sizeof(real), "/dev");
	fd = fopen(real, "r");
	if (fd == NULL)
		return 0;
	if (fscanf(fd, "%u:%u", &maj, &min) != 2)
		maj = min = 0;
	fclose(fd);
	return makedev(maj, min);
}

// NB: Upstream compares the drive letter of the image with the ones of the target
BOOL IsSourceImageLocatedOnTargetDrive(const char* target)
{
	struct stat image_st, target_st;
	dev_t disk;

	if ((opts.boot_type != BT_IMAGE) || (image_path == NULL) || (target == NULL))
		return FALSE;
	if ((stat(image_path, &image_st) != 0) || (stat(target, &target_st) != 0) || !S_ISBLK(target_st.st_mode))
		return FALSE;
	disk = GetWholeDisk(image_st.st_dev);
	return (disk != 0) && (disk == GetWholeDisk(target_st.st_rdev));
}

// Read a file that the user previously downloaded into grub2_buf
static void ReadGrub2Buf(const char* path, const char* name, const char* err)
{
	FILE* fd = fopen(path, "rb");

	if (fd != NULL) {
		fseek(fd, 0, SEEK_END);
		grub2_len = ftell(fd);
		fseek(fd, 0, SEEK_SET);
		if (grub2_len > 0)
			grub2_buf = malloc(grub2_len);
	}
	// grub2_buf was set to NULL at the beginning of the boot check
	if ((fd == NULL) || (grub2_buf == NULL) || (fread(grub2_buf, 1, (size_t)grub2_len, fd) != (size_t)grub2_len)) {
		uprintf("Failed to read %s'%s' data - will use embedded version", err, name);
		safe_free(grub2_buf);
	}
	if (fd != NULL)
		fclose(fd);
}

enum bootcheck_return BootCheck(const bootcheck_drive_t* drive)
{
	int i, r;
	FILE *fd;
	uint32_t len;
	enum bootcheck_return ret = BOOTCHECK_CANCEL;
	BOOL esp_already_asked = FALSE;
	BOOL is_windows_to_go = ((opts.image_options & IMOP_WINTOGO) && (opts.image_option == IMOP_WIN_TO_GO));
	const char* msg;
	const char* grub = "grub";
	const char* core_img = "core.img";
	const char* ldlinux = "ldlinux";
	const char* syslinux = "syslinux";
	const char* ldlinux_ext[3] = { "sys", "bss", "c32" };
	char files_dir[MAX_PATH], tmp[MAX_PATH], tmp2[MAX_PATH], url[MAX_PATH], c;
	selection_dialog_options_t selection = { 0 };

	syslinux_ldlinux_len[0] = 0; syslinux_ldlinux_len[1] = 0;
	safe_free(grub2_buf);
	opts.append_silent = FALSE;
	static_sprintf(files_dir, "%s/%s", app_data_dir, FILES_DIR);

	if ((drive == NULL) || (drive->path == NULL))
		goto out;

	if ((opts.zero_drive) || (opts.boot_type == BT_NON_BOOTABLE)) {
		// Nothing to check
		ret = BOOTCHECK_PROCEED;
		goto out;
	}

	if (opts.boot_type == BT_IMAGE) {
		if_assert_fails(image_path != NULL)
			goto out;
		if (IsSourceImageLocatedOnTargetDrive(drive->path)) {
			// You cannot use an image that is located on the target drive
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_358), lmprintf(MSG_359));
			goto out;
		}
		/* Add 4 KB extra margin for VHD footers and so on */
		if ((size_check) && (img_report.projected_size > (opts.disk_size + 4 * KB))) {
			// This ISO image is too big for the selected target
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_088), lmprintf(MSG_089));
			goto out;
		}

		// Display an alert if any of the UEFI bootloaders have been revoked
		if (img_report.has_secureboot_bootloader & 0xfe) {
			switch (img_report.has_secureboot_bootloader & 0xfe) {
			case 4:
				msg = lmprintf(MSG_341, "Error code: 0xc0000428");
				break;
			default:
				msg = lmprintf(MSG_340);
				break;
			}
			r = Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_338), lmprintf(MSG_339, msg));
			if (r == IDCANCEL)
				goto out;
		}

		if (IS_DD_BOOTABLE(img_report)) {
			if (!img_report.is_iso) {
				// Pure DD images are fine at this stage
				opts.write_as_image = TRUE;
			} else if (opts.persistence_size == 0) {
				// Ask users how they want to write ISOHybrid images,
				// but only do so if persistence has not been selected.
				char* iso_image = lmprintf(MSG_036);
				char* dd_image = lmprintf(MSG_095);
				StrArrayCreate(&selection.choices, 4);
				// If the ISO is small enough to be written as an ESP and we are using GPT add the ISO → ESP option
				if ((img_report.projected_size < MAX_ISO_TO_ESP_SIZE) && HAS_REGULAR_EFI(img_report) &&
					(opts.partition_type == PARTITION_STYLE_GPT) && IS_FAT(opts.fs_type)) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_276, iso_image), TRUE);
					StrArrayAdd(&selection.choices, lmprintf(MSG_277, "ISO → ESP"), TRUE);
					StrArrayAdd(&selection.choices, lmprintf(MSG_277, dd_image), TRUE);
					i = SelectionDialog(lmprintf(MSG_274, "ISOHybrid"), lmprintf(MSG_275, iso_image, dd_image, iso_image, dd_image), &selection);
					StrArrayDestroy(&selection.choices);
					if (i < 0)	// Cancel
						goto out;
					opts.write_as_esp = (i & 2);
					opts.write_as_image = (i & 4);
					esp_already_asked = TRUE;
				} else {
					StrArrayAdd(&selection.choices, lmprintf(MSG_276, iso_image), TRUE);
					StrArrayAdd(&selection.choices, lmprintf(MSG_277, dd_image), TRUE);
					i = SelectionDialog(lmprintf(MSG_274, "ISOHybrid"), lmprintf(MSG_275, iso_image, dd_image, iso_image, dd_image), &selection);
					StrArrayDestroy(&selection.choices);
					if (i < 0)	// Cancel
						goto out;
					opts.write_as_image = (i & 2);
				}
			}
		}

		if (opts.write_as_image) {
				ret = BOOTCHECK_PROCEED;
				goto out;
		}

		if (is_windows_to_go) {
			if (opts.fs_type != FS_NTFS) {
				// Windows To Go only works for NTFS
				Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_092), lmprintf(MSG_097, "Windows To Go"));
				goto out;
			}
			// NB: Upstream refuses UEFI/GPT on removable media for Windows hosts older than the Creators
			// Update, which can't mount more than one partition of a removable drive (MSG_190/MSG_198).
			// If multiple versions are available, asks the user to select one before we commit to format the drive
			switch(SetWinToGoIndex(drive->is_fixed)) {
			case -1:
				Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_291), lmprintf(MSG_073));
				// fall through
			case -2:
				goto out;
			default:
				break;
			}
			// NB: Upstream requires a Windows 8 or later host for the Windows User Experience
			if (IS_WINDOWS_1X(img_report)) {
				int arch = _log2(img_report.has_efi >> 1);
				uint16_t map[16] = { 0 }, b = 1;
				StrArrayCreate(&selection.choices, 8);
				StrArrayCreate(&selection.tooltips, 8);
				StrArrayAdd(&selection.choices, lmprintf(MSG_332), TRUE);
				StrArrayAdd(&selection.tooltips, lmprintf(MSG_363), TRUE);
				MAP_BIT(UNATTEND_OFFLINE_INTERNAL_DRIVES);
				if (img_report.win_version.build >= 22500) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_330), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_361), TRUE);
					MAP_BIT(UNATTEND_NO_ONLINE_ACCOUNT);
				}
				StrArrayAdd(&selection.choices, lmprintf(MSG_333), TRUE);
				StrArrayAdd(&selection.tooltips, lmprintf(MSG_364), TRUE);
				selection.username_index = _log2(b) + 1;
				MAP_BIT(UNATTEND_SET_USER);
				StrArrayAdd(&selection.choices, lmprintf(MSG_334), TRUE);
				StrArrayAdd(&selection.tooltips, lmprintf(MSG_365), TRUE);
				MAP_BIT(UNATTEND_DUPLICATE_LOCALE);
				StrArrayAdd(&selection.choices, lmprintf(MSG_331), TRUE);
				StrArrayAdd(&selection.tooltips, lmprintf(MSG_362), TRUE);
				MAP_BIT(UNATTEND_NO_DATA_COLLECTION);
				if (IS_WINDOWS_11(img_report)) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_324), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_370), TRUE);
					MAP_BIT(UNATTEND_QOL_ENHANCEMENTS);
					if (img_report.win_version.build >= 26200 && bcdboot_supports_ex) {
						StrArrayAdd(&selection.choices, lmprintf(MSG_350), TRUE);
						StrArrayAdd(&selection.tooltips, lmprintf(MSG_368), TRUE);
						MAP_BIT(UNATTEND_USE_MS2023_BOOTLOADERS);
					}
				}
				if (expert_mode) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_346), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_367), TRUE);
					MAP_BIT(UNATTEND_FORCE_S_MODE);
				}
				selection.mask = remap16(unattend_xml_mask, map, FALSE);
				selection.style = BS_AUTOCHECKBOX;
				i = SelectionDialog(lmprintf(MSG_327), lmprintf(MSG_328), &selection);
				StrArrayDestroy(&selection.choices);
				StrArrayDestroy(&selection.tooltips);
				if (i < 0)
					goto out;
				// Remap i to the correct bit positions before calling CreateUnattendXml()
				i = remap16(i, map, TRUE);
				unattend_xml_path = CreateUnattendXml(arch, i | UNATTEND_WINDOWS_TO_GO);
				// Keep the bits we didn't process
				unattend_xml_mask &= ~(remap16(0x1ff, map, TRUE));
				// And add back the bits we did process
				unattend_xml_mask |= i;
				WriteSetting32(SETTING_WUE_OPTIONS, (UNATTEND_DEFAULT_MASK << 16) | unattend_xml_mask);
			}
		} else if (opts.target_type == TT_UEFI) {
			if (!IS_EFI_BOOTABLE(img_report)) {
				// Unsupported ISO
				Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_090), lmprintf(MSG_091));
				goto out;
			}
		} else if ( ((opts.fs_type == FS_NTFS) && !HAS_WINDOWS(img_report) && !HAS_GRUB(img_report) &&
					 (!HAS_SYSLINUX(img_report) || (SL_MAJOR(img_report.sl_version) <= 5)))
				 || ((IS_FAT(opts.fs_type)) && (!HAS_SYSLINUX(img_report)) && (!opts.allow_dual_uefi_bios) && !IS_EFI_BOOTABLE(img_report) &&
					 (!HAS_REACTOS(img_report)) && !HAS_KOLIBRIOS(img_report) && (!HAS_GRUB(img_report)))
				 || ((IS_FAT(opts.fs_type)) && (HAS_WINDOWS(img_report) || HAS_WININST(img_report)) && (!opts.allow_dual_uefi_bios)) ) {
			// Incompatible FS and ISO
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_092), lmprintf(MSG_096));
			goto out;
		} else if ((opts.fs_type == FS_FAT16) && HAS_KOLIBRIOS(img_report)) {
			// KolibriOS doesn't support FAT16
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_099), lmprintf(MSG_189));
			goto out;
		}
		if (IS_FAT(opts.fs_type) && img_report.has_4GB_file != 0 && img_report.has_4GB_file != 0x11){
			// This ISO image contains a file larger than 4GB file (FAT32)
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_099), lmprintf(MSG_100));
			goto out;
		}
		// NB: Upstream requires a Windows 8 or later host for the Windows User Experience
		if (IS_WINDOWS_1X(img_report) && (!is_windows_to_go)) {
			if (img_report.has_panther_unattend) {
				uprintf("NOTICE: A '/sources/$OEM$/$$/Panther/unattend.xml' was detected on the ISO.");
				uprintf("As a result, the 'Windows User Experience dialog' will not be displayed.");
			} else {
				int arch = _log2(img_report.has_efi >> 1);
				uint16_t map[16] = { 0 }, b = 1;
				StrArrayCreate(&selection.choices, 16);
				StrArrayCreate(&selection.tooltips, 16);
				if (IS_WINDOWS_11(img_report)) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_329), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_360), TRUE);
					MAP_BIT(UNATTEND_SECUREBOOT_TPM_MINRAM);
				}
				if (img_report.win_version.build >= 22500) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_330), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_361), TRUE);
					MAP_BIT(UNATTEND_NO_ONLINE_ACCOUNT);
				}
				StrArrayAdd(&selection.choices, lmprintf(MSG_333), TRUE);
				StrArrayAdd(&selection.tooltips, lmprintf(MSG_364), TRUE);
				selection.username_index = _log2(b) + 1;
				MAP_BIT(UNATTEND_SET_USER);
				StrArrayAdd(&selection.choices, lmprintf(MSG_334), TRUE);
				StrArrayAdd(&selection.tooltips, lmprintf(MSG_365), TRUE);
				selection.regional_index = _log2(b) + 1;
				MAP_BIT(UNATTEND_DUPLICATE_LOCALE);
				StrArrayAdd(&selection.choices, lmprintf(MSG_331), TRUE);
				StrArrayAdd(&selection.tooltips, lmprintf(MSG_362), TRUE);
				selection.privacy_index = _log2(b) + 1;
				MAP_BIT(UNATTEND_NO_DATA_COLLECTION);
				if (IS_WINDOWS_11(img_report)) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_355), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_371), TRUE);
					selection.edition_index = _log2(b) + 1;
					MAP_BIT(UNATTEND_SILENT_INSTALL);
					StrArrayAdd(&selection.choices, lmprintf(MSG_335), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_366), TRUE);
					MAP_BIT(UNATTEND_DISABLE_BITLOCKER);
					StrArrayAdd(&selection.choices, lmprintf(MSG_324), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_370), TRUE);
					MAP_BIT(UNATTEND_QOL_ENHANCEMENTS);
					if (img_report.win_version.build >= 26200) {
						StrArrayAdd(&selection.choices, lmprintf(MSG_350), TRUE);
						StrArrayAdd(&selection.tooltips, lmprintf(MSG_368), TRUE);
						MAP_BIT(UNATTEND_USE_MS2023_BOOTLOADERS);
						StrArrayAdd(&selection.choices, lmprintf(MSG_323), TRUE);
						StrArrayAdd(&selection.tooltips, lmprintf(MSG_369), TRUE);
						MAP_BIT(UNATTEND_APPLY_SKUSIPOLICY);
					}
				}
				if (expert_mode) {
					StrArrayAdd(&selection.choices, lmprintf(MSG_346), TRUE);
					StrArrayAdd(&selection.tooltips, lmprintf(MSG_367), TRUE);
					MAP_BIT(UNATTEND_FORCE_S_MODE);
				}
				selection.mask = remap16(unattend_xml_mask, map, FALSE);
				selection.style = BS_AUTOCHECKBOX;
				i = SelectionDialog(lmprintf(MSG_327), lmprintf(MSG_328), &selection);
				StrArrayDestroy(&selection.choices);
				StrArrayDestroy(&selection.tooltips);
				if (i < 0)
					goto out;
				i = remap16(i, map, TRUE);
				opts.append_silent = (i & UNATTEND_SILENT_INSTALL);
				unattend_xml_path = CreateUnattendXml(arch, i);
				// Remember the user preferences for the current session.
				unattend_xml_mask &= ~(remap16(UNATTEND_FULL_MASK, map, TRUE));
				unattend_xml_mask |= i;
				WriteSetting32(SETTING_WUE_OPTIONS, (UNATTEND_DEFAULT_MASK << 16) | unattend_xml_mask);
			}
		}

		if ((img_report.projected_size < MAX_ISO_TO_ESP_SIZE) && HAS_REGULAR_EFI(img_report) &&
			(opts.partition_type == PARTITION_STYLE_GPT) && IS_FAT(opts.fs_type) && !esp_already_asked) {
			// The ISO is small enough to be written as an ESP and we are using GPT
			// so ask the users if they want to write it as an ESP.
			char* iso_image = lmprintf(MSG_036);
			StrArrayCreate(&selection.choices, 2);
			StrArrayAdd(&selection.choices, lmprintf(MSG_276, iso_image), TRUE);
			StrArrayAdd(&selection.choices, lmprintf(MSG_277, "ISO → ESP"), TRUE);
			i = SelectionDialog(lmprintf(MSG_274, "ESP"), lmprintf(MSG_310), &selection);
			StrArrayDestroy(&selection.choices);
			if (i < 0)	// Cancel
				goto out;
			opts.write_as_esp = (i & 2);
		}

		// If the selected target doesn't include BIOS, skip file downloads for GRUB/Syslinux
		if (opts.target_type != TT_BIOS)
			goto uefi_target;

		if ((opts.partition_type == PARTITION_STYLE_MBR) && (img_report.has_grub2) && (img_report.grub2_version[0] != 0) &&
			(strcmp(img_report.grub2_version, GRUB2_PACKAGE_VERSION) != 0)) {
			// We may have to download a different Grub2 version if we can find one
			static_sprintf(tmp, "%s/%s-%s/%s", files_dir, grub, img_report.grub2_version, core_img);
			fd = fopen(tmp, "rb");
			if (fd != NULL) {
				// If a file already exists in the current directory, use that one
				fclose(fd);
				uprintf("Will reuse '%s' from '%s/%s-%s/' for Grub 2.x installation",
					core_img, files_dir, grub, img_report.grub2_version);
				ReadGrub2Buf(tmp, core_img, "existing ");
			} else {
				r = Notification(MB_YESNOCANCEL | MB_ICONWARNING, lmprintf(MSG_115),
					lmprintf(MSG_116, img_report.grub2_version, GRUB2_PACKAGE_VERSION));
				if (r == IDCANCEL)
					goto out;
				else if (r == IDYES) {
					static_sprintf(tmp, "%s/%s-%s", files_dir, grub, img_report.grub2_version);
					IGNORE_RETVAL(mkdir_p(tmp, 0755));
					static_sprintf(tmp2, "%s/%s", tmp, core_img);
					// The following loops through the grub2 version (which may have the ISO label appended)
					// and breaks it according to '.' or '-' until it finds a match on the server.
					for (i = (int)strlen(img_report.grub2_version), grub2_len = 0; i > 0 && grub2_len <= 0; i--) {
						c = img_report.grub2_version[i];
						if (c != 0 && c != '.' && c != '-')
							continue;
						img_report.grub2_version[i] = 0;
						static_sprintf(url, "%s/%s-%s/%s", FILES_URL, grub, img_report.grub2_version, core_img);
						grub2_len = (long)DownloadSignedFile(url, tmp2, NULL, FALSE);
						img_report.grub2_version[i] = c;
					}
					if (grub2_len <= 0) {
						PrintInfo(0, MSG_195, "Grub2");
						uprintf("%s was not found - will use embedded version", url);
					} else {
						PrintInfo(0, MSG_193, url);
						ReadGrub2Buf(tmp2, core_img, "");
					}
				}
			}
		}

		if ((opts.partition_type == PARTITION_STYLE_MBR) && HAS_SYSLINUX(img_report)) {
			if (SL_MAJOR(img_report.sl_version) < 5) {
				for (i = 0; i < NB_OLD_C32; i++) {
					if (img_report.has_old_c32[i]) {
						static_sprintf(tmp, "%s/%s-%s/%s", files_dir, syslinux, embedded_sl_version_str[0], old_c32_name[i]);
						fd = fopen(tmp, "rb");
						if (fd != NULL) {
							// If a file already exists in the current directory, use that one
							uprintf("Will replace obsolete '%s' from ISO with the one found in '%s'", old_c32_name[i], tmp);
							fclose(fd);
							opts.use_own_c32[i] = TRUE;
						} else {
							PrintInfo(0, MSG_204, old_c32_name[i]);
							if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_083, old_c32_name[i]),
								lmprintf(MSG_084, old_c32_name[i], old_c32_name[i])) == IDYES) {
								static_sprintf(tmp2, "%s/%s-%s", files_dir, syslinux, embedded_sl_version_str[0]);
								IGNORE_RETVAL(mkdir_p(tmp2, 0755));
								static_sprintf(url, "%s/%s-%s/%s", FILES_URL, syslinux, embedded_sl_version_str[0], old_c32_name[i]);
								len = DownloadSignedFile(url, tmp, NULL, TRUE);
								if (len == 0) {
									uprintf("Could not download file - cancelling");
									ret = BOOTCHECK_DOWNLOAD_ERROR;
									goto out;
								}
								opts.use_own_c32[i] = TRUE;
							}
						}
					}
				}
			} else if ((img_report.sl_version != embedded_sl_version[1]) ||
				(safe_strcmp(img_report.sl_version_ext, embedded_sl_version_ext[1]) != 0)) {
				// Unlike what was the case for v4 and earlier, Syslinux v5+ versions are INCOMPATIBLE with one another!
				for (i = 0; i < 2; i++) {
					// Check if we already have the relevant ldlinux_v#.##.sys & ldlinux_v#.##.bss files
					static_sprintf(tmp, "%s/%s-%s%s/%s.%s", files_dir, syslinux, img_report.sl_version_str,
						img_report.sl_version_ext, ldlinux, ldlinux_ext[i]);
					fd = fopen(tmp, "rb");
					if (fd != NULL) {
						fseek(fd, 0, SEEK_END);
						syslinux_ldlinux_len[i] = (DWORD)ftell(fd);
						fclose(fd);
					}
				}
				if ((syslinux_ldlinux_len[0] != 0) && (syslinux_ldlinux_len[1] != 0)) {
					// NB: Upstream converts sl_version_ext to a Windows path here
					uprintf("Will reuse '%s.%s' and '%s.%s' from '%s/%s-%s%s/' for Syslinux installation",
						ldlinux, ldlinux_ext[0], ldlinux, ldlinux_ext[1], files_dir, syslinux,
						img_report.sl_version_str, img_report.sl_version_ext);
				} else {
					r = Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_115),
						lmprintf(MSG_114, img_report.sl_version_str, img_report.sl_version_ext,
							embedded_sl_version_str[1], embedded_sl_version_ext[1]));
					if (r != IDYES)
						goto out;
					for (i = 0; i < 2; i++) {
						static_sprintf(tmp, "%s/%s-%s%s", files_dir, syslinux, img_report.sl_version_str, img_report.sl_version_ext);
						IGNORE_RETVAL(mkdir_p(tmp, 0755));
						static_sprintf(url, "%s/%s-%s%s/%s.%s", FILES_URL, syslinux, img_report.sl_version_str,
							img_report.sl_version_ext, ldlinux, ldlinux_ext[i]);
						static_sprintf(tmp, "%s/%s", files_dir, &url[sizeof(FILES_URL)]);
						syslinux_ldlinux_len[i] = DownloadSignedFile(url, tmp, NULL, (*img_report.sl_version_ext == 0));
						if ((syslinux_ldlinux_len[i] == 0) && (DownloadStatus == 404) && (*img_report.sl_version_ext != 0)) {
							// Couldn't locate the file on the server => try to download without the version extra
							uprintf("Extended version was not found, trying main version...");
							static_sprintf(url, "%s/%s-%s/%s.%s", FILES_URL, syslinux, img_report.sl_version_str,
								ldlinux, ldlinux_ext[i]);
							static_sprintf(tmp, "%s/%s", files_dir, &url[sizeof(FILES_URL)]);
							syslinux_ldlinux_len[i] = DownloadSignedFile(url, tmp, NULL, (*img_report.sl_version_ext == 0));
							if (syslinux_ldlinux_len[i] != 0) {
								// Duplicate the file so that the user won't be prompted to download again
								static_sprintf(tmp2, "%s/%s-%s%s/%s.%s", files_dir, syslinux, img_report.sl_version_str,
									img_report.sl_version_ext, ldlinux, ldlinux_ext[i]);
								copy_file(tmp, tmp2, FALSE);
							}
						}
						if (syslinux_ldlinux_len[i] == 0) {
							// If the version matches our embedded one, try to use that as a last ditch effort
							if (img_report.sl_version == embedded_sl_version[1]) {
								uprintf("Could not download the file - will try to use embedded %s version instead", img_report.sl_version_str);
							} else {
								uprintf("Could not download the file - cancelling");
								ret = BOOTCHECK_DOWNLOAD_ERROR;
								goto out;
							}
						}
					}
				}
			}
		}
	} else if (opts.boot_type == BT_SYSLINUX_V6) {
		static_sprintf(tmp, "%s/%s-%s/%s.%s", files_dir, syslinux, embedded_sl_version_str[1], ldlinux, ldlinux_ext[2]);
		fd = fopen(tmp, "rb");
		if (fd != NULL) {
			uprintf("Will reuse '%s' for Syslinux installation", tmp);
			fclose(fd);
		} else {
			static_sprintf(tmp2, "%s.%s", ldlinux, ldlinux_ext[2]);
			PrintInfo(0, MSG_206, tmp2);
			// MSG_104: "Syslinux v5.0 or later requires a '%s' file to be installed"
			r = Notification(MB_YESNOCANCEL | MB_ICONWARNING, lmprintf(MSG_103, tmp2),
				lmprintf(MSG_104, "Syslinux v5.0", tmp2, "Syslinux v5+", tmp2));
			if (r == IDCANCEL)
				goto out;
			if (r == IDYES) {
				static_sprintf(tmp2, "%s/%s-%s", files_dir, syslinux, embedded_sl_version_str[1]);
				IGNORE_RETVAL(mkdir_p(tmp2, 0755));
				static_sprintf(url, "%s/%s-%s/%s.%s", FILES_URL, syslinux, embedded_sl_version_str[1], ldlinux, ldlinux_ext[2]);
				if (DownloadSignedFile(url, tmp, NULL, TRUE) == 0) {
					ret = BOOTCHECK_DOWNLOAD_ERROR;
					goto out;
				}
			}
		}
	} else if (opts.boot_type == BT_MSDOS) {
		if ((size_check) && (opts.cluster_size >= 65536)) {
			// MS-DOS cannot boot from a drive using a 64 kilobytes Cluster size
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_111), lmprintf(MSG_110));
			goto out;
		}
		static_sprintf(tmp, "%s/diskcopy.dll", files_dir);
		if (access(tmp, F_OK) == 0) {
			uprintf("Will reuse '%s' for MS-DOS installation", tmp);
		} else {
			if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_115), lmprintf(MSG_337, "diskcopy.dll")) != IDYES)
				goto out;
			IGNORE_RETVAL(mkdir_p(files_dir, 0755));
			if (DownloadToFileOrBufferEx(DISKCOPY_URL, tmp, SYMBOL_SERVER_USER_AGENT, NULL, NULL, FALSE) != DISKCOPY_SIZE) {
				ret = BOOTCHECK_DOWNLOAD_ERROR;
				goto out;
			}
		}
	} else if (opts.boot_type == BT_GRUB4DOS) {
		static_sprintf(tmp, "%s/grub4dos-%s/grldr", files_dir, GRUB4DOS_VERSION);
		fd = fopen(tmp, "rb");
		if (fd != NULL) {
			uprintf("Will reuse '%s' for Grub4DOS installation", tmp);
			fclose(fd);
		} else {
			static_sprintf(tmp2, "grldr");
			PrintInfo(0, MSG_206, tmp2);
			r = Notification(MB_YESNOCANCEL | MB_ICONWARNING, lmprintf(MSG_103, tmp2),
				lmprintf(MSG_104, "Grub4DOS 0.4", tmp2, "Grub4DOS", tmp2));
			if (r == IDCANCEL)
				goto out;
			if (r == IDYES) {
				static_sprintf(tmp2, "%s/grub4dos-%s", files_dir, GRUB4DOS_VERSION);
				IGNORE_RETVAL(mkdir_p(tmp2, 0755));
				static_sprintf(url, "%s/grub4dos-%s/grldr", FILES_URL, GRUB4DOS_VERSION);
				if (DownloadSignedFile(url, tmp, NULL, TRUE) == 0) {
					ret = BOOTCHECK_DOWNLOAD_ERROR;
					goto out;
				}
			}
		}
	}

uefi_target:
	if (opts.boot_type == BT_UEFI_NTFS) {
		if (opts.fs_type != FS_NTFS && opts.fs_type != FS_EXFAT) {
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_092), lmprintf(MSG_097, "UEFI:NTFS"));
			goto out;
		}
	}
	ret = BOOTCHECK_PROCEED;

out:
	return ret;
}

static void* BootCheckThread(void* param)
{
	bootcheck_drive_t* drive = (bootcheck_drive_t*)param;
	enum bootcheck_return r = BootCheck(drive);

	free((char*)drive->path);
	free(drive);
	ui_event(UM_FORMAT_START, (intptr_t)r);
	return NULL;
}

BOOL StartBootCheck(const bootcheck_drive_t* drive)
{
	pthread_t thread;
	pthread_attr_t attr;
	bootcheck_drive_t* copy = calloc(1, sizeof(bootcheck_drive_t));
	int r = ENOMEM;

	if (copy != NULL) {
		if (drive != NULL) {
			*copy = *drive;
			copy->path = safe_strdup(drive->path);
		}
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		r = pthread_create(&thread, &attr, BootCheckThread, copy);
		pthread_attr_destroy(&attr);
	}
	if (r != 0) {
		if (copy != NULL)
			free((char*)copy->path);
		free(copy);
		uprintf("Unable to start boot check thread: %s", strerror(r));
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		return FALSE;
	}
	return TRUE;
}

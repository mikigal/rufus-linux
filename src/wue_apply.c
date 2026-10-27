/*
 * Rufus: The Reliable USB Formatting Utility
 * Windows User Experience: the changes applied to the target drive
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
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
 * The parts of wue.c that modify the target drive once its files are copied:
 * the WinPE setup of Windows XP, and the Windows User Experience (boot.wim and
 * the files of the media). The drive letter of upstream is the mount point of
 * the main partition here, whose file system may be case sensitive (NTFS), so
 * the paths of the ISO are looked up without case.
 *
 * The registry keys of the SYSTEM hive of boot.wim get created with hivex
 * (RegLoadKey() upstream), when Rufus is built with it, or else through the
 * unattend.xml, as the fallback of upstream does. Windows To Go is stage 5.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(RUFUS_HAVE_HIVEX)
#include <hivex.h>
#endif

#include "rufus.h"
#include "options.h"
#include "resource.h"
#include "wimlib.h"
#include "wue.h"
#include "localization.h"

// From wue.c
extern const char* bypass_name[3];
extern uint32_t removable_section[2];

extern uint64_t md5sum_totalbytes;
extern StrArray modified_files;
extern const char* efi_archname[ARCH_MAX];

static uint64_t FileSize(const char* path)
{
	struct stat st;

	return (stat(path, &st) == 0) ? (uint64_t)st.st_size : 0;
}

static int RemoveEntry(const char* path, const struct stat* st, int type, struct FTW* ftw)
{
	return remove(path);
}

static void RemoveDirectory(const char* path)
{
	nftw(path, RemoveEntry, 16, FTW_DEPTH | FTW_PHYS);
}

// The path of a file of the media, with the case it has there (if it exists)
static void MediaPath(char* path, size_t len, const char* mount_point, const char* name)
{
	snprintf(path, len, "%s/%s", mount_point, name);
	PathFileExistsNoCase(path);
}

/// <summary>
/// Setup and patch WinPE for Windows XP bootable USBs.
/// </summary>
/// <param name="mount_point">Where the target drive is mounted.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL SetupWinPE(const char* mount_point)
{
	char src[MAX_PATH], dst[MAX_PATH], name[64];
	const char* basedir[3] = { "i386", "amd64", "minint" };
	const char* patch_str_org[2] = { "\\minint\\txtsetup.sif", "\\minint\\system32\\" };
	const char* patch_str_rep[2][2] = { { "\\i386\\txtsetup.sif", "\\i386\\system32\\" } ,
										{ "\\amd64\\txtsetup.sif", "\\amd64\\system32\\" } };
	const char* setupsrcdev = "SetupSourceDevice = \"\\device\\harddisk1\\partition1\"";
	const char* win_nt_bt_org = "$win_nt$.~bt";
	const char* rdisk_zero = "rdisk(0)";
	uint32_t i, j, size, index = 0;
	BOOL r = FALSE;
	char* buffer = NULL;

	if ((img_report.winpe & WINPE_AMD64) == WINPE_AMD64)
		index = 1;
	else if ((img_report.winpe & WINPE_MININT) == WINPE_MININT)
		index = 2;
	// Copy of ntdetect.com in root
	static_sprintf(name, "%s/ntdetect.com", basedir[2 * (index / 2)]);
	MediaPath(src, sizeof(src), mount_point, name);
	static_sprintf(dst, "%s/ntdetect.com", mount_point);
	copy_file(src, dst, TRUE);
	if (!img_report.uses_minint) {
		// Create a copy of txtsetup.sif, as we want to keep the i386/amd64 files unmodified
		static_sprintf(name, "%s/txtsetup.sif", basedir[index]);
		MediaPath(src, sizeof(src), mount_point, name);
		static_sprintf(dst, "%s/txtsetup.sif", mount_point);
		if (!copy_file(src, dst, TRUE)) {
			uprintf("Did not copy %s as %s: %s", src, dst, WindowsErrorString());
		}
		if (insert_section_data(dst, "[SetupData]", setupsrcdev, FALSE) == NULL) {
			uprintf("Failed to add SetupSourceDevice in %s", dst);
			goto out;
		}
		uprintf("Successfully added '%s' to %s", setupsrcdev, dst);
	}

	static_sprintf(name, "%s/setupldr.bin", basedir[2 * (index / 2)]);
	MediaPath(src, sizeof(src), mount_point, name);
	static_sprintf(dst, "%s/BOOTMGR", mount_point);
	if (!copy_file(src, dst, TRUE)) {
		uprintf("Did not copy %s as %s: %s", src, dst, WindowsErrorString());
	}

	// \minint with /minint option doesn't require further processing => return true
	// \minint and no \i386 without /minint is unclear => return error
	if (img_report.winpe & WINPE_MININT) {
		if (img_report.uses_minint) {
			uprintf("Detected \\minint directory with /minint option: nothing to patch");
			r = TRUE;
		} else if (!(img_report.winpe & (WINPE_I386 | WINPE_AMD64))) {
			uprintf("Detected \\minint directory only but no /minint option: not sure what to do");
		}
		goto out;
	}

	// At this stage we only handle \i386
	size = read_file(dst, (uint8_t**)&buffer);
	if (size <= 32) {
		uprintf("Could not read file %s: %s", dst, WindowsErrorString());
		goto out;
	}

	// Patch setupldr.bin
	uprintf("Patching file %s", dst);
	// Remove CRC check for 32 bit part of setupldr.bin from Win2k3
	if ((size > 0x2061) && (buffer[0x2060] == 0x74) && (buffer[0x2061] == 0x03)) {
		buffer[0x2060] = (char)0xeb;
		buffer[0x2061] = 0x1a;
		uprintf("  0x00002060: 0x74 0x03 -> 0xEB 0x1A (disable Win2k3 CRC check)");
	}
	for (i = 1; i < size - 32; i++) {
		for (j = 0; j < ARRAYSIZE(patch_str_org); j++) {
			if (safe_strnicmp(&buffer[i], patch_str_org[j], strlen(patch_str_org[j]) - 1) == 0) {
				assert(index < 2);
				uprintf("  0x%08X: '%s' -> '%s'", i, &buffer[i], patch_str_rep[index][j]);
				strcpy(&buffer[i], patch_str_rep[index][j]);
				i += (uint32_t)max(strlen(patch_str_org[j]), strlen(patch_str_rep[index][j]));	// in case org is a substring of rep
			}
		}
	}

	if (!img_report.uses_minint) {
		// Additional setupldr.bin/bootmgr patching
		for (i = 0; i < size - 32; i++) {
			// rdisk(0) -> rdisk(#) disk masquerading
			// NB: only the first one seems to be needed
			if (safe_strnicmp(&buffer[i], rdisk_zero, strlen(rdisk_zero) - 1) == 0) {
				buffer[i + 6] = 0x31;
				uprintf("  0x%08X: '%s' -> 'rdisk(%c)'", i, rdisk_zero, buffer[i + 6]);
			}
			// $WIN_NT$_~BT -> i386/amd64
			if (safe_strnicmp(&buffer[i], win_nt_bt_org, strlen(win_nt_bt_org) - 1) == 0) {
				uprintf("  0x%08X: '%s' -> '%s%s'", i, &buffer[i], basedir[index], &buffer[i + strlen(win_nt_bt_org)]);
				strcpy(&buffer[i], basedir[index]);
				// This ensures that we keep the terminator backslash
				buffer[i + strlen(basedir[index])] = buffer[i + strlen(win_nt_bt_org)];
				buffer[i + strlen(basedir[index]) + 1] = 0;
			}
		}
	}

	if (write_file(dst, (uint8_t*)buffer, size) != size) {
		uprintf("Could not write patched file: %s", WindowsErrorString());
		goto out;
	}
	r = TRUE;

out:
	safe_free(buffer);
	return r;
}

/*
 * Create the HKLM\SYSTEM\Setup\LabConfig keys that bypass the TPM, Secure Boot
 * and RAM checks of the Windows 11 setup, in a SYSTEM hive extracted from boot.wim.
 */
static BOOL AddLabConfigKeys(const char* hive_path)
{
#if defined(RUFUS_HAVE_HIVEX)
	// REG_DWORD 1, which is little endian in the hive
	static const char dword_one[4] = { 1, 0, 0, 0 };
	hive_h* h;
	hive_node_h setup, lab_config;
	hive_set_value value = { .t = hive_t_REG_DWORD, .len = sizeof(dword_one), .value = (char*)dword_one };
	BOOL r = FALSE;
	size_t i;

	h = hivex_open(hive_path, HIVEX_OPEN_WRITE);
	if (h == NULL) {
		uprintf("Could not open '%s' registry hive: %s", hive_path, WindowsErrorString());
		return FALSE;
	}
	errno = 0;
	setup = hivex_node_get_child(h, hivex_root(h), "Setup");
	if (setup == 0) {
		if (errno == 0)
			errno = ENOENT;
		uprintf("Could not open 'HKLM\\SYSTEM\\Setup' registry key: %s", WindowsErrorString());
		goto out;
	}
	errno = 0;
	lab_config = hivex_node_get_child(h, setup, "LabConfig");
	if (lab_config == 0 && errno == 0)
		lab_config = hivex_node_add_child(h, setup, "LabConfig");
	if (lab_config == 0) {
		uprintf("Could not create 'HKLM\\SYSTEM\\Setup\\LabConfig' registry key: %s", WindowsErrorString());
		goto out;
	}
	for (i = 0; i < ARRAYSIZE(bypass_name); i++) {
		value.key = (char*)bypass_name[i];
		if (hivex_node_set_value(h, lab_config, &value, 0) != 0) {
			uprintf("Could not set 'HKLM\\SYSTEM\\Setup\\LabConfig\\%s' registry key: %s",
				bypass_name[i], WindowsErrorString());
			goto out;
		}
		uprintf("Created 'HKLM\\SYSTEM\\Setup\\LabConfig\\%s' registry key", bypass_name[i]);
	}
	if (hivex_commit(h, NULL, 0) != 0) {
		uprintf("Could not save '%s' registry hive: %s", hive_path, WindowsErrorString());
		goto out;
	}
	r = TRUE;

out:
	hivex_close(h);
	return r;
#else
	uprintf("This version of " APPLICATION_NAME " cannot edit registry hives (it was built without hivex)");
	return FALSE;
#endif
}

// Copy a file of the media, and account for the change of size in md5sum.txt
static BOOL ReplaceMediaFile(const char* src, const char* dst)
{
	if (opts.validate_md5sum)
		md5sum_totalbytes -= FileSize(dst);
	// TODO: Could we get fragmented data from copy_file() that may trigger the AMI bug?
	if (!copy_file(src, dst, FALSE))
		return FALSE;
	if (opts.validate_md5sum) {
		md5sum_totalbytes += FileSize(dst);
		StrArrayAdd(&modified_files, dst, TRUE);
	}
	return TRUE;
}

/// <summary>
/// Add unattend.xml to 'sources\boot.wim' (install) or 'Windows\Panther\' (Windows To Go).
/// </summary>
/// <param name="mount_point">Where the target drive, with the \sources\boot.wim image, is mounted.</param>
/// <param name="flags">A bitmap of unattend flags to apply.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL ApplyWindowsCustomization(const char* mount_point, int flags)
// NB: Work with a copy of unattend_xml_flags as a parameter since we will modify it.
{
	BOOL r = FALSE, update_boot_wim = FALSE;
	int i, wim_index = 2, wuc_index = 0, num_replaced = 0;
	const char* reg_path = "Windows/System32/config/SYSTEM";
	const char* efi_ex_path = "Windows/Boot/EFI_EX";
	const char* fonts_ex_path = "Windows/Boot/Fonts_EX";
	char boot_wim_path[MAX_PATH], tmp_path[2][MAX_PATH] = { "", "" }, hive_path[MAX_PATH + 16];
	char appraiserres_dll_src[MAX_PATH], appraiserres_dll_dst[MAX_PATH];
	char setup_exe[MAX_PATH], setup_dll[MAX_PATH], md5sum_path[MAX_PATH];
	char path[MAX_PATH], src[MAX_PATH], *rep, *tmp_dir_end;
	uint8_t* buf = NULL;
	uint16_t setup_arch;
	uint32_t len;
	DWORD dwSize;
	FILE* fd_md5sum;
	int fd;
	WIMStruct* wim = NULL;
	struct wimlib_update_command wuc[2] = { 0 };

	assert(unattend_xml_path != NULL);
	uprintf("Applying Windows customization:");
	PrintStatus(0, MSG_326);
	if (flags & UNATTEND_WINDOWS_TO_GO) {
		// TODO: Windows To Go (stage 5): copy unattend.xml to Windows\Panther
		uprintf("Windows To Go is not supported on Linux yet");
		goto out;
	}

	MediaPath(boot_wim_path, sizeof(boot_wim_path), mount_point, "sources/boot.wim");
	if (flags & UNATTEND_WINPE_SETUP_MASK) {
		// Create a backup of sources\appraiserres.dll and then create an empty file to
		// allow in-place upgrades without TPM/SB. Note that we need to create an empty,
		// appraiserres.dll otherwise setup.exe extracts its own.
		MediaPath(appraiserres_dll_src, sizeof(appraiserres_dll_src), mount_point, "sources/appraiserres.dll");
		static_strcpy(appraiserres_dll_dst, appraiserres_dll_src);
		static_strcpy(&appraiserres_dll_dst[strlen(appraiserres_dll_dst) - 4], ".bak");
		errno = 0;
		if (rename(appraiserres_dll_src, appraiserres_dll_dst) != 0 && errno != ENOENT) {
			uprintf("Could not rename '%s': %s", appraiserres_dll_src, WindowsErrorString());
		} else {
			if (errno == 0)
				uprintf("Renamed '%s' → '%s'", appraiserres_dll_src, appraiserres_dll_dst);
			fd = open(appraiserres_dll_src, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
			if (fd >= 0)
				close(fd);
			uprintf("Created '%s' placeholder", appraiserres_dll_src);
			if (opts.validate_md5sum) {
				md5sum_totalbytes -= FileSize(appraiserres_dll_dst);
				StrArrayAdd(&modified_files, appraiserres_dll_src, TRUE);
			}
		}
		// Apply the 'setup.exe' wrapper for Windows 11 24H2 in-place upgrades
		if (img_report.win_version.build >= 26000) {
			MediaPath(setup_exe, sizeof(setup_exe), mount_point, "setup.exe");
			static_strcpy(setup_dll, setup_exe);
			static_strcpy(&setup_dll[strlen(setup_dll) - 4], ".dll");
			MediaPath(md5sum_path, sizeof(md5sum_path), mount_point, "md5sum.txt");
			dwSize = read_file(setup_exe, &buf);
			if (dwSize != 0) {
				setup_arch = GetPeArch(buf);
				safe_free(buf);
				if (setup_arch != IMAGE_FILE_MACHINE_AMD64 && setup_arch != IMAGE_FILE_MACHINE_ARM64) {
					uprintf("WARNING: Unsupported arch 0x%x -- in-place upgrade wrapper will not be added", setup_arch);
				} else if (rename(setup_exe, setup_dll) != 0) {
					uprintf("Could not rename '%s': %s", setup_exe, WindowsErrorString());
				} else {
					uprintf("Renamed '%s' → '%s'", setup_exe, setup_dll);
					buf = GetResource((setup_arch == IMAGE_FILE_MACHINE_AMD64) ? IDR_SETUP_X64 : IDR_SETUP_ARM64,
						"setup.exe", &dwSize, FALSE);
					if (buf == NULL) {
						uprintf("Could not access embedded 'setup.exe'");
					} else if (write_file(setup_exe, buf, dwSize) == dwSize) {
						uprintf("Created '%s' bypass wrapper (from embedded)", setup_exe);
						if (opts.validate_md5sum) {
							if ((fd_md5sum = fopen(md5sum_path, "ab")) != NULL) {
								fprintf(fd_md5sum, "00000000000000000000000000000000  ./setup.dll\n");
								fclose(fd_md5sum);
							}
							StrArrayAdd(&modified_files, setup_exe, TRUE);
							StrArrayAdd(&modified_files, setup_dll, TRUE);
							md5sum_totalbytes += dwSize;
						}
					} else {
						uprintf("Could not create '%s' bypass wrapper", setup_exe);
					}
					buf = NULL;
				}
			}
		}
	}

	UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 0, PATCH_PROGRESS_TOTAL);
	// We only need to alter boot.wim if we have windowsPE data to deal with.
	// If not, we can just copy our unattend.xml in \sources\$OEM$\$$\Panther\.
	if (flags & UNATTEND_WINPE_SETUP_MASK || flags & UNATTEND_USE_MS2023_BOOTLOADERS) {
		if (opts.validate_md5sum)
			md5sum_totalbytes -= FileSize(boot_wim_path);
		// Some "unofficial" ISOs have a modified boot.wim that doesn't have Windows Setup at index 2...
		// TODO: we could try to look for "Microsoft Windows Setup" in the XML DESCRIPTION to locate the index
		wimlib_global_init(0);
		wimlib_set_print_errors(true);
		update_boot_wim = (wimlib_open_wim(boot_wim_path, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim) == 0);
		if (!update_boot_wim) {
			uprintf("Could not open '%s'", boot_wim_path);
			wimlib_global_cleanup();
			goto out;
		}
		// Setup image should be index 2
		if (wimlib_resolve_image(wim, "2") != 2) {
			uprintf("WARNING: This image appears to be an UNOFFICIAL Windows ISO!");
			uprintf("Rufus recommends that you only use OFFICIAL retail Microsoft Windows images, such as");
			uprintf("the ones that can be downloaded through the download facility of this application.");
			wim_index = 1;
		}
	}

	if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
		static_sprintf(tmp_path[0], "%s/%s-XXXXXX", temp_dir, APPLICATION_NAME);
		if (mkdtemp(tmp_path[0]) == NULL) {
			uprintf("WARNING: Could not create temp dir for registry changes");
			tmp_path[0][0] = 0;
			goto copy_unattend;
		}
		static_sprintf(hive_path, "%s/SYSTEM", tmp_path[0]);
		// Try to create the registry keys directly, and fallback to using unattend
		// if that fails (which the Windows Store version is expected to do).
		if (wimlib_extract_paths(wim, wim_index, tmp_path[0], &reg_path, 1,
				WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0 ||
			!AddLabConfigKeys(hive_path)) {
			uprintf("Falling back to creating the registry keys through unattend.xml");
			goto copy_unattend;
		}
		UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 101, PATCH_PROGRESS_TOTAL);
		wuc[wuc_index].op = WIMLIB_UPDATE_OP_ADD;
		wuc[wuc_index].add.fs_source_path = strdup(hive_path);
		wuc[wuc_index].add.wim_target_path = (char*)reg_path;
		wuc_index++;

		// We were successfull in creating the keys so remove this part from unattend.xml
		if ((flags & UNATTEND_WINPE_SETUP_MASK) == UNATTEND_SECUREBOOT_TPM_MINRAM) {
			// If this is all we used the WindowsPE pass for, then we just replace
			// '<settings pass="WindowsPE">' with '<settings pass="disabled">'
			if (replace_in_token_data(unattend_xml_path, "<settings", "windowsPE", "disabled", FALSE) == NULL)
				uprintf("WARNING: Could not disable 'WindowsPE' pass from unattend.xml");
		} else {
			// Otherwise, remove the relevant section from our temporary unattend.xml
			len = read_file(unattend_xml_path, &buf);
			if (len != 0 && removable_section[0] != 0 && removable_section[0] < len && removable_section[1] < len) {
				memmove(&buf[removable_section[0]], &buf[removable_section[1]], len - removable_section[1]);
				len -= removable_section[1] - removable_section[0];
				if (write_file(unattend_xml_path, buf, len) != len)
					uprintf("Failed to remove 'WindowsPE' section from unattend.xml");
			} else {
				uprintf("Failed to remove 'WindowsPE' section from unattend.xml");
			}
			safe_free(buf);
		}
		// Remove the flags, since we accomplished the registry creation outside of unattend.
		flags &= ~UNATTEND_SECUREBOOT_TPM_MINRAM;
		UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 102, PATCH_PROGRESS_TOTAL);
	}

copy_unattend:
	if (flags & UNATTEND_WINPE_SETUP_MASK) {
		// If we have a windowsPE section, copy the answer files to the root of boot.wim as
		// Autounattend.xml. This also results in that file being automatically copied over
		// to %WINDIR%\Panther\unattend.xml for later passes processing.
		if_assert_fails(update_boot_wim)
			goto out;
		wuc[wuc_index].op = WIMLIB_UPDATE_OP_ADD;
		wuc[wuc_index].add.fs_source_path = strdup(unattend_xml_path);
		wuc[wuc_index].add.wim_target_path = "Autounattend.xml";
		uprintf("Added '%s' to '%s'", wuc[wuc_index].add.wim_target_path, boot_wim_path);
		wuc_index++;
	} else {
		// If there is no windowsPE section in our unattend, then copying it as Autounattend.xml on
		// the root of boot.wim will not work as Windows Setup does *NOT* carry Autounattend.xml into
		// %WINDIR%\Panther\unattend.xml then (See: https://github.com/pbatard/rufus/issues/1981).
		// So instead, copy it to \sources\$OEM$\$$\Panther\unattend.xml on the media, as the content
		// of \sources\$OEM$\$$\* will get copied into %WINDIR%\ during the file copy phase.
		MediaPath(path, sizeof(path), mount_point, "sources/$OEM$/$$/Panther");
		if (mkdir_p(path, 0755) != 0) {
			uprintf("Error: Could not create directory '%s': %s", path, WindowsErrorString());
			goto out;
		}
		static_strcat(path, "/unattend.xml");
		if (!copy_file(unattend_xml_path, path, TRUE)) {
			uprintf("Could not create '%s': %s", path, WindowsErrorString());
			goto out;
		}
		uprintf("Created '%s'", path);
	}
	UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 103, PATCH_PROGRESS_TOTAL);

	if ((flags & UNATTEND_USE_MS2023_BOOTLOADERS) && !(flags & UNATTEND_WINDOWS_TO_GO)) {
		if_assert_fails(update_boot_wim)
			goto out;
		static_sprintf(tmp_path[1], "%s/%s-XXXXXX", temp_dir, APPLICATION_NAME);
		if (mkdtemp(tmp_path[1]) == NULL) {
			uprintf("WARNING: Could not create temp dir for 2023 signed UEFI bootloaders");
			tmp_path[1][0] = 0;
			goto out;
		}
		// If we have a '_EX' in the tmp name, we will have an issue
		if_assert_fails(strstr(tmp_path[1], "_EX") == NULL)
			goto out;
		// Extract the EFI_EX and Fonts_EX files
		if (wimlib_extract_paths(wim, wim_index, tmp_path[1], &efi_ex_path, 1,
				WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0 ||
			wimlib_extract_paths(wim, wim_index, tmp_path[1], &fonts_ex_path, 1,
				WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0) {
			uprintf("Could not extract 2023 signed UEFI bootloaders - Ignoring option");
		} else {
			StrArray files;
			len = (uint32_t)strlen(tmp_path[1]);
			tmp_dir_end = &tmp_path[1][len];

			// Copy/override the Font files
			static_strcat(tmp_path[1], "/Fonts_EX");
			StrArrayCreate(&files, 64);
			ListDirectoryContent(&files, tmp_path[1], LIST_DIR_TYPE_FILE);
			for (i = 0; i < (int)files.Index; i++) {
				static_sprintf(src, "efi/microsoft/boot%s", &files.String[i][len]);
				rep = remove_substr(src, "_EX");
				if (rep == NULL)
					continue;
				MediaPath(path, sizeof(path), mount_point, rep);
				safe_free(rep);
				if (!ReplaceMediaFile(files.String[i], path))
					uprintf("WARNING: Could not copy '%s': %s", path, WindowsErrorString());
				else
					num_replaced++;
			}
			StrArrayDestroy(&files);

			// Replace /EFI/Boot/boot###.efi
			for (i = 1; i < ARRAYSIZE(efi_archname); i++) {
				*tmp_dir_end = '\0';
				static_strcat(tmp_path[1], "/EFI_EX/bootmgfw_EX.efi");
				static_sprintf(src, "efi/boot/boot%s.efi", efi_archname[i]);
				MediaPath(path, sizeof(path), mount_point, src);
				if (access(path, F_OK) != 0)
					continue;
				if (!ReplaceMediaFile(tmp_path[1], path))
					uprintf("WARNING: Could not replace 'boot%s.efi': %s", efi_archname[i], WindowsErrorString());
				else
					num_replaced++;
				break;
			}

			// Replace /bootmgr.efi
			*tmp_dir_end = '\0';
			static_strcat(tmp_path[1], "/EFI_EX/bootmgr_EX.efi");
			MediaPath(path, sizeof(path), mount_point, "bootmgr.efi");
			if (access(tmp_path[1], F_OK) == 0) {	// 26H2 doesn't have a bootmgr.efi in EFI_EX
				if (!ReplaceMediaFile(tmp_path[1], path))
					uprintf("WARNING: Could not replace 'bootmgr.efi': %s", WindowsErrorString());
				else
					num_replaced++;
			}
			if (num_replaced != 0) {
				uprintf("Replaced %d EFI bootloader files with 'Windows UEFI CA 2023' compatible versions.", num_replaced);
				uprintf("Note that to boot this media, you must have a system where the 'Windows UEFI CA 2023'");
				uprintf("Secure Boot certificate has been installed.");
				uprintf("If needed, this can be accomplished using Mosby [https://github.com/pbatard/Mosby],");
				uprintf("which can be found, ready to use, in the UEFI Shell ISO images downloaded by Rufus.");
			}
			*tmp_dir_end = '\0';	// Else we won't be able to delete the temp dir
		}
	}

	r = TRUE;

out:
	if (update_boot_wim) {
		uprintf("Updating '%s[%d]'...", boot_wim_path, wim_index);
		if (wimlib_update_image(wim, wim_index, wuc, wuc_index, 0) != 0 ||
			// We use WIMLIB_WRITE_FLAG_UNSAFE_COMPACT unconditionally because, due to the
			// AMI NTFS driver bug that may produce false positives for MD5, we want to
			// have an unfragmented boot.wim after it is updated (one VCN), just as the
			// original boot.wim was written to disk, and our changes are such that wimlib
			// should be able to carry them all out within the existing file cluster area,
			// without needing to allocate new cluster ranges (and thus break our file).
			wimlib_overwrite(wim, WIMLIB_WRITE_FLAG_UNSAFE_COMPACT, 0) != 0) {
			uprintf("Error: Failed to update %s", boot_wim_path);
			r = FALSE;
		}
		for (i = 0; i < wuc_index; i++)
			free(wuc[i].add.fs_source_path);
		wimlib_free(wim);
		wimlib_global_cleanup();
		if (opts.validate_md5sum) {
			md5sum_totalbytes += FileSize(boot_wim_path);
			StrArrayAdd(&modified_files, boot_wim_path, TRUE);
		}
		UpdateProgressWithInfo(OP_PATCH, MSG_325, PATCH_PROGRESS_TOTAL, PATCH_PROGRESS_TOTAL);
	}
	for (i = 0; i < ARRAYSIZE(tmp_path); i++)
		if (tmp_path[i][0])
			RemoveDirectory(tmp_path[i]);
	return r;
}

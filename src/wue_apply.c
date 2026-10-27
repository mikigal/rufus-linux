/*
 * Rufus: The Reliable USB Formatting Utility
 * Windows User Experience: the changes applied to the target drive
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
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
 * The parts of wue.c that modify the target drive once it is formatted
 * (WinPE, Windows To Go and the customization of boot.wim), which come back
 * with the format pipeline in stage 2 and later. This file is not built yet.
 */

#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "rufus.h"
#include "options.h"
#include "vhd.h"
#include "xml.h"
#include "drive.h"
#include "format.h"
#include "wimlib.h"
#include "missing.h"
#include "resource.h"
#include "registry.h"
#include "msapi_utf8.h"
#include "timezoneapi.h"
#include "localization.h"

 /* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

// From wue.c
extern const char* bypass_name[3];
extern int wintogo_index, wininst_index;
extern uint32_t removable_section[2];

extern BOOL bcdboot_supports_ex;
extern uint64_t md5sum_totalbytes;
extern StrArray modified_files;
extern const char* efi_archname[ARCH_MAX];

/// <summary>
/// Setup and patch WinPE for Windows XP bootable USBs.
/// </summary>
/// <param name="drive_letter">The letter identifying the target drive.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL SetupWinPE(char drive_letter)
{
	char src[64], dst[32];
	const char* basedir[3] = { "i386", "amd64", "minint" };
	const char* patch_str_org[2] = { "\\minint\\txtsetup.sif", "\\minint\\system32\\" };
	const char* patch_str_rep[2][2] = { { "\\i386\\txtsetup.sif", "\\i386\\system32\\" } ,
										{ "\\amd64\\txtsetup.sif", "\\amd64\\system32\\" } };
	const char* setupsrcdev = "SetupSourceDevice = \"\\device\\harddisk1\\partition1\"";
	const char* win_nt_bt_org = "$win_nt$.~bt";
	const char* rdisk_zero = "rdisk(0)";
	const LARGE_INTEGER liZero = { {0, 0} };
	HANDLE handle = INVALID_HANDLE_VALUE;
	DWORD i, j, size, read_size, index = 0;
	BOOL r = FALSE;
	char* buffer = NULL;

	if ((img_report.winpe & WINPE_AMD64) == WINPE_AMD64)
		index = 1;
	else if ((img_report.winpe & WINPE_MININT) == WINPE_MININT)
		index = 2;
	// Copy of ntdetect.com in root
	static_sprintf(src, "%c:\\%s\\ntdetect.com", toupper(drive_letter), basedir[2 * (index / 2)]);
	static_sprintf(dst, "%c:\\ntdetect.com", toupper(drive_letter));
	CopyFileA(src, dst, TRUE);
	if (!img_report.uses_minint) {
		// Create a copy of txtsetup.sif, as we want to keep the i386/amd64 files unmodified
		static_sprintf(src, "%c:\\%s\\txtsetup.sif", toupper(drive_letter), basedir[index]);
		static_sprintf(dst, "%c:\\txtsetup.sif", toupper(drive_letter));
		if (!CopyFileA(src, dst, TRUE)) {
			uprintf("Did not copy %s as %s: %s\n", src, dst, WindowsErrorString());
		}
		if (insert_section_data(dst, "[SetupData]", setupsrcdev, FALSE) == NULL) {
			uprintf("Failed to add SetupSourceDevice in %s\n", dst);
			goto out;
		}
		uprintf("Successfully added '%s' to %s\n", setupsrcdev, dst);
	}

	static_sprintf(src, "%c:\\%s\\setupldr.bin", toupper(drive_letter), basedir[2 * (index / 2)]);
	static_sprintf(dst, "%c:\\BOOTMGR", toupper(drive_letter));
	if (!CopyFileA(src, dst, TRUE)) {
		uprintf("Did not copy %s as %s: %s\n", src, dst, WindowsErrorString());
	}

	// \minint with /minint option doesn't require further processing => return true
	// \minint and no \i386 without /minint is unclear => return error
	if (img_report.winpe & WINPE_MININT) {
		if (img_report.uses_minint) {
			uprintf("Detected \\minint directory with /minint option: nothing to patch\n");
			r = TRUE;
		} else if (!(img_report.winpe & (WINPE_I386 | WINPE_AMD64))) {
			uprintf("Detected \\minint directory only but no /minint option: not sure what to do\n");
		}
		goto out;
	}

	// At this stage we only handle \i386
	handle = CreateFileA(dst, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		uprintf("Could not open %s for patching: %s\n", dst, WindowsErrorString());
		goto out;
	}
	size = GetFileSize(handle, NULL);
	if (size == INVALID_FILE_SIZE) {
		uprintf("Could not get size for file %s: %s\n", dst, WindowsErrorString());
		goto out;
	}
	buffer = (char*)malloc(size);
	if (buffer == NULL)
		goto out;
	if ((!ReadFile(handle, buffer, size, &read_size, NULL)) || (size != read_size)) {
		uprintf("Could not read file %s: %s\n", dst, WindowsErrorString());
		goto out;
	}
	if (!SetFilePointerEx(handle, liZero, NULL, FILE_BEGIN)) {
		uprintf("Could not rewind file %s: %s\n", dst, WindowsErrorString());
		goto out;
	}

	// Patch setupldr.bin
	uprintf("Patching file %s\n", dst);
	// Remove CRC check for 32 bit part of setupldr.bin from Win2k3
	if ((size > 0x2061) && (buffer[0x2060] == 0x74) && (buffer[0x2061] == 0x03)) {
		buffer[0x2060] = 0xeb;
		buffer[0x2061] = 0x1a;
		uprintf("  0x00002060: 0x74 0x03 -> 0xEB 0x1A (disable Win2k3 CRC check)\n");
	}
	for (i = 1; i < size - 32; i++) {
		for (j = 0; j < ARRAYSIZE(patch_str_org); j++) {
			if (safe_strnicmp(&buffer[i], patch_str_org[j], strlen(patch_str_org[j]) - 1) == 0) {
				assert(index < 2);
				uprintf("  0x%08X: '%s' -> '%s'\n", i, &buffer[i], patch_str_rep[index][j]);
				strcpy(&buffer[i], patch_str_rep[index][j]);
				i += (DWORD)max(strlen(patch_str_org[j]), strlen(patch_str_rep[index][j]));	// in case org is a substring of rep
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
				uprintf("  0x%08X: '%s' -> 'rdisk(%c)'\n", i, rdisk_zero, buffer[i + 6]);
			}
			// $WIN_NT$_~BT -> i386/amd64
			if (safe_strnicmp(&buffer[i], win_nt_bt_org, strlen(win_nt_bt_org) - 1) == 0) {
				uprintf("  0x%08X: '%s' -> '%s%s'\n", i, &buffer[i], basedir[index], &buffer[i + strlen(win_nt_bt_org)]);
				strcpy(&buffer[i], basedir[index]);
				// This ensures that we keep the terminator backslash
				buffer[i + strlen(basedir[index])] = buffer[i + strlen(win_nt_bt_org)];
				buffer[i + strlen(basedir[index]) + 1] = 0;
			}
		}
	}

	if (!WriteFileWithRetry(handle, buffer, size, NULL, WRITE_RETRIES)) {
		uprintf("Could not write patched file: %s\n", WindowsErrorString());
		goto out;
	}
	r = TRUE;

out:
	safe_closehandle(handle);
	safe_free(buffer);
	return r;
}

/// <summary>
/// Setup a Windows To Go drive according to the official Microsoft instructions detailed at:
/// https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/deployment/windows-to-go/deploy-windows-to-go
/// Note that as opposed to the technet guide above we use bcdedit rather than 'unattend.xml'
/// to disable the recovery environment.
/// </summary>
/// <param name="DriveIndex">The Rufus drive index for the target media.</param>
/// <param name="drive_name">The path of the target media.</param>
/// <param name="use_esp">Whether to create an ESP on the target media.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL SetupWinToGo(DWORD DriveIndex, const char* drive_name, BOOL use_esp)
{
	char *ms_efi = NULL, wim_path[4 * MAX_PATH], cmd[MAX_PATH], path[MAX_PATH];
	BOOL use_bootex = FALSE;
	ULONG cluster_size;

	uprintf("Windows To Go mode selected");
	// Additional sanity checks
	if ((use_esp) && (SelectedDrive.MediaType != FixedMedia) && (WindowsVersion.BuildNumber < 15000)) {
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
		return FALSE;
	}

	assert(safe_strlen(image_path) < ARRAYSIZE(wim_path));
	static_strcpy(wim_path, image_path);
	if (!img_report.is_windows_img) {
		assert(safe_strlen(image_path) + safe_strlen(&img_report.wininst_path[wininst_index][3]) + 1 < ARRAYSIZE(wim_path));
		static_strcat(wim_path, "|");
		static_strcat(wim_path, &img_report.wininst_path[wininst_index][3]);
	}

	// Now we use the WIM API to apply that image
	if (!WimApplyImage(wim_path, wintogo_index, drive_name)) {
		uprintf("Failed to apply Windows To Go image");
		if (!IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_EXTRACT));
		return FALSE;
	}

	if (use_esp) {
		uprintf("Setting up EFI System Partition");
		// According to Ubuntu (https://bugs.launchpad.net/ubuntu/+source/partman-efi/+bug/811485) you want to use FAT32.
		// However, you have to be careful that the cluster size needs to be greater or equal to the sector size, which
		// in turn has an impact on the minimum EFI partition size we can create (see ms_efi_size_MB in drive.c)
		if (SelectedDrive.SectorSize <= 1024)
			cluster_size = 1024;
		else if (SelectedDrive.SectorSize <= 4096)
			cluster_size = 4096;
		else	// Go for broke
			cluster_size = (ULONG)SelectedDrive.SectorSize;
		// Boy do you *NOT* want to specify a label here, and spend HOURS figuring out why your EFI partition cannot boot...
		// Also, we use the Large FAT32 facility Microsoft APIs are *UTTERLY USELESS* for achieving what we want:
		// VDS cannot list ESP volumes (talk about allegedly improving on the old disk and volume APIs, only to
		// completely neuter it) and IVdsDiskPartitionMF::FormatPartitionEx(), which is what you are supposed to
		// use for ESPs, explicitly states: "This method cannot be used to format removable media."
		if (!FormatPartition(DriveIndex, SelectedDrive.Partition[partition_index[PI_ESP]].Offset, cluster_size, FS_FAT32, "",
			FP_QUICK | FP_FORCE | FP_LARGE_FAT32 | FP_NO_BOOT | FP_NO_PROGRESS)) {
			uprintf("Could not format EFI System Partition");
			return FALSE;
		}
		Sleep(200);
		// Need to have the ESP mounted to invoke bcdboot
		ms_efi = AltMountVolume(DriveIndex, SelectedDrive.Partition[partition_index[PI_ESP]].Offset, FALSE);
		if (ms_efi == NULL) {
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_ASSIGN_LETTER));
			return FALSE;
		}
	}

	// We invoke the 'bcdboot' command from the host, as the one from the drive produces problems (#558)
	// and of course, we couldn't invoke an ARM64 'bcdboot' binary on an x86 host anyway...
	// Also, since Rufus may be running as a 32 bit app, on 64 bit systems, we need to use
	// 'C:\Windows\Sysnative' and not 'C:\Windows\System32' to invoke bcdboot, as 'C:\Windows\System32'
	// will get converted to 'C:\Windows\SysWOW64' behind the scenes, and there is no bcdboot.exe there.
	// Finally, due to the whole _EX mess, we have to detect if the system bcdboot supports the /offline
	// option, and if it does, whether the target has `Windows\Boot\EFI_EX\` so that we don't let bcdboot
	// attempt to use the _EX files and fail. See https://github.com/pbatard/rufus/issues/2940.
	uprintf("Enabling boot using command:");
	// Create the EFI\Microsoft\Boot\ directory on the ESP, since bcdboot is apparently unable to do that!
	static_sprintf(path, "%s\\EFI\\Microsoft\\Boot", (use_esp) ? ms_efi : drive_name);
	_mkdirExU(path);
	// The default of recent bcdboot is to try to use the EX files (even if they don't exist) unless
	// /offline is specified *without* /bootex, which of course results in an error if, say, we try
	// to create a Windows 10 Windows To Go drive on an up to date Windows platform.
	// So we need to specify /offline and /bootex very carefully.
	static_sprintf(path, "%s\\Windows\\Boot\\EFI_EX", drive_name);
	use_bootex = (bcdboot_supports_ex && (opts.unattend_xml_flags & UNATTEND_USE_MS2023_BOOTLOADERS) && PathFileExistsU(path));
	static_sprintf(cmd, "%s\\bcdboot.exe %s\\Windows /v %s%s/f %s /s %s", sysnative_dir, drive_name,
		bcdboot_supports_ex ? "/offline " : "", use_bootex ? "/bootex " : "",
		HAS_BOOTMGR_BIOS(img_report) ? (HAS_BOOTMGR_EFI(img_report) ? "ALL" : "BIOS") : "UEFI",
		(use_esp) ? ms_efi : drive_name);
	// I don't believe we can ever have a stray '%' in cmd, but just in case...
	assert(strchr(cmd, '%') == NULL);
	uprintf(cmd);
	if (RunCommand(cmd, sysnative_dir, usb_debug) != 0) {
		// Try to continue... but report a failure
		uprintf("Failed to enable boot");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_EXTRACT));
	}

	UpdateProgressWithInfo(OP_FILE_COPY, MSG_267, 99, 100);

	// Setting internal drives offline for Windows To Go is crucial if, for instance, you are using ReFS
	// on Windows 10 (therefore ReFS v3.4) and don't want a Windows 11 To Go boot to automatically
	// "upgrade" the ReFS version on all drives to v3.7, thereby preventing you from being able to mount
	// those volumes back on Windows 10 ever again. Yes, I have been stung by this Microsoft bullshit!
	// See: https://gist.github.com/0xbadfca11/da0598e47dd643d933dc#Mountability
	if (opts.unattend_xml_flags & UNATTEND_OFFLINE_INTERNAL_DRIVES) {
		uprintf("Setting the target's internal drives offline using command:");
		// This applies the "offlineServicing" section of the unattend.xml (while ignoring the other sections)
		static_sprintf(cmd, "dism /Image:%s\\ /Apply-Unattend:%s", drive_name, unattend_xml_path);
		uprintf(cmd);
		RunCommand(cmd, NULL, usb_debug);
	}

	uprintf("Disabling use of the Windows Recovery Environment using command:");
	static_sprintf(cmd, "%s\\bcdedit.exe /store %s\\EFI\\Microsoft\\Boot\\BCD /set {default} recoveryenabled no",
		sysnative_dir, (use_esp) ? ms_efi : drive_name);
	assert(strchr(cmd, '%') == NULL);
	uprintf(cmd);
	RunCommand(cmd, sysnative_dir, usb_debug);

	UpdateProgressWithInfo(OP_FILE_COPY, MSG_267, 100, 100);

	if (use_esp) {
		Sleep(200);
		AltUnmountVolume(ms_efi, FALSE);
	}

	return TRUE;
}

/// <summary>
/// Add unattend.xml to 'sources\boot.wim' (install) or 'Windows\Panther\' (Windows To Go).
/// </summary>
/// <param name="drive_letter">The letter of the drive where the \sources\boot.wim image resides.</param>
/// <param name="flags">A bitmap of unattend flags to apply.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL ApplyWindowsCustomization(char drive_letter, int flags)
// NB: Work with a copy of unattend_xml_flags as a parameter since we will modify it.
{
	BOOL r = FALSE, is_hive_mounted = FALSE, update_boot_wim = FALSE;
	int i, wim_index = 2, wuc_index = 0, num_replaced = 0;
	const char* offline_hive_name = "RUFUS_OFFLINE_HIVE";
	const char* reg_path = "Windows\\System32\\config\\SYSTEM";
	const char* efi_ex_path = "Windows\\Boot\\EFI_EX";
	const char* fonts_ex_path = "Windows\\Boot\\Fonts_EX";
	char boot_wim_path[] = "?:\\sources\\boot.wim", key_path[64];
	char tmp_path[2][MAX_PATH] = { "", "" };
	char appraiserres_dll_src[] = "?:\\sources\\appraiserres.dll";
	char appraiserres_dll_dst[] = "?:\\sources\\appraiserres.bak";
	char setup_exe[] = "?:\\setup.exe";
	char setup_dll[] = "?:\\setup.dll";
	char md5sum_path[] = "?:\\md5sum.txt";
	char path[MAX_PATH], *rep, *tmp_dir_end;
	uint8_t* buf = NULL;
	uint16_t setup_arch;
	uint32_t len;
	HKEY hKey = NULL, hSubKey = NULL;
	LSTATUS status;
	DWORD dwDisp, dwVal = 1, dwSize;
	FILE* fd_md5sum;
	WIMStruct* wim = NULL;
	struct wimlib_update_command wuc[2] = { 0 };

	assert(unattend_xml_path != NULL);
	uprintf("Applying Windows customization:");
	PrintStatus(0, MSG_326);
	if (flags & UNATTEND_WINDOWS_TO_GO) {
		static_sprintf(path, "%c:\\Windows\\Panther", drive_letter);
		if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
			uprintf("Could not create '%s' : %s", path, WindowsErrorString());
			goto out;
		}
		static_sprintf(path, "%c:\\Windows\\Panther\\unattend.xml", drive_letter);
		if (!CopyFileA(unattend_xml_path, path, TRUE)) {
			uprintf("Could not create '%s' : %s", path, WindowsErrorString());
			goto out;
		}
		uprintf("Added '%s'", path);
	} else {
		boot_wim_path[0] = drive_letter;
		if (flags & UNATTEND_WINPE_SETUP_MASK) {
			// Create a backup of sources\appraiserres.dll and then create an empty file to
			// allow in-place upgrades without TPM/SB. Note that we need to create an empty,
			// appraiserres.dll otherwise setup.exe extracts its own.
			appraiserres_dll_src[0] = drive_letter;
			appraiserres_dll_dst[0] = drive_letter;
			if (!MoveFileExU(appraiserres_dll_src, appraiserres_dll_dst, MOVEFILE_REPLACE_EXISTING)
				&& GetLastError() != ERROR_FILE_NOT_FOUND) {
				uprintf("Could not rename '%s': %s", appraiserres_dll_src, WindowsErrorString());
			} else {
				if (GetLastError() == ERROR_SUCCESS)
					uprintf("Renamed '%s' → '%s'", appraiserres_dll_src, appraiserres_dll_dst);
				CloseHandle(CreateFileU(appraiserres_dll_src, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
					NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
				uprintf("Created '%s' placeholder", appraiserres_dll_src);
				if (opts.validate_md5sum) {
					md5sum_totalbytes -= _filesizeU(appraiserres_dll_dst);
					StrArrayAdd(&modified_files, appraiserres_dll_src, TRUE);
				}
			}
			// Apply the 'setup.exe' wrapper for Windows 11 24H2 in-place upgrades
			if (img_report.win_version.build >= 26000) {
				setup_exe[0] = drive_letter;
				setup_dll[0] = drive_letter;
				md5sum_path[0] = drive_letter;
				dwSize = read_file(setup_exe, &buf);
				if (dwSize != 0) {
					setup_arch = GetPeArch(buf);
					safe_free(buf);
					if (setup_arch != IMAGE_FILE_MACHINE_AMD64 && setup_arch != IMAGE_FILE_MACHINE_ARM64) {
						uprintf("WARNING: Unsupported arch 0x%x -- in-place upgrade wrapper will not be added", setup_arch);
					} else if (!MoveFileExU(setup_exe, setup_dll, 0)) {
						uprintf("Could not rename '%s': %s", setup_exe, WindowsErrorString());
					} else {
						uprintf("Renamed '%s' → '%s'", setup_exe, setup_dll);
						buf = GetResource(hMainInstance, MAKEINTRESOURCEA(setup_arch == IMAGE_FILE_MACHINE_AMD64 ? IDR_SETUP_X64 : IDR_SETUP_ARM64),
							_RT_RCDATA, "setup.exe", &dwSize, FALSE);
						if (buf == NULL) {
							uprintf("Could not access embedded 'setup.exe'");
						} else if (write_file(setup_exe, buf, dwSize) == dwSize) {
							uprintf("Created '%s' bypass wrapper (from embedded)", setup_exe);
							if (opts.validate_md5sum) {
								if ((fd_md5sum = fopenU(md5sum_path, "ab")) != NULL) {
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
				md5sum_totalbytes -= _filesizeU(boot_wim_path);
			// Some "unofficial" ISOs have a modified boot.wim that doesn't have Windows Setup at index 2...
			// TODO: we could try to look for "Microsoft Windows Setup" in the XML DESCRIPTION to locate the index
			wimlib_global_init(0);
			wimlib_set_print_errors(true);
			update_boot_wim = (wimlib_open_wimU(boot_wim_path, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim) == 0);
			if (!update_boot_wim) {
				uprintf("Could not open '%s'", boot_wim_path);
				goto out;
			}
			// Setup image should be index 2
			if (wimlib_resolve_image(wim, L"2") != 2) {
				uprintf("WARNING: This image appears to be an UNOFFICIAL Windows ISO!");
				uprintf("Rufus recommends that you only use OFFICIAL retail Microsoft Windows images, such as");
				uprintf("the ones that can be downloaded through the download facility of this application.");
				wim_index = 1;
			}
		}

		if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
			if (GetTempDirNameU(temp_dir, APPLICATION_NAME, 0, tmp_path[0]) == 0) {
				uprintf("WARNING: Could not create temp dir for registry changes");
				goto copy_unattend;
			}
			static_sprintf(tmp_path[1], "%s\\SYSTEM", tmp_path[0]);
			// Try to create the registry keys directly, and fallback to using unattend
			// if that fails (which the Windows Store version is expected to do).
			if (wimlib_extract_pathsU(wim, wim_index, tmp_path[0], &reg_path, 1,
					WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0 ||
				!MountRegistryHive(HKEY_LOCAL_MACHINE, offline_hive_name, tmp_path[1])) {
				uprintf("Falling back to creating the registry keys through unattend.xml");
				goto copy_unattend;
			}
			UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 101, PATCH_PROGRESS_TOTAL);
			is_hive_mounted = TRUE;

			static_sprintf(key_path, "%s\\Setup", offline_hive_name);
			status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path, 0, KEY_READ | KEY_CREATE_SUB_KEY, &hKey);
			if (status != ERROR_SUCCESS) {
				SetLastError(status);
				uprintf("Could not open 'HKLM\\SYSTEM\\Setup' registry key: %s", WindowsErrorString());
				goto copy_unattend;
			}

			status = RegCreateKeyExA(hKey, "LabConfig", 0, NULL, 0,
				KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_CREATE_SUB_KEY, NULL, &hSubKey, &dwDisp);
			if (status != ERROR_SUCCESS) {
				SetLastError(status);
				uprintf("Could not create 'HKLM\\SYSTEM\\Setup\\LabConfig' registry key: %s", WindowsErrorString());
				goto copy_unattend;
			}

			for (i = 0; i < ARRAYSIZE(bypass_name); i++) {
				status = RegSetValueExA(hSubKey, bypass_name[i], 0, REG_DWORD, (LPBYTE)&dwVal, sizeof(DWORD));
				if (status != ERROR_SUCCESS) {
					SetLastError(status);
					uprintf("Could not set 'HKLM\\SYSTEM\\Setup\\LabConfig\\%s' registry key: %s",
						bypass_name[i], WindowsErrorString());
					goto copy_unattend;
				}
				uprintf("Created 'HKLM\\SYSTEM\\Setup\\LabConfig\\%s' registry key", bypass_name[i]);
			}
			wuc[wuc_index].op = WIMLIB_UPDATE_OP_ADD;
			wuc[wuc_index].add.fs_source_path = utf8_to_wchar(tmp_path[1]);
			tmp_path[1][0] = '\0';
			wuc[wuc_index].add.wim_target_path = L"Windows\\System32\\config\\SYSTEM";
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
			wuc[wuc_index].add.fs_source_path = utf8_to_wchar(unattend_xml_path);
			wuc[wuc_index].add.wim_target_path = L"Autounattend.xml";
			uprintf("Added '%S' to '%s'", wuc[wuc_index].add.wim_target_path, boot_wim_path);
			wuc_index++;
		} else {
			// If there is no windowsPE section in our unattend, then copying it as Autounattend.xml on
			// the root of boot.wim will not work as Windows Setup does *NOT* carry Autounattend.xml into
			// %WINDIR%\Panther\unattend.xml then (See: https://github.com/pbatard/rufus/issues/1981).
			// So instead, copy it to \sources\$OEM$\$$\Panther\unattend.xml on the media, as the content
			// of \sources\$OEM$\$$\* will get copied into %WINDIR%\ during the file copy phase.
			static_sprintf(path, "%c:\\sources\\$OEM$\\$$\\Panther", drive_letter);
			i = SHCreateDirectoryExA(NULL, path, NULL);
			if (i != ERROR_SUCCESS) {
				SetLastError(i);
				uprintf("Error: Could not create directory '%s': %s", path, WindowsErrorString());
				goto out;
			}
			static_sprintf(path, "%c:\\sources\\$OEM$\\$$\\Panther\\unattend.xml", drive_letter);
			if (!CopyFileU(unattend_xml_path, path, TRUE)) {
				uprintf("Could not create '%s': %s", path, WindowsErrorString());
				goto out;
			}
			uprintf("Created '%s'", path);
		}
		UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 103, PATCH_PROGRESS_TOTAL);
	}

	if ((flags & UNATTEND_USE_MS2023_BOOTLOADERS) && !(flags & UNATTEND_WINDOWS_TO_GO)) {
		if_assert_fails(update_boot_wim)
			goto out;
		if (GetTempDirNameU(temp_dir, APPLICATION_NAME, 0, tmp_path[1]) == 0) {
			uprintf("WARNING: Could not create temp dir for 2023 signed UEFI bootloaders");
			goto out;
		}
		// If we have a '_EX' in the tmp name, we will have an issue
		if_assert_fails(strstr(tmp_path[1], "_EX") == NULL)
			goto out;
		// Extract the EFI_EX and Fonts_EX files
		if (wimlib_extract_pathsU(wim, wim_index, tmp_path[1], &efi_ex_path, 1,
				WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0 ||
			wimlib_extract_pathsU(wim, wim_index, tmp_path[1], &fonts_ex_path, 1,
				WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0) {
			uprintf("Could not extract 2023 signed UEFI bootloaders - Ignoring option");
		} else {
			StrArray files;
			len = (uint32_t)strlen(tmp_path[1]);
			tmp_dir_end = &tmp_path[1][len];

			// Copy/override the Font files
			static_strcat(tmp_path[1], "\\Fonts_EX");
			StrArrayCreate(&files, 64);
			ListDirectoryContent(&files, tmp_path[1], LIST_DIR_TYPE_FILE);
			for (i = 0; i < (int)files.Index; i++) {
				static_sprintf(path, "%c:\\efi\\microsoft\\boot%s", drive_letter, &files.String[i][len]);
				rep = remove_substr(path, "_EX");
				if (opts.validate_md5sum)
					md5sum_totalbytes -= _filesizeU(rep);
				// TODO: Could we get fragmented data from CopyFile() that may trigger the AMI bug?
				if (!CopyFileU(files.String[i], rep, FALSE)) {
					uprintf("WARNING: Could not copy '%s': %s", path, WindowsErrorString());
				} else {
					num_replaced++;
					if (opts.validate_md5sum) {
						md5sum_totalbytes += _filesizeU(files.String[i]);
						StrArrayAdd(&modified_files, rep, TRUE);
					}
				}
				safe_free(rep);
			}
			StrArrayDestroy(&files);

			// Replace /EFI/Boot/boot###.efi
			for (i = 1; i < ARRAYSIZE(efi_archname); i++) {
				*tmp_dir_end = '\0';
				static_strcat(tmp_path[1], "\\EFI_EX\\bootmgfw_EX.efi");
				static_sprintf(path, "%c:\\efi\\boot\\boot%s.efi", drive_letter, efi_archname[i]);
				if (!PathFileExistsA(path))
					continue;
				if (opts.validate_md5sum)
					md5sum_totalbytes -= _filesizeU(path);
				if (!CopyFileU(tmp_path[1], path, FALSE)) {
					uprintf("WARNING: Could not replace 'boot%s.efi': %s", efi_archname[i], WindowsErrorString());
				} else {
					num_replaced++;
					if (opts.validate_md5sum) {
						md5sum_totalbytes += _filesizeU(tmp_path[1]);
						StrArrayAdd(&modified_files, path, TRUE);
					}
				}
				break;
			}

			// Replace /bootmgr.efi
			*tmp_dir_end = '\0';
			static_strcat(tmp_path[1], "\\EFI_EX\\bootmgr_EX.efi");
			static_sprintf(path, "%c:\\bootmgr.efi", drive_letter);
			if (PathFileExistsA(tmp_path[1])) {	// 26H2 doesn't have a bootmgr.efi in EFI_EX
				if (!CopyFileU(tmp_path[1], path, FALSE)) {
					uprintf("WARNING: Could not replace 'bootmgr.efi': %s", WindowsErrorString());
				} else {
					num_replaced++;
					if (opts.validate_md5sum) {
						md5sum_totalbytes -= _filesizeU(tmp_path[1]);
						md5sum_totalbytes += _filesizeU(path);
						StrArrayAdd(&modified_files, path, TRUE);
					}
				}
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
	if (hSubKey != NULL)
		RegCloseKey(hSubKey);
	if (hKey != NULL)
		RegCloseKey(hKey);
	if (is_hive_mounted) {
		UnmountRegistryHive(HKEY_LOCAL_MACHINE, offline_hive_name);
		UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 104, PATCH_PROGRESS_TOTAL);
	}
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
		for (i = 0; i < ARRAYSIZE(tmp_path); i++)
			if (tmp_path[i][0])
				SHDeleteDirectoryExU(NULL, tmp_path[i], FOF_NO_UI);
		for (i = 0; i < wuc_index; i++)
			free(wuc[i].add.fs_source_path);
		wimlib_free(wim);
		wimlib_global_cleanup();
		if (opts.validate_md5sum) {
			md5sum_totalbytes += _filesizeU(boot_wim_path);
			StrArrayAdd(&modified_files, boot_wim_path, TRUE);
		}
		UpdateProgressWithInfo(OP_PATCH, MSG_325, PATCH_PROGRESS_TOTAL, PATCH_PROGRESS_TOTAL);
	}
	return r;
}

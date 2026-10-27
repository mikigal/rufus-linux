/*
 * Rufus: The Reliable USB Formatting Utility
 * Image scan
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
 * ImageScanThread() and the functions it uses, from rufus.c. What the scan
 * thread did to the main window (enabling the controls, updating the boot
 * selection, the file system and the label) is in rules_on_image_scan_start()
 * and rules_on_image_scan_end(), which the front end calls around the scan.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "ui_iface.h"
#include "image_scan.h"
#include "vhd.h"
#include "wue.h"
#include "winpe.h"
#include "cregex.h"
#include "resource.h"
#include "localization.h"
#include "bled/bled.h"

static const char* arch_name[ARCH_MAX] = {
	"unknown", "x86_32", "x86_64", "ARM", "ARM64", "IA64", "RISC-V 64", "LoongArch 64", "EBC" };
extern const char* old_c32_name[NB_OLD_C32];

char* short_image_path = NULL;

#define PRINT_ISO_PROP(b, ...) do {if (b) uprintf(__VA_ARGS__);} while(0)

// Report the features of the selected ISO images
static void DisplayISOProps(void)
{
	static char inst_str[] = " [1/#]";
	int i;

	uprintf("ISO label: '%s'", img_report.label);
	if (img_report.win_version.major != 0) {
		if (img_report.win_version.minor == 0)
			uprintf("  Detected: Windows %d ISO (Build %d.%d)", img_report.win_version.major,
				img_report.win_version.build, img_report.win_version.revision);
		else
			uprintf("  Detected: Windows %d.%d ISO (Build %d.%d)", img_report.win_version.major,
				img_report.win_version.minor, img_report.win_version.build, img_report.win_version.revision);
	}

	uprintf("  Size: %s (Projected)", SizeToHumanReadable(img_report.projected_size, FALSE, FALSE));
	if (img_report.mismatch_size > 0) {
		uprintf("  ERROR: Detected that file on disk has been truncated by %s!",
			SizeToHumanReadable(img_report.mismatch_size, FALSE, FALSE));
		Notification(MB_OK | MB_ICONWARNING, lmprintf(MSG_297),
			lmprintf(MSG_298, SizeToHumanReadable(img_report.mismatch_size, FALSE, FALSE)));
	} else if (img_report.mismatch_size < 0) {
		// Not an error (ISOHybrid?), but we report it just in case
		uprintf("  Note: File on disk is larger than reported ISO size by %s...",
			SizeToHumanReadable(-img_report.mismatch_size, FALSE, FALSE));
	}

	PRINT_ISO_PROP(img_report.has_4GB_file, "  Has a >4GB file");
	PRINT_ISO_PROP(img_report.has_long_filename, "  Has a >64 chars filename");
	PRINT_ISO_PROP(img_report.has_deep_directories, "  Has a Rock Ridge deep directory");
	PRINT_ISO_PROP(HAS_SYSLINUX(img_report), "  Uses: Syslinux/Isolinux v%s", img_report.sl_version_str);
	if (HAS_SYSLINUX(img_report) && (SL_MAJOR(img_report.sl_version) < 5)) {
		for (i = 0; i < NB_OLD_C32; i++) {
			PRINT_ISO_PROP(img_report.has_old_c32[i], "    With an old %s", old_c32_name[i]);
		}
	}
	PRINT_ISO_PROP(HAS_KOLIBRIOS(img_report), "  Uses: KolibriOS");
	PRINT_ISO_PROP(HAS_REACTOS(img_report), "  Uses: ReactOS");
	PRINT_ISO_PROP(img_report.has_grub4dos, "  Uses: Grub4DOS");
	PRINT_ISO_PROP(img_report.has_grub2, "  Uses: GRUB2 (%s)", img_report.grub2_version);
	if (img_report.has_efi == 0x80)
		uprintf("  Uses: EFI (through '%s')", img_report.efi_img_path);
	else
		PRINT_ISO_PROP(img_report.has_efi, "  Uses: EFI %s", HAS_WIN7_EFI(img_report) ? "(win7_x64)" : "");
	PRINT_ISO_PROP(HAS_BOOTMGR(img_report), "  Uses: Bootmgr (%s)",
		HAS_BOOTMGR_BIOS(img_report) ? (HAS_BOOTMGR_EFI(img_report) ? "BIOS and UEFI" : "BIOS only") : "UEFI only");
	PRINT_ISO_PROP(HAS_WINPE(img_report), "  Uses: WinPE %s", (img_report.uses_minint) ? "(with /minint)" : "");
	if (HAS_WININST(img_report)) {
		inst_str[4] = '0' + img_report.wininst_index;
		assert(strlen(img_report.wininst_path[0]) >= 3);
		uprintf("  Uses: Install.%s%s (version %d.%d.%d%s)", &img_report.wininst_path[0][strlen(img_report.wininst_path[0]) - 3],
			(img_report.wininst_index > 1) ? inst_str : "", (img_report.wininst_version >> 24) & 0xff,
			(img_report.wininst_version >> 16) & 0xff, (img_report.wininst_version >> 8) & 0xff,
			(img_report.wininst_version >= SPECIAL_WIM_VERSION) ? "+": "");
	}
	if (img_report.needs_ntfs) {
		uprintf("  Note: This ISO uses symbolic links and was not designed to work without them.\n"
			"  Because of this, only NTFS will be allowed as the target file system.");
	} else {
		PRINT_ISO_PROP(img_report.has_symlinks,
			"  Note: This ISO uses symbolic links, which may not be replicated due to file system");
		PRINT_ISO_PROP((img_report.has_symlinks == SYMLINKS_RR),
			"  limitations. Because of this, some features from this image may not work...");
		PRINT_ISO_PROP((img_report.has_symlinks == SYMLINKS_UDF),
			"  limitations. Because of this, the size required for the target media may be much\n"
			"  larger than size of the ISO...");
	}
}

/// <summary>
/// Parse a PE executable file and return its CPU architecture.
/// </summary>
/// <param name="path">The path of the PE executable to parse.</param>
/// <returns>An enum ArchType value (as defined in rufus.h)</returns>
static uint8_t FindArch(const char* path)
{
	uint8_t ret = ARCH_UNKNOWN, *buf = NULL;
	uint32_t size;
	PIMAGE_DOS_HEADER pImageDOSHeader;
	// NB: The field we are after is at the same location for 32 and 64-bit
	// PE headers, so we don't need to care about using PIMAGE_NT_HEADERS[32|64]
	PIMAGE_NT_HEADERS pImageNTHeader;

	// NB: Upstream maps the file
	size = read_file(path, &buf);
	if (size < sizeof(IMAGE_DOS_HEADER)) {
		uprintf("FindArch: Could not read file '%s'", path);
		goto out;
	}
	pImageDOSHeader = (PIMAGE_DOS_HEADER)buf;
	if (pImageDOSHeader->e_magic != IMAGE_DOS_SIGNATURE) {
		uprintf("FindArch: DOS header not found");
		goto out;
	}
	// NB: Upstream does not check that the NT header is inside the file
	if ((pImageDOSHeader->e_lfanew < 0) || ((uint64_t)pImageDOSHeader->e_lfanew +
		offsetof(IMAGE_NT_HEADERS, FileHeader) + sizeof(IMAGE_FILE_HEADER) > size)) {
		uprintf("FindArch: NT header not found");
		goto out;
	}
	pImageNTHeader = (PIMAGE_NT_HEADERS)&buf[pImageDOSHeader->e_lfanew];
	if (pImageNTHeader->Signature != IMAGE_NT_SIGNATURE) {
		uprintf("FindArch: NT header not found");
		goto out;
	}

	ret = MachineToArch(pImageNTHeader->FileHeader.Machine);

out:
	free(buf);
	// NB: Upstream asserts that the file is a PE of a known architecture
	return ret;
}

void GetBootladerInfo(void)
{
	static const char* revocation_type[] = { "UEFI DBX", "Windows SSP", "Linux SBAT", "Windows SVN", "Cert DBX" };
	int r;
	BOOL sb_signed;
	uint32_t i, len;
	uint8_t* buf = NULL;

	// Check UEFI bootloaders for revocation
	if (!IS_EFI_BOOTABLE(img_report))
		return;

	assert(ARRAYSIZE(img_report.efi_boot_entry) > 0);
	PrintStatus(0, MSG_351);
	uprintf("UEFI bootloaders analysis:");
	for (i = 0; i < ARRAYSIZE(img_report.efi_boot_entry) && img_report.efi_boot_entry[i].path[0] != 0; i++) {
		len = ReadISOFileToBuffer(image_path, img_report.efi_boot_entry[i].path, &buf);
		if (len == 0) {
			uprintf("  Warning: Failed to extract '%s' to check for UEFI Secure Boot info", img_report.efi_boot_entry[i].path);
			continue;
		}
		sb_signed = IsSignedBySecureBootAuthority(buf, len);
		if (sb_signed)
			img_report.has_secureboot_bootloader |= 1;
		uprintf("  • %s%s", img_report.efi_boot_entry[i].path, sb_signed ? "*" : "");
		r = IsBootloaderRevoked(buf, len);
		if (r > 0) {
			assert(r <= ARRAYSIZE(revocation_type));
			assert(r <= 7);
			uprintf("  WARNING: '%s' has been revoked by %s", img_report.efi_boot_entry[i].path, revocation_type[r - 1]);
			img_report.has_secureboot_bootloader |= 1 << r;
		}
		safe_free(buf);
	}
}

enum rules_scan_result ImageScan(BOOL keep_image_name)
{
	// Regexp patterns used to match ISO labels for the Red Hat 8 derivatives
	// where we should apply an inst.stage2 ➔ inst.repo workaround for ISO
	// mode (per: https://github.com/rhinstaller/anaconda/pull/3529).
	const char* redhat8_derivative[] = {
		"^AlmaLinux-[8-9].*",		// AlmaLinux 8.x and 9.x
		"^Fedora.*-3[3-9].*",		// Fedora 33-39
		"^CentOS.*-[8-9].*",		// CentOS and CentOS Stream 8.x and 9.x
		"^Circle.*-[8-9].*",		// Circle Linux 8.x.and 9.x
		"^OL-[8-9].*",				// Oracle Linux 8.x and 9.x
		"^RHEL-[8-9].*",			// Red Hat 8.x and 9.x
		"^Rocky-[8-9].*",			// Rocky Linux 8.x and 9.x
		"^MIRACLE-LINUX-[8-9].*",	// MIRACLE LINUX 8.x and 9.x
	};
	// As expected, SUSE maintainers DO NOT care about making the lives of SUSE users easier
	// by applying simple changes. On the other hand, boy are they caring about making their
	// *own* lives easier, and angrily dismissing anything that dares challenges that...
	// See: https://bugzilla.suse.com/show_bug.cgi?id=1243236.
	const char* suse_derivative[] = {
		"Install-SUSE",
		"Install-LEAP",
		"openSUSE-Tumbleweed"
	};
	int i, fd;
	uint8_t arch;
	char tmp_path[MAX_PATH], tmp_str[64];
	const char* matches[REGEX_VM_MAX_MATCHES];
	cregex_node_t* node = NULL;
	cregex_program_t* program = NULL;
	enum rules_scan_result ret = RULES_SCAN_FAILED;

	// NB: Upstream ignores the device notifications during the scan, as mounting an ISO
	// to look up the Windows version produced some. Nothing gets mounted here.
	if (image_path == NULL)
		goto out;
	PrintInfoDebug(0, MSG_202);
	memset(&img_report, 0, sizeof(img_report));
	img_report.is_iso = (BOOLEAN)ExtractISO(image_path, "", TRUE);
	img_report.is_bootable_img = IsBootableImage(image_path);
	if (img_report.wininst_index > 0 || img_report.is_windows_img)
		PopulateWindowsVersion();

	if ((ErrorStatus == RUFUS_ERROR(ERROR_CANCELLED)) || (img_report.image_size == 0) ||
		(!img_report.is_iso && (img_report.is_bootable_img <= 0) && !img_report.is_windows_img)) {
		// Failed to scan image
		if (img_report.is_bootable_img < 0)
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_042), lmprintf(MSG_322, image_path));
		else
			Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_081), lmprintf(MSG_082));
		safe_free(image_path);
		short_image_path = NULL;
		ui_event(UM_PROGRESS_EXIT, 0);
		PrintInfoDebug(0, MSG_203);
		PrintStatus(0, MSG_203);
		goto out;
	}

	if (img_report.is_windows_img) {
		static_sprintf(tmp_path, "%s/%s-XXXXXX", temp_dir, APPLICATION_NAME);
		fd = mkstemp(tmp_path);
		if (fd >= 0) {
			close(fd);
			// Only look at index 1 for now. If people complain, we may look for more.
			if (WimExtractFile(image_path, 1, "Windows\\Boot\\EFI\\bootmgr.efi", tmp_path)) {
				arch = FindArch(tmp_path);
				if (arch != 0) {
					uprintf("  Image contains a%s %s EFI boot manager",
						(arch == ARCH_RISCV_64) ? "" : "n", arch_name[arch]);
					// NB: Upstream sets bit 'arch' rather than bit 'arch + 1' as iso.c does, so that
					// the boot check took an x86_64 image for an x86_32 one (and so on)
					img_report.has_efi = 1 | (2 << arch);
					img_report.has_bootmgr_efi = TRUE;
					img_report.wininst_index = 1;
				} else {
					uprintf("  Image does not contain an EFI boot manager");
				}
			}
			unlink(tmp_path);
		} else {
			uprintf("Could not create a temporary file in '%s': %s", temp_dir, strerror(errno));
		}
		tmp_str[0] = 0;
		if (img_report.win_version.major != 0) {
			if (img_report.win_version.minor == 0)
				static_sprintf(tmp_str, " %d (Build %d.%d)", img_report.win_version.major,
					img_report.win_version.build, img_report.win_version.revision);
			else
				static_sprintf(tmp_str, " %d.%d (Build %d.%d)", img_report.win_version.major,
					img_report.win_version.minor, img_report.win_version.build, img_report.win_version.revision);
		}
		uprintf("  Image is a %sUEFI bootable Windows%s installation image", img_report.has_efi ? "" : "NON-", tmp_str);
	} else if (IS_DD_BOOTABLE(img_report)) {
		if (img_report.is_bootable_img == 2)
			uprintf("  Image is a FORCED non-bootable image");
		else
			uprintf("  Image is a %sbootable %s image",
				(img_report.compression_type != BLED_COMPRESSION_NONE && img_report.compression_type < BLED_COMPRESSION_MAX) ?
				"compressed " : "", img_report.is_vhd ? "VHD" : "disk");
		if (img_report.projected_size > 0)
			uprintf("  Size: %s (Projected)", SizeToHumanReadable(img_report.projected_size, FALSE, FALSE));
	}

	if (img_report.is_iso) {
		GetBootladerInfo();
		DisplayISOProps();

		for (i = 0; i < ARRAYSIZE(redhat8_derivative) && !img_report.rh8_derivative; i++) {
			node = cregex_parse(redhat8_derivative[i]);
			if (node != NULL) {
				program = cregex_compile_node(node);
				cregex_parse_free(node);
			}
			if (node == NULL || program == NULL) {
				uprintf("Internal error: Failed to parse '%s'", redhat8_derivative[i]);
				break;
			}
			if (cregex_program_run(program, img_report.label, matches, ARRAYSIZE(matches)) > 0)
				img_report.rh8_derivative = TRUE;
			cregex_compile_free(program);
			program = NULL;
		}

		for (i = 0; i < ARRAYSIZE(suse_derivative); i++) {
			if (strncmp(img_report.label, suse_derivative[i], strlen(suse_derivative[i])) == 0) {
				img_report.disable_iso = TRUE;
				break;
			}
		}

		// If we have EFI GRUB, detect whether we have FAT, exFAT or NTFS support (with exFAT
		// or NTFS support being mandatory if we have a file that is larger than 4 GB)
		if (img_report.has_grub2 & 0x80) {
			if (!(img_report.has_grub2_fs & 0x7) || (img_report.has_4GB_file && !(img_report.has_grub2_fs & 0x6)))
				img_report.disable_iso = TRUE;
		}

		// If we have an ISOHybrid, but without an ISO method we support, disable ISO support altogether
		if (IS_DD_BOOTABLE(img_report) && (img_report.disable_iso ||
				(!IS_BIOS_BOOTABLE(img_report) && !IS_EFI_BOOTABLE(img_report)))) {
			Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_274, "ISOHybrid"), lmprintf(MSG_321));
			uprintf("Note: DD image mode enforced since this ISOHybrid is not ISO mode compatible.");
			img_report.is_iso = FALSE;
		}
	}
	if (!IS_DD_BOOTABLE(img_report) && !IS_BIOS_BOOTABLE(img_report) && !IS_EFI_BOOTABLE(img_report)) {
		// No boot method that we support
		PrintInfo(0, MSG_081);
		safe_free(image_path);
		short_image_path = NULL;
		Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_081), lmprintf(MSG_082));
		PrintStatus(0, MSG_086);
		ret = RULES_SCAN_UNSUPPORTED;
	} else {
		if (!keep_image_name) {
			// NB: Upstream looks for the last backslash
			short_image_path = strrchr(image_path, '/');
			short_image_path = (short_image_path == NULL) ? image_path : &short_image_path[1];
			PrintStatus(0, MSG_205, short_image_path);
			uprintf("Using image: %s (%s)", short_image_path, SizeToHumanReadable(img_report.image_size, FALSE, FALSE));
		}
		ret = RULES_SCAN_OK;
	}

out:
	PrintInfo(0, MSG_210);
	return ret;
}

static void* ImageScanThread(void* param)
{
	enum rules_scan_result r = ImageScan((BOOL)(intptr_t)param);

	ui_event(UM_IMAGE_SCAN_COMPLETED, (intptr_t)r);
	return NULL;
}

BOOL StartImageScan(BOOL keep_image_name)
{
	pthread_t thread;
	pthread_attr_t attr;
	int r;

	// As the IDC_SELECT handler does
	ErrorStatus = 0;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	r = pthread_create(&thread, &attr, ImageScanThread, (void*)(intptr_t)keep_image_name);
	pthread_attr_destroy(&attr);
	if (r != 0) {
		uprintf("Unable to start ISO scanning thread: %s", strerror(r));
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		return FALSE;
	}
	return TRUE;
}

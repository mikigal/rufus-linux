/*
 * Rufus: The Reliable USB Formatting Utility
 * Format thread
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
 * FormatThread() of format.c, on the Linux backend. The steps are the ones of
 * upstream, without the drive letters, VDS and volume locks:
 * 1. hide the drive from udisks, unmount it, and open it exclusively;
 * 2. zero it, or write an image to it, or clear its partition tables;
 * 3. partition it, and reread its partitions;
 * 4. release it and format its partitions (the mkfs tools open them
 *    exclusively), the persistence one first;
 * 5. take it back, and write its MBR, SBR and the PBR of the main partition;
 * 6. release it, mount the main partition, and copy the files (ISO
 *    extraction, Grub4DOS, extended label, extra archive);
 * 7. unmount it, and let udisks see the drive again.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "settings.h"
#include "progress.h"
#include "partition.h"
#include "boot_records.h"
#include "write_drive.h"
#include "format_thread.h"
#include "filesystems.h"
#include "disk.h"
#include "vhd.h"
#include "wue.h"
#include "badblocks.h"
#include "resource.h"
#include "localization.h"
#include "../res/grub/grub_version.h"

extern const char* md5sum_name[2];

// The file system type that the kernel mounts a volume with
static const char* MountType(int fs_type)
{
	switch (fs_type) {
	case FS_FAT16:
	case FS_FAT32: return "vfat";
	case FS_NTFS: return "ntfs3";
	case FS_EXFAT: return "exfat";
	case FS_UDF: return "udf";
	case FS_EXT2: return "ext2";
	case FS_EXT3: return "ext3";
	case FS_EXT4: return "ext4";
	default: return NULL;
	}
}

// Report a step that is not ported yet as an error, rather than silently producing a drive that doesn't work
static BOOL NotSupportedYet(const char* what, DWORD error)
{
	uprintf("%s is not supported on Linux yet", what);
	if (!IS_ERROR(ErrorStatus))
		ErrorStatus = RUFUS_ERROR(error);
	return FALSE;
}

// The local time, in the format of the reports of upstream
static void LocalTime(char* str, size_t len, const char* format)
{
	struct tm lt;
	time_t now = time(NULL);

	localtime_r(&now, &lt);
	strftime(str, len, format, &lt);
}

/*
 * The bad blocks check, with its report in target->bb_log_fd, or in a file of
 * temp_dir. The drive gets O_DIRECT for the check, and its MBR/GPT cleared after.
 */
static BOOL CheckBadBlocks(const format_target_t* target, drive_t* drive, uint32_t sector_size)
{
	badblocks_report report = { 0 };
	char logfile[MAX_PATH], date[32];
	const char* bb_msg;
	FILE* log_fd;
	int sel = opts.nb_passes_sel, flags, fd, r;
	BOOL ret = FALSE;

	flags = fcntl(drive->fd, F_GETFL);
	if ((flags < 0) || (fcntl(drive->fd, F_SETFL, flags | O_DIRECT) != 0)) {
		uprintf("Could not access the drive directly: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	do {
		if (target->bb_log_fd >= 0) {
			static_strcpy(logfile, (target->bb_log_path != NULL) ? target->bb_log_path : "");
			fd = dup(target->bb_log_fd);
			log_fd = (fd < 0) ? NULL : fdopen(fd, "w+");
			if ((log_fd == NULL) && (fd >= 0))
				close(fd);
			if ((log_fd != NULL) && (ftruncate(fileno(log_fd), 0) != 0))
				uprintf("Could not truncate the bad blocks report: %s", WindowsErrorString());
		} else {
			LocalTime(date, sizeof(date), "%Y%m%d_%H%M%S");
			static_sprintf(logfile, "%s/rufus_%s.log", temp_dir, date);
			log_fd = fopen(logfile, "w+e");
		}
		if (log_fd == NULL) {
			uprintf("Error: Could not create log file for bad blocks check");
			goto out;
		}
		LocalTime(date, sizeof(date), "%Y.%m.%d %H:%M:%S");
		fprintf(log_fd, APPLICATION_NAME " bad blocks check started on: %s", date);
		fflush(log_fd);

		if (!BadBlocks(drive->fd, drive->size, (sel >= 2) ? 4 : sel +1, sel, &report, log_fd)) {
			uprintf("Bad blocks: Check failed.");
			if (!IS_ERROR(ErrorStatus))
				ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BADBLOCKS_FAILURE));
			fcntl(drive->fd, F_SETFL, flags);
			ClearMBRGPT(drive->fd, drive->size, sector_size);
			fclose(log_fd);
			if (target->bb_log_fd < 0)
				unlink(logfile);
			goto out;
		}
		uprintf("Bad Blocks: Check completed, %d bad block%s found. (%d/%d/%d errors)",
			report.bb_count, (report.bb_count==1)?"":"s",
			report.num_read_errors, report.num_write_errors, report.num_corruption_errors);
		r = IDOK;
		if (report.bb_count) {
			bb_msg = lmprintf(MSG_011, report.bb_count, report.num_read_errors, report.num_write_errors,
				report.num_corruption_errors);
			fprintf(log_fd, "%s", bb_msg);
			LocalTime(date, sizeof(date), "%Y.%m.%d %H:%M:%S");
			fprintf(log_fd, APPLICATION_NAME " bad blocks check ended on: %s", date);
			fclose(log_fd);
			r = Notification(MB_ABORTRETRYIGNORE | MB_ICONWARNING, lmprintf(MSG_010), lmprintf(MSG_012, bb_msg, logfile));
		} else {
			// We didn't get any errors => delete the log file
			if (target->bb_log_fd >= 0)
				IGNORE_RETVAL(ftruncate(fileno(log_fd), 0));
			fclose(log_fd);
			if (target->bb_log_fd < 0)
				unlink(logfile);
		}
	} while (r == IDRETRY);
	if (r == IDABORT) {
		ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
		goto out;
	}
	ret = TRUE;

out:
	if (fcntl(drive->fd, F_SETFL, flags) != 0)
		uprintf("Could not restore the access to the drive: %s", WindowsErrorString());
	// Especially after destructive badblocks test, you must zero the MBR/GPT completely
	// before repartitioning. Else, all kind of bad things happen.
	if (ret && !ClearMBRGPT(drive->fd, drive->size, sector_size)) {
		uprintf("Could not zero MBR/GPT");
		if (!IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		ret = FALSE;
	}
	return ret;
}

BOOL FormatDrive(const format_target_t* target)
{
	BOOL windows_to_go, install_syslinux = FALSE, ret = FALSE;
	uint8_t extra_partitions = 0;
	uint32_t ClusterSize, sector_size;
	uint64_t main_offset = 0, main_size = 0;
	DWORD Flags;
	char label[64], main_dev[32], casper_dev[32], path[MAX_PATH], src[MAX_PATH];
	char* mount_point = NULL;
	uint8_t* syslinux_bootsect = NULL;
	partition_layout_t layout = { 0 };
	drive_t drive = { .fd = -1 };
	struct stat st;

	windows_to_go = (opts.image_options & IMOP_WINTOGO) && (opts.boot_type == BT_IMAGE) && HAS_WINTOGO(img_report) &&
		(opts.image_option == IMOP_WIN_TO_GO);
	large_drive = (opts.disk_size > (1*TB));
	if (large_drive)
		uprintf("Notice: Large drive detected (may produce short writes)");
	if (windows_to_go) {
		NotSupportedYet("Windows To Go", ERROR_NOT_SUPPORTED);
		goto out;
	}
	InitProgress((opts.boot_type == BT_IMAGE) && opts.write_as_image);

	// Find out if we need to add any extra partitions
	extra_partitions = 0;
	if ((opts.boot_type == BT_IMAGE) && !opts.write_as_image && HAS_PERSISTENCE(img_report) && opts.persistence_size)
		extra_partitions |= XP_PERSISTENCE;
	// According to Microsoft, every GPT disk (we RUN Windows from) must have an MSR due to not having hidden sectors
	// https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/windows-and-gpt-faq#disks-that-require-an-msr
	if ((windows_to_go) && (opts.target_type == TT_UEFI) && (opts.partition_type == PARTITION_STYLE_GPT))
		extra_partitions |= XP_ESP | XP_MSR;
	// If we have a bootable image with UEFI bootloaders and the target file system is NTFS or exFAT
	// or the UEFI:NTFS option is selected, we add the UEFI:NTFS partition...
	else if ((((opts.boot_type == BT_IMAGE) && IS_EFI_BOOTABLE(img_report)) && ((opts.fs_type == FS_NTFS) || (opts.fs_type == FS_EXFAT))) ||
			 (opts.boot_type == BT_UEFI_NTFS)) {
		extra_partitions |= XP_UEFI_NTFS;
		// ...but only if we're not dealing with a Windows image in installer mode with target
		//  system set to BIOS and without dual BIOS+UEFI boot enabled.
		if ((opts.boot_type == BT_IMAGE) && HAS_BOOTMGR_BIOS(img_report) && (!windows_to_go) &&
			(opts.target_type == TT_BIOS) && (!opts.allow_dual_uefi_bios))
			extra_partitions &= ~XP_UEFI_NTFS;
	}
	if (opts.old_bios_fixes)
		extra_partitions |= XP_COMPAT;

	// 1. Hide the drive from udisks, unmount it, and get exclusive access to it
	PrintInfoDebug(0, MSG_225);
	if ((stat(target->devnode, &st) != 0) || !S_ISBLK(st.st_mode)) {
		uprintf("%s is not a drive", target->devnode);
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}
	if (!SuppressAutomount(target->syspath))
		uprintf("WARNING: Could not disable automounting - the desktop may display the drive");
	if (!UnmountDrive(st.st_rdev))
		goto out;
	// Our own mounts go to a mount namespace that the desktop doesn't see (which requires a single thread)
	if (target->private_mounts && !EnterPrivateMountNamespace()) {
		ErrorStatus = RUFUS_ERROR(ERROR_ACCESS_DENIED);
		goto out;
	}
	if (!OpenDrive(target->devnode, target->diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive)) {
		if (!IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}
	sector_size = drive.sector_size;
	CHECK_FOR_USER_CANCEL;

	if (!opts.zero_drive && !opts.write_as_image) {
		PrintInfoDebug(0, MSG_226);
		AnalyzeMBR(drive.fd, sector_size, "Drive", FALSE);
		UpdateProgress(OP_ANALYZE_MBR, -1.0f);
	}

	// 2. Zero the drive, or write an image to it, or clear its partition tables
	if (opts.zero_drive) {
		ret = WriteDrive(drive.fd, drive.size, sector_size, -1, TRUE);
		goto out;
	}

	// Zap partition records. This may help prevent access errors.
	if ((opts.boot_type != BT_IMAGE) || (img_report.is_iso && !opts.write_as_image)) {
		// NB: Upstream also resets the disk with IOCTL_DISK_CREATE_DISK, which rereading the partitions does here
		if (!ClearMBRGPT(drive.fd, drive.size, sector_size) || !RefreshPartitions(&drive)) {
			uprintf("Could not reset partitions");
			ErrorStatus = (LastWriteError != 0) ? LastWriteError : RUFUS_ERROR(ERROR_PARTITION_FAILURE);
			goto out;
		}
	}

	if (opts.bad_blocks) {
		if (!CheckBadBlocks(target, &drive, sector_size))
			goto out;
	}

	// Write an image file
	if ((opts.boot_type == BT_IMAGE) && opts.write_as_image) {
		if (target->image_fd < 0) {
			uprintf("No image to write");
			ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
			goto out;
		}
		ret = WriteDrive(drive.fd, drive.size, sector_size, target->image_fd, FALSE) && RefreshPartitions(&drive);
		goto out;
	}

	UpdateProgress(OP_ZERO_MBR, -1.0f);
	CHECK_FOR_USER_CANCEL;

	// 3. Partition the drive
	if (!PlanPartitions(&layout, opts.partition_type, opts.fs_type, (opts.partition_type == PARTITION_STYLE_MBR)
		&& (opts.target_type == TT_UEFI), extra_partitions, drive.size, sector_size) ||
		!WritePartitionTable(drive.fd, &layout) || !RefreshPartitions(&drive)) {
		ErrorStatus = (LastWriteError != 0) ? LastWriteError : RUFUS_ERROR(ERROR_PARTITION_FAILURE);
		goto out;
	}
	UpdateProgress(OP_PARTITION, -1.0f);
	if (layout.count == 0) {
		// A super floppy: the volume is the whole drive
		static_strcpy(main_dev, target->devnode);
		main_size = drive.size;
	} else {
		main_offset = layout.part[layout.index[PI_MAIN]].offset;
		main_size = layout.part[layout.index[PI_MAIN]].size;
		if (!GetPartitionDevnode(&drive, layout.index[PI_MAIN] + 1, main_dev, sizeof(main_dev))) {
			uprintf("Logical drive was not found - aborting");
			ErrorStatus = RUFUS_ERROR(ERROR_TIMEOUT);
			goto out;
		}
	}

	// 4. Format the partitions, which the mkfs tools open exclusively
	CloseDrive(&drive);
	WaitForUdev(DRIVE_ACCESS_TIMEOUT);
	CHECK_FOR_USER_CANCEL;

	// Format Casper partition if required. Do it before we format anything with
	// a file system that Windows will recognize, to avoid concurrent access.
	if (extra_partitions & XP_PERSISTENCE) {
		uint32_t ext_version = ReadSetting32(SETTING_USE_EXT_VERSION);
		if ((ext_version < 2) || (ext_version > 4))
			ext_version = 3;
		uprintf("Using %s-like method to enable persistence", img_report.uses_casper ? "Ubuntu" : "Debian");
		if (!GetPartitionDevnode(&drive, layout.index[PI_CASPER] + 1, casper_dev, sizeof(casper_dev)) ||
			!FormatPartition(casper_dev, layout.part[layout.index[PI_CASPER]].offset, layout.part[layout.index[PI_CASPER]].size,
			sector_size, 0, FS_EXT2 + (ext_version - 2), img_report.uses_casper ? "casper-rw" : "persistence",
			(img_report.uses_casper ? 0 : FP_CREATE_PERSISTENCE_CONF) | (opts.quick_format ? FP_QUICK : 0))) {
			if (!IS_ERROR(ErrorStatus))
				ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
			goto out;
		}
	}

	static_strcpy(label, opts.label);
	// Append a " (SILENT)" suffix to the label for fully unattended silent installation media.
	if (opts.append_silent && strstr(label, " (SILENT)") == NULL)
		static_strcat(label, " (SILENT)");
	if (opts.fs_type < FS_EXT2)
		ToValidLabel(label, (opts.fs_type == FS_FAT16) || (opts.fs_type == FS_FAT32) || (opts.fs_type == FS_EXFAT), drive.size);
	else
		// NB: Upstream reads the label of ext volumes back from the volume
		static_strcpy(img_report.usb_label, label);
	ClusterSize = (uint32_t)opts.cluster_size;
	if ((ClusterSize < 0x200) || (opts.write_as_esp))
		ClusterSize = 0;	// 0 = default cluster size

	Flags = FP_FORCE;
	if (opts.quick_format)
		Flags |= FP_QUICK;
	if ((opts.fs_type == FS_NTFS) && (opts.enable_ntfs_compression))
		Flags |= FP_COMPRESSION;
	if (opts.write_as_esp)
		Flags |= FP_LARGE_FAT32;

	if (!FormatPartition(main_dev, main_offset, main_size, sector_size, ClusterSize, opts.fs_type, label, Flags)) {
		// Error will be set by FormatPartition() in ErrorStatus
		uprintf("Format error: %s", StrError(ErrorStatus, TRUE));
		goto out;
	}
	WaitForUdev(DRIVE_ACCESS_TIMEOUT);

	// 5. The MBR, SBR and PBR, through the drive, while the volume is not mounted
	if (!OpenDrive(target->devnode, target->diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive)) {
		if (!IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}
	// Thanks to Microsoft, we must fix the MBR AFTER the drive has been formatted
	if ((layout.count != 0) && ((opts.partition_type == PARTITION_STYLE_MBR) ||
		((opts.boot_type != BT_NON_BOOTABLE) && (opts.partition_type == PARTITION_STYLE_GPT)))) {
		PrintInfoDebug(0, MSG_228);	// "Writing master boot record..."
		if ((!WriteMBR(drive.fd, sector_size)) || (!WriteSBR(drive.fd, sector_size, layout.part[0].offset))) {
			if (!IS_ERROR(ErrorStatus))
				ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
			goto out;
		}
		UpdateProgress(OP_FIX_MBR, -1.0f);
	}

	if (opts.boot_type != BT_NON_BOOTABLE) {
		if (opts.boot_type == BT_UEFI_NTFS) {
			// All good
		} else if (opts.target_type == TT_UEFI) {
			// For once, no need to do anything - just check our sanity
			if_assert_fails((opts.boot_type == BT_IMAGE) && IS_EFI_BOOTABLE(img_report) && (opts.fs_type <= FS_NTFS)) {
				ErrorStatus = RUFUS_ERROR(ERROR_INSTALL_FAILURE);
				goto out;
			}
		} else if ( (opts.boot_type == BT_SYSLINUX_V4) || (opts.boot_type == BT_SYSLINUX_V6) ||
			((opts.boot_type == BT_IMAGE) && (HAS_SYSLINUX(img_report) || HAS_REACTOS(img_report)) &&
				(!HAS_WINDOWS(img_report) || !opts.allow_dual_uefi_bios)) ) {
			// NB: Syslinux is installed through the mounted volume, and its boot sector written once unmounted
			install_syslinux = TRUE;
		} else {
			PrintInfoDebug(0, MSG_229);
			if (!WritePBR(drive.fd, main_offset, sector_size, opts.fs_type)) {
				if (!IS_ERROR(ErrorStatus))
					ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
				goto out;
			}
		}
	}
	CloseDrive(&drive);
	WaitForUdev(DRIVE_ACCESS_TIMEOUT);
	CHECK_FOR_USER_CANCEL;

	// 6. The files
	mount_point = MountVolume(main_dev, MountType(opts.fs_type), FALSE);
	if (mount_point == NULL) {
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_MOUNT_VOLUME));
		goto out;
	}
	if (install_syslinux) {
		syslinux_bootsect = calloc(1, sector_size);
		if ((syslinux_bootsect == NULL) ||
			!InstallSyslinux(main_dev, mount_point, opts.fs_type, sector_size, syslinux_bootsect)) {
			ErrorStatus = RUFUS_ERROR(ERROR_INSTALL_FAILURE);
			goto out;
		}
	}
	if (opts.boot_type == BT_NON_BOOTABLE) {
		if (opts.extended_label)
			SetAutorun(mount_point);
	} else {
		if ((opts.boot_type == BT_MSDOS) || (opts.boot_type == BT_FREEDOS)) {
			UpdateProgress(OP_FILE_COPY, -1.0f);
			PrintInfoDebug(0, MSG_230);
			if (!ExtractDOS(mount_point)) {
				if (!IS_ERROR(ErrorStatus))
					ErrorStatus = RUFUS_ERROR(ERROR_CANNOT_COPY);
				goto out;
			}
		} else if (opts.boot_type == BT_GRUB4DOS) {
			static_sprintf(src, "%s/%s/grub4dos-%s/grldr", app_data_dir, FILES_DIR, GRUB4DOS_VERSION);
			static_sprintf(path, "%s/grldr", mount_point);
			uprintf("Installing: %s (Grub4DOS loader) %s", path, IsFileInDB(src) ? "✓" : "✗");
			if (!copy_file(src, path, FALSE))
				uprintf("Failed to copy file: %s", WindowsErrorString());
		} else if ((opts.boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.is_iso || img_report.is_windows_img)) {
			UpdateProgress(OP_FILE_COPY, 0.0f);
			if_assert_fails(!img_report.is_windows_img)
				goto out;
			if (!ExtractISO(image_path, mount_point, FALSE)) {
				if (!IS_ERROR(ErrorStatus))
					ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_EXTRACT));
				goto out;
			}
			if (HAS_KOLIBRIOS(img_report)) {
				static_sprintf(path, "%s/MTLD_F32", mount_point);
				uprintf("Installing: %s (KolibriOS loader)", path);
				if (ExtractISOFile(image_path, "HD_Load/USB_Boot/MTLD_F32", path,
					FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) == 0) {
					uprintf("WARNING: Loader installation failed - KolibriOS will not boot!");
				}
			}
			// EFI mode selected, with no 'boot###.efi' but Windows 7 x64's 'bootmgr.efi' (bit #0)
			if (((opts.target_type == TT_UEFI) || opts.allow_dual_uefi_bios) && HAS_WIN7_EFI(img_report)) {
				PrintInfo(0, MSG_232, lmprintf(MSG_307));
				uprintf("Win7 EFI boot setup");
				// NB: Upstream changes the drive letter of wininst_path[0], which is relative to the root here
				static_sprintf(src, "%s%s", mount_point, img_report.wininst_path[0]);
				static_sprintf(path, "%s/efi/boot", mount_point);
				if (mkdir_p(path, 0755) != 0) {
					uprintf("Could not create directory '%s': %s", path, WindowsErrorString());
					ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
				} else {
					static_strcat(path, "/bootx64.efi");
					if (!WimExtractFile(src, 1, "Windows\\Boot\\EFI\\bootmgfw.efi", path)) {
						uprintf("Failed to setup Win7 EFI boot");
						ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
					}
				}
			}
			if ((opts.target_type == TT_BIOS) && HAS_WINPE(img_report)) {
				// Apply WinPE fixup
				if (!SetupWinPE(mount_point))
					ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
			}
			if (unattend_xml_path != NULL) {
				if (!ApplyWindowsCustomization(mount_point, opts.unattend_xml_flags))
					ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
			}
		}

		UpdateProgress(OP_FINALIZE, -1.0f);
		PrintInfoDebug(0, MSG_233);
		if ((opts.boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.is_iso))
			UpdateMD5Sum(mount_point, md5sum_name[img_report.has_md5sum ? img_report.has_md5sum - 1 : 0]);
		if (opts.extended_label)
			SetAutorun(mount_point);
		// NB: Upstream runs CheckDisk on NTFS, for WinPE/AIK images
	}

	// Copy any additonal files from an optional zip archive selected by the user
	if (!IS_ERROR(ErrorStatus)) {
		UpdateProgress(OP_EXTRACT_ZIP, 0.0f);
		if (archive_path != NULL && opts.fs_type < FS_EXT2 && !ExtractZip(archive_path, mount_point) && !IS_ERROR(ErrorStatus))
			uprintf("WARNING: Could not copy additional files");
	}
	ret = !IS_ERROR(ErrorStatus);

out:
	if (mount_point != NULL) {
		// 7. The data must be on the drive before the drive is released
		if (!UnmountVolume(mount_point) && !IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		free(mount_point);
		mount_point = NULL;
		// And the boot sector of Syslinux, once the volume is no longer mounted
		if (ret && (syslinux_bootsect != NULL)) {
			WaitForUdev(DRIVE_ACCESS_TIMEOUT);
			if (!OpenDrive(target->devnode, target->diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive) ||
				!WriteSyslinuxBootsect(drive.fd, main_offset, sector_size, opts.fs_type, syslinux_bootsect)) {
				if (!IS_ERROR(ErrorStatus))
					ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
				ret = FALSE;
			}
		}
	}
	free(syslinux_bootsect);
	CloseDrive(&drive);
	WaitForUdev(DRIVE_ACCESS_TIMEOUT);
	RemoveAutomountRules();
	if ((opts.boot_type == BT_IMAGE) && opts.write_as_image)
		PrintInfo(0, MSG_320, lmprintf(MSG_307));
	return ret && !IS_ERROR(ErrorStatus);
}

static void* FormatThread(void* param)
{
	format_target_t* target = (format_target_t*)param;

	FormatDrive(target);
	free((char*)target->devnode);
	free((char*)target->syspath);
	free(target);
	ui_event(UM_FORMAT_COMPLETED, (intptr_t)TRUE);
	return NULL;
}

BOOL StartFormat(const format_target_t* target)
{
	pthread_t thread;
	pthread_attr_t attr;
	format_target_t* copy = calloc(1, sizeof(format_target_t));
	int r = ENOMEM;

	if (copy != NULL) {
		*copy = *target;
		// A thread can't have a mount namespace of its own
		copy->private_mounts = FALSE;
		copy->devnode = safe_strdup(target->devnode);
		copy->syspath = safe_strdup(target->syspath);
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		r = pthread_create(&thread, &attr, FormatThread, copy);
		pthread_attr_destroy(&attr);
	}
	if (r != 0) {
		if (copy != NULL) {
			free((char*)copy->devnode);
			free((char*)copy->syspath);
		}
		free(copy);
		uprintf("Unable to start format thread: %s", strerror(r));
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		return FALSE;
	}
	return TRUE;
}

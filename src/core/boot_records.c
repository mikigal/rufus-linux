/*
 * Rufus: The Reliable USB Formatting Utility
 * Boot records
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
 * WriteMBR(), WriteSBR() and WritePBR() of format.c, through ms-sys, on the
 * file descriptor of the drive. On Windows, the PBR is written through the
 * locked, mounted volume; here, it is written through the drive once the
 * volume is unmounted, at the offset of its partition.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rufus.h"
#include "options.h"
#include "boot_records.h"
#include "resource.h"
#include "localization.h"
#include "file.h"
#include "br.h"
#include "fat16.h"
#include "fat32.h"
#include "ntfs.h"
#include "partition_info.h"

BOOL WriteMBR(int fd, uint32_t sector_size)
{
	BOOL r = FALSE;
	BOOL needs_masquerading = HAS_WINPE(img_report) && (!img_report.uses_minint);
	uint8_t* buffer = NULL;
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	const char* using_msg = "Using %s MBR";

	if (sector_size < 512)
		goto out;

	if (opts.partition_type == PARTITION_STYLE_GPT) {
		// Add a notice with a protective MBR
		fake_fd._fd = fd;
		set_bytes_per_sector(sector_size);
		uprintf(using_msg, "Rufus protective");
		r = write_rufus_msg_mbr(fp);
		goto out;
	}

	// FormatEx rewrites the MBR and removes the LBA attribute of FAT16
	// and FAT32 partitions - we need to correct this in the MBR
	buffer = (uint8_t*)_mm_malloc(sector_size, 16);
	if (buffer == NULL) {
		uprintf("Could not allocate memory for MBR");
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
		goto out;
	}

	if (read_sectors(fd, sector_size, 0, 1, buffer) != sector_size) {
		uprintf("Could not read MBR");
		ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
		goto out;
	}

	switch (opts.fs_type) {
	case FS_FAT16:
		if (buffer[0x1c2] == 0x0e) {
			uprintf("Partition is already FAT16 LBA...");
		} else if ((buffer[0x1c2] != 0x04) && (buffer[0x1c2] != 0x06)) {
			uprintf("WARNING: converting a non FAT16 partition to FAT16 LBA: FS type=0x%02x", buffer[0x1c2]);
		}
		buffer[0x1c2] = 0x0e;
		break;
	case FS_FAT32:
		if (buffer[0x1c2] == 0x0c) {
			uprintf("Partition is already FAT32 LBA...");
		} else if (buffer[0x1c2] != 0x0b) {
			uprintf("WARNING: converting a non FAT32 partition to FAT32 LBA: FS type=0x%02x", buffer[0x1c2]);
		}
		buffer[0x1c2] = 0x0c;
		break;
	}
	if ((opts.boot_type != BT_NON_BOOTABLE) && (opts.target_type == TT_BIOS)) {
		// Set first partition bootable or masquerade as second disk
		buffer[0x1be] = needs_masquerading ? 0x81 : 0x80;
		uprintf("Set bootable USB partition as 0x%02X", buffer[0x1be]);
	}

	if (write_sectors(fd, sector_size, 0, 1, buffer) != sector_size) {
		uprintf("Could not write MBR");
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}

	fake_fd._fd = fd;
	set_bytes_per_sector(sector_size);

	// What follows is really a case statement with complex conditions listed
	// by order of preference
	if ((opts.boot_type == BT_IMAGE) && HAS_WINDOWS(img_report) && (opts.allow_dual_uefi_bios) && (opts.target_type == TT_BIOS))
		goto windows_mbr;

	// Non bootable or forced UEFI (zeroed MBR)
	if ((opts.boot_type == BT_NON_BOOTABLE) || (opts.target_type == TT_UEFI)) {
		uprintf(using_msg, "Zeroed");
		r = write_zero_mbr(fp);
		goto out;
	}

	// Syslinux
	if ( (opts.boot_type == BT_SYSLINUX_V4) || (opts.boot_type == BT_SYSLINUX_V6) ||
		 ((opts.boot_type == BT_IMAGE) && HAS_SYSLINUX(img_report)) ) {
		uprintf(using_msg, "Syslinux");
		r = write_syslinux_mbr(fp);
		goto out;
	}

	// Grub 2.0
	if ( ((opts.boot_type == BT_IMAGE) && (img_report.has_grub2)) || (opts.boot_type == BT_GRUB2) ) {
		uprintf(using_msg, "Grub 2.0");
		r = write_grub2_mbr(fp);
		goto out;
	}

	// Grub4DOS
	if ( ((opts.boot_type == BT_IMAGE) && (img_report.has_grub4dos)) || (opts.boot_type == BT_GRUB4DOS) ) {
		uprintf(using_msg, "Grub4DOS");
		r = write_grub4dos_mbr(fp);
		goto out;
	}

	// ReactOS
	if (opts.boot_type == BT_REACTOS) {
		uprintf(using_msg, "ReactOS");
		r = write_reactos_mbr(fp);
		goto out;
	}

	// KolibriOS
	if ( (opts.boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report) && (IS_FAT(opts.fs_type))) {
		uprintf(using_msg, "KolibriOS");
		r = write_kolibrios_mbr(fp);
		goto out;
	}

	// If everything else failed, fall back to a conventional Windows/Rufus MBR
windows_mbr:
	if (needs_masquerading || opts.use_rufus_mbr) {
		uprintf(using_msg, APPLICATION_NAME);
		r = write_rufus_mbr(fp);
	} else {
		uprintf(using_msg, "Windows 7");
		r = write_win7_mbr(fp);
	}
	// NB: Upstream tells the system that the disk properties changed, which the partition table didn't

out:
	safe_mm_free(buffer);
	return r;
}

BOOL WriteSBR(int fd, uint32_t sector_size, uint64_t first_partition_offset)
{
	// TODO: Do we need anything special for 4K sectors?
	DWORD size, max_size, br_size = 0x200;
	int r, sub_type = opts.boot_type;
	uint8_t *buf = NULL;
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;

	fake_fd._fd = fd;
	set_bytes_per_sector(sector_size);
	// Syslinux has precedence over Grub
	if ((opts.boot_type == BT_IMAGE) && (!HAS_SYSLINUX(img_report))) {
		if (img_report.has_grub4dos)
			sub_type = BT_GRUB4DOS;
		if (img_report.has_grub2)
			sub_type = BT_GRUB2;
	}

	// Use BT_MAX for the protective message
	if ((opts.boot_type != BT_NON_BOOTABLE) && (opts.partition_type == PARTITION_STYLE_GPT))
		sub_type = BT_MAX;

	switch (sub_type) {
	case BT_GRUB4DOS:
		uprintf("Writing Grub4Dos SBR");
		buf = GetResource(IDR_GR_GRUB_GRLDR_MBR, "grldr.mbr", &size, FALSE);
		if ((buf == NULL) || (size <= br_size)) {
			uprintf("grldr.mbr is either not present or too small");
			return FALSE;
		}
		buf = &buf[br_size];
		size -= br_size;
		break;
	case BT_GRUB2:
		if (grub2_buf != NULL) {
			uprintf("Writing Grub 2.0 SBR (from download) %s",
				IsBufferInDB(grub2_buf, grub2_len)?"✓":"✗");
			buf = grub2_buf;
			size = (DWORD)grub2_len;
		} else {
			uprintf("Writing Grub 2.0 SBR (from embedded)");
			buf = GetResource(IDR_GR_GRUB2_CORE_IMG, "core.img", &size, FALSE);
			if (buf == NULL) {
				uprintf("Could not access core.img");
				return FALSE;
			}
		}
		break;
	case BT_MAX:
		uprintf("Writing protective message SBR");
		size = 4 * KB;
		br_size = 17 * KB;	// 34 sectors are reserved for protective MBR + primary GPT
		buf = GetResource(IDR_SBR_MSG, "msg.txt", &size, TRUE);
		if (buf == NULL) {
			uprintf("Could not access message");
			return FALSE;
		}
		break;
	default:
		// No need to write secondary block
		return TRUE;
	}

	// Ensure that we have sufficient space for the SBR
	max_size = (DWORD)first_partition_offset;
	if (br_size + size > max_size) {
		uprintf("  SBR size is too large - You may need to uncheck 'Add fixes for old BIOSes'.");
		if (sub_type == BT_MAX)
			safe_free(buf);
		return FALSE;
	}

	r = write_data(fp, br_size, buf, (uint64_t)size);
	safe_free(grub2_buf);
	if (sub_type == BT_MAX)
		safe_free(buf);
	return (r != 0);
}

static __inline const char* bt_to_name(void) {
	switch (opts.boot_type) {
	case BT_FREEDOS: return "FreeDOS";
	case BT_REACTOS: return "ReactOS";
	default:
		return ((opts.boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) ? "KolibriOS" : "Standard";
	}
}

BOOL WritePBR(int fd, uint64_t partition_offset, uint32_t sector_size, int fs_type)
{
	int i;
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	const char* using_msg = "Using %s %s partition boot record";

	fake_fd._fd = fd;
	fake_fd._offset = partition_offset;
	set_bytes_per_sector(sector_size);

	switch (fs_type) {
	case FS_FAT16:
		uprintf(using_msg, bt_to_name(), "FAT16");
		if (!is_fat_16_fs(fp)) {
			uprintf("New volume does not have a FAT16 boot sector - aborting");
			break;
		}
		uprintf("Confirmed new volume has a FAT16 boot sector");
		if (opts.boot_type == BT_FREEDOS) {
			if (!write_fat_16_fd_br(fp, 0)) break;
		} else if (opts.boot_type == BT_REACTOS) {
			if (!write_fat_16_ros_br(fp, 0)) break;
		} else if ((opts.boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) {
			uprintf("FAT16 is not supported for KolibriOS"); break;
		} else {
			if (!write_fat_16_br(fp, 0)) break;
		}
		// Disk Drive ID needs to be corrected on XP
		if (!write_partition_physical_disk_drive_id_fat16(fp))
			break;
		return TRUE;
	case FS_FAT32:
		uprintf(using_msg, bt_to_name(), "FAT32");
		for (i = 0; i < 2; i++) {
			if (!is_fat_32_fs(fp)) {
				uprintf("New volume does not have a %s FAT32 boot sector - aborting", i ? "secondary" : "primary");
				break;
			}
			uprintf("Confirmed new volume has a %s FAT32 boot sector", i ? "secondary" : "primary");
			uprintf("Setting %s FAT32 boot sector for boot...", i ? "secondary" : "primary");
			if (opts.boot_type == BT_FREEDOS) {
				if (!write_fat_32_fd_br(fp, 0)) break;
			} else if (opts.boot_type == BT_REACTOS) {
				if (!write_fat_32_ros_br(fp, 0)) break;
			} else if ((opts.boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) {
				if (!write_fat_32_kos_br(fp, 0)) break;
			} else if ((opts.boot_type == BT_IMAGE) && HAS_BOOTMGR(img_report)) {
				if (!write_fat_32_pe_br(fp, 0)) break;
			} else if ((opts.boot_type == BT_IMAGE) && HAS_WINPE(img_report)) {
				if (!write_fat_32_nt_br(fp, 0)) break;
			} else {
				if (!write_fat_32_br(fp, 0)) break;
			}
			// Disk Drive ID needs to be corrected on XP
			if (!write_partition_physical_disk_drive_id_fat32(fp))
				break;
			fake_fd._offset += 6 * sector_size;
		}
		return TRUE;
	case FS_NTFS:
		uprintf(using_msg, bt_to_name(), "NTFS");
		if (!is_ntfs_fs(fp)) {
			uprintf("New volume does not have an NTFS boot sector - aborting");
			break;
		}
		uprintf("Confirmed new volume has an NTFS boot sector");
		if (!write_ntfs_br(fp)) break;
		return TRUE;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		return TRUE;
	default:
		uprintf("Unsupported FS for FS BR processing - aborting");
		break;
	}
	ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
	return FALSE;
}

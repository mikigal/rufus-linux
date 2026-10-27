/*
 * Rufus: The Reliable USB Formatting Utility
 * File systems
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

#pragma once

#include "rufus.h"

/* From drive.h */
#define FP_FORCE                    0x00000001
#define FP_QUICK                    0x00000002
#define FP_COMPRESSION              0x00000004
#define FP_DUPLICATE_METADATA       0x00000008
#define FP_LARGE_FAT32              0x00010000
#define FP_NO_BOOT                  0x00020000
#define FP_CREATE_PERSISTENCE_CONF  0x00040000
#define FP_NO_PROGRESS              0x00080000

/*
 * Format a volume: a partition (by its device node) or an image file. The
 * offset of the partition on its drive goes to the hidden sectors of FAT and
 * NTFS. The label must have been made valid with ToValidLabel().
 * FAT16/FAT32 use mkfs.fat (unless FormatLargeFAT32() is needed), NTFS mkntfs,
 * exFAT mkfs.exfat, UDF mkudffs, and ext2/3/4 libext2fs. ReFS is not available.
 */
extern BOOL FormatPartition(const char* devnode, uint64_t offset, uint64_t size, uint32_t sector_size,
	uint32_t cluster_size, int fs_type, const char* label, DWORD flags);

/*
 * Make a label valid for a file system (FAT and exFAT: no forbidden or non-ASCII
 * characters, upper case, 11 characters; NTFS and others: 32 characters), and
 * copy it to img_report.usb_label. A FAT label that is mostly underscores gets
 * replaced with the size of the drive.
 */
extern void ToValidLabel(char* Label, BOOL bFAT, uint64_t disk_size);

/* From format_fat32.c and format_ext.c */
extern BOOL FormatLargeFAT32(int fd, uint64_t PartitionOffset, uint64_t PartitionLength, DWORD SectorSize,
	DWORD ClusterSize, LPCSTR FSName, LPCSTR Label, DWORD Flags);
extern BOOL FormatExtFs(const char* devnode, DWORD BlockSize, LPCSTR FSName, LPCSTR Label, DWORD Flags);

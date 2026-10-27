/*
 * Rufus: The Reliable USB Formatting Utility
 * Partitioning
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
 * CreatePartition() of drive.c, in two parts: PlanPartitions() computes the
 * layout with the maths of upstream, and WritePartitionTable() writes it as an
 * MBR or a GPT, with the fields that Windows' IOCTL_DISK_SET_DRIVE_LAYOUT_EX
 * sets. Both work on a file descriptor, so a disk image file will do.
 */

#pragma once

#include "rufus.h"

/* From drive.h */
#define RUFUS_EXTRA_PARTITION_TYPE  0xea

#define XP_MSR                      0x01
#define XP_ESP                      0x02
#define XP_UEFI_NTFS                0x04
#define XP_COMPAT                   0x08
#define XP_PERSISTENCE              0x10

#define PI_MAIN                     0
#define PI_ESP                      1
#define PI_CASPER                   2
#define PI_UEFI_NTFS                3
#define PI_MAX                      4

#define GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER    0x8000000000000000ULL

/*
 * Windows gives USB drives a geometry of 255 heads and 63 sectors per track,
 * which the alignment of the partitions depends on.
 */
#define DEFAULT_SECTORS_PER_TRACK   63
#define DEFAULT_HEADS               255

typedef struct {
	char name[40];					// As upstream names them ("Main Data Partition"...)
	uint64_t offset;
	uint64_t size;
	uint8_t mbr_type;
	BOOL bootable;					// MBR boot indicator
	GUID gpt_type;
	GUID gpt_id;
	uint64_t gpt_attributes;
} partition_t;

typedef struct {
	int style;						// PARTITION_STYLE_MBR, _GPT, or _SFD (no partition table)
	int count;
	partition_t part[MAX_PARTITIONS];
	int index[PI_MAX];				// partition_index[] upstream: where the main, ESP, casper and UEFI:NTFS ones are
	uint64_t disk_size;
	uint32_t sector_size;
	uint32_t sectors_per_track;
	uint32_t mbr_signature;
	GUID disk_id;
} partition_layout_t;

/*
 * Compute the layout of the partitions of a drive, for the file system of the
 * main partition and the extra partitions (XP_*), from opts (old BIOS fixes,
 * cluster size, persistence size, ISO → ESP mode, boot type). With
 * mbr_uefi_marker, the MBR signature is MBR_UEFI_MARKER, as upstream uses it to
 * remember that the drive was partitioned for UEFI.
 */
extern BOOL PlanPartitions(partition_layout_t* layout, int partition_style, int file_system,
	BOOL mbr_uefi_marker, uint8_t extra_partitions, uint64_t disk_size, uint32_t sector_size);

/*
 * Write the partition table of a layout: zero the first sectors of each
 * partition, write the UEFI:NTFS image if there is one, then the MBR, or the
 * protective MBR and both GPTs. The boot code of the MBR is left zeroed, as
 * Windows leaves it, for WriteMBR() to write.
 */
extern BOOL WritePartitionTable(int fd, const partition_layout_t* layout);

/*
 * Zero the beginning of a drive (MBR, GPT, and the boot records of the first
 * partition) and its end (backup GPT), as ClearMBRGPT() does upstream.
 */
extern BOOL ClearMBRGPT(int fd, uint64_t disk_size, uint32_t sector_size);

/* The CRC32 of GPT headers and entries */
extern uint32_t GptCrc32(const uint8_t* buf, size_t len);

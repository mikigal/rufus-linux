/*
 * Rufus: The Reliable USB Formatting Utility
 * Boot record analysis
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
 * From drive.c. The boot records are read through a file descriptor (of a
 * drive or an image), rather than a Windows handle; for a partition, the PBR
 * is read at its offset in the drive.
 */

#include <stdio.h>

#include "rufus.h"
#include "file.h"
#include "br.h"
#include "fat16.h"
#include "fat32.h"
#include "ntfs.h"

const struct {int (*fn)(FILE *fp); char* str;} known_mbr[] = {
	{ is_dos_mbr, "DOS/NT/95A" },
	{ is_dos_f2_mbr, "DOS/NT/95A (F2)" },
	{ is_95b_mbr, "Windows 95B/98/98SE/ME" },
	{ is_2000_mbr, "Windows 2000/XP/2003" },
	{ is_vista_mbr, "Windows Vista" },
	{ is_win7_mbr, "Windows 7" },
	{ is_rufus_mbr, "Rufus" },
	{ is_syslinux_mbr, "Syslinux" },
	{ is_isolinux_mbr, "Isolinux" },
	{ is_reactos_mbr, "ReactOS" },
	{ is_kolibrios_mbr, "KolibriOS" },
	{ is_grub4dos_mbr, "Grub4DOS" },
	{ is_grub2_mbr, "Grub 2.0" },
	{ is_zero_mbr_not_including_disk_signature_or_copy_protect, "Zeroed" },
};

// Returns TRUE if the drive seems bootable, FALSE otherwise
BOOL AnalyzeMBR(int fd, uint32_t SectorSize, const char* TargetName, BOOL bSilent)
{
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	int i;

	fake_fd._fd = fd;
	set_bytes_per_sector(SectorSize);

	if (!is_br(fp)) {
		suprintf("%s does not have a Boot Marker", TargetName);
		return FALSE;
	}
	for (i=0; i<ARRAYSIZE(known_mbr); i++) {
		if (known_mbr[i].fn(fp)) {
			suprintf("%s has a %s Master Boot Record", TargetName, known_mbr[i].str);
			return TRUE;
		}
	}

	suprintf("%s has an unknown Master Boot Record", TargetName);
	return TRUE;
}

const struct {int (*fn)(FILE *fp); char* str;} known_pbr[] = {
	{ entire_fat_16_br_matches, "FAT16 DOS" },
	{ entire_fat_16_fd_br_matches, "FAT16 FreeDOS" },
	{ entire_fat_16_ros_br_matches, "FAT16 ReactOS" },
	{ entire_fat_32_br_matches, "FAT32 DOS" },
	{ entire_fat_32_nt_br_matches, "FAT32 NT" },
	{ entire_fat_32_fd_br_matches, "FAT32 FreeDOS" },
	{ entire_fat_32_ros_br_matches, "FAT32 ReactOS" },
	{ entire_fat_32_kos_br_matches, "FAT32 KolibriOS" },
};

BOOL AnalyzePBR(int fd, uint64_t PartitionOffset, uint32_t SectorSize)
{
	const char* pbr_name = "Partition Boot Record";
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	int i;

	fake_fd._fd = fd;
	fake_fd._offset = PartitionOffset;
	set_bytes_per_sector(SectorSize);

	if (!is_br(fp)) {
		uprintf("Volume does not have an x86 %s", pbr_name);
		return FALSE;
	}

	if (is_fat_16_br(fp) || is_fat_32_br(fp)) {
		for (i=0; i<ARRAYSIZE(known_pbr); i++) {
			if (known_pbr[i].fn(fp)) {
				uprintf("Drive has a %s %s", known_pbr[i].str, pbr_name);
				return TRUE;
			}
		}
		uprintf("Volume has an unknown FAT16 or FAT32 %s", pbr_name);
	} else {
		uprintf("Volume has an unknown %s", pbr_name);
	}
	return TRUE;
}

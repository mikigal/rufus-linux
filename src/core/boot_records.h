/*
 * Rufus: The Reliable USB Formatting Utility
 * Boot records
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

/*
 * Write the boot code of the MBR that suits opts and the image (or the
 * protective MBR message for GPT), keeping the partition table, and setting
 * the boot flag of the first partition for BIOS boot.
 */
extern BOOL WriteMBR(int fd, uint32_t sector_size);

/*
 * Write the secondary boot record (GRUB's core.img, Grub4DOS' grldr.mbr, or
 * the protective message for GPT) after the MBR, which must fit before the
 * first partition. grub2_buf, if set, is used and freed.
 */
extern BOOL WriteSBR(int fd, uint32_t sector_size, uint64_t first_partition_offset);

/* Write the boot record of the partition at partition_offset, for its file system */
extern BOOL WritePBR(int fd, uint64_t partition_offset, uint32_t sector_size, int fs_type);

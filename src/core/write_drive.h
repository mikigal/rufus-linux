/*
 * Rufus: The Reliable USB Formatting Utility
 * Disk image writing and drive zeroing
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
 * Write the disk image of src_fd (as img_report describes it: raw, compressed
 * or VHD) to the drive of fd, up to its size, or zero the drive (fast zeroing
 * skips the blocks that are already empty, with opts.fast_zeroing).
 */
extern BOOL WriteDrive(int fd, uint64_t disk_size, uint32_t sector_size, int src_fd, BOOL bZeroDrive);

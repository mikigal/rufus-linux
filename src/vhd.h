/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual Disk Handling definitions and prototypes
 * Copyright © 2022-2025 Pete Batard <pete@akeo.ie>
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

#include <stdint.h>

#include "rufus.h"

#define WIM_MAGIC							0x0000004D4957534DULL	// "MSWIM\0\0\0"
#define MBR_SIZE							512	// Might need to review this once we see bootable 4k systems

// NB: The VirtDisk structures and WIMGAPI messages of upstream are not needed: VHD and VHDX
// images are read and saved by vdisk.c.

extern int8_t IsBootableImage(const char* path);
extern uint32_t GetWimVersion(const char* image);
extern BOOL WimExtractFile(const char* wim_image, int index, const char* src, const char* dst);
extern BOOL WimApplyImage(const char* image, int index, const char* dst);
// root_len is the length of the destination root in dst (for the md5sum entries)
extern BOOL WimSplitFile(const char* src, const char* dst, size_t root_len);

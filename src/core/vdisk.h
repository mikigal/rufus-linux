/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual disks (VHD and VHDX)
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

enum vdisk_type {
	VDISK_VHD = 1,
	VDISK_VHDX,
};

/*
 * A VHD (fixed or dynamic) or VHDX (fixed or dynamic) image, read as the disk it
 * holds: what upstream gets from VirtDisk, when it mounts an image. Differencing
 * images, and VHDX images with a log to replay, are not supported.
 */
typedef struct vdisk vdisk_t;

extern vdisk_t* VdiskOpen(int fd, enum vdisk_type type);
extern void VdiskClose(vdisk_t* vd);
// The size of the disk
extern uint64_t VdiskSize(const vdisk_t* vd);
// Read the disk: returns the number of bytes read (short at the end of the disk), or -1 (errno)
extern ssize_t VdiskRead(vdisk_t* vd, void* buf, size_t len, uint64_t offset);

/*
 * Save a disk to an image: a fixed VHD, which is the raw disk with a footer (so
 * that it can be written as a DD image), or a dynamic VHDX, without the blocks
 * that only hold zeroes (as the VHDX of VirtDisk). The disk is read from src_fd,
 * the image written to dst_fd, and the progress reported with MSG_261.
 */
extern BOOL VdiskSave(int src_fd, uint64_t disk_size, uint32_t sector_size, int dst_fd, enum vdisk_type type);

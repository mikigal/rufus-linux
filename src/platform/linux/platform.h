/*
 * Rufus: The Reliable USB Formatting Utility
 * Linux platform layer
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

#include <sys/types.h>

#include "rufus.h"
#include "devices.h"

/* A set of disks, as device numbers */
typedef struct {
	dev_t* dev;
	size_t count;
} disk_set_t;

extern BOOL DiskSetContains(const disk_set_t* set, dev_t dev);
extern void DiskSetFree(disk_set_t* set);

/*
 * Add the disks that a block device is on: a partition is on its disk, and a
 * device mapper (LVM, LUKS) or RAID device on the disks of its slaves.
 */
extern void AddDisksOfDevice(disk_set_t* set, dev_t dev);

/*
 * The devices of a disk: the disk itself, its partitions, and the device mapper
 * or RAID devices built on any of them.
 */
extern void GetDiskDevices(dev_t disk, disk_set_t* set);

/* Add the disks that the file system holding 'path' is on */
extern void AddDisksOfPath(disk_set_t* set, const char* path);

/*
 * The disks that hold the system: the file systems mounted on /, /boot,
 * /boot/efi, /efi, /usr, /var and /home, and the swap areas.
 */
extern void GetSystemDisks(disk_set_t* set);

/*
 * List the devices that Rufus can write to, sorted by size, as GetDevices()
 * does on Windows. Returns the number of devices, or -1 on error. Nothing
 * here needs root: sysfs and the udev database can be read by anyone.
 */
extern int GetDevices(rufus_device_t* devs, int max_devs, const device_filter_t* filter);

/*
 * Disconnect a USB drive and connect it again, as CyclePort() disables and enables
 * the device on Windows: the USB device gets deauthorized, then authorized again.
 * Needs root.
 */
extern BOOL CyclePort(const rufus_device_t* dev);

/*
 * The first volume of a drive (at syspath): its device node, the file system type
 * that the kernel mounts it with, and its label. Returns FALSE if there is none.
 */
extern BOOL GetFirstVolume(const char* syspath, char* devnode, size_t devnode_size, char* fs_type, size_t fs_type_size,
	char* label, size_t label_size);

/*
 * The first optical drive with a data disc in it: its device node, and the label
 * and size of the disc (for Alt-O, saving it to an ISO). Returns FALSE if there is none.
 */
extern BOOL GetOpticalMedia(char* devnode, size_t devnode_size, char* label, size_t label_size, uint64_t* size);

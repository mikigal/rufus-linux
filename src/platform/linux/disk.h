/*
 * Rufus: The Reliable USB Formatting Utility
 * Linux disk access
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
 * What drive.c does with \\.\PhysicalDriveN handles, volume locks, drive
 * letters and VDS, on Linux:
 * - a drive is opened from its device node, and then checked to still be the
 *   one that was selected (its diskseq), so that a stick that got replaced is
 *   never written to;
 * - its file systems are unmounted, and the whole disk is opened with O_EXCL,
 *   which the kernel refuses while a partition is mounted or used by the
 *   device mapper or RAID, and which then prevents any mount;
 * - the partition table is read again with BLKRRPART;
 * - a temporary udev rule keeps udisks from mounting the new partitions, or
 *   displaying them on the desktop;
 * - the file systems are mounted in a private mount namespace, which the rest
 *   of the system doesn't see, and which goes away with the process.
 * All of this needs root, except the process search on one's own processes.
 */

#pragma once

#include <sys/types.h>

#include "rufus.h"
#include "platform.h"

/* An open drive */
typedef struct {
	int fd;
	char devnode[32];
	dev_t devnum;
	uint64_t diskseq;
	uint64_t size;
	uint32_t sector_size;
	uint32_t physical_sector_size;
} drive_t;

#define DRIVE_OPEN_WRITE            0x01
#define DRIVE_OPEN_EXCLUSIVE        0x02

/* How long OpenDrive() and UnmountDrive() retry, in ms (DRIVE_ACCESS_TIMEOUT by default) */
extern uint32_t drive_access_timeout;

/*
 * Open a drive, and check that it is still the one with this diskseq (unless
 * diskseq is 0, for kernels that have none). With DRIVE_OPEN_EXCLUSIVE, the
 * open is retried while the drive is busy (udev probing it, or a file system
 * still being unmounted). The drive must not be write protected for
 * DRIVE_OPEN_WRITE. On failure, ErrorStatus is set.
 */
extern BOOL OpenDrive(const char* devnode, uint64_t diskseq, int flags, drive_t* drive);
extern void CloseDrive(drive_t* drive);

/*
 * Unmount the file systems of a drive and its partitions, including those of
 * the device mapper devices it holds, in the current mount namespace.
 */
extern BOOL UnmountDrive(dev_t disk);

/*
 * Make the kernel read the partition table of an open drive again (BLKRRPART),
 * and wait for udev to process the partitions.
 */
extern BOOL RefreshPartitions(drive_t* drive);

/* Wait (up to timeout ms) for udev to have processed all its events */
extern BOOL WaitForUdev(uint32_t timeout);

/*
 * The device node of a partition of a drive, by number (from 1), from sysfs.
 * Returns FALSE if the partition doesn't exist.
 */
extern BOOL GetPartitionDevnode(const drive_t* drive, int partition, char* devnode, size_t size);

/* The mount points of the file systems of a disk, its partitions and the devices built on them */
extern void GetDriveMounts(dev_t disk, StrArray* mount_points);

/*
 * The devices that hold a disk or its partitions (device mapper, RAID), as
 * names such as "dm-0 (luks-1234)".
 */
extern void GetDriveHolders(dev_t disk, StrArray* holders);

/*
 * The processes that use a drive: that have the disk, a partition or a device
 * built on them open, or a file, current directory, root or mapping on one of
 * their file systems (which prevents unmounting them). Entries are formatted
 * as "[pid] command (access)". Rufus itself is not included. Returns the
 * number of processes found.
 * NB: Upstream searches the handles of every process (process.c).
 */
extern int SearchProcesses(dev_t disk, StrArray* processes);

/*
 * Keep udisks from mounting the partitions of a drive, or displaying them, with
 * a udev rule in /run/udev/rules.d, until RemoveAutomountRules().
 */
extern BOOL SuppressAutomount(const char* syspath);
/* Remove the rules of SuppressAutomount(), including those a crashed instance left */
extern void RemoveAutomountRules(void);
/* The text of the rule, for a drive at 'syspath' */
extern void GetAutomountRule(const char* syspath, char* rule, size_t size);

/*
 * Make the mounts of the process private: unless they are made in a mount
 * namespace of its own, they are visible to the desktop (and its file manager).
 * Must be called before any thread is started.
 */
extern BOOL EnterPrivateMountNamespace(void);
/*
 * Mount a volume on a new directory of /run/rufus, and return that directory
 * (to be freed by the caller), or NULL on error.
 */
extern char* MountVolume(const char* devnode, const char* fs_type, BOOL read_only);
/* Unmount a volume of MountVolume(), and remove its directory */
extern BOOL UnmountVolume(const char* mount_point);

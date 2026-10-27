/*
 * Rufus: The Reliable USB Formatting Utility
 * Linux disk access
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
 * GetPhysicalHandle(), the volume unmounting and RefreshDriveLayout() of
 * drive.c, on Linux (see disk.h).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h>

#include "rufus.h"
#include "disk.h"

#ifndef BLKGETDISKSEQ
#define BLKGETDISKSEQ               _IOR(0x12, 128, __u64)
#endif

/* How long the kernel or udev may hold a partition after a change (udev probes them) */
#define PARTITION_REFRESH_TIMEOUT   5000

uint32_t drive_access_timeout = DRIVE_ACCESS_TIMEOUT;

static uint64_t GetTimeMs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Log what keeps a drive busy
static void ReportDriveUsers(dev_t disk)
{
	StrArray list;
	uint32_t i;

	StrArrayCreate(&list, 8);
	GetDriveMounts(disk, &list);
	for (i = 0; i < list.Index; i++)
		uprintf("  Mounted on '%s'", list.String[i]);
	StrArrayClear(&list);
	GetDriveHolders(disk, &list);
	for (i = 0; i < list.Index; i++)
		uprintf("  Used by %s", list.String[i]);
	StrArrayClear(&list);
	SearchProcesses(disk, &list);
	for (i = 0; i < list.Index; i++)
		uprintf("  Used by process %s", list.String[i]);
	StrArrayDestroy(&list);
}

BOOL OpenDrive(const char* devnode, uint64_t diskseq, int flags, drive_t* drive)
{
	BOOL write = (flags & DRIVE_OPEN_WRITE), exclusive = (flags & DRIVE_OPEN_EXCLUSIVE);
	uint64_t seq = 0, end_time = GetTimeMs() + drive_access_timeout;
	struct stat st;
	int fd = -1, ro = 0, i;
	unsigned int ssz = 0, pssz = 0;

	memset(drive, 0, sizeof(*drive));
	drive->fd = -1;
	for (i = 0; ; i++) {
		fd = open(devnode, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC | (exclusive ? O_EXCL : 0));
		if ((fd >= 0) || (errno != EBUSY) || !exclusive)
			break;
		// The kernel refuses an exclusive open while a partition is mounted or used by the
		// device mapper, which may only be transient (udev probing, an unmount in progress)
		if (i == 0) {
			uprintf("Waiting for exclusive access on %s...", devnode);
		} else if (i == DRIVE_ACCESS_RETRIES / 3) {
			uprintf("%s is still in use:", devnode);
			if (stat(devnode, &st) == 0)
				ReportDriveUsers(st.st_rdev);
		}
		if (IS_ERROR(ErrorStatus) || (GetTimeMs() >= end_time))
			break;
		usleep(DRIVE_ACCESS_TIMEOUT * 1000 / DRIVE_ACCESS_RETRIES);
	}
	if (fd < 0) {
		uprintf("Could not open %s: %s", devnode, WindowsErrorString());
		if (!IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR((errno == EBUSY) ? ERROR_SHARING_VIOLATION : WindowsErrorFromErrno(errno));
		return FALSE;
	}
	if ((fstat(fd, &st) != 0) || !S_ISBLK(st.st_mode)) {
		uprintf("%s is not a block device", devnode);
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_HANDLE);
		goto fail;
	}

	// Make sure that this is the drive that was selected, and not one that took its device node since
	if (ioctl(fd, BLKGETDISKSEQ, &seq) != 0) {
		if (diskseq != 0) {
			uprintf("Could not get the sequence number of %s: %s", devnode, WindowsErrorString());
			ErrorStatus = RUFUS_ERROR(ERROR_DEV_NOT_EXIST);
			goto fail;
		}
	} else if ((diskseq != 0) && (seq != diskseq)) {
		uprintf("%s is no longer the drive that was selected (sequence number %llu rather than %llu)",
			devnode, (unsigned long long)seq, (unsigned long long)diskseq);
		ErrorStatus = RUFUS_ERROR(ERROR_DEV_NOT_EXIST);
		goto fail;
	}

	if ((ioctl(fd, BLKGETSIZE64, &drive->size) != 0) || (ioctl(fd, BLKSSZGET, &ssz) != 0)) {
		uprintf("Could not get the size of %s: %s", devnode, WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
		goto fail;
	}
	if (ioctl(fd, BLKPBSZGET, &pssz) != 0)
		pssz = ssz;
	if (write && (ioctl(fd, BLKROGET, &ro) == 0) && (ro != 0)) {
		uprintf("%s is write protected", devnode);
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_PROTECT);
		goto fail;
	}

	drive->fd = fd;
	static_strcpy(drive->devnode, devnode);
	drive->devnum = st.st_rdev;
	drive->diskseq = seq;
	drive->sector_size = ssz;
	drive->physical_sector_size = pssz;
	if (write)
		uprintf("Opened %s for %s write access", devnode, exclusive ? "exclusive" : "shared");
	return TRUE;

fail:
	close(fd);
	return FALSE;
}

void CloseDrive(drive_t* drive)
{
	if (drive->fd >= 0) {
		fsync(drive->fd);
		close(drive->fd);
	}
	drive->fd = -1;
}

// Longest mount points first, so that nested mounts get unmounted before their parent
static int CompareLength(const void* a, const void* b)
{
	return (int)strlen(*(char* const*)b) - (int)strlen(*(char* const*)a);
}

BOOL UnmountDrive(dev_t disk)
{
	StrArray mounts;
	uint64_t end_time;
	uint32_t i;
	BOOL r = TRUE, reported = FALSE;

	StrArrayCreate(&mounts, 8);
	GetDriveMounts(disk, &mounts);
	qsort(mounts.String, mounts.Index, sizeof(char*), CompareLength);
	for (i = 0; i < mounts.Index; i++) {
		uprintf("Unmounting '%s'...", mounts.String[i]);
		end_time = GetTimeMs() + drive_access_timeout;
		while (umount2(mounts.String[i], UMOUNT_NOFOLLOW) != 0) {
			if ((errno != EBUSY) || IS_ERROR(ErrorStatus) || (GetTimeMs() >= end_time)) {
				uprintf("Could not unmount '%s': %s", mounts.String[i], WindowsErrorString());
				if ((errno == EBUSY) && !reported) {
					ReportDriveUsers(disk);
					reported = TRUE;
				}
				r = FALSE;
				break;
			}
			usleep(DRIVE_ACCESS_TIMEOUT * 1000 / DRIVE_ACCESS_RETRIES);
		}
	}
	StrArrayDestroy(&mounts);
	if (!r && !IS_ERROR(ErrorStatus))
		ErrorStatus = RUFUS_ERROR(ERROR_SHARING_VIOLATION);
	return r;
}

BOOL WaitForUdev(uint32_t timeout)
{
	struct udev* udev = udev_new();
	struct udev_queue* queue = (udev == NULL) ? NULL : udev_queue_new(udev);
	uint64_t end_time = GetTimeMs() + timeout;
	BOOL r = TRUE;

	if (queue != NULL) {
		// The queue is only meaningful if udev runs
		while (udev_queue_get_udev_is_active(queue) && !udev_queue_get_queue_is_empty(queue)) {
			if (GetTimeMs() >= end_time) {
				r = FALSE;
				break;
			}
			usleep(20 * 1000);
		}
		udev_queue_unref(queue);
	}
	udev_unref(udev);
	return r;
}

BOOL RefreshPartitions(drive_t* drive)
{
	uint64_t end_time = GetTimeMs() + PARTITION_REFRESH_TIMEOUT;

	// udev briefly opens the partitions to probe them after a change, during which the kernel
	// refuses to drop them, so retry for a while (as upstream does with its VDS refresh)
	WaitForUdev(PARTITION_REFRESH_TIMEOUT);
	while (ioctl(drive->fd, BLKRRPART) != 0) {
		if ((errno != EBUSY) || (GetTimeMs() >= end_time)) {
			uprintf("Could not refresh the partitions of %s: %s", drive->devnode, WindowsErrorString());
			if (errno == EBUSY)
				ReportDriveUsers(drive->devnum);
			ErrorStatus = RUFUS_ERROR(ERROR_PARTITION_FAILURE);
			return FALSE;
		}
		usleep(100 * 1000);
	}
	if (!WaitForUdev(PARTITION_REFRESH_TIMEOUT))
		uprintf("WARNING: udev did not process the new partitions of %s in time", drive->devnode);
	return TRUE;
}

BOOL GetPartitionDevnode(const drive_t* drive, int partition, char* devnode, size_t size)
{
	char path[PATH_MAX], real[PATH_MAX];
	struct dirent* entry;
	DIR* dir;
	FILE* fd;
	int num;
	BOOL r = FALSE;

	static_sprintf(path, "/sys/dev/block/%u:%u", major(drive->devnum), minor(drive->devnum));
	if ((realpath(path, real) == NULL) || ((dir = opendir(real)) == NULL))
		return FALSE;
	while (!r && ((entry = readdir(dir)) != NULL)) {
		if (entry->d_name[0] == '.')
			continue;
		static_sprintf(path, "%s/%s/partition", real, entry->d_name);
		fd = fopen(path, "r");
		if (fd == NULL)
			continue;
		if ((fscanf(fd, "%d", &num) == 1) && (num == partition)) {
			safe_sprintf(devnode, size, "/dev/%s", entry->d_name);
			r = TRUE;
		}
		fclose(fd);
	}
	closedir(dir);
	return r;
}

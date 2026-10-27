/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the disk access, without root
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
 * What can be checked as a user, on the disk that holds the build directory:
 * its mounts, the partition lookup and the process search. Opening, locking,
 * unmounting and repartitioning a drive need root, and are checked in a VM
 * (test_disk_vm.c).
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rufus.h"
#include "ui_iface.h"
#include "disk.h"
#include "test.h"

static char log_buf[64 * 1024];

static void test_log(const char* text)
{
	safe_strcat(log_buf, sizeof(log_buf), text);
}

static BOOL contains(const StrArray* arr, const char* str)
{
	uint32_t i;

	for (i = 0; i < arr->Index; i++) {
		if (strstr(arr->String[i], str) != NULL)
			return TRUE;
	}
	return FALSE;
}

// The disk and partition number of a block device, from sysfs
static dev_t disk_of(dev_t dev, int* partition)
{
	char path[PATH_MAX], real[PATH_MAX];
	unsigned int maj, min;
	FILE* fd;

	*partition = 0;
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", major(dev), minor(dev));
	if (realpath(path, real) == NULL)
		return 0;
	snprintf(path, sizeof(path), "%s/partition", real);
	fd = fopen(path, "r");
	if (fd == NULL)
		return dev;
	if (fscanf(fd, "%d", partition) != 1)
		*partition = 0;
	fclose(fd);
	*strrchr(real, '/') = 0;
	safe_strcat(real, sizeof(real), "/dev");
	fd = fopen(real, "r");
	if (fd == NULL)
		return 0;
	if (fscanf(fd, "%u:%u", &maj, &min) != 2)
		maj = min = 0;
	fclose(fd);
	return makedev(maj, min);
}

static void test_open(void)
{
	drive_t drive;
	char path[] = "/tmp/rufus-test-disk-XXXXXX";
	int fd = mkstemp(path);

	CHECK(fd >= 0);
	close(fd);
	ErrorStatus = 0;
	CHECK(!OpenDrive(path, 0, 0, &drive));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_INVALID_HANDLE));
	CHECK(drive.fd == -1);
	ErrorStatus = 0;
	CHECK(!OpenDrive("/dev/null", 0, 0, &drive));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_INVALID_HANDLE));
	ErrorStatus = 0;
	CHECK(!OpenDrive("/dev/rufus-test-nonexistent", 0, DRIVE_OPEN_EXCLUSIVE, &drive));
	CHECK(IS_ERROR(ErrorStatus));
	ErrorStatus = 0;
	unlink(path);
}

static void test_automount_rule(void)
{
	char rule[1024];

	GetAutomountRule("/sys/devices/pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host2/target2:0:0/2:0:0:0/block/sdb",
		rule, sizeof(rule));
	CHECK_EQ_STR(rule, "# Added by Rufus while it writes to this drive\n"
		"SUBSYSTEM==\"block\", DEVPATH==\"/devices/pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host2/target2:0:0/"
		"2:0:0:0/block/sdb*\", ENV{UDISKS_IGNORE}=\"1\", ENV{UDISKS_AUTO}=\"0\"\n");
}

/* The disk of the build directory, if it is on one */
static void test_this_disk(void)
{
	char devnode[64], file[] = "rufus-test-disk-XXXXXX", entry[32];
	StrArray list;
	disk_set_t devs = { 0 };
	struct stat st, part_st;
	drive_t drive = { .fd = -1 };
	int partition, ready[2], done[2];
	pid_t child;
	dev_t disk;

	if ((stat(".", &st) != 0) || ((disk = disk_of(st.st_dev, &partition)) == 0)) {
		fprintf(stderr, "The build directory is not directly on a disk - skipping its checks\n");
		return;
	}

	// The devices of the disk include the partition of the build directory
	GetDiskDevices(disk, &devs);
	CHECK(DiskSetContains(&devs, disk));
	CHECK(DiskSetContains(&devs, st.st_dev));
	DiskSetFree(&devs);

	// Its mounts include the one of the build directory
	StrArrayCreate(&list, 8);
	GetDriveMounts(disk, &list);
	CHECK(list.Index > 0);
	StrArrayClear(&list);
	GetDriveHolders(disk, &list);
	StrArrayClear(&list);

	// The partition, by number
	drive.devnum = disk;
	if (partition != 0) {
		CHECK(GetPartitionDevnode(&drive, partition, devnode, sizeof(devnode)));
		// A container has no node for it
		if ((stat(devnode, &part_st) != 0) && (errno == ENOENT))
			fprintf(stderr, "There is no %s - skipping its check\n", devnode);
		else
			CHECK((stat(devnode, &part_st) == 0) && (part_st.st_rdev == st.st_dev));
	}
	CHECK(!GetPartitionDevnode(&drive, 999, devnode, sizeof(devnode)));

	// A process that has a file of the disk open
	CHECK((pipe(ready) == 0) && (pipe(done) == 0));
	child = fork();
	if (child == 0) {
		int fd = mkstemp(file);
		char c;
		IGNORE_RETVAL(write(ready[1], "", 1));
		IGNORE_RETVAL(read(done[0], &c, 1));
		close(fd);
		unlink(file);
		_exit(0);
	}
	CHECK(child > 0);
	{
		char c;
		IGNORE_RETVAL(read(ready[0], &c, 1));
	}
	CHECK(SearchProcesses(disk, &list) > 0);
	snprintf(entry, sizeof(entry), "[%d] ", (int)child);
	CHECK(contains(&list, entry));
	snprintf(entry, sizeof(entry), "[%d] ", (int)getpid());
	CHECK(!contains(&list, entry));
	IGNORE_RETVAL(write(done[1], "", 1));
	waitpid(child, NULL, 0);
	close(ready[0]);
	close(ready[1]);
	close(done[0]);
	close(done[1]);
	StrArrayDestroy(&list);
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };

	ui_set_iface(&iface);
	test_open();
	test_automount_rule();
	test_this_disk();
	if (test_failures != 0)
		fprintf(stderr, "Log:\n%s\n", log_buf);
	return TEST_RESULT();
}

/*
 * Rufus: The Reliable USB Formatting Utility
 * Tests of the disk access, as root in a VM
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
 * This test erases a USB stick: tests/e2e/test_disk.py runs it as root in a VM,
 * with an emulated one. It refuses to run without RUFUS_TEST_VM=1, and only
 * touches a QEMU stick (VID 46f4), so that it can't erase a real drive.
 *
 * It checks the identity check of OpenDrive(), that a busy drive can't be
 * opened exclusively nor unmounted, and that what keeps it busy gets reported,
 * that an exclusive open keeps its partitions from being mounted, the
 * partition refresh, the automount suppression, and the private mounts.
 */

#include <fcntl.h>
#include <libudev.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rufus.h"
#include "ui_iface.h"
#include "devices.h"
#include "disk.h"
#include "options.h"
#include "partition.h"
#include "filesystems.h"
#include "test.h"

#define QEMU_VID                    0x46f4
#define TEST_MOUNT                  "/run/rufus-test-mnt"
#define TEST_DM                     "rufus-test-dm"

static char log_buf[256 * 1024];
static rufus_device_t stick;
static dev_t stick_dev;

static void test_log(const char* text)
{
	safe_strcat(log_buf, sizeof(log_buf), text);
}

static BOOL sh(const char* cmd)
{
	const char* argv[] = { "sh", "-c", cmd, NULL };
	DWORD r = RunCommand(argv, NULL, TRUE);

	if (r != 0)
		fprintf(stderr, "'%s' failed with %u\n", cmd, r);
	return (r == 0);
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

static BOOL is_mounted(const char* mount_point)
{
	char line[1024], mp[512];
	BOOL r = FALSE;
	FILE* fd = fopen("/proc/self/mountinfo", "r");

	while ((fd != NULL) && !r && (fgets(line, sizeof(line), fd) != NULL))
		r = (sscanf(line, "%*u %*u %*s %*s %511s", mp) == 1) && (strcmp(mp, mount_point) == 0);
	if (fd != NULL)
		fclose(fd);
	return r;
}

// The device node of a partition of the stick
static void partition(int n, char* devnode, size_t size)
{
	drive_t drive = { .fd = -1, .devnum = stick_dev };

	devnode[0] = 0;
	CHECK(GetPartitionDevnode(&drive, n, devnode, size));
}

static void put32(uint8_t* p, uint32_t v)
{
	p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = v >> 24;
}

/* Find the stick, and give it two partitions */
static BOOL setup(void)
{
	static rufus_device_t devs[MAX_DRIVES];
	device_filter_t filter = { .enable_HDDs = TRUE };
	char cmd[256], p1[32], p2[32];
	struct stat st;
	int i, n;

	n = GetDevices(devs, ARRAYSIZE(devs), &filter);
	for (i = 0; i < n; i++) {
		if (devs[i].props.vid == QEMU_VID)
			break;
	}
	if ((i >= n) || (stat(devs[i].devnode, &st) != 0)) {
		fprintf(stderr, "No QEMU USB stick found\n");
		return FALSE;
	}
	stick = devs[i];
	stick_dev = st.st_rdev;
	CHECK(stick.diskseq != 0);
	snprintf(cmd, sizeof(cmd), "printf 'label: dos\\n,32MiB,c\\n,,83\\n' | sfdisk -q %s", stick.devnode);
	if (!sh(cmd))
		return FALSE;
	WaitForUdev(5000);
	partition(1, p1, sizeof(p1));
	partition(2, p2, sizeof(p2));
	snprintf(cmd, sizeof(cmd), "mkfs.vfat -n RUFUSTEST %s >/dev/null && mkfs.ext4 -q -F %s", p1, p2);
	return sh(cmd);
}

/* The drive that was selected, and no other */
static void test_identity(void)
{
	drive_t drive;

	ErrorStatus = 0;
	CHECK(!OpenDrive(stick.devnode, stick.diskseq + 1, 0, &drive));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_DEV_NOT_EXIST));
	CHECK_CONTAINS(log_buf, "is no longer the drive that was selected");
	ErrorStatus = 0;
	CHECK(OpenDrive(stick.devnode, stick.diskseq, 0, &drive));
	CHECK_EQ_INT(drive.diskseq, stick.diskseq);
	CHECK_EQ_INT(drive.size, stick.size);
	CHECK_EQ_INT(drive.sector_size, 512);
	CHECK(drive.devnum == stick_dev);
	CloseDrive(&drive);
}

/* A mounted partition, which a process uses (as a desktop session would) */
static void test_busy(void)
{
	char p1[32], entry[32], c;
	StrArray list;
	drive_t drive;
	int ready[2];
	pid_t child;

	partition(1, p1, sizeof(p1));
	CHECK(mkdir_p(TEST_MOUNT, 0755) == 0);
	CHECK(mount(p1, TEST_MOUNT, "vfat", 0, NULL) == 0);
	CHECK(pipe(ready) == 0);
	child = fork();
	if (child == 0) {
		IGNORE_RETVAL(chdir(TEST_MOUNT));
		IGNORE_RETVAL(write(ready[1], "", 1));
		pause();
		_exit(0);
	}
	IGNORE_RETVAL(read(ready[0], &c, 1));
	close(ready[0]);
	close(ready[1]);

	StrArrayCreate(&list, 8);
	GetDriveMounts(stick_dev, &list);
	CHECK(contains(&list, TEST_MOUNT));
	StrArrayClear(&list);
	CHECK(SearchProcesses(stick_dev, &list) == 1);
	snprintf(entry, sizeof(entry), "[%d] ", (int)child);
	CHECK(contains(&list, entry));
	CHECK(contains(&list, "files"));
	StrArrayDestroy(&list);

	// Neither an exclusive open nor an unmount can succeed, and both say why
	drive_access_timeout = 1000;
	ErrorStatus = 0;
	log_buf[0] = 0;
	CHECK(!OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_SHARING_VIOLATION));
	CHECK_CONTAINS(log_buf, "Waiting for exclusive access on ");
	ErrorStatus = 0;
	log_buf[0] = 0;
	CHECK(!UnmountDrive(stick_dev));
	CHECK_EQ_INT(ErrorStatus, RUFUS_ERROR(ERROR_SHARING_VIOLATION));
	CHECK_CONTAINS(log_buf, "Mounted on '" TEST_MOUNT "'");
	CHECK_CONTAINS(log_buf, entry);

	// Once the process is gone, the drive gets unmounted
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	ErrorStatus = 0;
	CHECK(UnmountDrive(stick_dev));
	CHECK(!is_mounted(TEST_MOUNT));
	CHECK(OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive));

	// And then, its partitions can't be mounted
	CHECK(mount(p1, TEST_MOUNT, "vfat", 0, NULL) != 0);
	CloseDrive(&drive);
	drive_access_timeout = DRIVE_ACCESS_TIMEOUT;
}

/* A partition below the device mapper */
static void test_holders(void)
{
	char p2[32], cmd[256];
	StrArray list;
	drive_t drive;

	// Closing a drive that was open for writing makes udev reread its partitions
	WaitForUdev(5000);
	partition(2, p2, sizeof(p2));
	snprintf(cmd, sizeof(cmd), "echo \"0 $(blockdev --getsz %s) linear %s 0\" | dmsetup create " TEST_DM, p2, p2);
	if (!sh(cmd)) {
		fprintf(stderr, "No device mapper - skipping its checks\n");
		return;
	}
	StrArrayCreate(&list, 8);
	GetDriveHolders(stick_dev, &list);
	CHECK_EQ_INT(list.Index, 1);
	CHECK(contains(&list, "(" TEST_DM ")"));
	StrArrayDestroy(&list);
	drive_access_timeout = 1000;
	log_buf[0] = 0;
	ErrorStatus = 0;
	CHECK(!OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive));
	CHECK(sh("dmsetup remove " TEST_DM));
	ErrorStatus = 0;
	CHECK(OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive));
	CloseDrive(&drive);
	drive_access_timeout = DRIVE_ACCESS_TIMEOUT;
}

/* A new partition table, which udisks must not see */
static void test_refresh(void)
{
	uint8_t mbr[512] = { 0 };
	char p1[32], p2[32], rule[1024], rule_path[128], *content = NULL;
	struct udev* udev;
	struct udev_device* dev;
	struct stat st;
	drive_t drive;

	CHECK(SuppressAutomount(stick.syspath));
	snprintf(rule_path, sizeof(rule_path), "/run/udev/rules.d/90-rufus-%s.rules", strrchr(stick.syspath, '/') + 1);
	GetAutomountRule(stick.syspath, rule, sizeof(rule));
	CHECK(read_file(rule_path, (uint8_t**)&content) == strlen(rule));
	CHECK((content != NULL) && (strcmp(content, rule) == 0));
	free(content);

	// A single FAT32 LBA partition, written through the exclusive descriptor
	ErrorStatus = 0;
	CHECK(OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive));
	mbr[446 + 4] = 0x0c;
	put32(&mbr[446 + 8], 2048);
	put32(&mbr[446 + 12], (uint32_t)(drive.size / 512) - 2048);
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	CHECK(pwrite(drive.fd, mbr, sizeof(mbr), 0) == sizeof(mbr));
	CHECK(fsync(drive.fd) == 0);
	CHECK(RefreshPartitions(&drive));
	partition(1, p1, sizeof(p1));
	CHECK(!GetPartitionDevnode(&drive, 2, p2, sizeof(p2)));
	CloseDrive(&drive);

	// The new partition is hidden from udisks
	CHECK(stat(p1, &st) == 0);
	udev = udev_new();
	dev = udev_device_new_from_devnum(udev, 'b', st.st_rdev);
	CHECK(dev != NULL);
	if (dev != NULL) {
		CHECK_EQ_STR(udev_device_get_property_value(dev, "UDISKS_IGNORE"), "1");
		CHECK_EQ_STR(udev_device_get_property_value(dev, "UDISKS_AUTO"), "0");
		udev_device_unref(dev);
	}
	udev_unref(udev);
	RemoveAutomountRules();
	CHECK(access(rule_path, F_OK) != 0);
}

/* What gets mounted in the private namespace stays there, and goes away with it */
static void test_private_mounts(void)
{
	char p1[32], cmd[128], mount_point[64] = "", *buf = NULL;
	int ready[2], done[2];
	drive_t drive;
	pid_t child;

	partition(1, p1, sizeof(p1));
	snprintf(cmd, sizeof(cmd), "mkfs.vfat -n PRIVATE %s >/dev/null", p1);
	CHECK(sh(cmd));
	CHECK((pipe(ready) == 0) && (pipe(done) == 0));
	child = fork();
	if (child == 0) {
		char* mp;
		char c;
		if (!EnterPrivateMountNamespace() || ((mp = MountVolume(p1, "vfat", FALSE)) == NULL))
			_exit(1);
		snprintf(cmd, sizeof(cmd), "%s/hello.txt", mp);
		if ((write_file(cmd, (const uint8_t*)"hello", 5) != 5) || !is_mounted(mp))
			_exit(2);
		IGNORE_RETVAL(write(ready[1], mp, strlen(mp) + 1));
		IGNORE_RETVAL(read(done[0], &c, 1));
		// Exit without unmounting: the namespace goes, and the mount with it
		_exit(0);
	}
	IGNORE_RETVAL(read(ready[0], mount_point, sizeof(mount_point)));
	CHECK(strncmp(mount_point, "/run/rufus/mnt-", 15) == 0);
	CHECK(!is_mounted(mount_point));
	IGNORE_RETVAL(write(done[1], "", 1));
	{
		int status = -1;
		waitpid(child, &status, 0);
		CHECK(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
	}
	rmdir(mount_point);

	// The file system is no longer in use, and has the file
	ErrorStatus = 0;
	CHECK(OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_EXCLUSIVE, &drive));
	CloseDrive(&drive);
	{
		char* mp = MountVolume(p1, "vfat", TRUE);
		CHECK(mp != NULL);
		if (mp != NULL) {
			snprintf(cmd, sizeof(cmd), "%s/hello.txt", mp);
			CHECK(read_file(cmd, (uint8_t**)&buf) == 5);
			CHECK((buf != NULL) && (memcmp(buf, "hello", 5) == 0));
			free(buf);
			// Read-only
			snprintf(cmd, sizeof(cmd), "%s/other.txt", mp);
			CHECK(open(cmd, O_CREAT | O_WRONLY, 0644) < 0);
			CHECK(UnmountVolume(mp));
			CHECK(access(mp, F_OK) != 0);
			free(mp);
		}
	}
	close(ready[0]);
	close(ready[1]);
	close(done[0]);
	close(done[1]);
}

// A number from sysfs
static uint64_t sysfs_u64(const char* path)
{
	unsigned long long v = 0;
	FILE* fd = fopen(path, "r");

	if (fd != NULL) {
		if (fscanf(fd, "%llu", &v) != 1)
			v = 0;
		fclose(fd);
	}
	return v;
}

/* A layout of PlanPartitions(), as the kernel and udev see it once written */
static void test_partitioning(void)
{
	partition_layout_t layout;
	char devnode[32], path[128];
	struct udev* udev;
	struct udev_device* dev;
	struct stat st;
	drive_t drive;
	int i;

	ResetOptions(&opts);
	opts.cluster_size = 4096;
	opts.boot_type = BT_IMAGE;
	opts.persistence_size = 32 * MB;
	ErrorStatus = 0;
	CHECK(OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive));
	CHECK(ClearMBRGPT(drive.fd, drive.size, drive.sector_size));
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_GPT, FS_FAT32, FALSE, XP_PERSISTENCE, drive.size, drive.sector_size));
	CHECK_EQ_INT(layout.count, 2);
	CHECK(WritePartitionTable(drive.fd, &layout));
	CHECK(RefreshPartitions(&drive));
	udev = udev_new();
	for (i = 0; i < layout.count; i++) {
		CHECK(GetPartitionDevnode(&drive, i + 1, devnode, sizeof(devnode)));
		snprintf(path, sizeof(path), "/sys/class/block/%s/start", &devnode[5]);
		CHECK_EQ_INT(sysfs_u64(path) * 512, layout.part[i].offset);
		snprintf(path, sizeof(path), "/sys/class/block/%s/size", &devnode[5]);
		CHECK_EQ_INT(sysfs_u64(path) * 512, layout.part[i].size);
		// udev reads the GPT names back (with its escapes for the spaces)
		CHECK(stat(devnode, &st) == 0);
		dev = udev_device_new_from_devnum(udev, 'b', st.st_rdev);
		CHECK(dev != NULL);
		if (dev != NULL) {
			char* name = replace_char(layout.part[i].name, ' ', "\\x20");
			CHECK_EQ_STR(udev_device_get_property_value(dev, "ID_PART_ENTRY_NAME"), name);
			free(name);
			udev_device_unref(dev);
		}
	}
	udev_unref(udev);
	CHECK(!GetPartitionDevnode(&drive, 3, devnode, sizeof(devnode)));
	CloseDrive(&drive);
}

/* The partitions of test_partitioning(), formatted and mounted */
static void test_formatting(void)
{
	partition_layout_t layout;
	char p1[32], p2[32], path[128], *mp, *buf = NULL;
	drive_t drive;

	ResetOptions(&opts);
	opts.cluster_size = 4096;
	opts.boot_type = BT_IMAGE;
	opts.persistence_size = 32 * MB;
	opts.disk_size = stick.size;
	ErrorStatus = 0;
	WaitForUdev(5000);
	CHECK(OpenDrive(stick.devnode, stick.diskseq, DRIVE_OPEN_WRITE | DRIVE_OPEN_EXCLUSIVE, &drive));
	CHECK(PlanPartitions(&layout, PARTITION_STYLE_MBR, FS_FAT32, FALSE, XP_PERSISTENCE, drive.size, drive.sector_size));
	CHECK(WritePartitionTable(drive.fd, &layout));
	CHECK(RefreshPartitions(&drive));
	CHECK(GetPartitionDevnode(&drive, 1, p1, sizeof(p1)));
	CHECK(GetPartitionDevnode(&drive, 2, p2, sizeof(p2)));
	// The mkfs tools open the partitions exclusively, which a drive that is held prevents
	CHECK(!FormatPartition(p1, layout.part[0].offset, layout.part[0].size, drive.sector_size, 0, FS_FAT32, "RUFUS", FP_QUICK));
	CloseDrive(&drive);
	WaitForUdev(5000);
	ErrorStatus = 0;
	CHECK(FormatPartition(p1, layout.part[0].offset, layout.part[0].size, drive.sector_size, 0, FS_FAT32, "RUFUS", FP_QUICK));
	CHECK(FormatPartition(p2, layout.part[1].offset, layout.part[1].size, drive.sector_size, 0, FS_EXT4, "casper-rw",
		FP_QUICK | FP_CREATE_PERSISTENCE_CONF));
	WaitForUdev(5000);

	mp = MountVolume(p1, "vfat", FALSE);
	CHECK(mp != NULL);
	if (mp != NULL) {
		snprintf(path, sizeof(path), "%s/test.txt", mp);
		CHECK(write_file(path, (const uint8_t*)"test", 4) == 4);
		CHECK(UnmountVolume(mp));
		free(mp);
	}
	mp = MountVolume(p2, "ext4", TRUE);
	CHECK(mp != NULL);
	if (mp != NULL) {
		snprintf(path, sizeof(path), "%s/persistence.conf", mp);
		CHECK(read_file(path, (uint8_t**)&buf) == 8);
		CHECK((buf != NULL) && (strcmp(buf, "/ union\n") == 0));
		free(buf);
		CHECK(UnmountVolume(mp));
		free(mp);
	}
	snprintf(path, sizeof(path), "fsck.fat -n %s >/dev/null && e2fsck -fn %s >/dev/null", p1, p2);
	CHECK(sh(path));
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };

	if ((getenv("RUFUS_TEST_VM") == NULL) || (getuid() != 0)) {
		fprintf(stderr, "This test erases a USB stick, and only runs as root in the VM of tests/e2e\n");
		return 77;
	}
	ui_set_iface(&iface);
	if (!setup()) {
		fprintf(stderr, "Log:\n%s\n", log_buf);
		return 1;
	}
	test_identity();
	test_busy();
	test_holders();
	test_refresh();
	test_private_mounts();
	test_partitioning();
	test_formatting();
	if (test_failures != 0)
		fprintf(stderr, "Log:\n%s\n", log_buf);
	else
		printf("ALL DISK TESTS PASSED\n");
	return TEST_RESULT();
}

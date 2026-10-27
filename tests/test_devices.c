/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the device list
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
 * The filters of the drive list, on devices described by the test, and the
 * enumeration of the devices of this machine, of which no system disk may be
 * listed. The listing of real USB sticks is checked in a VM (tests/e2e).
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "rufus.h"
#include "ui_iface.h"
#include "devices.h"
#include "platform.h"
#include "localization.h"
#include "test.h"

static char log_buf[64 * 1024];

static void test_log(const char* text)
{
	safe_strcat(log_buf, sizeof(log_buf), text);
}

// A 16 GB USB 2.0 flash drive
static rufus_device_t flash_drive(void)
{
	rufus_device_t d = { 0 };

	static_strcpy(d.devnode, "/dev/sdb");
	static_strcpy(d.name, "SanDisk Cruzer Blade USB Device");
	d.props.vid = 0x0781;
	d.props.pid = 0x5567;
	d.props.is_USB = TRUE;
	d.props.is_Removable = TRUE;
	d.props.current_speed = USB_SPEED_HIGH_480M;
	d.props.max_speed = USB_SPEED_HIGH_480M;
	d.size = 16 * GB;
	d.sector_size = 512;
	d.partition_style = PARTITION_STYLE_MBR;
	static_strcpy(d.fs_name, "FAT32");
	static_strcpy(d.label, "MY_USB");
	d.nb_volumes = 1;
	return d;
}

static BOOL listed(const rufus_device_t* d, const device_filter_t* f)
{
	log_buf[0] = 0;
	return ShouldListDevice(d, f);
}

static void test_filters(void)
{
	device_filter_t none = { 0 }, all = { .enable_HDDs = TRUE, .list_non_usb_removable_drives = TRUE };
	rufus_device_t d;

	// A flash drive
	d = flash_drive();
	CHECK(listed(&d, &none));
	CHECK_CONTAINS(log_buf, "Found USB 2.1 device 'SanDisk Cruzer Blade USB Device' (0781:5567)\n");

	// A USB 3 drive in a USB 2 port, and a UAS one
	d.props.max_speed = USB_SPEED_SUPER_5G;
	CHECK(listed(&d, &none));
	CHECK_CONTAINS(log_buf, "NOTE: This a USB 3.0 (5 Gbps) device operating at USB 2.1 speed (480 Mbps)...\n");
	d.props.current_speed = USB_SPEED_SUPER_5G;
	d.props.is_UASP = TRUE;
	static_strcpy(d.name, "Samsung Flash Drive FIT UAS Device");
	CHECK(listed(&d, &none));
	CHECK_CONTAINS(log_buf, "Found UAS (USB 3.0) device 'Samsung Flash Drive FIT UAS Device' (0781:5567)\n");

	// Without media, or too small
	d = flash_drive();
	d.size = 0;
	CHECK(!listed(&d, &all));
	CHECK_CONTAINS(log_buf, "Device eliminated because it appears to contain no media\n");
	d.size = 4 * MB;
	CHECK(!listed(&d, &all));
	CHECK_CONTAINS(log_buf, "Device eliminated because it is smaller than 8 MB\n");

	// Ignored by the user
	d = flash_drive();
	none.ignore_vid_pid[3] = (0x0781 << 16) | 0x5567;
	CHECK(!listed(&d, &none));
	CHECK_CONTAINS(log_buf, "Ignoring 'SanDisk Cruzer Blade USB Device' (0781:5567), per user settings\n");
	none.ignore_vid_pid[3] = 0;

	// A USB hard drive is only listed on request
	d = flash_drive();
	static_strcpy(d.name, "WD Elements 25A2 USB Device");
	d.props.vid = 0x1058;
	d.props.pid = 0x25a2;
	d.props.is_Removable = FALSE;
	d.size = 2000 * GB;
	CHECK(!listed(&d, &none));
	CHECK_CONTAINS(log_buf, "Device eliminated because it was detected as a Hard Drive or SSD (score ");
	CHECK(listed(&d, &all));

	// No VID:PID
	d = flash_drive();
	d.props.vid = -1;
	CHECK(listed(&d, &none));
	CHECK_CONTAINS(log_buf, "(\?\?\?\?:\?\?\?\?)");

	// Non-USB drives: removable ones on request (Ctrl-Alt-F), and never the others
	d = flash_drive();
	d.props.is_USB = FALSE;
	d.props.is_SCSI = TRUE;
	d.props.vid = d.props.pid = -1;
	static_strcpy(d.name, "Generic eSATA Disk");
	CHECK(!listed(&d, &none));
	CHECK_CONTAINS(log_buf, "Found non-USB removable device 'Generic eSATA Disk' => Eliminated\n");
	CHECK(listed(&d, &all));
	d.props.is_Removable = FALSE;
	CHECK(!listed(&d, &all));

	// Card readers, unless they are very large
	d = flash_drive();
	d.props.is_USB = FALSE;
	d.props.is_SCSI = TRUE;
	d.props.is_CARD = TRUE;
	d.props.vid = d.props.pid = -1;
	static_strcpy(d.name, "SD32G");
	CHECK(listed(&d, &none));
	CHECK_CONTAINS(log_buf, "Found card reader device 'SD32G'\n");
	d.size = 1000 * GB;
	CHECK(!listed(&d, &none));
	CHECK_CONTAINS(log_buf, "Device eliminated because it was detected as a card larger than 500 GB\n");
	CHECK(listed(&d, &all));

	// Never the system disk, nor the one Rufus runs from
	d = flash_drive();
	d.is_system = TRUE;
	CHECK(!listed(&d, &all));
	CHECK_CONTAINS(log_buf, "Removing /dev/sdb from the list: This is the system disk!\n");
	d.is_system = FALSE;
	d.is_app_disk = TRUE;
	CHECK(!listed(&d, &all));
	CHECK_CONTAINS(log_buf, "Removing /dev/sdb from the list: This is the disk from which Rufus is running!\n");
}

static void test_names(void)
{
	device_filter_t filter = { 0 };
	rufus_device_t d[3];
	rules_drive_t drive;

	d[0] = flash_drive();
	SetDeviceDisplayName(&d[0], &filter);
	CHECK_EQ_STR(d[0].display_name, "MY_USB (sdb) [16 GB]");
	d[0].nb_volumes = 2;
	SetDeviceDisplayName(&d[0], &filter);
	CHECK_EQ_STR(d[0].display_name, "Multiple Partitions (sdb) [16 GB]");
	d[0].nb_volumes = 1;
	d[0].size = 16000000000ULL;
	filter.use_fake_units = TRUE;
	SetDeviceDisplayName(&d[0], &filter);
	CHECK_EQ_STR(d[0].display_name, "MY_USB (sdb) [16 GB]");
	filter.use_fake_units = FALSE;
	SetDeviceDisplayName(&d[0], &filter);
	CHECK_EQ_STR(d[0].display_name, "MY_USB (sdb) [14.9 GB]");
	static_strcpy(d[0].devnode, "/dev/mmcblk0");
	static_strcpy(d[0].label, STR_NO_LABEL);
	SetDeviceDisplayName(&d[0], &filter);
	CHECK_EQ_STR(d[0].display_name, "NO_LABEL (mmcblk0) [14.9 GB]");

	// What the rules get
	d[0] = flash_drive();
	d[0].size = 8 * GB;
	GetRulesDrive(&d[0], &drive);
	CHECK_EQ_INT(drive.disk_size, 8 * GB);
	CHECK_EQ_INT(drive.sector_size, 512);
	CHECK_EQ_INT(drive.partition_style, PARTITION_STYLE_MBR);
	CHECK(!drive.is_fixed);
	CHECK_EQ_STR(drive.fs_name, "FAT32");
	CHECK_EQ_STR(drive.label, "MY_USB");
	CHECK_EQ_STR(drive.proposed_label, "8 GB");

	// Sorted by size
	d[0].size = 32 * GB;
	d[1] = flash_drive();
	d[1].size = 8 * GB;
	d[2] = flash_drive();
	d[2].size = 16 * GB;
	SortDevices(d, 3);
	CHECK(d[0].size == 8 * GB && d[1].size == 16 * GB && d[2].size == 32 * GB);

	// The file systems, as Windows names them
	CHECK_EQ_STR(GetFsNameFromBlkid("vfat", "FAT32"), "FAT32");
	CHECK_EQ_STR(GetFsNameFromBlkid("vfat", "FAT16"), "FAT");
	CHECK_EQ_STR(GetFsNameFromBlkid("ntfs", NULL), "NTFS");
	CHECK_EQ_STR(GetFsNameFromBlkid("exfat", "1.0"), "exFAT");
	CHECK_EQ_STR(GetFsNameFromBlkid("ext4", "1.0"), "ext4");
	CHECK_EQ_STR(GetFsNameFromBlkid("btrfs", NULL), "");
	CHECK_EQ_STR(GetFsNameFromBlkid(NULL, NULL), "");
}

// The disk of a block device, from sysfs
static dev_t whole_disk(dev_t dev)
{
	char path[PATH_MAX], real[PATH_MAX];
	unsigned int maj, min;
	FILE* fd;

	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", major(dev), minor(dev));
	if (realpath(path, real) == NULL)
		return 0;
	snprintf(path, sizeof(path), "%s/partition", real);
	if (access(path, F_OK) != 0)
		return dev;
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

/* The devices of this machine */
static void test_this_machine(void)
{
	static rufus_device_t devs[MAX_DRIVES];
	device_filter_t all = { .enable_HDDs = TRUE, .list_non_usb_removable_drives = TRUE };
	disk_set_t system_disks = { 0 };
	struct stat st;
	int i, n;

	GetSystemDisks(&system_disks);
	// If / is on a block device (and not on btrfs or a device mapper), its disk is a system disk
	if ((stat("/", &st) == 0) && (whole_disk(st.st_dev) != 0))
		CHECK(DiskSetContains(&system_disks, whole_disk(st.st_dev)));
	else
		fprintf(stderr, "The root file system is not directly on a disk - skipping its check\n");

	// No listed device may be a system disk, whatever the options
	n = GetDevices(devs, ARRAYSIZE(devs), &all);
	CHECK(n >= 0);
	for (i = 0; i < n; i++) {
		fprintf(stderr, "Listed: %s '%s'\n", devs[i].devnode, devs[i].display_name);
		CHECK(stat(devs[i].devnode, &st) == 0);
		CHECK(!DiskSetContains(&system_disks, st.st_rdev));
		CHECK(!devs[i].is_system);
		CHECK(devs[i].size >= MIN_DRIVE_SIZE);
	}
	DiskSetFree(&system_disks);

	// The disk of the current directory
	AddDisksOfPath(&system_disks, ".");
	if ((stat(".", &st) == 0) && (whole_disk(st.st_dev) != 0))
		CHECK(DiskSetContains(&system_disks, whole_disk(st.st_dev)));
	DiskSetFree(&system_disks);
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };
	const char* src = getenv("RUFUS_SOURCE_DIR");
	char loc_path[PATH_MAX];

	ui_set_iface(&iface);
	if (src == NULL) {
		fprintf(stderr, "RUFUS_SOURCE_DIR is not set\n");
		return 77;
	}
	snprintf(loc_path, sizeof(loc_path), "%s/res/loc/rufus.loc", src);
	init_localization();
	CHECK(get_supported_locales(loc_path));
	CHECK(get_loc_data_file(loc_path, get_locale_from_name("en-US", FALSE)));

	test_filters();
	test_names();
	test_this_machine();

	exit_localization();
	if (test_failures != 0)
		fprintf(stderr, "Log:\n%s\n", log_buf);
	return TEST_RESULT();
}

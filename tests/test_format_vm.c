/*
 * Rufus: The Reliable USB Formatting Utility
 * Tests of the format pipeline, as root in a VM
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
 * This test erases a USB stick: tests/e2e/test_format.py runs it as root in a
 * VM, with an emulated one, and then boots that stick. It refuses to run
 * without RUFUS_TEST_VM=1, and only touches a QEMU stick (VID 46f4).
 *
 * Usage: test_format_vm <scenario> [image]
 *   fat32     non bootable, MBR, FAT32 with an extended label
 *   ext4      non bootable, GPT, ext4
 *   iso-uefi  the image in ISO mode, GPT, UEFI, FAT32
 *   dd        the image in DD mode
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "devices.h"
#include "disk.h"
#include "image_scan.h"
#include "format_thread.h"
#include "test.h"

#define QEMU_VID                    0x46f4

static rufus_device_t stick;

static void cli_log(const char* text)
{
	fputs(text, stderr);
}

static BOOL sh(const char* cmd)
{
	const char* argv[] = { "sh", "-c", cmd, NULL };

	return (RunCommand(argv, NULL, TRUE) == 0);
}

static BOOL find_stick(void)
{
	static rufus_device_t devs[MAX_DRIVES];
	device_filter_t filter = { .enable_HDDs = TRUE };
	int i, n = GetDevices(devs, ARRAYSIZE(devs), &filter);

	for (i = 0; i < n; i++) {
		if (devs[i].props.vid == QEMU_VID) {
			stick = devs[i];
			return TRUE;
		}
	}
	fprintf(stderr, "No QEMU USB stick found\n");
	return FALSE;
}

static void reset(void)
{
	ResetOptions(&opts);
	opts.disk_size = stick.size;
	opts.quick_format = TRUE;
	opts.cluster_size = 4096;
	ErrorStatus = 0;
}

static BOOL format(int image_fd)
{
	format_target_t target = { stick.devnode, stick.diskseq, stick.syspath, image_fd, TRUE };

	return FormatDrive(&target);
}

static BOOL scan(const char* image)
{
	image_path = strdup(image);
	return (ImageScan(FALSE) == RULES_SCAN_OK);
}

int main(int argc, char** argv)
{
	const ui_iface_t iface = { .log = cli_log };
	char cmd[256], p1[32];
	drive_t drive = { .fd = -1 };
	int fd;

	if ((getenv("RUFUS_TEST_VM") == NULL) || (getuid() != 0) || (argc < 2)) {
		fprintf(stderr, "This test erases a USB stick, and only runs as root in the VM of tests/e2e\n");
		return 77;
	}
	ui_set_iface(&iface);
	if (!find_stick())
		return 1;
	reset();

	if (strcmp(argv[1], "fat32") == 0) {
		opts.boot_type = BT_NON_BOOTABLE;
		opts.partition_type = PARTITION_STYLE_MBR;
		opts.target_type = TT_BIOS;
		opts.fs_type = FS_FAT32;
		opts.cluster_size = 0;
		opts.extended_label = TRUE;
		static_strcpy(opts.label, "Mój pendrive");
		CHECK(format(-1));
	} else if (strcmp(argv[1], "ext4") == 0) {
		opts.boot_type = BT_NON_BOOTABLE;
		opts.partition_type = PARTITION_STYLE_GPT;
		opts.target_type = TT_UEFI;
		opts.fs_type = FS_EXT4;
		opts.extended_label = FALSE;
		static_strcpy(opts.label, "Linux data");
		CHECK(format(-1));
	} else if ((strcmp(argv[1], "iso-uefi") == 0) && (argc > 2)) {
		CHECK(scan(argv[2]));
		opts.boot_type = BT_IMAGE;
		opts.partition_type = PARTITION_STYLE_GPT;
		opts.target_type = TT_UEFI;
		opts.fs_type = FS_FAT32;
		// The default, as 4 KB would give the 256 MB drive too few clusters for FAT32
		opts.cluster_size = 0;
		static_strcpy(opts.label, "RUFUS TEST");
		CHECK(format(-1));
	} else if ((strcmp(argv[1], "dd") == 0) && (argc > 2)) {
		CHECK(scan(argv[2]));
		opts.boot_type = BT_IMAGE;
		opts.write_as_image = TRUE;
		fd = open(argv[2], O_RDONLY | O_CLOEXEC);
		CHECK(fd >= 0);
		CHECK(format(fd));
		close(fd);
	} else {
		fprintf(stderr, "Unknown scenario '%s'\n", argv[1]);
		return 2;
	}

	// What the system sees of the drive
	if ((strcmp(argv[1], "fat32") == 0) || (strcmp(argv[1], "ext4") == 0)) {
		drive.devnum = 0;
		CHECK(OpenDrive(stick.devnode, stick.diskseq, 0, &drive));
		CHECK(GetPartitionDevnode(&drive, 1, p1, sizeof(p1)));
		CloseDrive(&drive);
		if (strcmp(argv[1], "fat32") == 0) {
			snprintf(cmd, sizeof(cmd), "fsck.fat -n %s && blkid -p -o value -s LABEL %s | grep -qx 'M_J PENDRIV'", p1, p1);
			CHECK(sh(cmd));
			snprintf(cmd, sizeof(cmd), "mdir -i %s ::/autorun.inf ::/autorun.ico", p1);
			CHECK(sh(cmd));
		} else {
			snprintf(cmd, sizeof(cmd), "e2fsck -fn %s && blkid -p -o value -s LABEL %s | grep -qx 'Linux data'", p1, p1);
			CHECK(sh(cmd));
		}
	}
	CHECK(!IS_ERROR(ErrorStatus));
	if (test_failures == 0)
		printf("FORMAT TEST PASSED\n");
	return TEST_RESULT();
}

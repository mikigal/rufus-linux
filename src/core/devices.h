/*
 * Rufus: The Reliable USB Formatting Utility
 * Devices that Rufus can write to
 * Copyright © 2014-2026 Pete Batard <pete@akeo.ie>
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
 * On Windows, GetDevices() (dev.c) gathers the properties of the drives from
 * SetupAPI and decides which ones to list, along the way. Here, the platform
 * code fills a rufus_device_t per disk, and ShouldListDevice() applies the
 * rules of dev.c to it, so that they can be tested without the hardware.
 */

#pragma once

#include "rufus.h"
#include "rules.h"

/* From dev.h */
#define USB_SPEED_UNKNOWN           0
#define USB_SPEED_LOW_1_5M          1	// USB 1.0
#define USB_SPEED_FULL_12M          2	// USB 1.1
#define USB_SPEED_HIGH_480M         3	// USB 2.0
#define USB_SPEED_SUPER_5G          4	// USB 3.0
#define USB_SPEED_SUPER_10G         5	// USB 3.1
#define USB_SPEED_SUPER_20G         6	// USB 3.2
#define USB_SPEED_SUPER_40G         7	// USB 4.0
#define USB_SPEED_SUPER_80G         8	// USB 4.1
#define USB_SPEED_MAX               9

typedef struct usb_device_props {
	int32_t  vid;					// -1 if unknown
	int32_t  pid;
	uint32_t current_speed;			// USB_SPEED_*
	uint32_t max_speed;
	uint32_t port;					// Port on the parent hub
	BOOLEAN  is_USB;
	BOOLEAN  is_SCSI;				// Any other disk (SATA, NVMe, MMC...), as the "SCSI" enumerator is on Windows
	BOOLEAN  is_CARD;				// Card reader
	BOOLEAN  is_UASP;
	BOOLEAN  is_VHD;
	BOOLEAN  is_Removable;
} usb_device_props;

typedef struct {
	/* Identification. The device node is used again only once its diskseq has been checked. */
	char devnode[32];				// e.g. "/dev/sdb"
	char syspath[256];				// sysfs path of the disk
	char usb_syspath[256];			// sysfs path of its USB device, or "" (the hub and port upstream, for port cycling)
	uint64_t diskseq;				// Disk sequence number of the kernel, 0 if it has none (before 5.15)
	/* What upstream gets from SetupAPI and the drive */
	char name[128];					// As Windows names it ("<vendor> <model> USB Device"), which IsHDD() relies on
	usb_device_props props;
	uint64_t size;					// 0 if there is no media
	uint32_t sector_size;
	/* Its contents, from the udev database */
	int partition_style;			// PARTITION_STYLE_MBR, _GPT or _RAW
	char fs_name[16];				// File system of the first volume, as Windows names it ("FAT32", "NTFS"...), or ""
	char label[64];					// Label of the first volume, or STR_NO_LABEL
	int nb_volumes;					// Partitions (or the whole disk) with a file system
	/* What it is used for on this system */
	BOOL is_system;					// It holds a system mount (/, /boot, /usr...) or swap
	BOOL is_app_disk;				// It holds app_dir
	/* Set when the device gets listed */
	char display_name[192];
} rufus_device_t;

/* The options of the drive list */
typedef struct {
	BOOL enable_HDDs;				// List USB hard drives ("List USB Hard Drives" or Alt-F)
	BOOL list_non_usb_removable_drives;	// Ctrl-Alt-F
	BOOL use_fake_units;			// Display sizes with the units of the manufacturers (1 KB = 1000 bytes)
	BOOL enable_VHDs;				// List virtual disks (Alt-G): writable loop devices backed by a file, and
									// the virtual disks of hypervisors that upstream knows
	BOOL enable_vmdk;				// Also count the VMware virtual disks as virtual disks (Alt-W)
	uint32_t ignore_vid_pid[MAX_IGNORE_USB];	// VID << 16 | PID of the devices to ignore (IgnoreUsb## settings)
} device_filter_t;

/*
 * Whether a device can be listed. As upstream, the device and the reason why
 * it was eliminated get logged. A system disk, or the disk Rufus runs from, is
 * never listed.
 */
extern BOOL ShouldListDevice(const rufus_device_t* dev, const device_filter_t* filter);

/* Set the name of a device for the drive list, e.g. "MY_USB (sdb) [8 GB]" */
extern void SetDeviceDisplayName(rufus_device_t* dev, const device_filter_t* filter);

/* Sort devices by increasing size, as upstream lists them */
extern void SortDevices(rufus_device_t* devs, int nb_devs);

/* Whether the name of a disk ("<vendor> <model>") is the one of a virtual disk that upstream knows */
extern BOOL IsVHDName(const char* name, BOOL enable_vmdk);

/* The description of a device that the main window rules use */
extern void GetRulesDrive(const rufus_device_t* dev, rules_drive_t* drive);

/* The name that Windows gives to a file system, from the type and version that blkid reports */
extern const char* GetFsNameFromBlkid(const char* type, const char* version);

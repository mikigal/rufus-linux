/*
 * Rufus: The Reliable USB Formatting Utility
 * Device enumeration
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
 * GetDevices() of dev.c, on libudev. The properties that upstream gets from
 * SetupAPI and the hub IOCTLs come from sysfs (USB device, SCSI INQUIRY data,
 * size), and the contents of the disks (partition table, file systems,
 * labels) from the udev database, which blkid fills as root. So none of this
 * needs root, nor opens a device.
 */

#include <fcntl.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysmacros.h>

#include "rufus.h"
#include "devices.h"
#include "platform.h"
#include "resource.h"
#include "localization.h"

// From dev.c
static uint32_t version_to_speed(uint16_t usb_version) {
	switch (usb_version) {
	case 0x0100: return USB_SPEED_LOW_1_5M;
	case 0x0110: return USB_SPEED_FULL_12M;
	case 0x0200:
	case 0x0201:
	case 0x0210: return USB_SPEED_HIGH_480M;
	case 0x0300: return USB_SPEED_SUPER_5G;
	case 0x0310: return USB_SPEED_SUPER_10G;
	case 0x0320: return USB_SPEED_SUPER_20G;
	case 0x0400: return USB_SPEED_SUPER_40G;
	case 0x0410: return USB_SPEED_SUPER_80G;
	default: return USB_SPEED_UNKNOWN;
	}
}

// The 'speed' attribute of a USB device is in Mbps
static uint32_t mbps_to_speed(const char* mbps)
{
	static const struct { const char* mbps; uint32_t speed; } map[] = {
		{ "1.5", USB_SPEED_LOW_1_5M }, { "12", USB_SPEED_FULL_12M }, { "480", USB_SPEED_HIGH_480M },
		{ "5000", USB_SPEED_SUPER_5G }, { "10000", USB_SPEED_SUPER_10G }, { "20000", USB_SPEED_SUPER_20G },
		{ "40000", USB_SPEED_SUPER_40G }, { "80000", USB_SPEED_SUPER_80G },
	};
	size_t i;

	for (i = 0; (mbps != NULL) && (i < ARRAYSIZE(map)); i++) {
		if (strcmp(mbps, map[i].mbps) == 0)
			return map[i].speed;
	}
	return USB_SPEED_UNKNOWN;
}

static uint64_t sysattr_u64(struct udev_device* dev, const char* attr)
{
	const char* val = udev_device_get_sysattr_value(dev, attr);

	return (val == NULL) ? 0 : strtoull(val, NULL, 0);
}

// Copy a sysfs string attribute, without the padding of the SCSI INQUIRY data
static void sysattr_str(struct udev_device* dev, const char* attr, char* dst, size_t size)
{
	const char* val = udev_device_get_sysattr_value(dev, attr);

	dst[0] = 0;
	if (val == NULL)
		return;
	while (*val == ' ')
		val++;
	safe_strcpy(dst, size, val);
	trim(dst);
}

static void GetUsbProperties(struct udev_device* dev, rufus_device_t* d)
{
	struct udev_device *usb, *intf;
	const char *val, *driver;
	unsigned int major_version = 0, minor_version = 0;

	usb = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
	if (usb == NULL)
		return;
	d->props.is_USB = TRUE;
	static_strcpy(d->usb_syspath, udev_device_get_syspath(usb));
	val = udev_device_get_sysattr_value(usb, "idVendor");
	d->props.vid = (val == NULL) ? -1 : (int32_t)strtol(val, NULL, 16);
	val = udev_device_get_sysattr_value(usb, "idProduct");
	d->props.pid = (val == NULL) ? -1 : (int32_t)strtol(val, NULL, 16);
	d->props.current_speed = mbps_to_speed(udev_device_get_sysattr_value(usb, "speed"));
	val = udev_device_get_sysattr_value(usb, "version");
	if ((val != NULL) && (sscanf(val, " %u.%u", &major_version, &minor_version) == 2))
		d->props.max_speed = version_to_speed((uint16_t)((major_version << 8) | minor_version));
	// The port is the last component of the path of the device on its bus (e.g. "1.4")
	val = udev_device_get_sysattr_value(usb, "devpath");
	if (val != NULL)
		d->props.port = (uint32_t)strtoul((strrchr(val, '.') != NULL) ? &strrchr(val, '.')[1] : val, NULL, 10);
	// NB: Upstream finds UAS devices from the name of their storage driver
	intf = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_interface");
	driver = (intf == NULL) ? NULL : udev_device_get_driver(intf);
	d->props.is_UASP = (safe_strcmp(driver, "uas") == 0);
}

static void GetDeviceName(struct udev_device* dev, rufus_device_t* d)
{
	char vendor[64], model[64];

	sysattr_str(dev, "device/vendor", vendor, sizeof(vendor));
	// Some buses (virtio...) have a numerical vendor ID there, rather than a name
	if (strncmp(vendor, "0x", 2) == 0)
		vendor[0] = 0;
	sysattr_str(dev, "device/model", model, sizeof(model));
	// MMC cards have a name rather than a model
	if (model[0] == 0)
		sysattr_str(dev, "device/name", model, sizeof(model));
	if ((vendor[0] != 0) && (model[0] != 0))
		static_sprintf(d->name, "%s %s", vendor, model);
	else if ((vendor[0] != 0) || (model[0] != 0))
		static_strcpy(d->name, (vendor[0] != 0) ? vendor : model);
	else
		static_strcpy(d->name, lmprintf(MSG_045));
	// As Windows names them, which the HDD detection tables rely on
	if (d->props.is_USB)
		static_strcat(d->name, d->props.is_UASP ? " UAS Device" : " USB Device");
}

// The file systems of a disk and of its partitions, from the udev database
static void GetVolumes(struct udev* udev, struct udev_device* dev, rufus_device_t* d)
{
	struct udev_enumerate* e;
	struct udev_list_entry* entry;
	struct udev_device* part;
	const char *type, *fs_name, *label, *pt_type;
	uint64_t first = UINT64_MAX, num;

	pt_type = udev_device_get_property_value(dev, "ID_PART_TABLE_TYPE");
	if (safe_strcmp(pt_type, "dos") == 0)
		d->partition_style = PARTITION_STYLE_MBR;
	else if (safe_strcmp(pt_type, "gpt") == 0)
		d->partition_style = PARTITION_STYLE_GPT;
	else
		d->partition_style = PARTITION_STYLE_RAW;
	static_strcpy(d->label, STR_NO_LABEL);

	// A file system on the whole disk (a "super floppy")
	type = udev_device_get_property_value(dev, "ID_FS_TYPE");
	fs_name = GetFsNameFromBlkid(type, udev_device_get_property_value(dev, "ID_FS_VERSION"));
	if (fs_name[0] != 0) {
		d->nb_volumes = 1;
		static_strcpy(d->fs_name, fs_name);
		label = udev_device_get_property_value(dev, "ID_FS_LABEL");
		if ((label != NULL) && (label[0] != 0))
			static_strcpy(d->label, label);
		return;
	}

	e = udev_enumerate_new(udev);
	if (e == NULL)
		return;
	udev_enumerate_add_match_parent(e, dev);
	udev_enumerate_add_match_subsystem(e, "block");
	udev_enumerate_add_match_property(e, "DEVTYPE", "partition");
	udev_enumerate_scan_devices(e);
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		part = udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
		if (part == NULL)
			continue;
		fs_name = GetFsNameFromBlkid(udev_device_get_property_value(part, "ID_FS_TYPE"),
			udev_device_get_property_value(part, "ID_FS_VERSION"));
		num = sysattr_u64(part, "partition");
		if (fs_name[0] != 0) {
			d->nb_volumes++;
			// The first volume is the one of the lowest partition number
			if (num < first) {
				first = num;
				static_strcpy(d->fs_name, fs_name);
				label = udev_device_get_property_value(part, "ID_FS_LABEL");
				static_strcpy(d->label, ((label != NULL) && (label[0] != 0)) ? label : STR_NO_LABEL);
			}
		}
		udev_device_unref(part);
	}
	udev_enumerate_unref(e);
}

// Whether a disk is a card reader: an MMC card, or a USB reader that udev identified as such
static BOOL IsCardReader(struct udev_device* dev)
{
	struct udev_list_entry* entry;

	if (udev_device_get_parent_with_subsystem_devtype(dev, "mmc", NULL) != NULL)
		return TRUE;
	udev_list_entry_foreach(entry, udev_device_get_properties_list_entry(dev)) {
		if (strncmp(udev_list_entry_get_name(entry), "ID_DRIVE_FLASH_", 15) == 0)
			return TRUE;
	}
	return FALSE;
}

/*
 * The loop devices that can be written to like a VHD: backed by a file, not
 * read-only (which leaves out the squashfs images of snaps), and with partition
 * scanning, so that the partitions Rufus creates get their devices.
 */
static BOOL IsLoopVHD(struct udev_device* dev)
{
	char backing_file[MAX_PATH];

	if (strncmp(udev_device_get_sysname(dev), "loop", 4) != 0)
		return FALSE;
	sysattr_str(dev, "loop/backing_file", backing_file, sizeof(backing_file));
	if (backing_file[0] == 0)
		return FALSE;
	if (sysattr_u64(dev, "ro") != 0) {
		uuprintf("Skipping '%s' (read-only loop device of '%s')", udev_device_get_sysname(dev), backing_file);
		return FALSE;
	}
	if (sysattr_u64(dev, "loop/partscan") == 0) {
		uprintf("Skipping '%s' (loop device of '%s' without partition scanning: use 'losetup -P')",
			udev_device_get_sysname(dev), backing_file);
		return FALSE;
	}
	return TRUE;
}

int GetDevices(rufus_device_t* devs, int max_devs, const device_filter_t* filter)
{
	struct udev* udev;
	struct udev_enumerate* e;
	struct udev_list_entry* entry;
	struct udev_device* dev;
	disk_set_t system_disks = { 0 }, app_disks = { 0 };
	const char *syspath, *sysname, *devnode;
	char backing_file[MAX_PATH];
	rufus_device_t* d;
	dev_t devnum;
	int nb_devs = 0;
	BOOL is_loop;

	udev = udev_new();
	if (udev == NULL) {
		uprintf("Could not access udev: %s", WindowsErrorString());
		return -1;
	}
	e = udev_enumerate_new(udev);
	if (e == NULL) {
		udev_unref(udev);
		return -1;
	}
	GetSystemDisks(&system_disks);
	if (app_dir[0] != 0)
		AddDisksOfPath(&app_disks, app_dir);

	udev_enumerate_add_match_subsystem(e, "block");
	udev_enumerate_add_match_property(e, "DEVTYPE", "disk");
	udev_enumerate_scan_devices(e);
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		if (nb_devs >= max_devs) {
			uprintf("WARNING: Found more than %d drives - ignoring remaining ones...", max_devs);
			break;
		}
		syspath = udev_list_entry_get_name(entry);
		dev = udev_device_new_from_syspath(udev, syspath);
		if (dev == NULL)
			continue;
		sysname = udev_device_get_sysname(dev);
		devnode = udev_device_get_devnode(dev);
		// RAM, zram, device mapper, RAID, NBD... devices are virtual, and optical drives are not disks.
		// A loop device is the VHD of Linux, if it is backed by a file (see IsLoopVHD()).
		is_loop = (strstr(syspath, "/devices/virtual/") != NULL) && IsLoopVHD(dev);
		if ((devnode == NULL) || ((strstr(syspath, "/devices/virtual/") != NULL) && !is_loop) ||
			(strncmp(sysname, "sr", 2) == 0) || (strncmp(sysname, "fd", 2) == 0)) {
			uuprintf("Skipping '%s'", sysname);
			udev_device_unref(dev);
			continue;
		}
		d = &devs[nb_devs];
		memset(d, 0, sizeof(*d));
		d->props.vid = -1;
		d->props.pid = -1;
		static_strcpy(d->devnode, devnode);
		static_strcpy(d->syspath, syspath);
		d->diskseq = sysattr_u64(dev, "diskseq");
		d->size = sysattr_u64(dev, "size") * 512;
		d->sector_size = (uint32_t)sysattr_u64(dev, "queue/logical_block_size");
		d->props.is_Removable = (sysattr_u64(dev, "removable") != 0);
		GetUsbProperties(dev, d);
		d->props.is_SCSI = !d->props.is_USB;
		d->props.is_CARD = IsCardReader(dev);
		// NB: Windows sets a "surprise removal" policy on SD cards, which the MMC driver doesn't report as removable
		if (udev_device_get_parent_with_subsystem_devtype(dev, "mmc", NULL) != NULL)
			d->props.is_Removable = TRUE;
		GetDeviceName(dev, d);
		if (is_loop) {
			sysattr_str(dev, "loop/backing_file", backing_file, sizeof(backing_file));
			static_sprintf(d->name, "Loop Device (%s)", (strrchr(backing_file, '/') == NULL) ? backing_file :
				&strrchr(backing_file, '/')[1]);
		}
		d->props.is_VHD = is_loop || IsVHDName(d->name, filter->enable_vmdk);
		GetVolumes(udev, dev, d);
		devnum = udev_device_get_devnum(dev);
		d->is_system = DiskSetContains(&system_disks, devnum);
		d->is_app_disk = DiskSetContains(&app_disks, devnum);
		uuprintf("Processing '%s' (%s, diskseq %llu):", d->devnode, d->syspath, (unsigned long long)d->diskseq);
		if (ShouldListDevice(d, filter)) {
			SetDeviceDisplayName(d, filter);
			nb_devs++;
		}
		udev_device_unref(dev);
	}
	udev_enumerate_unref(e);
	udev_unref(udev);
	DiskSetFree(&system_disks);
	DiskSetFree(&app_disks);

	SortDevices(devs, nb_devs);
	return nb_devs;
}

static BOOL WriteAuthorized(const char* usb_syspath, const char* value)
{
	char path[sizeof(((rufus_device_t*)0)->usb_syspath) + 16];
	int fd;
	BOOL r;

	static_sprintf(path, "%s/authorized", usb_syspath);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return FALSE;
	r = (write(fd, value, 1) == 1);
	close(fd);
	return r;
}

BOOL CyclePort(const rufus_device_t* dev)
{
	if (dev->usb_syspath[0] == 0) {
		uprintf("Could not find a device to cycle!");
		return FALSE;
	}
	uprintf("Cycling port %d of '%s'...", dev->props.port, dev->name);
	if (!WriteAuthorized(dev->usb_syspath, "0")) {
		uprintf("Could not cycle device (D1): %s", WindowsErrorString());
		return FALSE;
	}
	usleep(250 * 1000);
	if (!WriteAuthorized(dev->usb_syspath, "1")) {
		uprintf("Could not cycle device (E1): %s", WindowsErrorString());
		return FALSE;
	}
	return TRUE;
}

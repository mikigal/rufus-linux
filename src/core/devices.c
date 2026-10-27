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
 * The filter chain of GetDevices() (dev.c), in its order, on the properties
 * that the platform code gathered. What depends on Windows only (VHD names,
 * Windows Sandbox, Dev Drives, UEFI:TOGO volumes) is left out, as no Linux
 * device has these properties.
 */

#include <stdlib.h>
#include <string.h>

#include "rufus.h"
#include "devices.h"
#include "resource.h"
#include "localization.h"

static const char* usb_version[USB_SPEED_MAX] = {
	"USB", "USB 1.0", "USB 1.1", "USB 2.1", "USB 3.0", "USB 3.1", "USB 3.2", "USB 4.0", "USB 4.1"
};	// See, USB Forum, it *REALLY* isn't that hard!!!
static const char* usb_speed[USB_SPEED_MAX] = {
	"???", "1.5 Mbps", "12 Mbps", "480 Mbps", "5 Gbps", "10 Gbps", "20 Gbps", "40 Gbps", "80 Gbps"
};

BOOL IsVHDName(const char* name, BOOL enable_vmdk)
{
	// The Hardware IDs of the VHD devices that upstream knows, as Linux names them
	static const char* vhd_name[] = {
		"Arsenal Virtual",
		"KernSafe Virtual",
		"Msft Virtual Disk",
		"BHYVE SATA DISK",
		"VMware Virtual",	// Enabled through a cheat mode, as this lists primary disks on VMWare instances
	};
	size_t i;

	if ((name == NULL) || (name[0] == 0))
		return FALSE;
	for (i = 0; i < ARRAYSIZE(vhd_name) - (enable_vmdk ? 0 : 1); i++) {
		if (strstr(name, vhd_name[i]) != NULL)
			return TRUE;
	}
	return FALSE;
}

BOOL ShouldListDevice(const rufus_device_t* dev, const device_filter_t* filter)
{
	usb_device_props props = dev->props;
	const char* buffer = dev->name;
	char str[16];
	int s, score;

	if ((!props.is_USB) && (!props.is_SCSI)) {
		uuprintf("  Unsupported or disabled by policy");
		return FALSE;
	}

	if (props.is_VHD) {
		uprintf("Found VHD device '%s'", buffer);
	} else if ((props.is_CARD) && ((!props.is_USB) || ((props.vid == 0) && (props.pid == 0)))) {
		uprintf("Found card reader device '%s'", buffer);
	} else if ((!props.is_USB) && (!props.is_UASP) && (props.is_Removable)) {
		if (!filter->list_non_usb_removable_drives) {
			uprintf("Found non-USB removable device '%s' => Eliminated", buffer);
			uuprintf("If you *REALLY* need, you can enable listing of this device with <Ctrl><Alt><F>");
			return FALSE;
		}
		uprintf("Found non-USB removable device '%s'", buffer);
	} else {
		if (props.vid == -1 || props.pid == -1) {
			if (!props.is_USB) {
				// If we have a non removable SCSI drive and couldn't get a VID:PID,
				// we are most likely dealing with a system drive => eliminate it!
				uuprintf("Found non-USB non-removable device '%s' => Eliminated", buffer);
				return FALSE;
			}
			static_strcpy(str, "????:????");	// Couldn't figure VID:PID
		} else {
			static_sprintf(str, "%04X:%04X", props.vid, props.pid);
			// Also ignore USB devices that have been specifically flagged by the user
			for (s = 0; s < MAX_IGNORE_USB; s++) {
				if (filter->ignore_vid_pid[s] != 0 && (props.vid == (int32_t)(filter->ignore_vid_pid[s] >> 16)) &&
					(props.pid == (int32_t)(filter->ignore_vid_pid[s] & 0xffff))) {
					uprintf("Ignoring '%s' (%s), per user settings", buffer, str);
					return FALSE;
				}
			}
		}
		if (props.current_speed >= USB_SPEED_MAX)
			props.current_speed = 0;
		if (props.max_speed >= USB_SPEED_MAX)
			props.max_speed = 0;
		// NB: Upstream appends how the properties were found ("[ID]", "[GP]"), which doesn't apply here
		uprintf("Found %s%s%s device '%s' (%s)", props.is_UASP ? "UAS (" : "",
			usb_version[props.current_speed], props.is_UASP ? ")" : "", buffer, str);
		if (props.max_speed > props.current_speed)
			uprintf("NOTE: This a %s (%s) device operating at %s speed (%s)...", usb_version[props.max_speed],
				usb_speed[props.max_speed], usb_version[props.current_speed], usb_speed[props.current_speed]);
	}

	if (dev->size == 0) {
		uprintf("Device eliminated because it appears to contain no media");
		return FALSE;
	}
	if (dev->size < MIN_DRIVE_SIZE) {
		uprintf("Device eliminated because it is smaller than %s", SizeToHumanReadable(MIN_DRIVE_SIZE, FALSE, FALSE));
		return FALSE;
	}

	// Non removables should have been eliminated above, but since we
	// are potentially dealing with system drives, better safe than sorry
	// NB: Upstream also checks that the mounted volumes of the drive are removable, which they always are here
	if ((props.is_SCSI) && (!props.is_UASP) && (!props.is_VHD) && (!props.is_Removable))
		return FALSE;
	if ((!filter->enable_HDDs) && (!props.is_VHD) && (!props.is_CARD) &&
		((score = IsHDD(!props.is_Removable, dev->size, (uint16_t)props.vid, (uint16_t)props.pid, buffer)) > 0)) {
		uprintf("Device eliminated because it was detected as a Hard Drive or SSD (score %d > 0)", score);
		if (!filter->list_non_usb_removable_drives)
			uprintf("If this device is not a Hard Drive or SSD, please e-mail the author of this application");
		uprintf("NOTE: You can enable the listing of Hard Drives under 'advanced drive properties'");
		return FALSE;
	} else if ((!filter->enable_HDDs) && (props.is_CARD) && (dev->size > MAX_DEFAULT_LIST_CARD_SIZE)) {
		uprintf("Device eliminated because it was detected as a card larger than %s",
			SizeToHumanReadable(MAX_DEFAULT_LIST_CARD_SIZE, FALSE, FALSE));
		uprintf("To use such a card, check 'List USB Hard Drives' under 'advanced drive properties'");
		return FALSE;
	}

	if (props.is_VHD && (!filter->enable_VHDs)) {
		uprintf("Device eliminated because listing of VHDs is disabled (Alt-G)");
		return FALSE;
	}

	// Make sure that we don't list any drive that should not be listed
	// NB: Upstream only checks the drive letters of the drives that have some, so
	// this is done whatever the other options are
	if (dev->is_app_disk || dev->is_system) {
		uprintf("Removing %s from the list: This is the %s!", dev->devnode,
			dev->is_app_disk ? "disk from which " APPLICATION_NAME " is running" : "system disk");
		return FALSE;
	}
	return TRUE;
}

void SetDeviceDisplayName(rufus_device_t* dev, const device_filter_t* filter)
{
	const char* name = strrchr(dev->devnode, '/');
	const char* rtl = (right_to_left_mode) ? RIGHT_TO_LEFT_MARK : "";

	name = (name == NULL) ? dev->devnode : &name[1];
	// NB: Upstream shows the drive letters of the volumes, or the disk number if there are none (MSG_046)
	static_sprintf(dev->display_name, "%s %s(%s)%s [%s]", (dev->nb_volumes > 1) ? lmprintf(MSG_047) : dev->label,
		rtl, name, rtl, SizeToHumanReadable(dev->size, FALSE, filter->use_fake_units));
}

void SortDevices(rufus_device_t* devs, int nb_devs)
{
	int u, v, min_index;
	uint64_t min_drive_size;
	rufus_device_t tmp;

	// Reorder the drives by increasing size, using the "selection sort" algorithm
	for (u = 0; u < nb_devs - 1; u++) {
		min_drive_size = devs[u].size;
		min_index = u;
		for (v = u + 1; v < nb_devs; v++) {
			if (devs[v].size < min_drive_size) {
				min_drive_size = devs[v].size;
				min_index = v;
			}
		}
		if (min_index != u) {
			tmp = devs[u];
			devs[u] = devs[min_index];
			devs[min_index] = tmp;
		}
	}
}

void GetRulesDrive(const rufus_device_t* dev, rules_drive_t* drive)
{
	memset(drive, 0, sizeof(*drive));
	drive->disk_size = dev->size;
	drive->sector_size = dev->sector_size;
	drive->partition_style = dev->partition_style;
	// Windows reports flash drives as removable media and hard drives as fixed, from the same SCSI bit
	drive->is_fixed = !dev->props.is_Removable;
	static_strcpy(drive->fs_name, dev->fs_name);
	static_strcpy(drive->label, dev->label);
	// Set a proposed label according to the size (eg: "256 MB", "8 GB")
	static_strcpy(drive->proposed_label, SizeToHumanReadable(dev->size, FALSE, TRUE));
}

const char* GetFsNameFromBlkid(const char* type, const char* version)
{
	if (type == NULL)
		return "";
	if (strcmp(type, "vfat") == 0)
		return (safe_strcmp(version, "FAT32") == 0) ? "FAT32" : "FAT";
	if (strcmp(type, "ntfs") == 0)
		return "NTFS";
	if (strcmp(type, "exfat") == 0)
		return "exFAT";
	if (strcmp(type, "udf") == 0)
		return "UDF";
	if (strcmp(type, "refs") == 0)
		return "ReFS";
	if ((strcmp(type, "ext2") == 0) || (strcmp(type, "ext3") == 0) || (strcmp(type, "ext4") == 0))
		return type;
	return "";
}

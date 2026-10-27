/*
 * Rufus: The Reliable USB Formatting Utility
 * Boot check
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

#include "rufus.h"

enum bootcheck_return {
	BOOTCHECK_PROCEED = 0,
	BOOTCHECK_CANCEL = -1,
	BOOTCHECK_DOWNLOAD_ERROR = -2,
	BOOTCHECK_GENERAL_ERROR = -3,
};

/* What the boot check needs to know about the target, besides opts.disk_size */
typedef struct {
	const char* path;				// Block device of the drive (e.g. "/dev/sdb"), NULL if there is none
	BOOL is_fixed;					// Fixed rather than removable media (SelectedDrive.MediaType)
} bootcheck_drive_t;

/*
 * Check that the options of START (opts, from rules_get_options()) can be used
 * with the image and the drive, ask the user what still needs to be decided
 * (through ui_ask()), and fetch the files that the bootloaders need. The
 * decisions go to opts (write_as_image, write_as_esp, unattend_xml_flags,
 * append_silent, use_own_c32), the unattend.xml to unattend_xml_path and the
 * bootloader files to grub2_buf and syslinux_ldlinux_len (from app_data_dir).
 */
extern enum bootcheck_return BootCheck(const bootcheck_drive_t* drive);

/*
 * Run BootCheck() in a thread, which reports its result with a UM_FORMAT_START
 * event. The drive is copied. Returns FALSE if the thread can't be started.
 */
extern BOOL StartBootCheck(const bootcheck_drive_t* drive);

/* Whether image_path is stored on the drive (block device) 'target' */
extern BOOL IsSourceImageLocatedOnTargetDrive(const char* target);

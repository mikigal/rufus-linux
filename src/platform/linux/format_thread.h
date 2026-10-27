/*
 * Rufus: The Reliable USB Formatting Utility
 * Format thread
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

/* The drive to format, and what the operation needs besides opts and img_report */
typedef struct {
	const char* devnode;			// e.g. "/dev/sdb"
	uint64_t diskseq;				// Of the drive that was selected (0 if the kernel has none)
	const char* syspath;			// sysfs path of the drive, for the automount suppression
	int image_fd;					// The image to write in DD mode, or -1
	BOOL private_mounts;			// Mount the volumes in a mount namespace of the process (single threaded callers only)
} format_target_t;

/*
 * Format a drive with opts, as FormatThread() does on Windows. image_path is
 * the image to extract (a "/proc/self/fd/N" path will do). Returns TRUE on
 * success; ErrorStatus has the reason otherwise. Must run as root.
 */
extern BOOL FormatDrive(const format_target_t* target);

/* Run FormatDrive() in a thread, which reports UM_FORMAT_COMPLETED once done */
extern BOOL StartFormat(const format_target_t* target);

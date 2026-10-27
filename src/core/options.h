/*
 * Rufus: The Reliable USB Formatting Utility
 * Options of a format operation
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
 * On Windows, the format thread reads the main window controls (file system,
 * cluster size, label, check boxes...) and a set of globals that the dialog
 * and the cheat modes modify at any time. Here, everything a format operation
 * depends on is captured once, when START is pressed, into the 'opts'
 * snapshot, which the boot check then completes with its own decisions, and
 * which the worker code only reads. This is also what the GUI will send to
 * the privileged helper.
 *
 * Files are not part of the options: the image (image_path), the extra
 * archive (archive_path) and the unattend.xml (unattend_xml_path) are the
 * context of the process that runs the operation, and the helper will get
 * them as file descriptors. The same goes for the image report (img_report)
 * and the drive properties (SelectedDrive), which the helper establishes
 * again itself.
 *
 * The fields keep the names of the globals or controls they replace.
 */

#pragma once

#include "rufus.h"

typedef struct {
	/* The target drive */
	DWORD DeviceNum;				// Drive index (DRIVE_INDEX_MIN and up), as stored in the device list
	int device_index;				// Position of the drive in the device list
	uint64_t disk_size;				// Size of the drive the options were selected for

	/* Main window selections (IDC_*) */
	int boot_type;					// BT_* (also the selection_default of the operation)
	int partition_type;				// PARTITION_STYLE_*
	int target_type;				// TT_*
	int fs_type;					// FS_*, or FS_UNKNOWN
	uint32_t cluster_size;			// In bytes, or SINGLE_CLUSTERSIZE_DEFAULT for the file system default
	char label[192];				// Volume label, as entered (UTF-8)
	BOOL quick_format;
	BOOL extended_label;			// Create extended label and icon files
	BOOL old_bios_fixes;			// Add fixes for old BIOSes (extra partition, alignment, etc.)
	BOOL bad_blocks;				// Check device for bad blocks...
	int nb_passes_sel;				// ...with this entry of the passes dropdown (0-1: 1-2 passes, 2-4: SLC/MLC/TLC)
	int image_option;				// IMOP_WIN_STANDARD or IMOP_WIN_TO_GO, -1 if none
	uint8_t image_options;			// IMOP_WINTOGO/IMOP_PERSISTENCE: the image option row that is displayed
	uint64_t persistence_size;		// 0 for no persistence
	BOOL validate_md5sum;			// Runtime UEFI media validation

	/* Decisions of the boot check */
	BOOL write_as_image;			// Write an ISOHybrid in DD mode
	BOOL write_as_esp;				// Write an ISO to an ESP
	int unattend_xml_flags;			// UNATTEND_* options of the Windows User Experience
	BOOL append_silent;				// Append " (SILENT)" to the label of silent install media
	BOOL use_own_c32[NB_OLD_C32];	// Replace obsolete .c32 files with the downloaded ones

	/* Settings and cheat modes */
	BOOL allow_dual_uefi_bios;		// Alt-E
	BOOL force_large_fat32;			// Alt-L
	BOOL enable_ntfs_compression;	// Alt-N
	BOOL enable_file_indexing;		// Alt-Q
	BOOL preserve_timestamps;		// Alt-T
	BOOL zero_drive;				// Alt-Z
	BOOL fast_zeroing;				// Ctrl-Alt-Z
	BOOL lock_drive;				// Alt-, (unset to not lock the drive)
	BOOL use_rufus_mbr;				// Alt-A (unset to not install the Rufus MBR)
	BOOL detect_fakes;				// Alt-B: detect fake drives during the bad blocks check

	/* The environment of the user */
	char locale[64];				// As in LC_CTYPE (e.g. "pl_PL.UTF-8"): the keyboard and codepage of DOS
} rufus_options_t;

/* The options of the current operation */
extern rufus_options_t opts;

/* Set the defaults of a freshly started Rufus */
extern void ResetOptions(rufus_options_t* o);

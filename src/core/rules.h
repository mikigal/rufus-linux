/*
 * Rufus: The Reliable USB Formatting Utility
 * Main window option rules
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
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
 * On Windows, the rules that decide which partition schemes, target systems,
 * file systems, cluster sizes and options are offered for a given drive and
 * image live in rufus.c, where they read and write the dialog controls
 * directly. Here they operate on rules_state_t instead, a plain description
 * of what the main window shows, so that they can be unit tested and shared
 * by every front end. A front end renders the state after each event and
 * forwards the user's actions to the rules_on_*() functions.
 *
 * The functions keep the names, order and behaviour of their rufus.c
 * counterparts (including upstream quirks, which are marked as such), and
 * tests/test_rules.c checks them against the original code.
 */

#pragma once

#include "rufus.h"
#include "options.h"

#define RULES_MAX_ENTRIES           24

/* Entry flags */
#define RULES_ENTRY_DEFAULT         0x01	// Display as "<entry> (Default)" (MSG_030)
#define RULES_ENTRY_LARGE_FAT32     0x02	// File system: display as "Large FAT32"
#define RULES_ENTRY_DUAL_TARGET     0x04	// Target system: "BIOS or UEFI" (MSG_033) rather than "BIOS (or UEFI-CSM)" (MSG_031)
#define RULES_ENTRY_NO_IMAGE        0x08	// Boot selection: "Disk or ISO image (Please select)" rather than the image name

/* Outcome of an image scan */
enum rules_scan_result {
	RULES_SCAN_OK = 0,				// The image can be written
	RULES_SCAN_FAILED,				// The image could not be read or is not an image
	RULES_SCAN_UNSUPPORTED,			// The image has no boot method that Rufus supports
};

/*
 * A dropdown. 'data' is what upstream stores as the item data: BT_* for the
 * boot selection, IMOP_* for the image option, PARTITION_STYLE_* for the
 * partition scheme, TT_* for the target system, FS_* for the file system and
 * the size in bytes for the cluster size (0x100 meaning "default").
 */
typedef struct {
	int count;
	int sel;						// -1 when nothing is selected
	struct {
		int data;
		uint8_t flags;
	} entry[RULES_MAX_ENTRIES];
} rules_combo_t;

/* A check box */
typedef struct {
	BOOL enabled;
	BOOL checked;
	// Used to restore the previous state when a check box gets re-enabled
	uint8_t state;
	BOOL saved;
} rules_check_t;

/* What the rules need to know about the selected drive */
typedef struct {
	uint64_t disk_size;
	uint32_t sector_size;
	int partition_style;			// PARTITION_STYLE_* of the current layout
	BOOL is_fixed;					// Fixed (rather than removable) media
	char fs_name[32];				// File system of the first partition, if any (e.g. "FAT32")
	char label[64];					// Current volume label, or STR_NO_LABEL
	char proposed_label[16];		// Label to use when the drive has none (e.g. "8 GB")
} rules_drive_t;

typedef struct {
	/* Settings and capabilities of the host, set by the front end */
	BOOL advanced_mode_device;
	BOOL advanced_mode_format;
	BOOL allow_dual_uefi_bios;		// Alt-E
	BOOL force_large_fat32;			// Alt-L
	BOOL validate_md5sum;			// Runtime UEFI media validation
	BOOL wintogo_available;			// Windows To Go drives can be created on this host
	BOOL refs_available;			// ReFS can be formatted on this host (fixed media only)
	char no_label_msg[64];			// Localized "New Volume" (MSG_207), which counts as no label

	/* Selected drive */
	BOOL has_device;
	rules_drive_t drive;
	int drive_fs_type;				// SelectedDrive.FSType
	char proposed_label[16];		// SelectedDrive.proposed_label
	struct {
		uint32_t Allowed;
		uint32_t Default;
	} ClusterSize[FS_MAX];

	/* Selected image */
	BOOL has_image;
	RUFUS_IMG_REPORT img;

	/* Dropdowns */
	rules_combo_t boot;
	rules_combo_t image_option;
	rules_combo_t partition;
	rules_combo_t target;
	rules_combo_t fs;
	rules_combo_t cluster;
	int image_index;				// Index of the image entry in the boot selection

	/* Check boxes */
	rules_check_t quick_format;
	rules_check_t extended_label;
	rules_check_t old_bios_fixes;
	rules_check_t uefi_validation;
	rules_check_t bad_blocks;		// Also applies to the passes dropdown
	int nb_passes_sel;				// Selected entry of the passes dropdown (0 to 4, see rufus_options_t)

	/* Enabled state of the other controls */
	BOOL device_enabled;
	BOOL boot_enabled;				// Also applies to the SELECT button and the global toolbars
	BOOL hash_enabled;
	BOOL save_enabled;
	BOOL start_enabled;
	BOOL image_option_enabled;
	BOOL persistence_slider_enabled;
	BOOL persistence_size_enabled;
	BOOL persistence_units_enabled;
	BOOL label_enabled;
	BOOL partition_enabled;			// Also applies to the CSM help indicator
	BOOL target_enabled;
	BOOL fs_enabled;
	BOOL cluster_enabled;

	/* Volume label */
	char label[192];
	BOOL user_changed_label;

	/* Persistence */
	uint64_t persistence_size;
	int persistence_unit_selection;	// 0 = MB, 1 = GB, 2 = TB, -1 = to be proposed
	rules_combo_t persistence_units;	// Data: 0 = MB (MSG_022), 1 = GB (MSG_023), 2 = TB (MSG_024)
	BOOL persistence_units_visible;	// Whether the units dropdown is displayed (persistence enabled)
	uint64_t persistence_max;		// Slider range, in the selected unit
	uint64_t persistence_pos;		// Slider position, in the selected unit

	/* Current selections and sticky user choices (the rufus.c globals) */
	int selection_default;
	int boot_type;
	int partition_type;
	int target_type;
	int fs_type;
	int default_fs;
	int selected_pt;
	int selected_fs;
	int preselected_fs;				// From the command line
	int imop_win_sel;
	uint8_t image_options;			// IMOP_WINTOGO/IMOP_PERSISTENCE: which image option row is displayed
	BOOL has_uefi_csm;
	BOOL op_in_progress;
} rules_state_t;

/* Item data of the selected entry, or -1 (CB_ERR) if none */
static __inline int rules_combo_data(const rules_combo_t* combo) {
	return ((combo->sel < 0) || (combo->sel >= combo->count)) ? -1 : combo->entry[combo->sel].data;
}

/* Set the defaults of a freshly started Rufus, before the window gets created */
extern void rules_init(rules_state_t* m);

/*
 * Events. Each one replicates a code path of the Windows main dialog.
 * 'drive' is NULL when no device is selected.
 */
// InitDialog() + GetDevices() + EnableControls() on startup (and on relaunch)
extern void rules_on_startup(rules_state_t* m, const rules_drive_t* drive);
// A device was selected (IDC_DEVICE)
extern void rules_on_device(rules_state_t* m, const rules_drive_t* drive);
// The device list was refreshed after a hotplug event (WM_DEVICECHANGE)
extern void rules_on_device_refresh(rules_state_t* m, const rules_drive_t* drive);
extern void rules_on_boot_selection(rules_state_t* m, int index);
extern void rules_on_image_option(rules_state_t* m, int index);
extern void rules_on_partition_scheme(rules_state_t* m, int index);
extern void rules_on_target_system(rules_state_t* m, int index);
extern void rules_on_file_system(rules_state_t* m, int index);
extern void rules_on_cluster_size(rules_state_t* m, int index);
extern void rules_on_nb_passes(rules_state_t* m, int index);
extern void rules_on_label(rules_state_t* m, const char* label);
// A check box was clicked: 'check' is one of the rules_check_t of the state
extern void rules_on_check(rules_state_t* m, rules_check_t* check);
extern void rules_on_toggle_advanced_device(rules_state_t* m);
extern void rules_on_toggle_advanced_format(rules_state_t* m);
// Alt-E
extern void rules_on_toggle_dual_uefi_bios(rules_state_t* m);
// Image scanning (ImageScanThread): 'img' is the final report, after the scan has applied
// its own adjustments. 'keep_image_name' only refreshes the image option dropdown.
extern void rules_on_image_scan_start(rules_state_t* m);
extern void rules_on_image_scan_end(rules_state_t* m, const RUFUS_IMG_REPORT* img, enum rules_scan_result result,
	BOOL keep_image_name);
// START was pressed, and the operation completed
extern void rules_on_start(rules_state_t* m);
extern void rules_on_operation_end(rules_state_t* m);

/*
 * Fill the options of a format operation with the selections of the window,
 * as the Windows version reads them on START and from its format thread, and
 * reset the decisions that the boot check makes. The other fields (the drive
 * index and the settings that the window does not hold) are left untouched,
 * so the front end typically copies its settings to 'o' first.
 */
extern void rules_get_options(const rules_state_t* m, rufus_options_t* o);

/* Lower level rules, also used by the events above */
extern void SetAllowedFileSystems(rules_state_t* m, BOOL allowed_filesystem[FS_MAX]);
extern void SetBootOptions(rules_state_t* m);
extern void SetPartitionSchemeAndTargetSystem(rules_state_t* m, BOOL only_target);
extern BOOL SetClusterSizes(rules_state_t* m, int FSType);
extern BOOL SetFileSystemAndClusterSize(rules_state_t* m, const char* fs_name);
extern void SetFSFromISO(rules_state_t* m);
extern void SetProposedLabel(rules_state_t* m, int ComboIndex);
extern void EnableControls(rules_state_t* m, BOOL enable, BOOL remove_checkboxes);
extern void SetPersistenceSize(rules_state_t* m);
extern void ToggleImageOptions(rules_state_t* m);

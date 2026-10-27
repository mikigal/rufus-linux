/*
 * Rufus: The Reliable USB Formatting Utility
 * Reference implementation of the main window rules, from the Windows code
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
 * The oracle (rules_win.c) runs the upstream Windows functions against
 * mock controls. test_rules.c drives it and core/rules.c with the same
 * events and compares what the main window would show, as a snapshot.
 */

#pragma once

#include <stdint.h>

#include "rufus.h"
#include "options.h"

#define SNAP_MAX_ITEMS              32
#define SNAP_TEXT                   256

/* Display strings of the mock lmprintf(): "MSG_xxx", or "MSG_xxx[arg]" for messages that take one */
#define SNAP_IMAGE_NAME             "test.iso"
#define SNAP_SL_V4                  "4.07"
#define SNAP_SL_V6                  "6.04"

typedef struct {
	int count;
	int sel;
	struct {
		char text[SNAP_TEXT];
		intptr_t data;
	} item[SNAP_MAX_ITEMS];
} snap_combo_t;

typedef struct {
	BOOL enabled;
	BOOL checked;
} snap_check_t;

typedef struct {
	snap_combo_t boot, image_option, partition, target, fs, cluster;
	snap_check_t quick_format, extended_label, old_bios_fixes, uefi_validation, bad_blocks;
	int nb_passes_sel;
	BOOL device_enabled, boot_enabled, hash_enabled, save_enabled, start_enabled;
	BOOL image_option_enabled, persistence_slider_enabled, persistence_size_enabled, persistence_units_enabled;
	BOOL persistence_units_visible, label_enabled;
	BOOL partition_enabled, target_enabled, fs_enabled, cluster_enabled;
	char label[SNAP_TEXT];
	int persistence_units, persistence_unit_sel;
	uint64_t persistence_max, persistence_pos, persistence_size;
	/* The rufus.c globals (prefixed, as the oracle renames some of the originals) */
	int g_selection_default, g_boot_type, g_partition_type, g_target_type, g_fs_type;
	int g_selected_pt, g_selected_fs, g_imop_win_sel;
	uint8_t g_image_options;
	BOOL g_has_uefi_csm, g_op_in_progress, g_validate_md5sum, g_user_changed_label;
} snapshot_t;

/* Environment: the same as the rules_state_t settings */
typedef struct {
	BOOL advanced_mode_device, advanced_mode_format, allow_dual_uefi_bios, force_large_fat32;
	BOOL wintogo_available, refs_available;
	int preselected_fs;
} oracle_env_t;

/* A drive, as GetDrivePartitionData() would report it */
typedef struct {
	uint64_t disk_size;
	uint32_t sector_size;
	int partition_style;
	BOOL is_fixed;
	char fs_name[32];
	char label[64];
	char proposed_label[16];
} oracle_drive_t;

enum oracle_check_id { ORACLE_QUICK_FORMAT, ORACLE_EXTENDED_LABEL, ORACLE_OLD_BIOS_FIXES, ORACLE_UEFI_VALIDATION,
	ORACLE_BAD_BLOCKS };

extern void oracle_init(const oracle_env_t* env);
extern void oracle_startup(const oracle_drive_t* drive);
extern void oracle_device(const oracle_drive_t* drive);
extern void oracle_device_refresh(const oracle_drive_t* drive);
extern void oracle_boot_selection(int index);
extern void oracle_image_option(int index);
extern void oracle_partition_scheme(int index);
extern void oracle_target_system(int index);
extern void oracle_file_system(int index);
extern void oracle_cluster_size(int index);
extern void oracle_nb_passes(int index);
extern void oracle_label(const char* label);
extern void oracle_check(enum oracle_check_id id);
extern void oracle_persistence_slider(uint64_t pos);
extern void oracle_persistence_units(int index);
extern void oracle_persistence_size(const char* text);
extern void oracle_toggle_advanced_device(void);
extern void oracle_toggle_advanced_format(void);
extern void oracle_toggle_dual_uefi_bios(void);
extern void oracle_image_scan_start(void);
// 'result' is one of the RULES_SCAN_* values
extern void oracle_image_scan_end(const RUFUS_IMG_REPORT* report, int result, BOOL keep_image_name);
extern void oracle_start(void);
extern void oracle_operation_end(void);
extern void oracle_snapshot(snapshot_t* snap);
// The options that the Windows format thread reads (from the controls and the globals), after START
extern void oracle_options(rufus_options_t* o);
// InitProgress() from ui.c, with or without an extra archive and an unattend.xml
extern void oracle_init_progress(BOOL bOnlyFormat, BOOL has_archive, BOOL has_unattend_xml,
	int nb_slots[OP_MAX], float slot_end[OP_MAX + 1]);

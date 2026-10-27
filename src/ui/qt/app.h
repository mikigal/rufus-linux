/*
 * Rufus: The Reliable USB Formatting Utility
 * Settings, directories and localization of the Qt user interface
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

#include "core.h"

/*
 * The settings and cheat modes of the main window: the rufus.c globals that
 * are not in the core. They get copied to the options of an operation by
 * SetOptionsFromSettings(), and to the rules by the main window.
 */
extern BOOL advanced_mode_device, advanced_mode_format, allow_dual_uefi_bios, force_large_fat32;
extern BOOL enable_HDDs, list_non_usb_removable_drives, use_fake_units, enable_VHDs, enable_vmdk;
extern BOOL enable_ntfs_compression, enable_file_indexing, preserve_timestamps, lock_drive, use_rufus_mbr;
extern BOOL detect_fakes, fast_zeroing, persistent_log, user_deleted_rufus_dir;
extern int preselected_fs, default_thread_priority;
extern uint32_t ignore_vid_pid[MAX_IGNORE_USB];

/* The locale of the user interface, and the .loc file it comes from */
extern loc_cmd* selected_locale;
extern char loc_file[MAX_PATH];

/*
 * Set up app_dir, app_data_dir ($XDG_DATA_HOME, with the downloads in
 * FILES_DIR), temp_dir and ini_file ($XDG_CONFIG_HOME/rufus/rufus.ini).
 */
extern void InitDirectories(const char* argv0);
/* Read the settings of rufus.ini into the globals above (and the core ones) */
extern void LoadSettings(void);
/* SETTING_DARK_MODE: 0 follows the desktop, 1 forces dark and 2 light (Ctrl-Alt-D) */
extern void ApplyColorScheme(void);
/* The settings and cheat modes that an operation depends on */
extern void SetOptionsFromSettings(rufus_options_t* o);
/*
 * Find the .loc file ($RUFUS_LOC, the installed one or the one of the source
 * tree) and the locales it supports. Returns FALSE if there is none.
 */
extern BOOL InitLocalization(void);
/*
 * Select a locale (by name, or from the environment if NULL), and load its
 * messages. Returns FALSE if no locale could be loaded.
 */
extern BOOL SelectLocale(const char* name);
/* Load the messages of selected_locale again (after it changed) */
extern BOOL ApplySelectedLocale(void);
/* The name of the selected locale, e.g. "en-US" */
extern const char* selected_locale_name(void);

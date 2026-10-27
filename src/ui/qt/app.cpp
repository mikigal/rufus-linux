/*
 * Rufus: The Reliable USB Formatting Utility
 * Settings, directories and localization of the Qt user interface
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

#include <limits.h>
#include <algorithm>
#include <stdlib.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStyleHints>
#include <QStandardPaths>

#include <cdio/logging.h>

#include "app.h"

extern "C" {
#include "wue.h"
extern BOOL enable_extra_hashes, ignore_boot_marker;
extern void libcdio_log_handler(cdio_log_level_t level, const char* message);
}

BOOL advanced_mode_device = FALSE, advanced_mode_format = FALSE, allow_dual_uefi_bios = FALSE;
BOOL force_large_fat32 = FALSE, enable_HDDs = FALSE, list_non_usb_removable_drives = FALSE;
BOOL use_fake_units = TRUE, enable_VHDs = TRUE, enable_vmdk = FALSE, enable_ntfs_compression = FALSE;
BOOL enable_file_indexing = FALSE, preserve_timestamps = FALSE, lock_drive = TRUE, use_rufus_mbr = TRUE;
BOOL detect_fakes = TRUE, fast_zeroing = FALSE, persistent_log = FALSE, user_deleted_rufus_dir = FALSE;
int preselected_fs = FS_UNKNOWN, default_thread_priority = THREAD_PRIORITY_ABOVE_NORMAL;
uint32_t ignore_vid_pid[MAX_IGNORE_USB] = { 0 };
loc_cmd* selected_locale = nullptr;
char loc_file[MAX_PATH] = "";

static QByteArray ini_path;

void InitDirectories(const char* argv0)
{
	QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/rufus");
	char path[PATH_MAX];
	ssize_t len;
	QFile ini;

	// The directory of the executable
	len = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (len > 0) {
		path[len] = 0;
		static_strcpy(app_dir, QFileInfo(QString::fromUtf8(path)).absolutePath().toUtf8().constData());
	} else {
		static_strcpy(app_dir, QFileInfo(QString::fromUtf8(argv0)).absolutePath().toUtf8().constData());
	}
	// The files that Rufus downloads go to $XDG_DATA_HOME/Rufus (FILES_DIR), as in %LOCALAPPDATA%\Rufus
	static_strcpy(app_data_dir, data.toUtf8().constData());
	static_strcpy(temp_dir, QDir::tempPath().toUtf8().constData());

	// The settings: $XDG_CONFIG_HOME/rufus/rufus.ini, created if needed
	if (QDir().mkpath(config)) {
		ini.setFileName(config + QStringLiteral("/rufus.ini"));
		if (ini.exists() || ini.open(QIODevice::WriteOnly)) {
			ini.close();
			ini_path = ini.fileName().toUtf8();
			ini_file = ini_path.data();
		}
	}
	uprintf("App dir: '%s'", app_dir);
	uprintf("Dat dir: '%s'", app_data_dir);
	uprintf("Tmp dir: '%s'", temp_dir);
	uprintf("Will use settings from %s", (ini_file != NULL) ? ini_file : "nowhere (they will not be saved)");
}

void LoadSettings(void)
{
	char setting_name[32];
	uint32_t wue_options;
	int i;

	advanced_mode_device = ReadSettingBool(SETTING_ADVANCED_MODE_DEVICE);
	advanced_mode_format = ReadSettingBool(SETTING_ADVANCED_MODE_FORMAT);
	preserve_timestamps = ReadSettingBool(SETTING_PRESERVE_TIMESTAMPS);
	use_fake_units = !ReadSettingBool(SETTING_USE_PROPER_SIZE_UNITS);
	usb_debug = ReadSettingBool(SETTING_ENABLE_USB_DEBUG);
	cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
	use_rufus_mbr = !ReadSettingBool(SETTING_DISABLE_RUFUS_MBR);
	detect_fakes = !ReadSettingBool(SETTING_DISABLE_FAKE_DRIVES_CHECK);
	allow_dual_uefi_bios = ReadSettingBool(SETTING_ENABLE_WIN_DUAL_EFI_BIOS);
	force_large_fat32 = ReadSettingBool(SETTING_FORCE_LARGE_FAT32_FORMAT);
	enable_vmdk = ReadSettingBool(SETTING_ENABLE_VMDK_DETECTION);
	enable_file_indexing = ReadSettingBool(SETTING_ENABLE_FILE_INDEXING);
	enable_VHDs = !ReadSettingBool(SETTING_DISABLE_VHDS);
	enable_extra_hashes = ReadSettingBool(SETTING_ENABLE_EXTRA_HASHES);
	expert_mode = ReadSettingBool(SETTING_EXPERT_MODE);
	ignore_boot_marker = ReadSettingBool(SETTING_IGNORE_BOOT_MARKER);
	persistent_log = ReadSettingBool(SETTING_PERSISTENT_LOG);
	// We want above normal priority by default, so we offset the value
	default_thread_priority = std::clamp((int)ReadSetting32(SETTING_DEFAULT_THREAD_PRIORITY) + THREAD_PRIORITY_ABOVE_NORMAL,
		THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_HIGHEST);
	// This restores the Windows User Experience/unattend.xml mask from the saved user
	// settings, and is designed to work even if we add new options later.
	wue_options = ReadSetting32(SETTING_WUE_OPTIONS);
	if ((wue_options >> 16) != 0) {
		uint32_t mask = wue_options >> 16;
		unattend_xml_mask &= ~mask;
		unattend_xml_mask |= (wue_options & mask);
	}
	// The USB devices to ignore
	for (i = 0; i < MAX_IGNORE_USB; i++) {
		static_sprintf(setting_name, "IgnoreUsb%02d", i + 1);
		ignore_vid_pid[i] = ReadSetting32(setting_name);
	}
	cdio_log_set_handler(libcdio_log_handler);
}

void SetOptionsFromSettings(rufus_options_t* o)
{
	ResetOptions(o);
	o->allow_dual_uefi_bios = allow_dual_uefi_bios;
	o->force_large_fat32 = force_large_fat32;
	o->enable_ntfs_compression = enable_ntfs_compression;
	o->enable_file_indexing = enable_file_indexing;
	o->preserve_timestamps = preserve_timestamps;
	o->fast_zeroing = fast_zeroing;
	o->lock_drive = lock_drive;
	o->use_rufus_mbr = use_rufus_mbr;
	o->detect_fakes = detect_fakes;
	o->thread_priority = default_thread_priority;
}

BOOL InitLocalization(void)
{
	const char* candidates[] = { getenv("RUFUS_LOC"), RUFUS_LOC_INSTALLED, RUFUS_LOC_SOURCE };

	init_localization();
	for (const char* file : candidates) {
		if ((file == NULL) || (access(file, R_OK) != 0))
			continue;
		if (get_supported_locales(file)) {
			static_strcpy(loc_file, file);
			if (file == candidates[0])
				ubprintf("Using external loc file '%s'", loc_file);
			return TRUE;
		}
	}
	return FALSE;
}

BOOL SelectLocale(const char* name)
{
	static const char* locale_vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
	loc_cmd* lcmd = NULL;
	const char* value;

	if ((name != NULL) && (name[0] != 0))
		lcmd = get_locale_from_name((char*)name, FALSE);
	for (size_t i = 0; (lcmd == NULL) && (i < ARRAYSIZE(locale_vars)); i++) {
		value = getenv(locale_vars[i]);
		if ((value != NULL) && (value[0] != 0))
			lcmd = get_locale_from_posix_name(value, FALSE);
		// The first variable that is set is the one that counts
		if (value != NULL && value[0] != 0)
			break;
	}
	if (lcmd == NULL)
		lcmd = get_locale_from_name((char*)"en-US", TRUE);
	if (lcmd == NULL)
		return FALSE;
	selected_locale = lcmd;
	return ApplySelectedLocale();
}

BOOL ApplySelectedLocale(void)
{
	if ((selected_locale == NULL) || !get_loc_data_file(loc_file, selected_locale))
		return FALSE;
	right_to_left_mode = (selected_locale->ctrl_id & LOC_RIGHT_TO_LEFT) ? TRUE : FALSE;
	QGuiApplication::setLayoutDirection(right_to_left_mode ? Qt::RightToLeft : Qt::LeftToRight);
	WriteSettingStr(SETTING_LOCALE, selected_locale->txt[0]);
	ubprintf("Localization set to '%s'", selected_locale->txt[0]);
	return TRUE;
}

void ApplyColorScheme(void)
{
	switch (ReadSetting32(SETTING_DARK_MODE)) {
	case 1:
		QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
		break;
	case 2:
		QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
		break;
	default:
		QGuiApplication::styleHints()->unsetColorScheme();
		break;
	}
}

const char* selected_locale_name(void)
{
	return (selected_locale == NULL) ? "en-US" : selected_locale->txt[0];
}

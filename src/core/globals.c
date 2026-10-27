/*
 * Rufus: The Reliable USB Formatting Utility
 * Globals shared by the core files
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
 * Upstream defines these in rufus.c and format.c, which are not part of the
 * core. They move back next to their users as those files get ported.
 */

#include "rufus.h"

RUFUS_UPDATE update = { 0 };
uint16_t rufus_version[3] = { RUFUS_VERSION_MAJOR, RUFUS_VERSION_MINOR, RUFUS_VERSION_PATCH };
// Directories, which the front end sets up: app_data_dir for the files that Rufus downloads
// (in a FILES_DIR subdirectory) and temp_dir for temporary files
char app_dir[MAX_PATH] = "", app_data_dir[MAX_PATH] = "", temp_dir[MAX_PATH] = "/tmp";
// The settings file (see settings.h), and the settings and cheat modes that the core reads
char* ini_file = NULL;
BOOL usb_debug = FALSE, expert_mode = FALSE, ignore_boot_marker = FALSE;
// Alt-Y and Ctrl-Alt-Y: force the update check to find an update
int force_update = 0;
BOOL size_check = TRUE;			// Alt-S: check the size of the image and the cluster size for MS-DOS
BOOL large_drive = FALSE;		// The drive of the operation is over 1 TB (and may produce short writes)
// The estimated duration of a format, which the front end computes (for UDF)
uint32_t dur_mins = 0, dur_secs = 0;
// Windows To Go can use the 'Windows CA 2023' bootloaders (bcdboot /bootex on Windows)
BOOL bcdboot_supports_ex = FALSE;
// The sector size that the Syslinux libraries work with (from syslinux.c, for 4K support)
uint32_t SECTOR_SHIFT = 9, SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_SHIFT = 9, LIBFAT_SECTOR_SIZE = 512, LIBFAT_SECTOR_MASK = 511;
// Secure Boot revocation data (the embedded defaults from db.h are used if they are NULL)
char *sbat_level_txt = NULL, *sb_active_txt = NULL, *sb_revoked_txt = NULL;
sbat_entry_t* sbat_entries = NULL;
thumbprint_list_t *sb_active_certs = NULL, *sb_revoked_certs = NULL;
// The context of the operation: the image, and the other files it uses (img_report is in iso.c)
char *image_path = NULL, *archive_path = NULL, *unattend_xml_path = NULL;
// The bootloader files that the boot check found in app_data_dir: GRUB's core.img (from
// format.c) and the sizes of Syslinux's ldlinux.sys and ldlinux.bss (from syslinux.c)
uint8_t* grub2_buf = NULL;
long grub2_len = 0;
unsigned long syslinux_ldlinux_len[2] = { 0, 0 };
_Atomic(DWORD) ErrorStatus = 0, LastWriteError = 0;
BOOL right_to_left_mode = FALSE;

void CancelOperation(void)
{
	ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
}

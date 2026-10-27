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
// Directories, which the front end sets up: app_data_dir for the files that Rufus downloads
// (in a FILES_DIR subdirectory) and temp_dir for temporary files
char app_dir[MAX_PATH] = "", app_data_dir[MAX_PATH] = "", temp_dir[MAX_PATH] = "/tmp";
// Settings that the core reads
BOOL usb_debug = FALSE, expert_mode = FALSE, ignore_boot_marker = FALSE;
// The sector size that the Syslinux libraries work with (from syslinux.c, for 4K support)
uint32_t SECTOR_SHIFT = 9, SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_SHIFT = 9, LIBFAT_SECTOR_SIZE = 512, LIBFAT_SECTOR_MASK = 511;
// Secure Boot revocation data (the embedded defaults from db.h are used if they are NULL)
char *sbat_level_txt = NULL, *sb_active_txt = NULL, *sb_revoked_txt = NULL;
sbat_entry_t* sbat_entries = NULL;
thumbprint_list_t *sb_active_certs = NULL, *sb_revoked_certs = NULL;
// The context of the operation: the image, and the other files it uses (img_report is in iso.c)
char *image_path = NULL, *archive_path = NULL, *unattend_xml_path = NULL;
DWORD ErrorStatus = 0, LastWriteError = 0;
BOOL right_to_left_mode = FALSE;

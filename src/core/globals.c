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
// Settings that the core reads
BOOL usb_debug = FALSE, expert_mode = FALSE;
// Secure Boot revocation data (the embedded defaults from db.h are used if they are NULL)
char *sbat_level_txt = NULL, *sb_active_txt = NULL, *sb_revoked_txt = NULL;
sbat_entry_t* sbat_entries = NULL;
thumbprint_list_t *sb_active_certs = NULL, *sb_revoked_certs = NULL;
// TODO: Back to iso.c once it is ported
const char* efi_archname[ARCH_MAX] = { "", "ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64", "ebc" };
// The context of the operation: the image, its report, and the other files it uses
RUFUS_IMG_REPORT img_report = { 0 };
char *image_path = NULL, *archive_path = NULL, *unattend_xml_path = NULL;
DWORD ErrorStatus = 0, LastWriteError = 0;
BOOL right_to_left_mode = FALSE;

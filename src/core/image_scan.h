/*
 * Rufus: The Reliable USB Formatting Utility
 * Image scan
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
#include "rules.h"

/* The file name part of image_path, for display */
extern char* short_image_path;

/*
 * Scan image_path and fill img_report, asking the user about what needs it
 * (through ui_ask()). If the image can't be used, image_path is freed and set
 * to NULL. Unless keep_image_name is set, the image name is displayed on the
 * status bar. The front end calls rules_on_image_scan_start() before, and
 * rules_on_image_scan_end() with the result after. ErrorStatus must be 0 when
 * the scan starts, and the scan is cancelled by setting it to
 * RUFUS_ERROR(ERROR_CANCELLED).
 */
extern enum rules_scan_result ImageScan(BOOL keep_image_name);

/*
 * Reset ErrorStatus and run ImageScan() in a thread, which reports its result
 * with a UM_IMAGE_SCAN_COMPLETED event. Returns FALSE if the thread can't be
 * started.
 */
extern BOOL StartImageScan(BOOL keep_image_name);

/* Check the UEFI bootloaders of the image for Secure Boot signatures and revocations */
extern void GetBootladerInfo(void);

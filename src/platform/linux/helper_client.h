/*
 * Rufus: The Reliable USB Formatting Utility
 * Front end side of rufus-helper
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

#include <sys/types.h>

#include "rufus.h"

typedef struct {
	int sock;						// -1 when not started
	pid_t pid;
} helper_t;

/* A format for the helper to run, with opts, img_report and the boot files of app_data_dir */
typedef struct {
	const char* devnode;			// The drive, as listed (e.g. "/dev/sdb")...
	uint64_t diskseq;				// ...and its diskseq
	int image_fd;					// The image (opened by the user), or -1
	const char* image_name;			// Its name, for the log
	int archive_fd;					// The archive to extract (archive_path), or -1
	int bb_log_fd;					// The report of the bad blocks check (see format_target_t), or -1
	int unattend_fd;				// The unattend.xml of the Windows User Experience (unattend_xml_path), or -1
	const char* bb_log_path;		// Its path, to show to the user
} helper_job_t;

/*
 * Start the helper: $RUFUS_HELPER, or the installed one, through pkexec unless
 * the process already runs as root. 'language' is the Rufus locale (e.g.
 * "en-US") for its messages. Blocks while polkit authenticates the user.
 */
extern BOOL StartHelper(helper_t* h, const char* language);

/*
 * Format a drive in the helper, as FormatDrive() does. Its messages go to the
 * user interface of the process (ui_log(), ui_ask()...), from the calling
 * thread, and cancelling (ErrorStatus) gets forwarded to it. Returns TRUE on
 * success, and ErrorStatus has the reason otherwise.
 */
extern BOOL HelperFormat(helper_t* h, const helper_job_t* job);

/* Stop the helper, which cancels what it is doing */
/*
 * Disconnect a USB drive and connect it again (Alt-C, and after a "device not
 * ready" error), through the helper. Runs in the calling thread.
 */
extern BOOL HelperCyclePort(helper_t* h, const char* devnode, uint64_t diskseq);

/*
 * Save a drive to an image (IDC_SAVE), through the helper: a VHD or VHDX
 * (SAVE_VHD, SAVE_VHDX, see vdisk.c), or an ISO of the files of its first volume
 * (SAVE_ISO, with xorriso). The image is written to image_fd, opened by the
 * caller. Runs in the calling thread, as HelperFormat() does.
 */
#define SAVE_VHD                    1
#define SAVE_VHDX                   2
#define SAVE_ISO                    3
extern BOOL HelperSave(helper_t* h, const char* devnode, uint64_t diskseq, int type, int image_fd, const char* image_name);

extern void StopHelper(helper_t* h);

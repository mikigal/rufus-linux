/*
 * Rufus: The Reliable USB Formatting Utility
 * Progress reporting
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
 * The progress computations of ui.c: the allocation of the progress bar to
 * the operations of a format (OP_*), and the percentage, speed and ETA that
 * are displayed on it. The progress bars themselves belong to the front end,
 * which gets the result through ui_progress() and ui_status().
 */

#include <limits.h>
#include <time.h>

#include "rufus.h"
#include "resource.h"
#include "ui_iface.h"
#include "options.h"
#include "progress.h"

// Number of steps for each FS for FCC_STRUCTURE_PROGRESS
const int nb_steps[FS_MAX] = { 5, 5, 12, 1, 10, 1, 1, 1, 1 };
int update_progress_type = UPT_PERCENT;
int nb_slots[OP_MAX];
float slot_end[OP_MAX+1];	// shifted +1 so that we can subtract 1 to OP indexes
float previous_end;

static uint64_t GetTickCount64(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

// Set up progress bar real estate allocation, from the options of the operation
void InitProgress(BOOL bOnlyFormat)
{
	int i;
	float last_end = 0.0f, slots_discrete = 0.0f, slots_analog = 0.0f;

	memset(nb_slots, 0, sizeof(nb_slots));
	memset(slot_end, 0, sizeof(slot_end));
	previous_end = 0.0f;

	if (bOnlyFormat) {
		nb_slots[OP_FORMAT] = -1;
	} else {
		nb_slots[OP_ANALYZE_MBR] = 1;
		if (opts.bad_blocks) {
			nb_slots[OP_BADBLOCKS] = -1;
		}
		if (opts.boot_type != BT_NON_BOOTABLE) {
			// 1 extra slot for PBR writing
			switch (opts.boot_type) {
			case BT_MSDOS:
				nb_slots[OP_FILE_COPY] = 3 + 1;
				break;
			case BT_FREEDOS:
				nb_slots[OP_FILE_COPY] = 5 + 1;
				break;
			case BT_IMAGE:
				nb_slots[OP_FILE_COPY] = (img_report.is_iso || img_report.is_windows_img) ? -1 : 0;
				if (HAS_WINDOWS(img_report) && (unattend_xml_path != NULL) &&
					(opts.image_option != IMOP_WIN_TO_GO))
					nb_slots[OP_PATCH] = -1;
				break;
			default:
				nb_slots[OP_FILE_COPY] = 2 + 1;
				break;
			}
		}
		if (opts.boot_type == BT_IMAGE && !(img_report.is_iso || img_report.is_windows_img)) {
			nb_slots[OP_FORMAT] = -1;
		} else {
			nb_slots[OP_ZERO_MBR] = 1;
			nb_slots[OP_PARTITION] = 1;
			nb_slots[OP_FIX_MBR] = 1;
			// NB: Upstream uses 2 when formatting through VDS, and has no check on the FS index
			nb_slots[OP_CREATE_FS] = ((opts.fs_type >= 0) && (opts.fs_type < FS_MAX)) ? nb_steps[opts.fs_type] : 0;
			// So, yeah, if you're doing slow format, or using Large FAT32, and have persistence, you'll see
			// the progress bar revert during format on account that we reuse the same operation for both
			// partitions. Maybe one day I'll be bothered to handle two separate OP_FORMAT ops...
			if ((!opts.quick_format) || (opts.persistence_size != 0) || IS_EXT(opts.fs_type) ||
				((opts.fs_type == FS_FAT32) && ((opts.disk_size >= LARGE_FAT32_SIZE) || (opts.force_large_fat32)))) {
				nb_slots[OP_FORMAT] = -1;
				nb_slots[OP_CREATE_FS] = 0;
			}
			nb_slots[OP_FINALIZE] = ((opts.boot_type == BT_IMAGE) && (opts.fs_type == FS_NTFS)) ? 3 : 2;
		}
	}
	if (archive_path != NULL) {
		nb_slots[OP_EXTRACT_ZIP] = -1;
	}

	for (i = 0; i < OP_MAX; i++) {
		if (nb_slots[i] > 0) {
			slots_discrete += nb_slots[i] * 1.0f;
		}
		if (nb_slots[i] < 0) {
			slots_analog += nb_slots[i] * 1.0f;
		}
	}

	for (i = 0; i < OP_MAX; i++) {
		if (nb_slots[i] == 0) {
			slot_end[i + 1] = last_end;
		} else if (nb_slots[i] > 0) {
			slot_end[i + 1] = last_end + (1.0f * nb_slots[i]);
		} else if (nb_slots[i] < 0) {
			slot_end[i + 1] = last_end + (((100.0f - slots_discrete) * nb_slots[i]) / slots_analog);
		}
		last_end = slot_end[i + 1];
	}

	// If there's no analog, adjust our discrete ends to fill the whole bar
	if (slots_analog == 0.0f) {
		for (i = 0; i < OP_MAX; i++) {
			slot_end[i + 1] *= 100.0f / slots_discrete;
		}
	}
}

// Position the progress bar within each operation range
void UpdateProgress(int op, float percent)
{
	int pos;
	static uint64_t LastRefresh = 0;

	if ((op < 0) || (op >= OP_MAX)) {
		duprintf("UpdateProgress: invalid op %d\n", op);
		return;
	}
	if (percent > 100.1f) {
		// duprintf("UpdateProgress(%d): invalid percentage %0.2f\n", op, percent);
		return;
	}
	if ((percent < 0.0f) && (nb_slots[op] <= 0)) {
		duprintf("UpdateProgress(%d): error negative percentage sent for negative slot value\n", op);
		return;
	}
	if (nb_slots[op] == 0)
		return;
	if (previous_end < slot_end[op]) {
		previous_end = slot_end[op];
	}

	if (percent < 0.0f) {
		// Negative means advance one slot (1.0%) - requires a positive slot allocation
		previous_end += (slot_end[op + 1] - slot_end[op]) / (1.0f * nb_slots[op]);
		pos = (int)(previous_end / 100.0f * MAX_PROGRESS);
	} else {
		pos = (int)((previous_end + ((slot_end[op + 1] - previous_end) * (percent / 100.0f))) / 100.0f * MAX_PROGRESS);
	}
	if (pos > MAX_PROGRESS) {
		duprintf("UpdateProgress(%d): rounding error - pos %d is greater than %d", op, pos, MAX_PROGRESS);
		pos = MAX_PROGRESS;
	}

	// Reduce the refresh rate, to avoid weird effects on the sliding part of progress bar
	if (GetTickCount64() > LastRefresh + (2 * MAX_REFRESH)) {
		LastRefresh = GetTickCount64();
		ui_progress(NULL, (uint32_t)pos, TRUE);
	}
}

/*
 * The following is taken from GNU wget (progress.c)
 */
struct bar_progress {
	uint64_t total_length;          // expected total byte count when the download finishes
	uint64_t count;                 // bytes downloaded so far
	uint64_t last_screen_update;    // time of the last screen update, measured since the beginning of download.
	uint64_t dltime;                // download time so far
	// Keep track of recent download speeds.
	struct bar_progress_hist {
		uint64_t pos;
		uint64_t times[SPEED_HISTORY_SIZE];
		uint64_t bytes[SPEED_HISTORY_SIZE];
		// The sum of times and bytes respectively, maintained for efficiency.
		uint64_t total_time;
		uint64_t total_bytes;
	} hist;
	uint64_t recent_start;          // timestamp of beginning of current position.
	uint64_t recent_bytes;          // bytes downloaded so far.
	BOOL stalled;                   // set when no data arrives for longer than STALL_START_TIME, then reset when new data arrives.

	// The following are used to make sure that ETA information doesn't flicker.
	uint64_t last_eta_time;         // time of the last update to download speed and ETA, measured since the beginning of download.
	int last_eta_value;
};

// This code attempts to maintain the notion of a "current" download speed, over the course
// of no less than 3s. (Shorter intervals produce very erratic results.)
//
// To do so, it samples the speed in 150ms intervals and stores the recorded samples in a
// FIFO history ring. The ring stores no more than 20 intervals, hence the history covers
// the period of at least three seconds and at most 20 reads into the past. This method
// should produce reasonable results for downloads ranging from very slow to very fast.
//
// The idea is that for fast downloads, we get the speed over exactly the last three seconds.
// For slow downloads (where a network read takes more than 150ms to complete), we get the
// speed over a larger time period, as large as it takes to complete twenty reads. This is
// good because slow downloads tend to fluctuate more and a 3-second average would be too
// erratic.
static void bar_update(struct bar_progress* bp, uint64_t howmuch, uint64_t dltime)
{
	struct bar_progress_hist* hist = &bp->hist;
	uint64_t recent_age = dltime - bp->recent_start;

	// Update the download count.
	bp->recent_bytes += howmuch;

	// For very small time intervals, we return after having updated the
	// "recent" download count. When its age reaches or exceeds minimum
	// sample time, it will be recorded in the history ring.
	if (recent_age < SPEED_SAMPLE_MIN)
		return;

	if (howmuch == 0) {
		// If we're not downloading anything, we might be stalling,
		// i.e. not downloading anything for an extended period of time.
		// Since 0-reads do not enter the history ring, recent_age
		// effectively measures the time since last read.
		if (recent_age >= STALL_START_TIME) {
			// If we're stalling, reset the ring contents because it's
			// stale and because it will make bar_update stop printing
			// the (bogus) current bandwidth.
			bp->stalled = TRUE;
			memset(hist, 0, sizeof(struct bar_progress_hist));
			bp->recent_bytes = 0;
		}
		return;
	}

	// We now have a non-zero amount of to store to the speed ring.

	// If the stall status was acquired, reset it.
	if (bp->stalled) {
		bp->stalled = FALSE;
		// "recent_age" includes the entire stalled period, which
		// could be very long. Don't update the speed ring with that
		// value because the current bandwidth would start too small.
		// Start with an arbitrary (but more reasonable) time value and
		// let it level out.
		recent_age = 1000;
	}

	// Store "recent" bytes and download time to history ring at the position POS.

	// To correctly maintain the totals, first invalidate existing data
	// (least recent in time) at this position. */
	hist->total_time -= hist->times[hist->pos];
	hist->total_bytes -= hist->bytes[hist->pos];

	// Now store the new data and update the totals.
	hist->times[hist->pos] = recent_age;
	hist->bytes[hist->pos] = bp->recent_bytes;
	hist->total_time += recent_age;
	hist->total_bytes += bp->recent_bytes;

	// Start a new "recent" period.
	bp->recent_start = dltime;
	bp->recent_bytes = 0;

	// Advance the current ring position.
	if (++hist->pos == SPEED_HISTORY_SIZE)
		hist->pos = 0;
}

// This updates the progress bar as well as the data displayed on it so that we can
// display percentage completed, rate of transfer and estimated remaining duration.
// During init (op = OP_INIT) the opaque handle of a dialog that has its own progress
// bar can be passed. Part of the code (eta, speed) comes from GNU wget.
void _UpdateProgressWithInfo(int op, int msg, uint64_t processed, uint64_t total, BOOL force)
{
	static int last_update_progress_type = UPT_PERCENT;
	static struct bar_progress bp = { 0 };
	void* hProgressDialog = (void*)(uintptr_t)processed;
	static void* hProgressBar = NULL;
	static BOOL bProgressBar = FALSE;
	static uint64_t start_time = 0, last_refresh = 0;
	uint64_t speed = 0, current_time = GetTickCount64();
	double percent = 0.0;
	char msg_data[128];
	static BOOL bNoAltMode = FALSE;

	if (op == OP_INIT) {
		start_time = current_time - 1;
		last_refresh = 0;
		last_update_progress_type = UPT_PERCENT;
		percent = 0.0f;
		speed = 0;
		memset(&bp, 0, sizeof(bp));
		bp.total_length = total;
		hProgressBar = NULL;
		bProgressBar = FALSE;
		bNoAltMode = (BOOL)msg;
		if (hProgressDialog != NULL) {
			// Use the progress bar of the dialog provided, if any (UM_PROGRESS_INIT resets it)
			hProgressBar = hProgressDialog;
			bProgressBar = TRUE;
			ui_event(UM_PROGRESS_INIT, (intptr_t)hProgressDialog);
		}
	} else if (bProgressBar || (op > 0)) {
		uint64_t dl_total_time = current_time - start_time;
		uint64_t howmuch = processed - bp.count;
		bp.count = processed;
		bp.total_length = total;
		if (bp.count > bp.total_length)
			bp.total_length = bp.count;
		if (bp.total_length > 0)
			percent = (100.0f * bp.count) / (1.0f * bp.total_length);
		else
			percent = 0.0f;

		if ((bp.hist.total_time > 999) && (bp.hist.total_bytes != 0)) {
			// Calculate the download speed using the history ring and
			// recent data that hasn't made it to the ring yet.
			uint64_t dlquant = bp.hist.total_bytes + bp.recent_bytes;
			uint64_t dltime = bp.hist.total_time + (dl_total_time - bp.recent_start);
			speed = (dltime == 0) ? 0 : (dlquant * 1000) / dltime;
		} else {
			speed = 0;
		}
		bar_update(&bp, howmuch, dl_total_time);

		if (bNoAltMode)
			update_progress_type = UPT_PERCENT;
		switch (update_progress_type) {
		case UPT_SPEED:
			if (speed != 0)
				static_sprintf(msg_data, "%s/s", SizeToHumanReadable(speed, FALSE, FALSE));
			else
				static_sprintf(msg_data, "---");
			break;
		case UPT_ETA:
			if ((bp.total_length > 0) && (bp.count > 0) && (dl_total_time > 3000)) {
				uint32_t eta = 0;

				// Don't change the value of ETA more than approximately once
				// per second; doing so would cause flashing without providing
				// any value to the user.
				if ((bp.total_length != processed) && (bp.last_eta_value != 0) &&
					(dl_total_time - bp.last_eta_time < ETA_REFRESH_INTERVAL)) {
					eta = bp.last_eta_value;
				} else {
					// Calculate ETA using the average download speed to predict
					// the future speed. If you want to use a speed averaged
					// over a more recent period, replace dl_total_time with
					// hist->total_time and bp->count with hist->total_bytes.
					// I found that doing that results in a very jerky and
					// ultimately unreliable ETA.
					uint64_t bytes_remaining = bp.total_length - processed;
					double d_eta = (dl_total_time / 1000.0) * (bytes_remaining * 1.0) / (bp.count * 1.0);
					if (d_eta >= INT_MAX - 1)
						goto skip_eta;
					eta = (uint32_t)(d_eta + 0.5);
					bp.last_eta_value = eta;
					bp.last_eta_time = dl_total_time;
				}
				static_sprintf(msg_data, "%d:%02d:%02d", eta / 3600, (uint16_t)((eta % 3600) / 60), (uint16_t)(eta % 60));
			} else {
			skip_eta:
				static_sprintf(msg_data, "-:--:--");
			}
			break;
		default:
			static_sprintf(msg_data, "%0.1f%%", percent);
			break;
		}
		if ((force) || (bp.count == bp.total_length) || (current_time > last_refresh + MAX_REFRESH)) {
			if (op < 0) {
				ui_progress(hProgressBar, (uint32_t)(MAX_PROGRESS * percent / 100.0f), (op == OP_NOOP_WITH_TASKBAR));
			} else {
				UpdateProgress(op, (float)percent);
			}
			if ((force) || ((msg >= 0) && ((current_time > bp.last_screen_update + SCREEN_REFRESH_INTERVAL) ||
				(last_update_progress_type != update_progress_type) || (bp.count == bp.total_length)))) {
				PrintInfo(0, msg, msg_data);
				bp.last_screen_update = current_time;
			}
			last_refresh = current_time;
		}
		last_update_progress_type = update_progress_type;
	}
}

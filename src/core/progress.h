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

#pragma once

// Types of update progress we report
enum update_progress_type {
	UPT_PERCENT = 0,
	UPT_SPEED,
	UPT_ETA,
	UPT_MAX
};

// Size of the download speed history ring.
#define SPEED_HISTORY_SIZE 20

// The minimum time length of a history sample. By default, each sample is at least 150ms long,
// which means that, over the course of 20 samples, "current" download speed spans at least 3s
// into the past.
#define SPEED_SAMPLE_MIN 150

// The time after which the download starts to be considered "stalled", i.e. the current
// bandwidth is not printed and the recent download speeds are scratched.
#define STALL_START_TIME 5000

// Time between screen refreshes will not be shorter than this.
// NB: In Rufus' case, "screen" means the text overlaid on the progress bar.
#define SCREEN_REFRESH_INTERVAL 200

// Don't refresh the ETA too often to avoid jerkiness in predictions.
// This allows ETA to change approximately once per second.
#define ETA_REFRESH_INTERVAL 990

// What the progress bar displays besides the bar: cycled through by the front end
extern int update_progress_type;
// Share of the progress bar of each operation (> 0: discrete steps, < 0: analog), and where it ends
extern int nb_slots[OP_MAX];
extern float slot_end[OP_MAX+1];
extern float previous_end;

// Allocate the progress bar to the operations that the current options (opts) require
extern void InitProgress(BOOL bOnlyFormat);

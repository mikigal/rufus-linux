/*
 * Rufus: The Reliable USB Formatting Utility
 * USB HDD detection
 * Copyright © 2013-2026 Pete Batard <pete@akeo.ie>
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
 * IsHDD() from smart.c, which also has the S.M.A.R.T. code of the test builds.
 */

#include <string.h>

#include "rufus.h"
#include "hdd_vs_ufd.h"

/*
 * This attempts to detect whether a drive is an USB HDD or an USB Flash Drive (UFD).
 * A positive score means that we think it's an USB HDD, zero or negative means that
 * we think it's an UFD.
 *
 * This is done so that, if someone already has an USB HDD plugged in (say as a
 * backup drive) and plugs an UFD we *try* to do what we can to avoid them formatting
 * that drive by mistake.
 * However, because there is no foolproof (let alone easy) way to differentiate UFDs
 * from HDDs, thanks to every manufacturer, Microsoft, and their mothers, making it
 * exceedingly troublesome to find what type of hardware we are actually accessing,
 * you are expected to pay heed to the following:
 *
 * WARNING: NO PROMISE IS MADE ABOUT THIS ALGORITHM BEING ABLE TO CORRECTLY
 * DIFFERENTIATE AN USB HDD FROM AN USB FLASH DRIVE. MOREOVER, YOU ARE REMINDED THAT
 * THE LICENSE OF THIS APPLICATION MAKES NO PROMISE ABOUT AVOIDING DATA LOSS EITHER
 * (PROVIDED "AS IS").
 * THUS, IF DATA LOSS IS INCURRED DUE TO THIS, OR ANY OTHER PART OF THIS APPLICATION,
 * NOT BEHAVING IN THE MANNER YOU EXPECTED, THE RESPONSIBILITY IS ENTIRELY ON YOU!
 *
 * What you have below, then, is our *current best guess* at differentiating UFDs
 * from HDDs. But short of a crystal ball, this remains just a guess, which may be
 * way off mark. Still, you are also reminded that Rufus does produce PROMINENT
 * warnings before you format a drive, and also provides extensive info about the
 * drive (from the tooltips and the log) => PAY ATTENTION TO THESE OR PAY THE PRICE!
 *
 * But let me just elaborate further on why differentiating UFDs from HDDs is not as
 * 'simple' as it seems:
 * - many USB flash drives manufacturer will present UFDs as non-removable, which used
 *   to be reserved for HDDs => we can't use that as differentiator.
 * - some UFDs (SanDisk Extreme) have added S.M.A.R.T. support, which also used to be
 *   reserved for HDDs => can't use that either
 * - even if S.M.A.R.T. was enough, not all USB->IDE or USB->SATA bridges support ATA
 *   passthrough, which is required S.M.A.R.T. data, and each manufacturer of an
 *   USB<->(S)ATA bridge seem to have their own method of implementing passthrough.
 * - SSDs have also changed the deal completely, as you can get something that looks
 *   like Flash but that is really an HDD.
 * - Some manufacturers (eg. Verbatim) provide both USB Flash Drives and USB HDDs, so
 *   we can't exactly use the VID to say for sure what we're looking at.
 * - Finally, Microsoft is absolutely no help either (which is kind of understandable
 *   from the above) => there is no magic API we can query that will tell us what we're
 *   really looking at.
 */
int IsHDD(BOOL is_fixed, uint64_t drive_size, uint16_t vid, uint16_t pid, const char* strid)
{
	int score = 0;
	size_t i, mlen, ilen, score_list_size = 0;
	BOOL wc;
	int8_t score_list[16];
	char str[64] = { 0 };

	// Boost the score if fixed, as these are *generally* HDDs
	// NB: Upstream gets the drive type and size from the drive index
	if (is_fixed) {
		score_list[score_list_size] = 3;
		score += score_list[score_list_size++];
	}

	// Adjust the score depending on the size
	if (drive_size > 800 * GB) {
		score_list[score_list_size] = 15;
		score += score_list[score_list_size++];
		if (drive_size > 1800 * GB) {
			score_list[score_list_size] = 15;
			score += score_list[score_list_size++];
		}
	} else if (drive_size < 128 * GB) {
		score_list[score_list_size] = -15;
		score += score_list[score_list_size++];
	}

	// Check the string against well known HDD identifiers
	if (strid != NULL) {
		ilen = strlen(strid);
		for (i = 0; i < ARRAYSIZE(str_score); i++) {
			mlen = strlen(str_score[i].name);
			if (mlen > ilen)
				break;
			wc = (str_score[i].name[mlen-1] == '#');
			if ( (_strnicmp(strid, str_score[i].name, mlen-((wc)?1:0)) == 0)
			  && ((!wc) || ((strid[mlen] >= '0') && (strid[mlen] <= '9'))) ) {
				score_list[score_list_size] = str_score[i].score;
				score += score_list[score_list_size++];
				break;
			}
		}
	}

	// Adjust for oddball devices
	if (strid != NULL) {
		for (i = 0; i < ARRAYSIZE(str_adjust); i++)
			if (strcasestr(strid, str_adjust[i].name) != NULL) {
				score_list[score_list_size] = str_adjust[i].score;
				score += score_list[score_list_size++];
			}
	}

	// Check against known VIDs
	for (i = 0; i < ARRAYSIZE(vid_score); i++) {
		if (vid == vid_score[i].vid) {
			score_list[score_list_size] = vid_score[i].score;
			score += score_list[score_list_size++];
			break;
		}
	}

	// Check against known VID:PIDs
	for (i = 0; i < ARRAYSIZE(vidpid_score); i++) {
		if ((vid == vidpid_score[i].vid) && (pid == vidpid_score[i].pid)) {
			score_list[score_list_size] = vidpid_score[i].score;
			score += score_list[score_list_size++];
			break;
		}
	}

	// Print a breakdown of the device score if requested
	if (usb_debug) {
		static_strcat(str, "Device score: ");
		for (i = 0; i < score_list_size; i++)
			safe_sprintf(&str[strlen(str)], sizeof(str) - strlen(str), "%+d", score_list[i]);
		uprintf("%s=%+d → Detected as %s", str, score, (score > 0) ? "HDD" : "UFD");
	}

	return score;
}

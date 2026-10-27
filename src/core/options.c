/*
 * Rufus: The Reliable USB Formatting Utility
 * Options of a format operation
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

#include "rufus.h"
#include "options.h"

rufus_options_t opts;

// The defaults of the rufus.c globals that these options replace
void ResetOptions(rufus_options_t* o)
{
	static const char* locale_vars[] = { "LC_ALL", "LC_CTYPE", "LANG" };
	const char* value;
	int i;

	memset(o, 0, sizeof(*o));
	o->fs_type = FS_UNKNOWN;
	o->image_option = -1;
	o->image_options = IMOP_WINTOGO;
	o->lock_drive = TRUE;
	o->use_rufus_mbr = TRUE;
	o->detect_fakes = TRUE;
	o->thread_priority = THREAD_PRIORITY_ABOVE_NORMAL;
	// The helper gets these from the options it receives, not from its own environment
	for (i = 0; i < ARRAYSIZE(locale_vars); i++) {
		value = getenv(locale_vars[i]);
		if ((value != NULL) && (value[0] != 0)) {
			static_strcpy(o->locale, value);
			break;
		}
	}
}

BOOL CheckOptions(const rufus_options_t* o)
{
	int i;

	if ((memchr(o->label, 0, sizeof(o->label)) == NULL) || (memchr(o->locale, 0, sizeof(o->locale)) == NULL))
		return FALSE;
	if ((o->boot_type < 0) || (o->boot_type >= BT_MAX) ||
		(o->partition_type < PARTITION_STYLE_MBR) || (o->partition_type > PARTITION_STYLE_SFD) ||
		(o->target_type < 0) || (o->target_type >= TT_MAX) ||
		(o->fs_type < FS_UNKNOWN) || (o->fs_type >= FS_MAX))
		return FALSE;
	// The default, or a power of 2 of up to 32 MB (the largest exFAT clusters)
	if ((o->cluster_size != 0) && (o->cluster_size != SINGLE_CLUSTERSIZE_DEFAULT) &&
		(((o->cluster_size & (o->cluster_size - 1)) != 0) || (o->cluster_size < 512) || (o->cluster_size > 32 * MB)))
		return FALSE;
	if ((o->nb_passes_sel < 0) || (o->nb_passes_sel > 4) ||
		(o->image_option < -1) || (o->image_option > IMOP_WIN_TO_GO) ||
		(o->persistence_size > o->disk_size) ||
		(o->thread_priority < THREAD_PRIORITY_LOWEST) || (o->thread_priority > THREAD_PRIORITY_HIGHEST))
		return FALSE;
	for (i = 0; i < NB_OLD_C32; i++) {
		if ((o->use_own_c32[i] != FALSE) && (o->use_own_c32[i] != TRUE))
			return FALSE;
	}
	return TRUE;
}

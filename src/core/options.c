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
	// The helper gets these from the options it receives, not from its own environment
	for (i = 0; i < ARRAYSIZE(locale_vars); i++) {
		value = getenv(locale_vars[i]);
		if ((value != NULL) && (value[0] != 0)) {
			static_strcpy(o->locale, value);
			break;
		}
	}
}

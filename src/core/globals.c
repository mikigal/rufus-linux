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
DWORD ErrorStatus = 0, LastWriteError = 0;
BOOL right_to_left_mode = FALSE;

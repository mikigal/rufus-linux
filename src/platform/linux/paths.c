/*
 * Rufus: The Reliable USB Formatting Utility
 * The files of the installation, relative to the executable
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

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"

BOOL GetInstalledFile(const char* relative, char* path, size_t size)
{
	char exe[PATH_MAX];
	char* slash;
	ssize_t len;

	len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len <= 0)
		return FALSE;
	exe[len] = 0;
	slash = strrchr(exe, '/');
	if (slash == NULL)
		return FALSE;
	*slash = 0;
	if ((size_t)snprintf(path, size, "%s/%s", exe, relative) >= size)
		return FALSE;
	return (access(path, F_OK) == 0);
}

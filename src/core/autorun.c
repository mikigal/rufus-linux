/*
 * Rufus: The Reliable USB Formatting Utility
 * Extended labels
 * Copyright © 2012-2026 Pete Batard <pete@akeo.ie>
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
 * SetAutorun() of icon.c: an autorun.inf with the label, as entered (it may
 * be longer than the file system allows, or not be in English), and the icon
 * of Rufus. Upstream writes it in UTF-16LE text mode (with a BOM and CR/LF line
 * endings), from the label of the main window, and extracts the icon from
 * its executable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "resource.h"

// UTF-8 to UTF-16LE, with CR/LF line endings. Returns the number of bytes.
static size_t ToUtf16le(const char* src, uint8_t* dst)
{
	const uint8_t* p = (const uint8_t*)src;
	uint32_t c;
	size_t n = 0;

	while (*p != 0) {
		if (p[0] < 0x80) {
			c = *p++;
		} else if (((p[0] & 0xe0) == 0xc0) && (p[1] != 0)) {
			c = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
			p += 2;
		} else if (((p[0] & 0xf0) == 0xe0) && (p[1] != 0) && (p[2] != 0)) {
			c = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
			p += 3;
		} else if (((p[0] & 0xf8) == 0xf0) && (p[1] != 0) && (p[2] != 0) && (p[3] != 0)) {
			c = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
			p += 4;
		} else {
			c = 0xfffd;
			p++;
		}
		if (c == '\n') {
			dst[n++] = '\r';
			dst[n++] = 0;
		}
		if (c >= 0x10000) {
			c -= 0x10000;
			dst[n++] = (uint8_t)(0xd800 + (c >> 10));
			dst[n++] = (uint8_t)((0xd800 + (c >> 10)) >> 8);
			c = 0xdc00 + (c & 0x3ff);
		}
		dst[n++] = (uint8_t)c;
		dst[n++] = (uint8_t)(c >> 8);
	}
	return n;
}

BOOL SetAutorun(const char* path)
{
	char filename[MAX_PATH], text[512];
	uint8_t* buf, *icon;
	uint32_t icon_size;
	size_t len;
	BOOL r;

	static_sprintf(filename, "%s/autorun.inf", path);
	// If there's an existing autorun, don't overwrite
	if (access(filename, F_OK) == 0) {
		uprintf("%s already exists - keeping it", filename);
		return FALSE;
	}
	static_sprintf(text, "; Created by " APPLICATION_NAME " %d.%d.%d\n; " RUFUS_URL "\n[autorun]\nicon  = autorun.ico\nlabel = %s\n",
		rufus_version[0], rufus_version[1], rufus_version[2], opts.label);
	buf = malloc(4 * strlen(text) + 2);
	if (buf == NULL)
		return FALSE;
	buf[0] = 0xff;
	buf[1] = 0xfe;
	len = 2 + ToUtf16le(text, &buf[2]);
	r = (write_file(filename, buf, (uint32_t)len) == len);
	free(buf);
	if (!r) {
		uprintf("Unable to create %s", filename);
		return FALSE;
	}
	uprintf("Created: %s", filename);

	// .inf -> .ico
	static_sprintf(filename, "%s/autorun.ico", path);
	icon = GetResource(IDR_RUFUS_ICON, "rufus.ico", &icon_size, FALSE);
	return (icon != NULL) && (write_file(filename, icon, icon_size) == icon_size);
}

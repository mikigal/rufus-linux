/*
 * Rufus: The Reliable USB Formatting Utility
 * The regional options of this system, as Windows names them
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

// The Windows name of an IANA time zone ("Europe/Warsaw" is "Central European Standard Time"), or NULL
extern const char* IanaToWindowsTimeZone(const char* iana);
// The IANA time zone of the system ($TZ, /etc/localtime or /etc/timezone)
extern BOOL GetSystemTimeZone(char* iana, size_t len);

// The locale of a category of the user ("LC_TIME"...): LC_ALL, then the category, then LANG, then "C"
extern const char* GetLocaleCategory(const char* category);
// A POSIX locale ("pl_PL.UTF-8", "sr_RS@latin") as a BCP-47 name ("pl-PL", "sr-Latn-RS")
extern BOOL LocaleToBcp47(const char* locale, char* name, size_t len);

// The Windows keyboard layout ID (KLID) of an XKB layout and variant, or 0 if unknown
extern uint32_t XkbToKlid(const char* layout, const char* variant);
// The first XKB layout of the user (from KDE's kxkbrc, or the configuration of localed)
extern BOOL GetXkbLayout(char* layout, size_t layout_len, char* variant, size_t variant_len);
// The input locale of the user, as the unattend.xml of Windows wants it ("0415:00000415")
extern BOOL GetInputLocale(char* input_locale, size_t len);

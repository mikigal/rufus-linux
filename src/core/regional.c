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

/*
 * What the "Set regional options to the same values as this user's" option of
 * the Windows User Experience duplicates (see LINUX_PORT.md §5.6), which wue.c
 * gets from the Windows APIs upstream:
 * - the time zone: the IANA zone of the system ($TZ or /etc/localtime), with its
 *   Windows name from the windowsZones table of Unicode CLDR;
 * - the locales: the POSIX locales of the user (LC_ALL, LC_*, LANG) as BCP-47
 *   names ("pl_PL.UTF-8" is "pl-PL");
 * - the keyboard: the first XKB layout of the user (KDE's kxkbrc, then the system
 *   configuration of localed), as a Windows keyboard layout ID (KLID).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "dos.h"
#include "regional.h"
#include "windows_zones.h"

/* The zoneinfo directories that /etc/localtime may point to */
static const char* zoneinfo_dirs[] = { "/usr/share/zoneinfo/", "/usr/lib/zoneinfo/", "/usr/share/lib/zoneinfo/" };

typedef __typeof__(windows_zones[0]) windows_zone_t;

static int compare_zone(const void* key, const void* entry)
{
	return strcmp((const char*)key, ((const windows_zone_t*)entry)->iana);
}

const char* IanaToWindowsTimeZone(const char* iana)
{
	const windows_zone_t* zone;

	if (iana == NULL)
		return NULL;
	zone = bsearch(iana, windows_zones, ARRAYSIZE(windows_zones), sizeof(windows_zones[0]), compare_zone);
	return (zone == NULL) ? NULL : zone->windows;
}

// The name of a zone from a path in a zoneinfo directory ("posix/" and "right/" are variants of the same zones)
static BOOL ZoneFromPath(const char* path, char* iana, size_t len)
{
	const char* p;
	size_t i;

	for (i = 0; i < ARRAYSIZE(zoneinfo_dirs); i++) {
		p = strstr(path, zoneinfo_dirs[i]);
		if (p != NULL) {
			p += strlen(zoneinfo_dirs[i]);
			if (strncmp(p, "posix/", 6) == 0 || strncmp(p, "right/", 6) == 0)
				p += 6;
			snprintf(iana, len, "%s", p);
			return (iana[0] != 0);
		}
	}
	return FALSE;
}

BOOL GetSystemTimeZone(char* iana, size_t len)
{
	const char* tz = getenv("TZ");
	char path[MAX_PATH], *buf = NULL;
	ssize_t r;

	// $TZ, if it names a zone (":Europe/Warsaw", "Europe/Warsaw" or a path), rather than a POSIX rule
	if (tz != NULL && tz[0] != 0) {
		if (tz[0] == ':')
			tz++;
		if (tz[0] == '/')
			return ZoneFromPath(tz, iana, len);
		if (strchr(tz, '/') != NULL || IanaToWindowsTimeZone(tz) != NULL) {
			snprintf(iana, len, "%s", tz);
			return TRUE;
		}
	}
	// The link of /etc/localtime (that systemd's timedatectl sets)
	r = readlink("/etc/localtime", path, sizeof(path) - 1);
	if (r > 0) {
		path[r] = 0;
		if (ZoneFromPath(path, iana, len))
			return TRUE;
	}
	// Debian's /etc/timezone
	if (read_file("/etc/timezone", (uint8_t**)&buf) != 0) {
		buf[strcspn(buf, " \t\r\n")] = 0;
		snprintf(iana, len, "%s", buf);
		free(buf);
		return (iana[0] != 0);
	}
	return FALSE;
}

const char* GetLocaleCategory(const char* category)
{
	const char* value = getenv("LC_ALL");

	if (value == NULL || value[0] == 0)
		value = (category == NULL) ? NULL : getenv(category);
	if (value == NULL || value[0] == 0)
		value = getenv("LANG");
	if (value == NULL || value[0] == 0)
		value = "C";
	return value;
}

/*
 * The region of the locales that have none ("de" or "C"): the one of the language,
 * except for the languages where it differs.
 */
static const struct { const char* lang; const char* region; } default_regions[] = {
	{ "ar", "SA" }, { "be", "BY" }, { "bs", "BA" }, { "ca", "ES" }, { "cs", "CZ" }, { "cy", "GB" },
	{ "da", "DK" }, { "el", "GR" }, { "en", "US" }, { "et", "EE" }, { "eu", "ES" }, { "fa", "IR" },
	{ "ga", "IE" }, { "gl", "ES" }, { "he", "IL" }, { "hi", "IN" }, { "ja", "JP" }, { "kk", "KZ" },
	{ "ko", "KR" }, { "ms", "MY" }, { "nb", "NO" }, { "nn", "NO" }, { "sl", "SI" }, { "sq", "AL" },
	{ "sr", "RS" }, { "sv", "SE" }, { "uk", "UA" }, { "ur", "PK" }, { "vi", "VN" }, { "zh", "CN" },
};

BOOL LocaleToBcp47(const char* locale, char* name, size_t len)
{
	char lang[16], region[8] = "", modifier[16] = "";
	const char *script = NULL, *p;
	size_t n, i;

	if (locale == NULL || locale[0] == 0 || strcmp(locale, "C") == 0 || strncmp(locale, "C.", 2) == 0 ||
		strcmp(locale, "POSIX") == 0) {
		snprintf(name, len, "en-US");
		return TRUE;
	}
	n = strcspn(locale, "_.@");
	if (n == 0 || n >= sizeof(lang))
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!isalpha((unsigned char)locale[i]))
			return FALSE;
		lang[i] = (char)tolower((unsigned char)locale[i]);
	}
	lang[n] = 0;
	if (locale[n] == '_') {
		p = &locale[n + 1];
		n = strcspn(p, ".@");
		if (n == 0 || n >= sizeof(region))
			return FALSE;
		for (i = 0; i < n; i++)
			region[i] = (char)toupper((unsigned char)p[i]);
		region[n] = 0;
	}
	p = strchr(locale, '@');
	if (p != NULL)
		snprintf(modifier, sizeof(modifier), "%s", &p[1]);
	// The scripts of the modifiers ("sr_RS@latin" is "sr-Latn-RS")
	if (strcmp(modifier, "latin") == 0)
		script = "Latn";
	else if (strcmp(modifier, "cyrillic") == 0)
		script = "Cyrl";
	if (region[0] == 0) {
		for (i = 0; i < ARRAYSIZE(default_regions) && strcmp(default_regions[i].lang, lang) != 0; i++);
		if (i < ARRAYSIZE(default_regions)) {
			snprintf(region, sizeof(region), "%s", default_regions[i].region);
		} else if (strlen(lang) == 2) {
			region[0] = (char)toupper((unsigned char)lang[0]);
			region[1] = (char)toupper((unsigned char)lang[1]);
			region[2] = 0;
		}
	}
	if (script != NULL)
		snprintf(name, len, "%s-%s%s%s", lang, script, region[0] ? "-" : "", region);
	else
		snprintf(name, len, "%s%s%s", lang, region[0] ? "-" : "", region);
	return TRUE;
}

/*
 * The Windows keyboard layouts (KLID) of the XKB layouts and variants. A layout
 * without a variant also stands for the variants that are not listed.
 */
static const struct {
	const char* layout;
	const char* variant;
	uint32_t klid;
} xkb_layouts[] = {
	{ "al", NULL, 0x0000041c },			// Albanian
	{ "am", NULL, 0x0002042b },			// Armenian Phonetic
	{ "ara", NULL, 0x00000401 },		// Arabic (101)
	{ "at", NULL, 0x00000407 },			// German
	{ "az", NULL, 0x0000042c },			// Azerbaijani Latin
	{ "ba", NULL, 0x0000141a },			// Bosnian (Latin)
	{ "be", NULL, 0x0000080c },			// Belgian French
	{ "bg", "phonetic", 0x00040402 },	// Bulgarian (Phonetic Traditional)
	{ "bg", NULL, 0x00030402 },			// Bulgarian
	{ "br", NULL, 0x00000416 },			// Portuguese (Brazil ABNT)
	{ "by", NULL, 0x00000423 },			// Belarusian
	{ "ca", "eng", 0x00000409 },		// US
	{ "ca", "multix", 0x00011009 },		// Canadian Multilingual Standard
	{ "ca", NULL, 0x00001009 },			// Canadian French
	{ "ch", "fr", 0x0000100c },			// Swiss French
	{ "ch", NULL, 0x00000807 },			// Swiss German
	{ "cn", NULL, 0x00000804 },			// Chinese (Simplified) - US
	{ "cz", "qwerty", 0x00010405 },		// Czech (QWERTY)
	{ "cz", NULL, 0x00000405 },			// Czech
	{ "de", NULL, 0x00000407 },			// German
	{ "dk", NULL, 0x00000406 },			// Danish
	{ "ee", NULL, 0x00000425 },			// Estonian
	{ "es", NULL, 0x0000040a },			// Spanish
	{ "fi", NULL, 0x0000040b },			// Finnish
	{ "fo", NULL, 0x00000438 },			// Faeroese
	{ "fr", NULL, 0x0000040c },			// French
	{ "gb", "extd", 0x00000452 },		// United Kingdom Extended
	{ "gb", NULL, 0x00000809 },			// United Kingdom
	{ "ge", NULL, 0x00000437 },			// Georgian
	{ "gr", NULL, 0x00000408 },			// Greek
	{ "hr", NULL, 0x0000041a },			// Croatian
	{ "hu", NULL, 0x0000040e },			// Hungarian
	{ "ie", NULL, 0x00001809 },			// Irish
	{ "il", NULL, 0x0000040d },			// Hebrew
	{ "in", NULL, 0x00010439 },			// Hindi Traditional
	{ "ir", NULL, 0x00000429 },			// Persian
	{ "is", NULL, 0x0000040f },			// Icelandic
	{ "it", NULL, 0x00000410 },			// Italian
	{ "jp", NULL, 0x00000411 },			// Japanese
	{ "kg", NULL, 0x00000440 },			// Kyrgyz Cyrillic
	{ "kr", NULL, 0x00000412 },			// Korean
	{ "kz", NULL, 0x0000043f },			// Kazakh
	{ "latam", NULL, 0x0000080a },		// Latin American
	{ "lt", NULL, 0x00010427 },			// Lithuanian
	{ "lv", NULL, 0x00020426 },			// Latvian (Standard)
	{ "mk", NULL, 0x0000042f },			// Macedonian
	{ "mn", NULL, 0x00000450 },			// Mongolian Cyrillic
	{ "mt", NULL, 0x0000043a },			// Maltese 47-Key
	{ "nl", NULL, 0x00000413 },			// Dutch
	{ "no", NULL, 0x00000414 },			// Norwegian
	{ "pl", "qwertz", 0x00010415 },		// Polish (214)
	{ "pl", NULL, 0x00000415 },			// Polish (Programmers)
	{ "pt", NULL, 0x00000816 },			// Portuguese
	{ "ro", NULL, 0x00010418 },			// Romanian (Standard)
	{ "rs", "latin", 0x0000081a },		// Serbian (Latin)
	{ "rs", NULL, 0x00000c1a },			// Serbian (Cyrillic)
	{ "ru", "phonetic", 0x00020419 },	// Russian - Mnemonic
	{ "ru", "typewriter", 0x00010419 },	// Russian (Typewriter)
	{ "ru", NULL, 0x00000419 },			// Russian
	{ "se", NULL, 0x0000041d },			// Swedish
	{ "si", NULL, 0x00000424 },			// Slovenian
	{ "sk", "qwerty", 0x0001041b },		// Slovak (QWERTY)
	{ "sk", NULL, 0x0000041b },			// Slovak
	{ "th", NULL, 0x0000041e },			// Thai Kedmanee
	{ "tr", "f", 0x0001041f },			// Turkish F
	{ "tr", NULL, 0x0000041f },			// Turkish Q
	{ "tw", NULL, 0x00000404 },			// Chinese (Traditional) - US
	{ "ua", NULL, 0x00020422 },			// Ukrainian (Enhanced)
	{ "us", "altgr-intl", 0x00020409 },	// United States-International
	{ "us", "dvorak", 0x00010409 },		// United States-Dvorak
	{ "us", "dvorak-l", 0x00030409 },	// United States-Dvorak for left hand
	{ "us", "dvorak-r", 0x00040409 },	// United States-Dvorak for right hand
	{ "us", "intl", 0x00020409 },		// United States-International
	{ "us", NULL, 0x00000409 },			// US
	{ "uz", NULL, 0x00000843 },			// Uzbek Cyrillic
	{ "vn", NULL, 0x0000042a },			// Vietnamese
};

uint32_t XkbToKlid(const char* layout, const char* variant)
{
	size_t i;

	if (layout == NULL)
		return 0;
	if (variant == NULL)
		variant = "";
	for (i = 0; i < ARRAYSIZE(xkb_layouts); i++) {
		if (strcmp(xkb_layouts[i].layout, layout) == 0 &&
			((xkb_layouts[i].variant == NULL) || (strcmp(xkb_layouts[i].variant, variant) == 0)))
			return xkb_layouts[i].klid;
	}
	return 0;
}

// The first entry of a comma separated list, without quotes and spaces
static void FirstEntry(const char* list, char* entry, size_t len)
{
	size_t n;

	while (*list == ' ' || *list == '"')
		list++;
	n = strcspn(list, ",\" \t\r\n");
	snprintf(entry, len, "%.*s", (int)n, list);
}

// The value of "key=value" (or 'Option "key" "value"' for an X.org file) in a configuration file
static BOOL ReadConfigValue(const char* path, const char* section, const char* key, char* value, size_t len)
{
	char line[512], *p, *v;
	BOOL in_section = (section == NULL), found = FALSE;
	size_t key_len = strlen(key);
	FILE* fd = fopen(path, "re");

	if (fd == NULL)
		return FALSE;
	while (!found && fgets(line, sizeof(line), fd) != NULL) {
		for (p = line; *p == ' ' || *p == '\t'; p++);
		if (*p == '[') {
			in_section = (section != NULL) && (strncmp(p, section, strlen(section)) == 0);
			continue;
		}
		if (!in_section)
			continue;
		if (strncmp(p, "Option", 6) == 0) {
			// Option "XkbLayout" "pl"
			v = strchr(p, '"');
			if (v == NULL || strncmp(&v[1], key, key_len) != 0 || v[1 + key_len] != '"')
				continue;
			v = strchr(&v[2 + key_len], '"');
			if (v == NULL)
				continue;
			FirstEntry(&v[1], value, len);
			found = TRUE;
		} else if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
			FirstEntry(&p[key_len + 1], value, len);
			found = TRUE;
		}
	}
	fclose(fd);
	return found;
}

BOOL GetXkbLayout(char* layout, size_t layout_len, char* variant, size_t variant_len)
{
	const char* config = getenv("XDG_CONFIG_HOME");
	const char* home = getenv("HOME");
	char path[MAX_PATH], use[16] = "";

	variant[0] = 0;
	// KDE Plasma: the layouts of the user, unless they are the ones of the system
	if (config != NULL && config[0] == '/')
		snprintf(path, sizeof(path), "%s/kxkbrc", config);
	else if (home != NULL)
		snprintf(path, sizeof(path), "%s/.config/kxkbrc", home);
	else
		path[0] = 0;
	if (path[0] != 0 && ReadConfigValue(path, "[Layout]", "Use", use, sizeof(use)) &&
		strcmp(use, "true") == 0 && ReadConfigValue(path, "[Layout]", "LayoutList", layout, layout_len) &&
		layout[0] != 0) {
		if (!ReadConfigValue(path, "[Layout]", "VariantList", variant, variant_len))
			variant[0] = 0;
		return TRUE;
	}
	// The layout of the system, which systemd-localed (localectl set-x11-keymap) writes
	if (ReadConfigValue("/etc/X11/xorg.conf.d/00-keyboard.conf", NULL, "XkbLayout", layout, layout_len) &&
		layout[0] != 0) {
		if (!ReadConfigValue("/etc/X11/xorg.conf.d/00-keyboard.conf", NULL, "XkbVariant", variant, variant_len))
			variant[0] = 0;
		return TRUE;
	}
	if (ReadConfigValue("/etc/vconsole.conf", NULL, "XKBLAYOUT", layout, layout_len) && layout[0] != 0) {
		if (!ReadConfigValue("/etc/vconsole.conf", NULL, "XKBVARIANT", variant, variant_len))
			variant[0] = 0;
		return TRUE;
	}
	return FALSE;
}

BOOL GetInputLocale(char* input_locale, size_t len)
{
	char layout[32], variant[32];
	uint32_t klid = 0;
	uint16_t langid = GetLocaleLangId(GetLocaleCategory("LC_CTYPE"));

	if (GetXkbLayout(layout, sizeof(layout), variant, sizeof(variant)))
		klid = XkbToKlid(layout, variant);
	// The ID of a language is also the one of its default keyboard layout
	if (klid == 0)
		klid = langid;
	if (klid == 0)
		return FALSE;
	// The language of the input profile is the one of the user, or the one of the layout
	if (langid == 0)
		langid = (uint16_t)klid;
	snprintf(input_locale, len, "%04x:%08x", langid, klid);
	return TRUE;
}

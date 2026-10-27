/*
 * Rufus: The Reliable USB Formatting Utility
 * Tests of the DOS file extraction and of its locale
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

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "test_files.h"

// Extract FreeDOS for a locale, and return its AUTOEXEC.BAT
static const char* freedos(const char* locale)
{
	char* dir = tmp_path(locale);
	char path[MAX_PATH];

	CHECK(mkdir_p(dir, 0755) == 0);
	ResetOptions(&opts);
	opts.boot_type = BT_FREEDOS;
	static_strcpy(opts.locale, locale);
	CHECK(ExtractDOS(dir));
	snprintf(path, sizeof(path), "%s/AUTOEXEC.BAT", dir);
	return file_content(path);
}

static BOOL has_crlf(const char* text)
{
	const char* p;

	for (p = strchr(text, '\n'); p != NULL; p = strchr(p + 1, '\n')) {
		if ((p == text) || (p[-1] != '\r'))
			return FALSE;
	}
	return TRUE;
}

static void test_freedos_files(void)
{
	static const char* files[] = { "KERNEL.SYS", "COMMAND.COM", "CONFIG.SYS", "AUTOEXEC.BAT",
		"LOCALE/KEYB.EXE", "LOCALE/KEYBOARD.SYS", "LOCALE/EGA.CPX", "LOCALE/EGA18.CPX" };
	char path[MAX_PATH];
	size_t i;

	freedos("pl_PL.UTF-8");
	for (i = 0; i < ARRAYSIZE(files); i++) {
		snprintf(path, sizeof(path), "%s/pl_PL.UTF-8/%s", tmp_dir, files[i]);
		CHECK(access(path, F_OK) == 0);
	}
	snprintf(path, sizeof(path), "%s/pl_PL.UTF-8/CONFIG.SYS", tmp_dir);
	CHECK_CONTAINS(file_content(path), "MENU 1) Use Polish keyboard with Central-European codepage [852]");
	CHECK(has_crlf(file_content(path)));
}

static void test_locales(void)
{
	static const struct { const char* locale; const char* keyb; const char* cp; } list[] = {
		{ "pl_PL.UTF-8", "keyb pl,,", "select=852" },
		{ "de_DE.UTF-8", "keyb gr,,", "select=858" },
		{ "de_CH.UTF-8", "keyb sg,,", "select=858" },
		{ "fr_CA.UTF-8", "keyb cf,,", "select=858" },
		{ "ru_RU.UTF-8", "keyb ru,,", "select=866" },
		{ "cs_CZ", "keyb cz,,", "select=852" },
		{ "sr_RS@latin", "keyb yu,,", "select=852" },
		{ "tr_TR.UTF-8", "keyb tr,,", "select=857" },
		{ "el_GR.UTF-8", "keyb gk,,", "select=737" },
	};
	const char* autoexec;
	size_t i;

	for (i = 0; i < ARRAYSIZE(list); i++) {
		autoexec = freedos(list[i].locale);
		CHECK_CONTAINS(autoexec, list[i].keyb);
		CHECK_CONTAINS(autoexec, list[i].cp);
		CHECK(has_crlf(autoexec));
	}

	// US keyboard and codepage: no language menu
	autoexec = freedos("en_US.UTF-8");
	CHECK_CONTAINS(autoexec, "echo Using US-English keyboard with US-English codepage [437]\r\n");
	CHECK(access(tmp_path("en_US.UTF-8/CONFIG.SYS"), F_OK) != 0);
	// As for a locale we don't know about
	CHECK_CONTAINS(freedos("C"), "[437]");
	CHECK_CONTAINS(freedos("xx_YY.UTF-8"), "[437]");
}

static void test_msdos_without_diskcopy(void)
{
	char* dir = tmp_path("msdos");

	CHECK(mkdir_p(dir, 0755) == 0);
	static_strcpy(app_data_dir, tmp_dir);
	ResetOptions(&opts);
	opts.boot_type = BT_MSDOS;
	CHECK(!ExtractDOS(dir));
	CHECK_CONTAINS(log_buf, "'diskcopy.dll' was either not found or is invalid");
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };

	ui_set_iface(&iface);
	if (!test_files_init())
		return 1;
	test_freedos_files();
	test_locales();
	test_msdos_without_diskcopy();
	test_files_cleanup();
	return TEST_RESULT();
}

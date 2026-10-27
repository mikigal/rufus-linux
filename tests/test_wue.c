/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the Windows image analysis and the unattend.xml creation
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
 * The WIMs are created with wimlib, with the image properties (version,
 * names, language) that Windows images have in their XML index, and put in
 * ISOs with xorriso. The questions are answered by a script (test_ui.h).
 */

#include <stdio.h>
#include <stdlib.h>

#include "rufus.h"
#include "options.h"
#include "resource.h"
#include "ui_iface.h"
#include "wue.h"
#include "xml.h"
#include "wimlib.h"
#include "missing.h"
#include "test.h"
#include "test_files.h"
#include "test_ui.h"

typedef struct {
	const char* name;
	const char* display_name;		// NULL to only have a DESCRIPTION, as some unofficial images do
	int major, minor, build, spbuild;
} wim_image_t;

static void set_property(WIMStruct* wim, int index, const char* name, int value)
{
	char str[16];

	snprintf(str, sizeof(str), "%d", value);
	CHECK(wimlib_set_image_property(wim, index, name, str) == 0);
}

static void make_wim(const char* path, const wim_image_t* images, int nb_images, const char* language)
{
	WIMStruct* wim = NULL;
	char str[64];
	int i;

	make_file("wimsrc/Windows/System32/ntoskrnl.exe", "kernel");
	CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) == 0);
	for (i = 0; i < nb_images; i++) {
		CHECK(wimlib_add_image(wim, tmp_path("wimsrc"), images[i].name, NULL, 0) == 0);
		if (images[i].display_name != NULL) {
			CHECK(wimlib_set_image_property(wim, i + 1, "DISPLAYNAME", images[i].display_name) == 0);
		} else {
			snprintf(str, sizeof(str), "%s (description)", images[i].name);
			CHECK(wimlib_set_image_property(wim, i + 1, "DESCRIPTION", str) == 0);
		}
		set_property(wim, i + 1, "WINDOWS/VERSION/MAJOR", images[i].major);
		set_property(wim, i + 1, "WINDOWS/VERSION/MINOR", images[i].minor);
		set_property(wim, i + 1, "WINDOWS/VERSION/BUILD", images[i].build);
		set_property(wim, i + 1, "WINDOWS/VERSION/SPBUILD", images[i].spbuild);
		if (language != NULL)
			CHECK(wimlib_set_image_property(wim, i + 1, "WINDOWS/LANGUAGES/LANGUAGE", language) == 0);
	}
	CHECK(wimlib_write(wim, path, WIMLIB_ALL_IMAGES, 0, 1) == 0);
	wimlib_free(wim);
}

static void make_iso(const char* iso, const char* dir)
{
	const char* xorriso[] = { "xorriso", "-as", "mkisofs", "-quiet", "-o", tmp_path(iso), "-V", "CCCOMA_X64FRE_EN-US_DV9",
		"-J", "-joliet-long", tmp_path(dir), NULL };

	CHECK(run(xorriso));
}

static void scan_iso(const char* iso)
{
	memset(&img_report, 0, sizeof(img_report));
	image_path = tmp_path(iso);
	CHECK(ExtractISO(image_path, "", TRUE));
}

/* The Windows version from the XML index, as upstream adjusts it */
static void test_versions(void)
{
	static const struct {
		int major, minor, build;
		int expected_major, expected_minor;
	} v[] = {
		{ 5, 1, 2600, 0, 0 },		// XP and earlier are not supported
		{ 6, 0, 6002, 0, 0 },		// Neither is Vista
		{ 6, 1, 7601, 7, 0 },
		{ 6, 2, 9200, 8, 0 },
		{ 6, 3, 9600, 8, 1 },
		{ 6, 4, 9841, 10, 0 },		// Windows 10 Technical Preview
		{ 10, 0, 19045, 10, 0 },
		{ 10, 0, 22631, 11, 0 },
	};
	wim_image_t image = { "Windows", "Windows", 0, 0, 0, 1234 };
	char name[32];
	size_t i;

	for (i = 0; i < ARRAYSIZE(v); i++) {
		image.major = v[i].major;
		image.minor = v[i].minor;
		image.build = v[i].build;
		snprintf(name, sizeof(name), "version%zu.wim", i);
		make_wim(tmp_path(name), &image, 1, NULL);
		memset(&img_report, 0, sizeof(img_report));
		img_report.is_windows_img = TRUE;
		image_path = tmp_path(name);
		CHECK_EQ_INT(PopulateWindowsVersion(), v[i].expected_major != 0);
		CHECK_EQ_INT(img_report.win_version.major, v[i].expected_major);
		CHECK_EQ_INT(img_report.win_version.minor, v[i].expected_minor);
		CHECK_EQ_INT(img_report.win_version.build, v[i].build);
		CHECK_EQ_INT(img_report.win_version.revision, 1234);
	}

	// Not a WIM
	make_file("not_a.wim", "This is not a WIM");
	image_path = tmp_path("not_a.wim");
	CHECK(!PopulateWindowsVersion());
}

/* A Windows ISO with three editions in its install.wim */
static const wim_image_t editions[] = {
	{ "Windows 11 Pro", "Windows 11 Pro", 10, 0, 22631, 2861 },
	{ "Windows 10 Home", NULL, 10, 0, 17763, 1 },		// Windows 10 1809
	{ "Windows 10 Education", "Windows 10 Education", 10, 0, 14393, 0 },
};

static void test_editions(void)
{
	StrArray names, indexes;
	// As on Windows ISOs, the language is read from the second image of boot.wim (Setup)
	const wim_image_t boot[] = {
		{ "Microsoft Windows PE (amd64)", "WinPE", 10, 0, 22631, 2861 },
		{ "Microsoft Windows Setup (amd64)", "Setup", 10, 0, 22631, 2861 },
	};

	CHECK(mkdir_p(tmp_path("windows/sources"), 0755) == 0);
	make_wim(tmp_path("windows/sources/install.wim"), editions, ARRAYSIZE(editions), "en-US");
	make_wim(tmp_path("windows/sources/boot.wim"), boot, ARRAYSIZE(boot), "pl-PL");
	make_file("windows/bootmgr", "bootmgr");
	make_iso("windows.iso", "windows");
	scan_iso("windows.iso");
	CHECK_EQ_INT(img_report.wininst_index, 1);
	CHECK_EQ_STR(img_report.wininst_path[0], "/sources/install.wim");

	// The version of the first image of install.wim
	CHECK(PopulateWindowsVersion());
	CHECK_EQ_INT(img_report.win_version.major, 11);
	CHECK_EQ_INT(img_report.win_version.build, 22631);
	CHECK_EQ_INT(img_report.win_version.revision, 2861);

	// The language of boot.wim
	CHECK_EQ_STR(GetUILanguage(), "pl-PL");

	// The editions, for the selection dialog
	StrArrayCreate(&names, 4);
	StrArrayCreate(&indexes, 4);
	answer(0);
	wininst_index = 0;
	log_buf[0] = 0;
	CHECK_EQ_INT(GetEditions(&names, &indexes), 3);
	CHECK_EQ_INT(names.Index, 3);
	CHECK_EQ_STR(names.String[0], "Windows 11 Pro");
	CHECK_EQ_STR(names.String[1], "Windows 10 Home (description)");
	CHECK_EQ_STR(names.String[2], "Windows 10 Education");
	CHECK_EQ_INT(indexes.Index, 3);
	CHECK_EQ_STR(indexes.String[2], "3");
	CHECK_CONTAINS(log_buf, "Nonstandard Windows image");
	CHECK_EQ_INT(unattend_edition_index, 1);
	CHECK_ALL_ANSWERED();
	StrArrayDestroy(&names);
	StrArrayDestroy(&indexes);
}

/* The edition to use for Windows To Go */
static void test_wintogo_index(void)
{
	scan_iso("windows.iso");
	ResetOptions(&opts);

	// Windows To Go requires NTFS
	opts.fs_type = FS_FAT32;
	answer(0);
	CHECK_EQ_INT(SetWinToGoIndex(TRUE), -1);
	CHECK_ALL_ANSWERED();

	// The first edition is preselected (the index that upstream matches with the host edition)
	opts.fs_type = FS_NTFS;
	answer(1, 0x01);
	CHECK_EQ_INT(SetWinToGoIndex(FALSE), 1);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(0, MSG_291));
	CHECK_EQ_INT(asked[0].type, UI_QUESTION_SELECTION);
	CHECK_EQ_INT(asked[0].nb_choices, 3);
	CHECK_EQ_STR(asked[0].choice[1], "Windows 10 Home (description)");
	CHECK_EQ_INT(asked[0].mask, 1 << 1);
	CHECK_EQ_INT(img_report.win_version.build, 22631);

	// Windows 10 1809: a notice about WppRecorder.sys
	answer(2, 0x02, IDCLOSE);
	CHECK_EQ_INT(SetWinToGoIndex(TRUE), 2);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(1, MSG_128));
	CHECK_EQ_INT(asked[1].flags, MB_ICONINFORMATION | MB_CLOSE);
	CHECK_EQ_INT(img_report.win_version.build, 17763);

	// Before the Creators Update, Windows To Go only boots from fixed drives
	answer(2, 0x04, IDNO);
	CHECK_EQ_INT(SetWinToGoIndex(FALSE), -2);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(1, MSG_190));
	answer(2, 0x04, IDYES);
	CHECK_EQ_INT(SetWinToGoIndex(FALSE), 3);
	CHECK_ALL_ANSWERED();
	answer(1, 0x04);
	CHECK_EQ_INT(SetWinToGoIndex(TRUE), 3);
	CHECK_ALL_ANSWERED();

	// Cancelled
	answer(1, -1);
	CHECK_EQ_INT(SetWinToGoIndex(TRUE), -2);
	CHECK_ALL_ANSWERED();
}

/* An ISO with an install.wim per architecture: the user picks the one to use */
static void test_multiple_install_images(void)
{
	StrArray names, indexes;
	const wim_image_t x86 = { "Windows 10 Pro (x86)", "Windows 10 Pro", 10, 0, 19045, 0 };

	CHECK(mkdir_p(tmp_path("dual/x64/sources"), 0755) == 0);
	CHECK(mkdir_p(tmp_path("dual/x86/sources"), 0755) == 0);
	make_wim(tmp_path("dual/x64/sources/install.wim"), editions, 2, NULL);
	make_wim(tmp_path("dual/x86/sources/install.wim"), &x86, 1, NULL);
	make_iso("dual.iso", "dual");
	scan_iso("dual.iso");
	CHECK_EQ_INT(img_report.wininst_index, 2);

	StrArrayCreate(&names, 4);
	StrArrayCreate(&indexes, 4);
	answer(1, 0x02);
	CHECK_EQ_INT(GetEditions(&names, &indexes), 1);
	CHECK_ALL_ANSWERED();
	CHECK(asked_msg(0, MSG_130));
	CHECK_EQ_INT(asked[0].nb_choices, 2);
	CHECK_EQ_STR(asked[0].choice[0], img_report.wininst_path[0]);
	CHECK_EQ_STR(asked[0].choice[1], img_report.wininst_path[1]);
	CHECK_EQ_INT(wininst_index, 1);
	CHECK_EQ_STR(img_report.wininst_path[1], "/x86/sources/install.wim");
	CHECK_EQ_STR(names.String[0], "Windows 10 Pro");
	StrArrayDestroy(&names);
	StrArrayDestroy(&indexes);

	// Cancelled
	StrArrayCreate(&names, 4);
	StrArrayCreate(&indexes, 4);
	answer(1, -1);
	CHECK_EQ_INT(GetEditions(&names, &indexes), -2);
	CHECK_ALL_ANSWERED();
	StrArrayDestroy(&names);
	StrArrayDestroy(&indexes);
	wininst_index = 0;
}

/* Create an unattend.xml, check that it is well formed and return its content */
static char* unattend(int arch, int flags)
{
	char* path = CreateUnattendXml(arch, flags);
	char* xml;
	ezxml_t doc;

	CHECK(path != NULL);
	if (path == NULL)
		return "";
	CHECK_EQ_INT(opts.unattend_xml_flags, flags);
	xml = file_content(path);
	doc = ezxml_parse_str(keep(strdup(xml)), strlen(xml));
	CHECK(doc != NULL);
	if (doc != NULL) {
		CHECK_EQ_STR(ezxml_error(doc), "");
		CHECK_EQ_STR(doc->name, "unattend");
		ezxml_free(doc);
	}
	unlink(path);
	return xml;
}

static void test_unattend(void)
{
	char *xml, section[2048];

	scan_iso("windows.iso");

	// Nothing to do
	opts.unattend_xml_flags = 0;
	CHECK(CreateUnattendXml(ARCH_X86_64, 0) == NULL);
	CHECK(CreateUnattendXml(ARCH_IA_64, UNATTEND_NO_DATA_COLLECTION) == NULL);

	// The default selection
	xml = unattend(ARCH_X86_64, UNATTEND_DEFAULT_SELECTION_MASK);
	CHECK_CONTAINS(xml, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");
	CHECK_CONTAINS(xml, "  <settings pass=\"windowsPE\">\n    <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"amd64\"");
	CHECK_CONTAINS(xml, "<Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>");
	CHECK_CONTAINS(xml, "/v BypassSecureBootCheck ");
	CHECK_CONTAINS(xml, "/v BypassRAMCheck ");
	CHECK_CONTAINS(xml, "  <settings pass=\"specialize\">");
	CHECK_CONTAINS(xml, "/v BypassNRO /t REG_DWORD /d 1 /f");
	CHECK_CONTAINS(xml, "  <settings pass=\"offlineServicing\">");
	CHECK_CONTAINS(xml, "      <SanPolicy>4</SanPolicy>\n");
	CHECK(strstr(xml, "oobeSystem") == NULL);
	// The section that the format removes when it can edit the registry of boot.wim instead
	CHECK(removable_section[0] != 0 && removable_section[1] > removable_section[0]);
	if (removable_section[1] < strlen(xml) && removable_section[1] - removable_section[0] < sizeof(section)) {
		memcpy(section, &xml[removable_section[0]], removable_section[1] - removable_section[0]);
		section[removable_section[1] - removable_section[0]] = 0;
		CHECK(strncmp(section, "      <RunSynchronous>\n", 23) == 0);
		CHECK_EQ_STR(&section[strlen(section) - 24], "      </RunSynchronous>\n");
		CHECK(strstr(section, "BypassRAMCheck") != NULL);
	}

	// A silent install, which takes the language of boot.wim and the selected edition
	unattend_edition_index = 2;
	xml = unattend(ARCH_ARM_64, UNATTEND_SILENT_INSTALL | UNATTEND_DISABLE_BITLOCKER);
	CHECK_CONTAINS(xml, "processorArchitecture=\"arm64\"");
	CHECK_CONTAINS(xml, "        <DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>\n");
	CHECK_CONTAINS(xml, "              <Key>/IMAGE/INDEX</Key>\n              <Value>2</Value>\n");
	CHECK_CONTAINS(xml, "    <component name=\"Microsoft-Windows-International-Core-WinPE\"");
	CHECK_CONTAINS(xml, "      <UILanguage>pl-PL</UILanguage>\n");
	CHECK_CONTAINS(xml, "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n");
	CHECK_CONTAINS(xml, "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n");
	CHECK(strstr(xml, "LabConfig") == NULL);
	unattend_edition_index = 1;

	// A local account: the name is sanitized, and some names are not allowed
	static_strcpy(unattend_username, "  John/Doe  ");
	xml = unattend(ARCH_X86_32, UNATTEND_SET_USER);
	CHECK_CONTAINS(xml, "processorArchitecture=\"x86\"");
	CHECK_CONTAINS(xml, "            <Name>John_Doe</Name>\n");
	CHECK_CONTAINS(xml, "<CommandLine>net user \"John_Doe\" /logonpasswordchg:yes</CommandLine>");
	CHECK_CONTAINS(xml, "<CommandLine>net accounts /maxpwage:unlimited</CommandLine>");
	static_strcpy(unattend_username, "administrator");
	log_buf[0] = 0;
	xml = unattend(ARCH_X86_64, UNATTEND_SET_USER);
	CHECK(strstr(xml, "<UserAccounts>") == NULL);
	CHECK_CONTAINS(log_buf, "'administrator' is not allowed as local account name");
	unattend_username[0] = 0;

	// The regional options of this system are not duplicated yet (stage 4)
	log_buf[0] = 0;
	xml = unattend(ARCH_X86_64, UNATTEND_DUPLICATE_LOCALE | UNATTEND_NO_DATA_COLLECTION);
	CHECK_CONTAINS(xml, "        <ProtectYourPC>3</ProtectYourPC>\n");
	CHECK(strstr(xml, "<TimeZone>") == NULL);
	CHECK(strstr(xml, "International-Core") == NULL);
	CHECK_CONTAINS(log_buf, "Duplicating the regional options of this system is not supported yet");

	// Everything else. NB: Upstream drops the first logon commands of these, as the Shell-Setup component
	// is only written for the options of UNATTEND_OOBE_SHELL_SETUP_MASK
	xml = unattend(ARCH_X86_64, UNATTEND_QOL_ENHANCEMENTS | UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_FORCE_S_MODE |
		UNATTEND_USE_MS2023_BOOTLOADERS);
	CHECK_CONTAINS(xml, "    <component name=\"Microsoft-Windows-Shell-Setup\"");
	CHECK_CONTAINS(xml, "/v DisableFileSyncNGSC /t REG_DWORD /d 1 /f");
	CHECK_CONTAINS(xml, "copy %WINDIR%\\system32\\SecureBootUpdates\\SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot &amp;&amp; ");
	CHECK_CONTAINS(xml, "      <SkuPolicyRequired>1</SkuPolicyRequired>\n");
	CHECK_CONTAINS(xml, "/v HiberbootEnabled /t REG_DWORD /d 0 /f");
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log, .ask = test_ask };
	const char* tools[] = { "xorriso" };

	ui_set_iface(&iface);
	if (!have_tools(tools, ARRAYSIZE(tools)))
		return 77;
	if (!test_files_init())
		return 1;
	static_strcpy(temp_dir, tmp_dir);
	ResetOptions(&opts);

	test_versions();
	test_editions();
	test_wintogo_index();
	test_multiple_install_images();
	test_unattend();

	test_files_cleanup();
	return TEST_RESULT();
}

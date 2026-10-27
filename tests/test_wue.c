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
#include "regional.h"
#include "xml.h"
#include "wimlib.h"
#include "vhd.h"
#include "resource.h"
#include "missing.h"
#include "test.h"
#include "test_files.h"
#include "test_ui.h"
#include "test_hive.h"

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

	// The regional options of this user: the time zone, the locales and the first XKB layout of KDE
	setenv("TZ", "Europe/Warsaw", 1);
	unsetenv("LC_ALL");
	setenv("LANG", "pl_PL.UTF-8", 1);
	setenv("LC_TIME", "de_CH.UTF-8", 1);
	setenv("LC_MESSAGES", "en_GB.UTF-8", 1);
	setenv("XDG_CONFIG_HOME", tmp_dir, 1);
	make_file("kxkbrc", "[Layout]\nDisplayNames=,\nLayoutList=pl,us\nUse=true\nVariantList=qwertz,\n");
	log_buf[0] = 0;
	xml = unattend(ARCH_X86_64, UNATTEND_DUPLICATE_LOCALE | UNATTEND_NO_DATA_COLLECTION);
	CHECK_CONTAINS(xml, "        <ProtectYourPC>3</ProtectYourPC>\n");
	CHECK_CONTAINS(xml, "      <TimeZone>Central European Standard Time</TimeZone>\n");
	CHECK_CONTAINS(xml, "    <component name=\"Microsoft-Windows-International-Core\" processorArchitecture=\"amd64\"");
	CHECK_CONTAINS(xml, "      <InputLocale>0415:00010415</InputLocale>\n");
	CHECK_CONTAINS(xml, "      <SystemLocale>pl-PL</SystemLocale>\n");
	CHECK_CONTAINS(xml, "      <UserLocale>de-CH</UserLocale>\n");
	CHECK_CONTAINS(xml, "      <UILanguage>en-GB</UILanguage>\n");
	CHECK_CONTAINS(xml, "      <UILanguageFallback>en-US</UILanguageFallback>\n");
	CHECK_CONTAINS(log_buf, "• Use the same regional options as this user's");
	// A zone that Windows doesn't know about, and LC_ALL
	setenv("TZ", "Antarctica/Troll", 1);
	setenv("LC_ALL", "fr_CA.UTF-8", 1);
	log_buf[0] = 0;
	xml = unattend(ARCH_X86_64, UNATTEND_DUPLICATE_LOCALE);
	CHECK(strstr(xml, "<TimeZone>") == NULL);
	CHECK_CONTAINS(log_buf, "'Antarctica/Troll' has no Windows equivalent");
	CHECK_CONTAINS(xml, "      <InputLocale>0c0c:00010415</InputLocale>\n");
	CHECK_CONTAINS(xml, "      <UserLocale>fr-CA</UserLocale>\n");
	CHECK_CONTAINS(xml, "      <UILanguage>fr-CA</UILanguage>\n");
	unsetenv("TZ");
	unsetenv("LC_ALL");
	unsetenv("LC_TIME");
	unsetenv("LC_MESSAGES");
	unsetenv("XDG_CONFIG_HOME");

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

// A boot.wim, with Windows Setup at index 2 (a SYSTEM hive, and the files of 'Windows CA 2023')
static void make_boot_wim(const char* path, BOOL with_setup_key)
{
	WIMStruct* wim = NULL;

	make_file("bootsrc/Windows/System32/ntoskrnl.exe", "kernel");
	make_file("bootsrc/Windows/Boot/EFI_EX/bootmgfw_EX.efi", "bootmgfw 2023");
	make_file("bootsrc/Windows/Boot/EFI_EX/bootmgr_EX.efi", "bootmgr 2023");
	make_file("bootsrc/Windows/Boot/Fonts_EX/segmono_boot_EX.ttf", "font 2023");
	make_file("bootsrc/Windows/System32/config/.keep", "");
	make_hive(tmp_path("bootsrc/Windows/System32/config/SYSTEM"));
#if defined(RUFUS_HAVE_HIVEX)
	if (with_setup_key)
		add_hive_key(tmp_path("bootsrc/Windows/System32/config/SYSTEM"), "Setup");
#endif
	CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim) == 0);
	CHECK(wimlib_add_image(wim, tmp_path("bootsrc"), "Microsoft Windows PE", NULL, 0) == 0);
	CHECK(wimlib_add_image(wim, tmp_path("bootsrc"), "Microsoft Windows Setup", NULL, 0) == 0);
	CHECK(wimlib_write(wim, path, WIMLIB_ALL_IMAGES, 0, 1) == 0);
	wimlib_free(wim);
}

// The content of a file of image 2 of a WIM
static char* wim_file(const char* wim_path, const char* path)
{
	char dst[MAX_PATH];
	const char* name = strrchr(path, '/');

	snprintf(dst, sizeof(dst), "%s/extracted-%s", tmp_dir, (name == NULL) ? path : &name[1]);
	unlink(dst);
	if (!WimExtractFile(wim_path, 2, path, dst))
		return "";
	return file_content(dst);
}

// A PE header of a machine type, as setup.exe has
static void make_pe(const char* name, uint16_t machine)
{
	uint8_t pe[0x200] = { 'M', 'Z' };

	pe[0x3c] = 0x80;
	memcpy(&pe[0x80], "PE\0\0", 4);
	pe[0x84] = (uint8_t)machine;
	pe[0x85] = (uint8_t)(machine >> 8);
	CHECK(write_file(tmp_path(name), pe, sizeof(pe)) == sizeof(pe));
}

/* The Windows User Experience on the files of the drive (ApplyWindowsCustomization()) */
static void test_apply(void)
{
	const char* media = tmp_path("media");
	const char* boot_wim = tmp_path("media/sources/boot.wim");
	char* xml;
	DWORD setup_size;
	uint8_t* setup;

	// A Windows 11 24H2 media, with the 'Windows CA 2023' option and the TPM bypass
	make_file("media/sources/appraiserres.dll", "appraiser");
	make_file("media/efi/boot/bootx64.efi", "bootmgfw 2011");
	make_file("media/bootmgr.efi", "bootmgr 2011");
	make_file("media/efi/microsoft/boot/fonts/segmono_boot.ttf", "font 2011");
	make_pe("media/setup.exe", IMAGE_FILE_MACHINE_AMD64);
	make_boot_wim(boot_wim, TRUE);
	memset(&img_report, 0, sizeof(img_report));
	img_report.win_version.build = 26100;
	opts.validate_md5sum = FALSE;
	unattend_xml_path = CreateUnattendXml(ARCH_X86_64, UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_ONLINE_ACCOUNT |
		UNATTEND_USE_MS2023_BOOTLOADERS);
	CHECK(unattend_xml_path != NULL);
	log_buf[0] = 0;
	CHECK(ApplyWindowsCustomization(media, opts.unattend_xml_flags));
	// appraiserres.dll is empty, and its original is a .bak
	CHECK_EQ_STR(file_content(tmp_path("media/sources/appraiserres.bak")), "appraiser");
	CHECK(access(tmp_path("media/sources/appraiserres.dll"), F_OK) == 0);
	CHECK_EQ_STR(file_content(tmp_path("media/sources/appraiserres.dll")), "");
	// setup.exe is the wrapper, and the original is setup.dll
	CHECK(GetPeArch((uint8_t*)file_content(tmp_path("media/setup.dll"))) == IMAGE_FILE_MACHINE_AMD64);
	setup = GetResource(IDR_SETUP_X64, "setup.exe", &setup_size, FALSE);
	xml = NULL;
	CHECK(setup != NULL && read_file(tmp_path("media/setup.exe"), (uint8_t**)&xml) == setup_size);
	CHECK(setup != NULL && memcmp(xml, setup, setup_size) == 0);
	free(xml);
	CHECK_CONTAINS(log_buf, "Created '");
	CHECK_CONTAINS(log_buf, "bypass wrapper (from embedded)");
	// The 'Windows CA 2023' bootloaders and fonts
	CHECK_EQ_STR(file_content(tmp_path("media/efi/boot/bootx64.efi")), "bootmgfw 2023");
	CHECK_EQ_STR(file_content(tmp_path("media/bootmgr.efi")), "bootmgr 2023");
	CHECK_EQ_STR(file_content(tmp_path("media/efi/microsoft/boot/fonts/segmono_boot.ttf")), "font 2023");
	CHECK_CONTAINS(log_buf, "Replaced 3 EFI bootloader files with 'Windows UEFI CA 2023' compatible versions.");
#if defined(RUFUS_HAVE_HIVEX)
	// The registry keys are in the SYSTEM hive of boot.wim, so the windowsPE pass is disabled, and
	// unattend.xml goes to \sources\$OEM$\$$\Panther (which Windows Setup copies to %WINDIR%)
	wim_file(boot_wim, "Windows/System32/config/SYSTEM");
	CHECK_EQ_INT(lab_config_value(tmp_path("extracted-SYSTEM"), "BypassTPMCheck"), 1);
	CHECK_EQ_INT(lab_config_value(tmp_path("extracted-SYSTEM"), "BypassSecureBootCheck"), 1);
	CHECK_EQ_INT(lab_config_value(tmp_path("extracted-SYSTEM"), "BypassRAMCheck"), 1);
	CHECK_CONTAINS(log_buf, "Created 'HKLM\\SYSTEM\\Setup\\LabConfig\\BypassRAMCheck' registry key");
	xml = file_content(tmp_path("media/sources/$OEM$/$$/Panther/unattend.xml"));
	CHECK_CONTAINS(xml, "<settings pass=\"disabled\">");
	CHECK(strstr(xml, "LabConfig") != NULL);
	CHECK_EQ_STR(wim_file(boot_wim, "/Autounattend.xml"), "");
#else
	CHECK_CONTAINS(log_buf, "Falling back to creating the registry keys through unattend.xml");
#endif
	unlink(unattend_xml_path);
	unattend_xml_path = NULL;

	// No Setup key in the hive, and a silent install: the keys are created by unattend.xml,
	// which goes to the root of boot.wim as Autounattend.xml
	make_boot_wim(boot_wim, FALSE);
	unattend_xml_path = CreateUnattendXml(ARCH_X86_64, UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SILENT_INSTALL);
	CHECK(unattend_xml_path != NULL);
	log_buf[0] = 0;
	CHECK(ApplyWindowsCustomization(media, opts.unattend_xml_flags));
	CHECK_CONTAINS(log_buf, "Falling back to creating the registry keys through unattend.xml");
	xml = wim_file(boot_wim, "/Autounattend.xml");
	CHECK_CONTAINS(xml, "<settings pass=\"windowsPE\">");
	CHECK_CONTAINS(xml, "reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f");
	// boot.wim is still a valid image with Windows Setup at index 2
	CHECK_EQ_STR(wim_file(boot_wim, "Windows/System32/ntoskrnl.exe"), "kernel");
	unlink(unattend_xml_path);
	unattend_xml_path = NULL;

	// An ISO without boot.wim
	unlink(boot_wim);
	unattend_xml_path = CreateUnattendXml(ARCH_X86_64, UNATTEND_SECUREBOOT_TPM_MINRAM);
	CHECK(!ApplyWindowsCustomization(media, opts.unattend_xml_flags));
	unlink(unattend_xml_path);
	unattend_xml_path = NULL;
}

/* The WinPE setup of Windows XP/2003 media (SetupWinPE()) */
static void test_winpe(void)
{
	const char* media = tmp_path("winpe");
	uint8_t* bin = calloc(1, 0x3000);
	char* bootmgr;
	uint32_t size;

	CHECK(bin != NULL);
	if (bin == NULL)
		return;
	make_file("winpe/I386/NTDETECT.COM", "ntdetect");
	make_file("winpe/I386/TXTSETUP.SIF", "[Version]\r\nSignature = \"$Windows NT$\"\r\n\r\n[SetupData]\r\nProductType = 0\r\n");
	// setupldr.bin, with the CRC check of Windows 2003 and the strings that get patched
	bin[0x2060] = 0x74;
	bin[0x2061] = 0x03;
	memcpy(&bin[0x100], "multi(0)disk(0)rdisk(0)partition(1)", 35);
	memcpy(&bin[0x200], "\\$WIN_NT$.~BT\\", 14);
	memcpy(&bin[0x300], "\\minint\\txtsetup.sif", 20);
	CHECK(write_file(tmp_path("winpe/I386/SETUPLDR.BIN"), bin, 0x3000) == 0x3000);
	free(bin);
	memset(&img_report, 0, sizeof(img_report));
	img_report.winpe = WINPE_I386;
	log_buf[0] = 0;
	CHECK(SetupWinPE(media));
	CHECK_EQ_STR(file_content(tmp_path("winpe/ntdetect.com")), "ntdetect");
	CHECK_CONTAINS(file_content(tmp_path("winpe/txtsetup.sif")), "SetupSourceDevice = \"\\device\\harddisk1\\partition1\"");
	// The original files are unmodified
	CHECK(strstr(file_content(tmp_path("winpe/I386/TXTSETUP.SIF")), "SetupSourceDevice") == NULL);
	size = read_file(tmp_path("winpe/BOOTMGR"), (uint8_t**)&bootmgr);
	CHECK_EQ_INT(size, 0x3000);
	if (size == 0x3000) {
		CHECK((uint8_t)bootmgr[0x2060] == 0xeb && bootmgr[0x2061] == 0x1a);
		CHECK(memcmp(&bootmgr[0x100], "multi(0)disk(0)rdisk(1)partition(1)", 35) == 0);
		CHECK_EQ_STR(&bootmgr[0x200], "\\i386\\");
		CHECK_EQ_STR(&bootmgr[0x300], "\\i386\\txtsetup.sif");
	}
	free(bootmgr);
	CHECK_CONTAINS(log_buf, "disable Win2k3 CRC check");
}

/* The Windows names of the regional options of Linux */
static void test_regional(void)
{
	char name[64], layout[32], variant[32];

	CHECK_EQ_STR(IanaToWindowsTimeZone("Europe/Warsaw"), "Central European Standard Time");
	CHECK_EQ_STR(IanaToWindowsTimeZone("America/New_York"), "Eastern Standard Time");
	CHECK_EQ_STR(IanaToWindowsTimeZone("Etc/UTC"), "UTC");
	CHECK_EQ_STR(IanaToWindowsTimeZone("UTC"), "UTC");
	// The current names of the zones, and the ones that CLDR uses
	CHECK_EQ_STR(IanaToWindowsTimeZone("Asia/Kolkata"), "India Standard Time");
	CHECK_EQ_STR(IanaToWindowsTimeZone("Asia/Calcutta"), "India Standard Time");
	CHECK_EQ_STR(IanaToWindowsTimeZone("Europe/Kyiv"), "FLE Standard Time");
	CHECK(IanaToWindowsTimeZone("Nowhere/Land") == NULL);
	CHECK(IanaToWindowsTimeZone(NULL) == NULL);

	setenv("TZ", ":Asia/Tokyo", 1);
	CHECK(GetSystemTimeZone(name, sizeof(name)));
	CHECK_EQ_STR(name, "Asia/Tokyo");
	setenv("TZ", "/usr/share/zoneinfo/right/Europe/Paris", 1);
	CHECK(GetSystemTimeZone(name, sizeof(name)));
	CHECK_EQ_STR(name, "Europe/Paris");
	// A POSIX rule is not a zone: /etc/localtime is used instead
	setenv("TZ", "CET-1CEST", 1);
	if (GetSystemTimeZone(name, sizeof(name)))
		CHECK(strcmp(name, "CET-1CEST") != 0);
	unsetenv("TZ");

	CHECK(LocaleToBcp47("pl_PL.UTF-8", name, sizeof(name)));
	CHECK_EQ_STR(name, "pl-PL");
	CHECK(LocaleToBcp47("sr_RS@latin", name, sizeof(name)));
	CHECK_EQ_STR(name, "sr-Latn-RS");
	CHECK(LocaleToBcp47("uz_UZ.UTF-8@cyrillic", name, sizeof(name)));
	CHECK_EQ_STR(name, "uz-Cyrl-UZ");
	CHECK(LocaleToBcp47("de", name, sizeof(name)));
	CHECK_EQ_STR(name, "de-DE");
	CHECK(LocaleToBcp47("en", name, sizeof(name)));
	CHECK_EQ_STR(name, "en-US");
	CHECK(LocaleToBcp47("C.UTF-8", name, sizeof(name)));
	CHECK_EQ_STR(name, "en-US");
	CHECK(LocaleToBcp47("POSIX", name, sizeof(name)));
	CHECK_EQ_STR(name, "en-US");
	CHECK(!LocaleToBcp47("_PL", name, sizeof(name)));
	CHECK(!LocaleToBcp47("p1_PL", name, sizeof(name)));

	CHECK_EQ_INT(XkbToKlid("us", ""), 0x00000409);
	CHECK_EQ_INT(XkbToKlid("us", "intl"), 0x00020409);
	CHECK_EQ_INT(XkbToKlid("us", "colemak"), 0x00000409);
	CHECK_EQ_INT(XkbToKlid("pl", NULL), 0x00000415);
	CHECK_EQ_INT(XkbToKlid("ch", "fr"), 0x0000100c);
	CHECK_EQ_INT(XkbToKlid("xx", NULL), 0);

	// kxkbrc, unless KDE uses the layouts of the system
	setenv("XDG_CONFIG_HOME", tmp_dir, 1);
	make_file("kxkbrc", "[Layout]\nLayoutList=de,us\nUse=true\nVariantList=nodeadkeys,\n");
	CHECK(GetXkbLayout(layout, sizeof(layout), variant, sizeof(variant)));
	CHECK_EQ_STR(layout, "de");
	CHECK_EQ_STR(variant, "nodeadkeys");
	setenv("LANG", "de_AT.UTF-8", 1);
	unsetenv("LC_ALL");
	unsetenv("LC_CTYPE");
	CHECK(GetInputLocale(name, sizeof(name)));
	CHECK_EQ_STR(name, "0c07:00000407");
	make_file("kxkbrc", "[Layout]\nLayoutList=de,us\nUse=false\n");
	if (GetXkbLayout(layout, sizeof(layout), variant, sizeof(variant)))
		CHECK(strcmp(variant, "nodeadkeys") != 0);
	// Without a layout that we know about, the default one of the language
	make_file("kxkbrc", "[Layout]\nLayoutList=epo\nUse=true\n");
	setenv("LANG", "cs_CZ.UTF-8", 1);
	CHECK(GetInputLocale(name, sizeof(name)));
	CHECK_EQ_STR(name, "0405:00000405");
	unsetenv("XDG_CONFIG_HOME");
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
	test_regional();
	test_unattend();
	test_apply();
	test_winpe();

	test_files_cleanup();
	return TEST_RESULT();
}

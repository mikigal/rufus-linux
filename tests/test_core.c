/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the core utility layer
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "rufus.h"
#include "resource.h"
#include "ui_iface.h"
#include "localization.h"
#include "progress.h"
#include "xml.h"
#include "test.h"

/* Front end callbacks that record what the core sends */
static char log_buf[16 * 1024];
static char status_buf[2][256];
static uint32_t progress_pos;
static void* progress_dialog;
static int progress_calls, init_events;

static void test_log(const char* text)
{
	safe_strcat(log_buf, sizeof(log_buf), text);
}

static void test_status(BOOL info, unsigned int duration, const char* text)
{
	static_strcpy(status_buf[info ? 1 : 0], (text == NULL) ? "" : text);
}

static void test_progress(void* dialog, uint32_t pos, BOOL taskbar)
{
	progress_dialog = dialog;
	progress_pos = pos;
	progress_calls++;
}

static void test_event(enum ui_event_type type, intptr_t param)
{
	if (type == UM_PROGRESS_INIT)
		init_events++;
}

static char tmp_dir[] = "/tmp/rufus-test-core-XXXXXX";

static char* tmp_path(const char* name)
{
	static char path[4][MAX_PATH];
	static int i = 0;
	char* p = path[i++ % 4];

	snprintf(p, MAX_PATH, "%s/%s", tmp_dir, name);
	return p;
}

static void write_bytes(const char* path, const void* data, size_t len)
{
	FILE* fd = fopen(path, "wb");
	CHECK(fd != NULL);
	if (fd != NULL) {
		CHECK(fwrite(data, 1, len, fd) == len);
		fclose(fd);
	}
}

static BOOL file_equals(const char* path, const void* data, size_t len)
{
	uint8_t* buf = NULL;
	uint32_t size = read_file(path, &buf);
	BOOL r = (size == len) && (memcmp(buf, data, len) == 0);

	if (!r) {
		fprintf(stderr, "'%s' has unexpected content (%u bytes):\n", path, size);
		if (buf != NULL)
			DumpBufferHex(buf, size);
	}
	free(buf);
	return r;
}
#define CHECK_FILE(path, lit) CHECK(file_equals(path, lit, sizeof(lit) - 1))
#define write_str(path, str) write_bytes(path, str, strlen(str))

static void test_log_and_errors(void)
{
	log_buf[0] = 0;
	uprintf("Hello %s  \t", "world");
	uprintf("Second line\n\n");
	CHECK_EQ_STR(log_buf, "Hello world\nSecond line\n");

	errno = ENOENT;
	CHECK_EQ_STR(WindowsErrorString(), "[ENOENT] No such file or directory");
	CHECK_EQ_INT(errno, ENOENT);
	CHECK_EQ_INT(WindowsErrorFromErrno(ENOSPC), ERROR_DISK_FULL);
	CHECK_EQ_INT(WindowsErrorFromErrno(EROFS), ERROR_WRITE_PROTECT);
	CHECK_EQ_INT(WindowsErrorFromErrno(EBUSY), ERROR_DEVICE_IN_USE);
	// Status codes
	CHECK_EQ_STR(StrError(RUFUS_ERROR(ERROR_CANCELLED), FALSE), "Cancelled by user.");
	CHECK_EQ_STR(StrError(0, FALSE), "Success.");
	CHECK_EQ_STR(StrError(RUFUS_ERROR(ERROR_FILE_NOT_FOUND), FALSE), "[0xC0030002] No such file or directory");
	CHECK_EQ_STR(StrError(0x8007FFFF, FALSE), "[0x8007FFFF] Undetermined error");
}

static void test_strings(void)
{
	char buf[16] = "abc", *s;
	htab_table htab = HTAB_EMPTY;
	StrArray arr;
	GUID guid;
	uint32_t i1, i2;

	safe_strcat(buf, sizeof(buf), "defghijklmnopqrstuvwxyz");
	CHECK_EQ_STR(buf, "abcdefghijklmno");
	static_strcpy(buf, "MiXeD.Txt");
	safe_strtolower(buf);
	CHECK_EQ_STR(buf, "mixed.txt");

	guid = StringToGuid("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}");
	CHECK_EQ_STR(GuidToString(&guid, TRUE), "{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}");
	CHECK_EQ_STR(GuidToString(&guid, FALSE), "C12A7328F81F11D2BA4B00A0C93EC93B");
	CHECK_EQ_STR(TimestampToHumanReadable(20260127113059ULL), "2026.01.27 11:30:59 (UTC)");
	CHECK_EQ_STR(_printbits(sizeof(uint8_t), &(uint8_t){ 0x05 }, 1), "0b00000101");

	CHECK(htab_create(7, &htab));
	i1 = htab_hash("foo", &htab);
	i2 = htab_hash("bar", &htab);
	CHECK(i1 != 0 && i2 != 0 && i1 != i2);
	CHECK_EQ_INT(htab_hash("foo", &htab), i1);
	htab_destroy(&htab);

	StrArrayCreate(&arr, 2);
	CHECK_EQ_INT(StrArrayAdd(&arr, "a", TRUE), 0);
	CHECK_EQ_INT(StrArrayAdd(&arr, "b", TRUE), 1);
	CHECK_EQ_INT(StrArrayAdd(&arr, "c", TRUE), 2);
	CHECK_EQ_INT(StrArrayAddUnique(&arr, "b", TRUE), 1);
	CHECK_EQ_INT(StrArrayFind(&arr, "c"), 2);
	StrArrayDestroy(&arr);

	s = replace_char("a/b/c", '/', "\\\\");
	CHECK_EQ_STR(s, "a\\\\b\\\\c");
	free(s);
	s = remove_substr("persistent persistence", "persist");
	CHECK_EQ_STR(s, "ent ence");
	free(s);
	static_strcpy(buf, "  trim me \t");
	trim(buf);
	CHECK_EQ_STR(buf, "trim me");
	static_strcpy(buf, "a:b;c");
	filter_chars(buf, ":;", '_');
	CHECK_EQ_STR(buf, "a_b_c");
	static_strcpy(buf, "Fedora-WS-Live");
	CHECK_EQ_INT(sanitize_label(buf), 0);
	CHECK_EQ_STR(buf, "fedora");
}

static void test_asn1(void)
{
	// SEQUENCE { OID 2.5.4.3 (commonName), UTF8String "Rufus" }
	static const uint8_t der[] = { 0x30, 0x0c, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x05, 'R', 'u', 'f', 'u', 's' };
	size_t len = 0;
	const char* data = get_data_from_asn1(der, sizeof(der), "2.5.4.3", 0x0c, &len);

	CHECK(data != NULL);
	CHECK_EQ_INT(len, 5);
	if (data != NULL)
		CHECK(memcmp(data, "Rufus", 5) == 0);
	CHECK(get_data_from_asn1(der, sizeof(der), "2.5.4.4", 0x0c, &len) == NULL);
}

static void test_localization(void)
{
	char loc_path[MAX_PATH];
	loc_cmd* lcmd;
	const char* src = getenv("RUFUS_SOURCE_DIR");
	int nb_locales = 0;

	snprintf(loc_path, sizeof(loc_path), "%s/res/loc/rufus.loc", (src == NULL) ? "." : src);
	init_localization();
	CHECK(get_supported_locales(loc_path));
	list_for_each_entry(lcmd, &locale_list, loc_cmd, list)
		nb_locales++;
	CHECK(nb_locales >= 38);

	CHECK_EQ_STR(get_locale_from_posix_name("pl_PL.UTF-8", FALSE)->txt[0], "pl-PL");
	CHECK_EQ_STR(get_locale_from_posix_name("de_AT.UTF-8", FALSE)->txt[0], "de-DE");
	CHECK_EQ_STR(get_locale_from_posix_name("pt_BR", FALSE)->txt[0], "pt-BR");
	CHECK_EQ_STR(get_locale_from_posix_name("pt_AO", FALSE)->txt[0], "pt-PT");
	CHECK_EQ_STR(get_locale_from_posix_name("zh_HK.UTF-8", FALSE)->txt[0], "zh-TW");
	CHECK_EQ_STR(get_locale_from_posix_name("sr_RS@latin", FALSE)->txt[0], "sr-RS");
	CHECK(get_locale_from_posix_name("C", FALSE) == NULL);
	CHECK_EQ_STR(get_locale_from_posix_name("C.UTF-8", TRUE)->txt[0], "en-US");
	CHECK_EQ_STR(get_locale_from_posix_name(NULL, TRUE)->txt[0], "en-US");

	// Default (English) messages
	CHECK(get_loc_data_file(loc_path, get_locale_from_name("en-US", FALSE)));
	CHECK_EQ_STR(lmprintf(MSG_050), "Success.");
	CHECK_EQ_STR(SizeToHumanReadable(1536, FALSE, FALSE), "1.5 KB");
	CHECK_EQ_STR(SizeToHumanReadable(8 * GB - 100 * MB, FALSE, TRUE), "8 GB");
	CHECK_EQ_STR(SizeToHumanReadable(15 * GB, FALSE, FALSE), "15 GB");
	CHECK_EQ_STR(SizeToHumanReadable(512, FALSE, FALSE), "512 bytes");

	// Polish, with the English messages still available for the log
	CHECK(get_loc_data_file(loc_path, get_locale_from_name("pl-PL", FALSE)));
	CHECK_EQ_STR(lmprintf(MSG_070), "Przerwane przez użytkownika.");
	CHECK_EQ_STR(StrError(RUFUS_ERROR(ERROR_CANCELLED), TRUE), "Cancelled by user.");
	CHECK_EQ_STR(StrError(RUFUS_ERROR(ERROR_CANCELLED), FALSE), "Przerwane przez użytkownika.");
	PrintInfoDebug(0, MSG_070);
	CHECK_EQ_STR(status_buf[1], "Przerwane przez użytkownika.");
	PrintStatus(0, MSG_050);
	CHECK_EQ_STR(status_buf[0], "Sukces.");
	PrintStatus(0, -1);
	CHECK_EQ_STR(status_buf[0], "");

	exit_localization();
}

// Collect the text of IDC_START for apply_localization()
static char start_text[64];
static BOOL set_text(void* ctx, int ctrl_id, const char* text)
{
	if (ctrl_id == IDC_START)
		static_strcpy(start_text, text);
	return TRUE;
}

static void test_apply_localization(void)
{
	char loc_path[MAX_PATH];
	const char* src = getenv("RUFUS_SOURCE_DIR");

	snprintf(loc_path, sizeof(loc_path), "%s/res/loc/rufus.loc", (src == NULL) ? "." : src);
	init_localization();
	CHECK(get_supported_locales(loc_path));
	CHECK(get_loc_data_file(loc_path, get_locale_from_name("fr-FR", FALSE)));
	apply_localization(IDD_DIALOG, set_text, NULL);
	CHECK_EQ_STR(start_text, "Démarrer");
	exit_localization();
}

static void test_config_files(void)
{
	char* data;
	// The UTF-16LE encoding of "﻿label=Labél\r\ntimeout 50\r\n"
	static const char utf16[] = "\xFF\xFE" "l\0a\0b\0e\0l\0=\0L\0a\0b\0\xE9\0l\0\r\0\n\0t\0i\0m\0e\0o\0u\0t\0 \0" "5\0" "0\0\r\0\n\0";

	/* ANSI file with LF line endings */
	write_str(tmp_path("isolinux.cfg"), "default vesamenu.c32\ntimeout 50\nlabel linux\n  kernel /casper/vmlinuz\n  append  file=/cdrom/preseed.seed boot=casper quiet splash ---\n");
	data = get_token_data_file("timeout", tmp_path("isolinux.cfg"));
	CHECK(data == NULL);	// "timeout 50" has no '='
	free(data);
	CHECK(replace_in_token_data(tmp_path("isolinux.cfg"), "append", "boot=casper", "boot=casper persistent", FALSE) != NULL);
	// Windows text mode converts all the line endings to CR/LF
	CHECK_FILE(tmp_path("isolinux.cfg"), "default vesamenu.c32\r\ntimeout 50\r\nlabel linux\r\n  kernel /casper/vmlinuz\r\n"
		"  append  file=/cdrom/preseed.seed boot=casper persistent quiet splash ---\r\n");
	// ...unless dos2unix is requested
	CHECK(replace_in_token_data(tmp_path("isolinux.cfg"), "kernel", "vmlinuz", "vmlinuz.efi", TRUE) != NULL);
	CHECK_FILE(tmp_path("isolinux.cfg"), "default vesamenu.c32\ntimeout 50\nlabel linux\n  kernel /casper/vmlinuz.efi\n"
		"  append  file=/cdrom/preseed.seed boot=casper persistent quiet splash ---\n");
	// No match: the file is left untouched
	CHECK(replace_in_token_data(tmp_path("isolinux.cfg"), "append", "nomatch", "x", FALSE) == NULL);
	CHECK(replace_in_token_data(tmp_path("isolinux.cfg"), "label", "linux", "linux", FALSE) == NULL);
	// At most 4 occurrences per line are replaced
	write_str(tmp_path("grub.cfg"), "linux a a a a a\n");
	CHECK(replace_in_token_data(tmp_path("grub.cfg"), "linux", "a", "b", TRUE) != NULL);
	CHECK_FILE(tmp_path("grub.cfg"), "linux b b b b a\n");

	/* Tokens with '=' (autorun.inf / syslinux.cfg style) */
	write_str(tmp_path("autorun.inf"), "[autorun]\r\nicon  = autorun.ico\r\nlabel=Old Label\r\n");
	data = get_token_data_file("label", tmp_path("autorun.inf"));
	CHECK_EQ_STR(data, "Old Label");
	free(data);
	CHECK(set_token_data_file("label", "New Label", tmp_path("autorun.inf")) != NULL);
	CHECK_FILE(tmp_path("autorun.inf"), "[autorun]\r\nicon  = autorun.ico\r\nlabel=New Label\r\n");
	CHECK(set_token_data_file("open", "setup.exe", tmp_path("autorun.inf")) != NULL);
	CHECK_FILE(tmp_path("autorun.inf"), "[autorun]\r\nicon  = autorun.ico\r\nlabel=New Label\r\nopen = setup.exe\r\n");
	data = get_token_data_file_indexed("icon", tmp_path("autorun.inf"), 1);
	CHECK_EQ_STR(data, "autorun.ico");
	free(data);
	CHECK(get_token_data_file_indexed("icon", tmp_path("autorun.inf"), 2) == NULL);

	/* Section insertion, in a UTF-8 file with a BOM */
	write_str(tmp_path("syslinux.cfg"), "\xEF\xBB\xBF" "[main]\nfoo=1\n");
	CHECK(insert_section_data(tmp_path("syslinux.cfg"), "[main]", "bar=2", FALSE) != NULL);
	CHECK_FILE(tmp_path("syslinux.cfg"), "\xEF\xBB\xBF" "[main]\r\nbar=2\r\nfoo=1\r\n");
	CHECK(insert_section_data(tmp_path("syslinux.cfg"), "[none]", "baz=3", FALSE) == NULL);

	/* UTF-16LE file with a BOM */
	write_bytes(tmp_path("utf16.inf"), utf16, sizeof(utf16) - 1);
	data = get_token_data_file("label", tmp_path("utf16.inf"));
	CHECK_EQ_STR(data, "Lab\xC3\xA9l");
	free(data);
	CHECK(set_token_data_file("label", "Nouveau", tmp_path("utf16.inf")) != NULL);
	CHECK_FILE(tmp_path("utf16.inf"), "\xFF\xFE" "l\0a\0b\0e\0l\0=\0N\0o\0u\0v\0e\0a\0u\0\r\0\n\0t\0i\0m\0e\0o\0u\0t\0 \0" "5\0" "0\0\r\0\n\0");

	/* Remote data */
	static const char ver[] = "version = 4.16.2310\r\nrelease_notes=\"{\\rtf1 Fixes}\"\r\ndownload_url = https://example.org/rufus.AppImage\r\n";
	data = get_token_data_buffer("download_url", 1, ver, sizeof(ver));
	CHECK_EQ_STR(data, "https://example.org/rufus.AppImage");
	free(data);
	data = get_token_data_buffer("release_notes", 1, ver, sizeof(ver));
	CHECK_EQ_STR(data, "{\\rtf1 Fixes}");
	free(data);
	parse_update((char*)ver, sizeof(ver));
	CHECK_EQ_INT(update.version[0], 4);
	CHECK_EQ_INT(update.version[1], 16);
	CHECK_EQ_INT(update.version[2], 2310);
	CHECK_EQ_STR(update.download_url, "https://example.org/rufus.AppImage");
}

static void test_xml(void)
{
	static const char xml_str[] = "<WIM><IMAGE INDEX=\"1\"><NAME>Windows 11 Pro &amp; Home</NAME>"
		"<DISPLAYNAME>&#x50;ro</DISPLAYNAME></IMAGE></WIM>";
	char* buf = strdup(xml_str);
	ezxml_t xml = ezxml_parse_str(buf, strlen(buf));
	ezxml_t image;

	CHECK(xml != NULL);
	image = ezxml_child(xml, "IMAGE");
	CHECK(image != NULL);
	CHECK_EQ_STR(ezxml_attr(image, "INDEX"), "1");
	// Upstream truncates the value at the first entity (see xml.c)
	CHECK_EQ_STR(ezxml_child_val(image, "NAME"), "Windows 11 Pro & Home");
	CHECK_EQ_STR(ezxml_child_val(image, "DISPLAYNAME"), "Pro");
	ezxml_free(xml);
	free(buf);
}

static void test_files(void)
{
	StrArray arr;
	DWORD written = 0;
	char path[MAX_PATH], *buf = "hello", *rbuf = NULL;
	DWORD size = 5;
	int fd;

	snprintf(path, sizeof(path), "%s/dir/sub", tmp_dir);
	CHECK(mkdir(tmp_path("dir"), 0755) == 0);
	CHECK(mkdir(path, 0755) == 0);
	write_bytes(tmp_path("dir/a.txt"), "a", 1);
	write_bytes(tmp_path("dir/sub/b.txt"), "b", 1);

	StrArrayCreate(&arr, 8);
	CHECK_EQ_INT(ListDirectoryContent(&arr, tmp_path("dir"), LIST_DIR_TYPE_FILE | LIST_DIR_TYPE_DIRECTORY | LIST_DIR_TYPE_RECURSIVE), NO_ERROR);
	CHECK_EQ_INT(arr.Index, 3);
	CHECK(StrArrayFind(&arr, tmp_path("dir/a.txt")) >= 0);
	CHECK(StrArrayFind(&arr, tmp_path("dir/sub/")) >= 0);
	CHECK(StrArrayFind(&arr, tmp_path("dir/sub/b.txt")) >= 0);
	StrArrayClear(&arr);
	CHECK_EQ_INT(ListDirectoryContent(&arr, tmp_path("dir/sub"), LIST_DIR_TYPE_DIRECTORY), NO_ERROR);
	CHECK_EQ_INT(arr.Index, 0);
	CHECK_EQ_INT(ListDirectoryContent(&arr, tmp_path("nowhere"), LIST_DIR_TYPE_FILE), ERROR_FILE_NOT_FOUND);
	StrArrayDestroy(&arr);

	CHECK(FileIO(FILE_IO_WRITE, tmp_path("io.txt"), &buf, &size));
	size = 6;
	buf = " world";
	CHECK(FileIO(FILE_IO_APPEND, tmp_path("io.txt"), &buf, &size));
	CHECK(FileIO(FILE_IO_READ, tmp_path("io.txt"), &rbuf, &size));
	CHECK_EQ_INT(size, 11);
	if (rbuf != NULL)
		CHECK(memcmp(rbuf, "hello world", 11) == 0);
	free(rbuf);

	fd = CreatePreallocatedFile(tmp_path("prealloc.bin"), O_RDWR | O_CREAT | O_TRUNC, 0644, 64 * KB);
	CHECK(fd >= 0);
	CHECK(WriteFileWithRetry(fd, "0123456789", 10, &written, 1));
	CHECK_EQ_INT(written, 10);
	CHECK(WriteFileWithRetry(fd, "abc", 3, NULL, 1));
	CHECK_EQ_INT(lseek(fd, 0, SEEK_CUR), 13);
	CHECK_EQ_INT(lseek(fd, 0, SEEK_END), 64 * KB);
	close(fd);
}

static void test_run_command(void)
{
	const char* echo[] = { "sh", "-c", "echo 'from the command'; exit 3", NULL };
	const char* missing[] = { "rufus-no-such-command", NULL };
	const char* pwd[] = { "pwd", NULL };

	log_buf[0] = 0;
	CHECK_EQ_INT(RunCommand(echo, NULL, TRUE), 3);
	CHECK(strstr(log_buf, "from the command") != NULL);
	CHECK_EQ_INT(RunCommand(missing, NULL, FALSE), ERROR_FILE_NOT_FOUND);
	log_buf[0] = 0;
	CHECK_EQ_INT(RunCommand(pwd, tmp_dir, TRUE), 0);
	CHECK(strstr(log_buf, tmp_dir) != NULL);
}

static void test_progress_reporting(void)
{
	static int dialog;

	// A dialog with its own progress bar
	progress_calls = init_events = 0;
	UpdateProgressWithInfoInit(&dialog, TRUE);
	CHECK_EQ_INT(init_events, 1);
	UpdateProgressWithInfoForce(OP_NOOP, MSG_241, 50, 200);
	CHECK(progress_calls > 0);
	CHECK(progress_dialog == &dialog);
	CHECK_EQ_INT(progress_pos, MAX_PROGRESS / 4);
	// The main progress bar, once slots are allocated
	memset(nb_slots, 0, sizeof(nb_slots));
	memset(slot_end, 0, sizeof(slot_end));
	previous_end = 0.0f;
	nb_slots[OP_FORMAT] = -1;
	slot_end[OP_FORMAT + 1] = 100.0f;
	UpdateProgressWithInfoInit(NULL, TRUE);
	usleep(2 * MAX_REFRESH * 1000 + 1000);
	UpdateProgressWithInfoForce(OP_FORMAT, MSG_241, 3, 4);
	CHECK(progress_dialog == NULL);
	CHECK_EQ_INT(progress_pos, 3 * MAX_PROGRESS / 4);
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log, .status = test_status, .progress = test_progress, .event = test_event };
	char cmd[64];

	if (mkdtemp(tmp_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	ui_set_iface(&iface);

	// Most of the error and size strings need a message table
	test_localization();
	init_localization();
	{
		char loc_path[MAX_PATH];
		const char* src = getenv("RUFUS_SOURCE_DIR");
		snprintf(loc_path, sizeof(loc_path), "%s/res/loc/rufus.loc", (src == NULL) ? "." : src);
		CHECK(get_supported_locales(loc_path));
		CHECK(get_loc_data_file(loc_path, get_locale_from_name("en-US", FALSE)));
	}
	test_log_and_errors();
	test_strings();
	test_asn1();
	test_config_files();
	test_xml();
	test_files();
	test_run_command();
	test_progress_reporting();
	exit_localization();
	test_apply_localization();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_dir);
	IGNORE_RETVAL(system(cmd));
	return TEST_RESULT();
}

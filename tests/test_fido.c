/*
 * Rufus: The Reliable USB Formatting Utility
 * Fido tests
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
 * fido.c and json.c, against a server of the test: the real, signed Fido.ps1
 * (data/download/github.com), and replies of the Microsoft download site, as
 * Fido.ps1 v1.71 gets them, which the server checks the requests of.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "ui_iface.h"
#include "download.h"
#include "json.h"
#include "fido.h"
#include "test.h"
#include "test_files.h"
#include "test_http.h"

static char data_dir[MAX_PATH];
static char last_session[64], last_referer[256];
static BOOL banned = FALSE, whitelisted = FALSE;

// The value of a parameter of the query of a path
static const char* query_param(const char* path, const char* name, char* value, size_t len)
{
	char key[64];
	const char* p;

	snprintf(key, sizeof(key), "%s=", name);
	for (p = strchr(path, '?'); p != NULL; p = strchr(p + 1, '&')) {
		if (strncmp(p + 1, key, strlen(key)) == 0) {
			p += 1 + strlen(key);
			snprintf(value, len, "%.*s", (int)strcspn(p, "&"), p);
			return value;
		}
	}
	value[0] = 0;
	return NULL;
}

static void serve(int fd, const char* method, const char* path, const char* req)
{
	char file[MAX_PATH * 2], value[256], session[64], body[2048];
	uint8_t* buf = NULL;
	uint32_t size;

	if (strncmp(path, "/www.microsoft.com/", 19) == 0 && strstr(path, "/software-download/windows11") != NULL) {
		// The site has pl-PL, but not xx-XX
		if (strstr(path, "/xx-XX/") != NULL)
			http_send_headers(fd, 302, "Found", 0, "Location: https://www.microsoft.com/en-us/software-download/windows11");
		else
			http_reply(fd, 200, "OK", "text/html", "<html></html>");
	} else if (strncmp(path, "/vlscppe.microsoft.com/tags?", 28) == 0) {
		query_param(path, "session_id", last_session, sizeof(last_session));
		whitelisted = (query_param(path, "org_id", value, sizeof(value)) != NULL) && (strcmp(value, "y6jn8c31") == 0);
		http_reply(fd, 200, "OK", "text/html", "");
	} else if (strncmp(path, "/ov-df.microsoft.com/mdt.js?", 28) == 0) {
		whitelisted &= (query_param(path, "session_id", session, sizeof(session)) != NULL) &&
			(strcmp(session, last_session) == 0) && (query_param(path, "instanceId", value, sizeof(value)) != NULL) &&
			(strcmp(value, "560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175") == 0);
		http_reply(fd, 200, "OK", "application/javascript",
			"var u=\"https://ov-df.microsoft.com/?w=0A1B2C3D4E5F&session_id=x\";var t={rticks=\"+1761580000123\"};");
	} else if (strncmp(path, "/ov-df.microsoft.com/?", 22) == 0) {
		whitelisted &= (query_param(path, "w", value, sizeof(value)) != NULL) && (strcmp(value, "0A1B2C3D4E5F") == 0) &&
			(query_param(path, "rticks", value, sizeof(value)) != NULL) && (strcmp(value, "1761580000123") == 0) &&
			(query_param(path, "mdt", value, sizeof(value)) != NULL) && (atoll(value) > 1700000000000LL);
		http_reply(fd, 200, "OK", "text/html", "");
	} else if (strstr(path, "/api/getskuinformationbyproductedition?") != NULL) {
		query_param(path, "productEditionId", value, sizeof(value));
		query_param(path, "sessionID", session, sizeof(session));
		if (!whitelisted || (strcmp(session, last_session) != 0)) {
			http_reply(fd, 200, "OK", "application/json", "{\"Errors\":[{\"Key\":\"Error\",\"Value\":\"Not whitelisted\",\"Type\":1}]}");
			return;
		}
		// The x64 and ARM64 IDs of Windows 11 have SKUs of their own
		snprintf(body, sizeof(body), "{\"Skus\":["
			"{\"Id\":\"%s1\",\"Language\":\"English\",\"LocalizedLanguage\":\"English (United States)\"},"
			"{\"Id\":\"%s2\",\"Language\":\"Polish\",\"LocalizedLanguage\":\"Polski\"},"
			"{\"Id\":\"%s3\",\"Language\":\"Chinese (Simplified)\",\"LocalizedLanguage\":\"\\u4e2d\\u6587(\\u7b80\\u4f53)\"}"
			"]}", value, value, value);
		http_reply(fd, 200, "OK", "application/json", body);
	} else if (strstr(path, "/api/GetProductDownloadLinksBySku?") != NULL) {
		http_get_header(req, "Referer:", last_referer, sizeof(last_referer));
		query_param(path, "SKU", value, sizeof(value));
		if (banned) {
			http_reply(fd, 200, "OK", "application/json", "{\"Errors\":[{\"Key\":\"Error\",\"Value\":\"Sentinel\",\"Type\":9}]}");
			return;
		}
		// The x64 edition (3813) has x64 links, and the ARM64 one (3816) ARM64 links
		snprintf(body, sizeof(body), "{\"ProductDownloadOptions\":[{\"Uri\":\"https://software.download.prss.microsoft.com/"
			"%s.iso?t=1\",\"DownloadType\":%d,\"Language\":\"Polish\"}]}", value, (strncmp(value, "3816", 4) == 0) ? 2 : 1);
		http_reply(fd, 200, "OK", "application/json", body);
	} else if (strcmp(path, "/github.com/pbatard/UEFI-Shell/releases/download/26H1/Version.xml") == 0) {
		http_reply(fd, 200, "OK", "text/xml", "<?xml version=\"1.0\"?><release><supported_archs><arch>x64</arch>"
			"<arch>ia32</arch><arch>aa64</arch></supported_archs></release>");
	} else {
		snprintf(file, sizeof(file), "%s%s", data_dir, path);
		size = (strstr(path, "..") == NULL) ? read_file(file, &buf) : 0;
		if (size == 0) {
			http_reply(fd, 404, "Not Found", "text/plain", "Not Found");
		} else {
			http_send_headers(fd, 200, "OK", size, NULL);
			if (strcmp(method, "HEAD") != 0)
				http_send_all(fd, buf, size);
		}
		free(buf);
	}
}

static void test_json(void)
{
	const char* text = "\xef\xbb\xbf {\"a\": [1, -2.5e2, true, false, null], \"s\": \"x\\\"\\u00e9\\ud83d\\ude00\\n\", "
		"\"o\": {}, \"e\": []}";
	json_value_t *doc = JsonParse(text, strlen(text)), *a;

	CHECK(doc != NULL && doc->type == JSON_OBJECT);
	a = JsonGet(doc, "a");
	CHECK(a != NULL && a->type == JSON_ARRAY && a->child != NULL);
	if (a != NULL && a->child != NULL) {
		CHECK(a->child->type == JSON_NUMBER && a->child->number == 1);
		CHECK(a->child->next->number == -250);
		CHECK(a->child->next->next->type == JSON_BOOL && a->child->next->next->number == 1);
		CHECK(a->child->next->next->next->number == 0);
		CHECK(a->child->next->next->next->next->type == JSON_NULL);
	}
	CHECK_EQ_STR(JsonGetString(doc, "s"), "x\"\xc3\xa9\xf0\x9f\x98\x80\n");
	CHECK(JsonGet(doc, "o") != NULL && JsonGet(doc, "o")->child == NULL);
	CHECK(JsonGetString(doc, "o") == NULL);
	CHECK(JsonGet(doc, "missing") == NULL);
	JsonFree(doc);
	// Invalid documents
	CHECK(JsonParse("{\"a\":}", 6) == NULL);
	CHECK(JsonParse("[1,]", 4) == NULL);
	CHECK(JsonParse("{\"a\":1} x", 9) == NULL);
	CHECK(JsonParse("\"\\ud800\"", 8) == NULL);
	CHECK(JsonParse("\"\\u0000\"", 8) == NULL);
	CHECK(JsonParse("\"abc", 4) == NULL);
	CHECK(JsonParse("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]"
		"]]]]]]]]]]]]]]]]]]]]]]]]]]]]]", 140) == NULL);
	CHECK(JsonParse(NULL, 0) == NULL);
}

// A script with the structure of Fido.ps1, as its data change from release to release
static void test_script_data(void)
{
	const char* script = "# Fido\n$WindowsVersions = @(\n"
		"\t@(\n\t\t@(\"Windows 11\", \"windows11\"),\n"
		"\t\t@(\n\t\t\t\"26H2 (Build 26300.9457 - 2026.09)\",\n"
		"\t\t\t# A comment\n"
		"\t\t\t@(\"Windows 11 Home/Pro/Edu\", @(3813, 3816)),\n"
		"\t\t\t@(\"Windows 11 Home China \", @(3814, 3817))\n\t\t)\n\t),\n"
		"\t@(\n\t\t@(\"Windows 10\", \"Windows10ISO\"),\n"
		"\t\t@(\n\t\t\t\"22H2 v1 (Build 19045.2965 - 2023.05)\",\n\t\t\t@(\"Windows 10 Home/Pro/Edu\", 2618)\n\t\t)\n\t)\n"
		"\t@(\n\t\t@(\"UEFI Shell 2.2\", \"UEFI_SHELL 2.2\"),\n"
		"\t\t@(\n\t\t\t\"26H1 (edk2-stable202602)\",\n\t\t\t@(\"Release\", 0),\n\t\t\t@(\"Debug\", 1)\n\t\t)\n\t)\n)\n"
		"$OrgId = \"y6jn8c31\"\n$ProfileId = \"606624d44113\"\n$InstanceId = \"560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175\"\n";
	fido_t* f = FidoOpenScript(script, strlen(script), "pl-PL");

	CHECK(f != NULL);
	if (f == NULL)
		return;
	CHECK_EQ_INT(FidoNbVersions(f), 3);
	CHECK_EQ_STR(FidoVersionName(f, 0), "Windows 11");
	CHECK_EQ_STR(FidoVersionName(f, 2), "UEFI Shell 2.2");
	CHECK(!FidoIsUefiShell(f, 0) && FidoIsUefiShell(f, 2));
	CHECK_EQ_INT(FidoNbReleases(f, 0), 1);
	CHECK_EQ_STR(FidoReleaseName(f, 0, 0), "26H2 (Build 26300.9457 - 2026.09)");
	// The Chinese editions are only for Chinese locales
	CHECK_EQ_INT(FidoNbEditions(f, 0, 0), 1);
	CHECK_EQ_STR(FidoEditionName(f, 0, 0, 0), "Windows 11 Home/Pro/Edu");
	CHECK_EQ_STR(FidoEditionName(f, 2, 0, 1), "Debug");
	FidoClose(f);
	f = FidoOpenScript(script, strlen(script), "zh-CN");
	CHECK(f != NULL && FidoNbEditions(f, 0, 0) == 2 && strcmp(FidoEditionName(f, 0, 0, 1), "Windows 11 Home China") == 0);
	FidoClose(f);
	// Without the IDs, or with broken data
	CHECK(FidoOpenScript("$WindowsVersions = @()\n", 23, "en-US") == NULL);
	CHECK(FidoOpenScript(script, strlen(script) / 2, "en-US") == NULL);
}

static void test_fido(void)
{
	fido_t* f;
	int preferred, n, i;

	setenv("RUFUS_TEST_URL_PREFIX", http_url(""), 1);
	// rufus.ie gives the URL of the script, which GitHub has
	CHECK(FidoCheck());
	CHECK(fido_available);
	CHECK_EQ_STR(fido_url, "https://github.com/pbatard/Fido/releases/download/v1.71/Fido.ps1.lzma");

	// The real script, with its signature
	log_buf[0] = 0;
	f = FidoOpen("pl-PL");
	CHECK(f != NULL);
	if (f == NULL)
		return;
	CHECK_CONTAINS(log_buf, "Download signature is valid ✓");
	CHECK(FidoNbVersions(f) >= 3);
	CHECK_EQ_STR(FidoVersionName(f, 0), "Windows 11");
	CHECK_EQ_STR(FidoEditionName(f, 0, 0, 0), "Windows 11 Home/Pro/Edu");

	// The languages of Windows 11: a whitelisted session for each of its IDs (x64 and ARM64)
	n = FidoGetLanguages(f, 0, 0, 0, &preferred);
	CHECK_EQ_INT(n, 3);
	CHECK(whitelisted);
	if (n == 3) {
		CHECK_EQ_STR(FidoLanguageName(f, 1), "Polski");
		CHECK_EQ_STR(FidoLanguageName(f, 2), "\xe4\xb8\xad\xe6\x96\x87(\xe7\xae\x80\xe4\xbd\x93)");
		CHECK_EQ_INT(preferred, 1);
		// The Polish SKUs of both IDs: an x64 and an ARM64 link
		n = FidoGetLinks(f, 0, 0, 0, 1, &preferred);
		CHECK_EQ_INT(n, 2);
		CHECK_EQ_STR(last_referer, "https://www.microsoft.com/software-download/windows11");
		if (n == 2) {
			CHECK_EQ_STR(FidoLinkArch(f, 0), "x64");
			CHECK_EQ_STR(FidoLinkUrl(f, 0), "https://software.download.prss.microsoft.com/38132.iso?t=1");
			CHECK_EQ_STR(FidoLinkArch(f, 1), "ARM64");
			CHECK_EQ_STR(FidoLinkUrl(f, 1), "https://software.download.prss.microsoft.com/38162.iso?t=1");
		}
		// Banned by Microsoft (code 715-123130)
		banned = TRUE;
		CHECK_EQ_INT(FidoGetLinks(f, 0, 0, 0, 1, &preferred), -1);
		CHECK_CONTAINS(FidoError(f), "715-123130");
		banned = FALSE;
	}

	// The UEFI Shell: no languages, and the ISO of the release on GitHub
	for (i = 0; (i < FidoNbVersions(f)) && !FidoIsUefiShell(f, i); i++);
	CHECK(i < FidoNbVersions(f));
	if (i < FidoNbVersions(f)) {
		CHECK_EQ_INT(FidoGetLanguages(f, i, 0, 1, &preferred), 1);
		CHECK_EQ_STR(FidoLanguageName(f, 0), "English (US)");
		CHECK_EQ_INT(FidoGetLinks(f, i, 0, 1, 0, &preferred), 1);
		CHECK_EQ_STR(FidoLinkArch(f, 0), "x64, ia32, aa64");
		CHECK_EQ_STR(FidoLinkUrl(f, 0),
			"https://github.com/pbatard/UEFI-Shell/releases/download/26H1/UEFI-Shell-2.2-26H1-DEBUG.iso");
	}
	FidoClose(f);

	// A locale that the download site does not have
	f = FidoOpen("xx-XX");
	CHECK(f != NULL);
	if (f != NULL) {
		CHECK_EQ_INT(FidoGetLanguages(f, 0, 0, 0, &preferred), 3);
		CHECK_EQ_INT(preferred, 0);
		FidoClose(f);
	}

	// No Fido.ver (nothing listens there)
	setenv("RUFUS_TEST_URL_PREFIX", "http://127.0.0.1:9", 1);
	CHECK(!FidoCheck());
	CHECK(fido_url == NULL);
	unsetenv("RUFUS_TEST_URL_PREFIX");
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };
	const char* data = getenv("RUFUS_TEST_DATA");

	if (data == NULL)
		return 1;
	snprintf(data_dir, sizeof(data_dir), "%s/download", data);
	ui_set_iface(&iface);
	if (!test_files_init() || !http_start(serve))
		return 1;

	test_json();
	test_script_data();
	test_fido();

	test_files_cleanup();
	return TEST_RESULT();
}

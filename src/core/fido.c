/*
 * Rufus: The Reliable USB Formatting Utility
 * Fido: the downloads of Windows and UEFI Shell ISOs
 * Copyright © 2019-2026 Pete Batard <pete@akeo.ie>
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
 * The steps of Fido.ps1 (https://github.com/pbatard/Fido), in C:
 * - the data of the script: $WindowsVersions (the versions, their releases and
 *   the product edition IDs of their editions), and the IDs that the Microsoft
 *   download site wants ($OrgId, $ProfileId, $InstanceId);
 * - Get-Windows-Languages: a session ID, whitelisted through vlscppe and ov-df,
 *   then the SKUs of each product edition (getskuinformationbyproductedition);
 * - Get-Windows-Download-Links: the links of the SKUs of a language
 *   (GetProductDownloadLinksBySku), or the ISOs of the UEFI Shell releases on
 *   GitHub, with the architectures of their Version.xml.
 * The script comes from DownloadISOThread() of net.c, which checks its signature.
 */

#include <endian.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/time.h>
#include <sys/utsname.h>

#include "rufus.h"
#include "settings.h"
#include "bled/bled.h"
#include "download.h"
#include "json.h"
#include "xml.h"
#include "fido.h"

#define FIDO_MAX_IDS                4
#define FIDO_MAX_SCRIPT_SIZE        (1 * MB)
#define MS_DOWNLOAD_URL             "https://www.microsoft.com/software-download-connector/api"
#define MS_REFERER                  "https://www.microsoft.com/software-download/windows11"
#define UEFI_SHELL_URL              "https://github.com/pbatard/UEFI-Shell/releases/download"

BOOL fido_available = FALSE;
char* fido_url = NULL;
// The verbosity of the logs of the queries, which the update check settings have upstream
static int verbose = 0;

typedef struct {
	char* name;
	int ids[FIDO_MAX_IDS];			// One per architecture group (x64, ARM64)
	int nb_ids;
} fido_edition_t;

typedef struct {
	char* name;
	fido_edition_t* editions;
	int nb;
} fido_release_t;

typedef struct {
	char* name;
	char* id;						// "windows11", "UEFI_SHELL 2.2"...
	fido_release_t* releases;
	int nb;
} fido_version_t;

typedef struct {
	char* name;						// "English"
	char* display_name;				// "English (United States)"
	struct {
		int session;
		char* id;
	} skus[FIDO_MAX_IDS];
	int nb_skus;
} fido_language_t;

typedef struct {
	char* arch;
	char* url;
} fido_link_t;

struct fido {
	fido_version_t* versions;
	int nb_versions;
	char org_id[64], profile_id[64], instance_id[64];
	char locale[16];				// Of the user
	char query_locale[16];			// For the download site (en-US if it doesn't have the one of the user)
	char session_id[FIDO_MAX_IDS][40];
	fido_language_t* languages;
	int nb_languages;
	fido_link_t* links;
	int nb_links;
	char error[1024];
};

/*
 * The data of the script: PowerShell arrays of strings and numbers
 */

typedef struct ps_node {
	BOOL is_array;
	char* str;						// A string, or NULL for a number or an array
	long num;
	struct ps_node* child;
	struct ps_node* next;
} ps_node_t;

static void PsFree(ps_node_t* n)
{
	ps_node_t* next;

	while (n != NULL) {
		next = n->next;
		PsFree(n->child);
		free(n->str);
		free(n);
		n = next;
	}
}

// The elements of an array, after its "@(", up to its ")"
static ps_node_t* PsParseArray(const char** pp, const char* end, int depth)
{
	ps_node_t *array = calloc(1, sizeof(ps_node_t)), *n, **last;
	const char *p = *pp, *start;

	if ((array == NULL) || (depth > 16))
		goto error;
	array->is_array = TRUE;
	last = &array->child;
	while (p < end) {
		// Separators, and comments
		if ((*p == ' ') || (*p == '\t') || (*p == '\r') || (*p == '\n') || (*p == ',')) {
			p++;
			continue;
		}
		if (*p == '#') {
			while ((p < end) && (*p != '\n'))
				p++;
			continue;
		}
		if (*p == ')') {
			*pp = p + 1;
			return array;
		}
		n = NULL;
		if ((*p == '@') && (p + 1 < end) && (p[1] == '(')) {
			p += 2;
			n = PsParseArray(&p, end, depth + 1);
		} else if (*p == '"') {
			start = ++p;
			while ((p < end) && (*p != '"'))
				p++;
			if (p >= end)
				goto error;
			n = calloc(1, sizeof(ps_node_t));
			if (n != NULL)
				n->str = strndup(start, (size_t)(p - start));
			p++;
			if ((n != NULL) && (n->str == NULL))
				goto error;
		} else if ((*p >= '0') && (*p <= '9')) {
			n = calloc(1, sizeof(ps_node_t));
			if (n != NULL)
				n->num = strtol(p, (char**)&p, 10);
		} else {
			goto error;
		}
		if (n == NULL)
			goto error;
		*last = n;
		last = &n->next;
	}

error:
	PsFree(array);
	return NULL;
}

// The value of a '$Name = "value"' assignment of the script
static BOOL PsGetVariable(const char* script, const char* name, char* value, size_t len)
{
	char pattern[64];
	const char *p, *q;

	snprintf(pattern, sizeof(pattern), "\n$%s = \"", name);
	p = strstr(script, pattern);
	if (p == NULL)
		return FALSE;
	p += strlen(pattern);
	q = strchr(p, '"');
	if ((q == NULL) || (q == p) || ((size_t)(q - p) >= len) || (memchr(p, '\n', (size_t)(q - p)) != NULL))
		return FALSE;
	memcpy(value, p, (size_t)(q - p));
	value[q - p] = 0;
	return TRUE;
}

static int PsCount(const ps_node_t* n)
{
	int i;

	for (i = 0; n != NULL; n = n->next)
		i++;
	return i;
}

// $WindowsVersions: @( @( @("name", "id"), @("release", @("edition", id or @(ids)), ...), ...), ...)
static BOOL ParseVersions(fido_t* f, const ps_node_t* versions)
{
	const ps_node_t *v, *r, *e, *id;
	fido_version_t* version;
	fido_release_t* release;
	fido_edition_t* edition;

	f->versions = calloc(PsCount(versions->child), sizeof(fido_version_t));
	if (f->versions == NULL)
		return FALSE;
	for (v = versions->child; v != NULL; v = v->next) {
		if (!v->is_array || (v->child == NULL) || !v->child->is_array || (PsCount(v->child->child) < 2) ||
			(v->child->child->str == NULL) || (v->child->child->next->str == NULL))
			return FALSE;
		version = &f->versions[f->nb_versions++];
		version->name = strdup(v->child->child->str);
		version->id = strdup(v->child->child->next->str);
		version->releases = calloc(PsCount(v->child->next), sizeof(fido_release_t));
		if ((version->name == NULL) || (version->id == NULL) || (version->releases == NULL))
			return FALSE;
		for (r = v->child->next; r != NULL; r = r->next) {
			if (!r->is_array || (r->child == NULL) || (r->child->str == NULL))
				return FALSE;
			release = &version->releases[version->nb++];
			release->name = strdup(r->child->str);
			release->editions = calloc(PsCount(r->child->next), sizeof(fido_edition_t));
			if ((release->name == NULL) || (release->editions == NULL))
				return FALSE;
			for (e = r->child->next; e != NULL; e = e->next) {
				if (!e->is_array || (PsCount(e->child) != 2) || (e->child->str == NULL) || (e->child->next->str != NULL))
					return FALSE;
				// "Windows 11 Home China " has a trailing space
				if ((strstr(e->child->str, "China") != NULL) && (strncmp(f->locale, "zh", 2) != 0))
					continue;
				edition = &release->editions[release->nb++];
				edition->name = strdup(e->child->str);
				if (edition->name == NULL)
					return FALSE;
				trim(edition->name);
				if (e->child->next->is_array) {
					for (id = e->child->next->child; (id != NULL) && (edition->nb_ids < FIDO_MAX_IDS); id = id->next)
						edition->ids[edition->nb_ids++] = (int)id->num;
				} else {
					edition->ids[edition->nb_ids++] = (int)e->child->next->num;
				}
			}
		}
	}
	return (f->nb_versions > 0);
}

fido_t* FidoOpenScript(const char* script, size_t len, const char* locale)
{
	fido_t* f = calloc(1, sizeof(fido_t));
	const char *p, *end = script + len;
	ps_node_t* versions = NULL;

	if (f == NULL)
		return NULL;
	static_strcpy(f->locale, (locale == NULL) ? "en-US" : locale);
	static_strcpy(f->query_locale, f->locale);
	p = strstr(script, "\n$WindowsVersions = @(");
	if ((p == NULL) || !PsGetVariable(script, "OrgId", f->org_id, sizeof(f->org_id)) ||
		!PsGetVariable(script, "ProfileId", f->profile_id, sizeof(f->profile_id)) ||
		!PsGetVariable(script, "InstanceId", f->instance_id, sizeof(f->instance_id))) {
		uprintf("The download script has no data");
		goto error;
	}
	p += strlen("\n$WindowsVersions = @(");
	versions = PsParseArray(&p, end, 0);
	if ((versions == NULL) || !ParseVersions(f, versions)) {
		uprintf("Could not parse the data of the download script");
		goto error;
	}
	PsFree(versions);
	return f;

error:
	PsFree(versions);
	FidoClose(f);
	return NULL;
}

/*
 * The script
 */

BOOL FidoCheck(void)
{
	char* ver = NULL;
	uint64_t len;

	fido_available = FALSE;
	safe_free(fido_url);
	// Get the Fido URL from parsing a 'Fido.ver' on our server. This enables the use of different
	// Fido versions from different versions of Rufus, if needed, as opposed to always downloading
	// the latest release from GitHub, which may contain incompatible changes...
	len = DownloadToFileOrBuffer(RUFUS_URL "/Fido.ver", NULL, (BYTE**)&ver, NULL, FALSE);
	if ((len == 0) || (len >= 4 * KB))
		goto out;
	len++;	// DownloadToFileOrBuffer allocated an extra NUL character if needed
	fido_url = get_token_data_buffer(FIDO_VERSION, 1, ver, (size_t)len);
	if (safe_strncmp(fido_url, "https://github.com/pbatard/Fido", 31) != 0) {
		uprintf("WARNING: Download script URL %s is invalid ✗", fido_url);
		safe_free(fido_url);
		goto out;
	}
	fido_available = IsDownloadable(fido_url);

out:
	free(ver);
	return fido_available;
}

fido_t* FidoOpen(const char* locale)
{
	char sig_url[256];
	BYTE *compressed = NULL, *sig = NULL;
	char* script = NULL;
	uint64_t compressed_size, sig_size, uncompressed_size;
	int64_t size = -1;
	fido_t* f = NULL;

	if (fido_url == NULL)
		return NULL;
	compressed_size = DownloadToFileOrBuffer(fido_url, NULL, &compressed, NULL, FALSE);
	if (compressed_size <= 13)
		goto out;
	static_sprintf(sig_url, "%s.sig", fido_url);
	sig_size = DownloadToFileOrBuffer(sig_url, NULL, &sig, NULL, FALSE);
	if ((sig_size != RSA_SIGNATURE_SIZE) || (!ValidateOpensslSignature(compressed, (DWORD)compressed_size, sig, (DWORD)sig_size))) {
		uprintf("FATAL: Download signature is invalid ✗");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BAD_SIGNATURE));
		goto out;
	}
	uprintf("Download signature is valid ✓");
	// The size, after the properties and the dictionary size of the LZMA header
	memcpy(&uncompressed_size, &compressed[5], sizeof(uncompressed_size));
	uncompressed_size = le64toh(uncompressed_size);
	if ((uncompressed_size < FIDO_MAX_SCRIPT_SIZE) && (bled_init(0, uprintf, NULL, NULL, NULL, NULL, &ErrorStatus) >= 0)) {
		script = calloc(1, (size_t)uncompressed_size + 1);
		if (script != NULL)
			size = bled_uncompress_from_buffer_to_buffer((char*)compressed, (size_t)compressed_size, script,
				(size_t)uncompressed_size, BLED_COMPRESSION_LZMA);
		bled_exit();
	}
	if ((script == NULL) || (size != (int64_t)uncompressed_size)) {
		uprintf("FATAL: Could not uncompress download script");
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_DATA);
		goto out;
	}
	f = FidoOpenScript(script, (size_t)size, locale);

out:
	free(compressed);
	free(sig);
	free(script);
	return f;
}

static void FreeLanguages(fido_t* f)
{
	int i, j;

	for (i = 0; i < f->nb_languages; i++) {
		free(f->languages[i].name);
		free(f->languages[i].display_name);
		for (j = 0; j < f->languages[i].nb_skus; j++)
			free(f->languages[i].skus[j].id);
	}
	safe_free(f->languages);
	f->nb_languages = 0;
}

static void FreeLinks(fido_t* f)
{
	int i;

	for (i = 0; i < f->nb_links; i++) {
		free(f->links[i].arch);
		free(f->links[i].url);
	}
	safe_free(f->links);
	f->nb_links = 0;
}

void FidoClose(fido_t* f)
{
	int i, j, k;

	if (f == NULL)
		return;
	for (i = 0; i < f->nb_versions; i++) {
		for (j = 0; j < f->versions[i].nb; j++) {
			for (k = 0; k < f->versions[i].releases[j].nb; k++)
				free(f->versions[i].releases[j].editions[k].name);
			free(f->versions[i].releases[j].editions);
			free(f->versions[i].releases[j].name);
		}
		free(f->versions[i].releases);
		free(f->versions[i].name);
		free(f->versions[i].id);
	}
	free(f->versions);
	FreeLanguages(f);
	FreeLinks(f);
	free(f);
}

const char* FidoError(const fido_t* f)
{
	return f->error;
}

/*
 * The data
 */

int FidoNbVersions(const fido_t* f)
{
	return f->nb_versions;
}

const char* FidoVersionName(const fido_t* f, int version)
{
	return f->versions[version].name;
}

BOOL FidoIsUefiShell(const fido_t* f, int version)
{
	return (strncmp(f->versions[version].id, "UEFI_SHELL", 10) == 0);
}

int FidoNbReleases(const fido_t* f, int version)
{
	return f->versions[version].nb;
}

const char* FidoReleaseName(const fido_t* f, int version, int release)
{
	return f->versions[version].releases[release].name;
}

int FidoNbEditions(const fido_t* f, int version, int release)
{
	return f->versions[version].releases[release].nb;
}

const char* FidoEditionName(const fido_t* f, int version, int release, int edition)
{
	return f->versions[version].releases[release].editions[edition].name;
}

const char* FidoLanguageName(const fido_t* f, int language)
{
	return f->languages[language].display_name;
}

const char* FidoLinkArch(const fido_t* f, int link)
{
	return f->links[link].arch;
}

const char* FidoLinkUrl(const fido_t* f, int link)
{
	return f->links[link].url;
}

/*
 * The network steps
 */

static void SetError(fido_t* f, const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(f->error, sizeof(f->error), format, args);
	va_end(args);
	uprintf("Error: %s", f->error);
}

// A GET request, which fails unless it gets 200 OK
static char* Query(fido_t* f, const char* url, const char* referer, BOOL follow_redirects)
{
	BYTE* buf = NULL;
	long status;

	vuprintf("Querying %s", url);
	if (HttpGet(url, referer, follow_redirects, &buf, &status) == 0 && buf == NULL) {
		SetError(f, "%s", WindowsErrorString());
		return NULL;
	}
	if (status != 200) {
		SetError(f, "%s returned HTTP status %ld", url, status);
		free(buf);
		return NULL;
	}
	return (char*)buf;
}

// Check-Locale: the download site may not have the locale of the user
static void CheckLocale(fido_t* f)
{
	char url[256];
	BYTE* buf = NULL;
	long status = 0;

	static_strcpy(f->query_locale, f->locale);
	static_sprintf(url, "https://www.microsoft.com/%s/software-download/windows11", f->query_locale);
	vuprintf("Querying %s", url);
	HttpGet(url, NULL, FALSE, &buf, &status);
	free(buf);
	if (status != 200)
		static_strcpy(f->query_locale, "en-US");
}

// A random session ID, as [guid]::NewGuid() gives it
static void NewSessionId(char* id, size_t len)
{
	uint8_t g[16];
	int i;

	if (getrandom(g, sizeof(g), 0) != sizeof(g)) {
		for (i = 0; i < 16; i++)
			g[i] = (uint8_t)rand();
	}
	g[6] = (g[6] & 0x0f) | 0x40;
	g[8] = (g[8] & 0x3f) | 0x80;
	snprintf(id, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

// The value of a parameter that follows 'key' in a script ("w=" or "rticks=\"")
static BOOL GetParameter(const char* text, const char* key, const char* allowed, char* value, size_t len)
{
	const char* p = text;
	size_t n;

	while ((p = strstr(p, key)) != NULL) {
		p += strlen(key);
		if (*p == '+')
			p++;
		n = strspn(p, allowed);
		if ((n > 0) && (n < len)) {
			memcpy(value, p, n);
			value[n] = 0;
			return TRUE;
		}
	}
	return FALSE;
}

// Microsoft download "protection": the session ID must be whitelisted, and go through ov-df
static BOOL WhitelistSession(fido_t* f, const char* session_id)
{
	char url[512], w[128], rticks[32], *r;
	struct timeval tv;

	static_sprintf(url, "https://vlscppe.microsoft.com/tags?org_id=%s&session_id=%s", f->org_id, session_id);
	r = Query(f, url, NULL, FALSE);
	if (r == NULL)
		return FALSE;
	free(r);

	// 1) Request mdt.js to get w and rticks. InstanceId is (currently) constant.
	static_sprintf(url, "https://ov-df.microsoft.com/mdt.js?instanceId=%s&PageId=si&session_id=%s",
		f->instance_id, session_id);
	r = Query(f, url, NULL, FALSE);
	if (r == NULL)
		return FALSE;
	if (!GetParameter(r, "?w=", "0123456789ABCDEF", w, sizeof(w)) &&
		!GetParameter(r, "&w=", "0123456789ABCDEF", w, sizeof(w))) {
		free(r);
		SetError(f, "Could not extract ov-df data");
		return FALSE;
	}
	if (!GetParameter(r, "rticks=\"", "0123456789", rticks, sizeof(rticks))) {
		free(r);
		SetError(f, "Could not extract ov-df data");
		return FALSE;
	}
	free(r);

	// 2) Send a reply with session ID, current epoch and previously retrieved w and rticks
	gettimeofday(&tv, NULL);
	static_sprintf(url, "https://ov-df.microsoft.com/?session_id=%s&CustomerId=%s&PageId=si&w=%s&mdt=%lld&rticks=%s",
		session_id, f->instance_id, w, (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000, rticks);
	r = Query(f, url, NULL, FALSE);
	if (r == NULL)
		return FALSE;
	free(r);
	return TRUE;
}

/*
 * Select-Language: the language of the download site that the locale of the user
 * is for (prefixes of the locale, and words of the English name of the language).
 */
static BOOL IsUserLanguage(const char* locale, const char* name)
{
	static const struct {
		const char* locale;			// A prefix, or a whole locale if it ends with '='
		const char* words[2];		// That the name contains, or is, with '='
	} map[] = {
		{ "ar", { "Arabic" } }, { "pt-BR=", { "Brazil" } }, { "bg", { "Bulgar" } },
		{ "zh-CN=", { "Chinese", "simp" } }, { "zh-TW=", { "Chinese", "trad" } }, { "hr", { "Croat" } },
		{ "cs", { "Czech" } }, { "da", { "Danish" } }, { "nl", { "Dutch" } }, { "en-US=", { "=English" } },
		{ "en", { "English", "inter" } }, { "en", { "English", "ingdom" } }, { "et", { "Eston" } },
		{ "fi", { "Finn" } }, { "fr-CA=", { "French", "Canad" } }, { "fr", { "=French" } }, { "de", { "German" } },
		{ "el", { "Greek" } }, { "he", { "Hebrew" } }, { "hu", { "Hungar" } }, { "id", { "Indones" } },
		{ "it", { "Italia" } }, { "ja", { "Japan" } }, { "ko", { "Korea" } }, { "lv", { "Latvia" } },
		{ "lt", { "Lithuania" } }, { "ms", { "Malay" } }, { "nb", { "Norw" } }, { "fa", { "Persia" } },
		{ "pl", { "Polish" } }, { "pt-PT=", { "=Portuguese" } }, { "ro", { "Romania" } }, { "ru", { "Russia" } },
		{ "sr", { "Serbia" } }, { "sk", { "Slovak" } }, { "sl", { "Slovenia" } }, { "es-ES=", { "=Spanish" } },
		{ "es", { "Spanish" } }, { "sv", { "Swed" } }, { "th", { "Thai" } }, { "tr", { "Turk" } },
		{ "uk", { "Ukrain" } }, { "vi", { "Vietnam" } },
	};
	size_t i, j, n;
	BOOL match;

	for (i = 0; i < ARRAYSIZE(map); i++) {
		n = strlen(map[i].locale);
		if (map[i].locale[n - 1] == '=') {
			if ((strlen(locale) != n - 1) || (strncmp(locale, map[i].locale, n - 1) != 0))
				continue;
		} else if (strncmp(locale, map[i].locale, n) != 0) {
			continue;
		}
		for (match = TRUE, j = 0; match && (j < ARRAYSIZE(map[i].words)) && (map[i].words[j] != NULL); j++) {
			if (map[i].words[j][0] == '=')
				match = (strcmp(name, &map[i].words[j][1]) == 0);
			else
				match = (strcasestr(name, map[i].words[j]) != NULL);
		}
		if (match)
			return TRUE;
	}
	return FALSE;
}

int FidoGetLanguages(fido_t* f, int version, int release, int edition, int* preferred)
{
	const fido_edition_t* e = &f->versions[version].releases[release].editions[edition];
	json_value_t *doc = NULL, *skus, *sku, *errors;
	const char *lang, *display, *id;
	char url[512], *r = NULL;
	int i, j, attempt;
	fido_language_t* l;

	FreeLanguages(f);
	FreeLinks(f);
	*preferred = 0;
	f->error[0] = 0;
	verbose = ReadSetting32(SETTING_VERBOSE_UPDATES);
	if (FidoIsUefiShell(f, version)) {
		f->languages = calloc(1, sizeof(fido_language_t));
		if (f->languages == NULL)
			return -1;
		f->languages[0].name = strdup("en-us");
		f->languages[0].display_name = strdup("English (US)");
		f->nb_languages = 1;
		return 1;
	}
	CheckLocale(f);
	for (i = 0; i < e->nb_ids; i++) {
		NewSessionId(f->session_id[i], sizeof(f->session_id[i]));
		if (!WhitelistSession(f, f->session_id[i]))
			goto error;
		static_sprintf(url, MS_DOWNLOAD_URL "/getskuinformationbyproductedition?profile=%s&productEditionId=%d"
			"&SKU=undefined&friendlyFileName=undefined&Locale=%s&sessionID=%s", f->profile_id, e->ids[i],
			f->query_locale, f->session_id[i]);
		// May require a few attempts
		for (attempt = 0; ; attempt++) {
			if (attempt != 0)
				sleep(2);
			if (IS_ERROR(ErrorStatus))
				goto error;
			JsonFree(doc);
			free(r);
			r = Query(f, url, NULL, TRUE);
			doc = (r == NULL) ? NULL : JsonParse(r, strlen(r));
			errors = JsonGet(doc, "Errors");
			skus = JsonGet(doc, "Skus");
			if ((doc == NULL) || ((errors != NULL) && (errors->child != NULL)) || (skus == NULL) || (skus->child == NULL)) {
				if (attempt < 2)
					continue;
				if ((errors != NULL) && (errors->child != NULL) && (JsonGetString(errors->child, "Value") != NULL))
					SetError(f, "%s", JsonGetString(errors->child, "Value"));
				else if (f->error[0] == 0)
					SetError(f, "Could not retrieve languages from server");
				goto error;
			}
			break;
		}
		for (sku = skus->child; sku != NULL; sku = sku->next) {
			lang = JsonGetString(sku, "Language");
			display = JsonGetString(sku, "LocalizedLanguage");
			id = JsonGetString(sku, "Id");
			if ((lang == NULL) || (id == NULL))
				continue;
			for (j = 0; (j < f->nb_languages) && (strcmp(f->languages[j].name, lang) != 0); j++);
			if (j == f->nb_languages) {
				l = realloc(f->languages, (f->nb_languages + 1) * sizeof(fido_language_t));
				if (l == NULL)
					goto error;
				f->languages = l;
				memset(&l[j], 0, sizeof(fido_language_t));
				l[j].name = strdup(lang);
				l[j].display_name = strdup((display != NULL) ? display : lang);
				f->nb_languages++;
			}
			l = &f->languages[j];
			if (l->nb_skus < FIDO_MAX_IDS) {
				l->skus[l->nb_skus].session = i;
				l->skus[l->nb_skus++].id = strdup(id);
			}
		}
	}
	JsonFree(doc);
	free(r);
	if (f->nb_languages == 0) {
		SetError(f, "Could not parse languages");
		return -1;
	}
	for (i = 0; i < f->nb_languages; i++) {
		if (IsUserLanguage(f->locale, f->languages[i].name))
			*preferred = i;
	}
	return f->nb_languages;

error:
	JsonFree(doc);
	free(r);
	FreeLanguages(f);
	if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED))
		SetError(f, "%s", StrError(ErrorStatus, FALSE));
	return -1;
}

static BOOL AddLink(fido_t* f, const char* arch, const char* url)
{
	fido_link_t* l = realloc(f->links, (f->nb_links + 1) * sizeof(fido_link_t));

	if (l == NULL)
		return FALSE;
	f->links = l;
	l[f->nb_links].arch = strdup(arch);
	l[f->nb_links].url = strdup(url);
	f->nb_links++;
	return TRUE;
}

// The architecture of this machine, as Microsoft names it
static const char* PlatformArch(void)
{
	struct utsname name;

	if (uname(&name) != 0)
		return "x64";
	if (strcmp(name.machine, "aarch64") == 0)
		return "ARM64";
	if ((name.machine[0] == 'i') && (strcmp(&name.machine[2], "86") == 0))
		return "x86";
	return "x64";
}

int FidoGetLinks(fido_t* f, int version, int release, int edition, int language, int* preferred)
{
	static const char* arch_names[] = { "x86", "x64", "ARM64" };
	const fido_version_t* v = &f->versions[version];
	const fido_language_t* l = &f->languages[language];
	json_value_t *doc = NULL, *options, *option, *errors, *type;
	char url[512], link[512], tag[64], archs[128], *r = NULL;
	const char* uri;
	ezxml_t xml, arch;
	int i;

	FreeLinks(f);
	*preferred = 0;
	f->error[0] = 0;
	if (FidoIsUefiShell(f, version)) {
		// The release is "26H1 (edk2-stable202602)", and the version "UEFI_SHELL 2.2"
		static_strcpy(tag, v->releases[release].name);
		tag[strcspn(tag, " ")] = 0;
		static_sprintf(link, UEFI_SHELL_URL "/%s/UEFI-Shell-%s-%s-%s.iso", tag, &v->id[strcspn(v->id, " ") + 1], tag,
			(v->releases[release].editions[edition].ids[0] == 0) ? "RELEASE" : "DEBUG");
		// Read the supported archs from the release URL
		static_sprintf(url, UEFI_SHELL_URL "/%s/Version.xml", tag);
		r = Query(f, url, NULL, TRUE);
		if (r == NULL)
			return -1;
		archs[0] = 0;
		xml = ezxml_parse_str(r, strlen(r));
		for (arch = ezxml_child(ezxml_child(xml, "supported_archs"), "arch"); arch != NULL; arch = ezxml_next(arch)) {
			if (archs[0] != 0)
				static_strcat(archs, ", ");
			static_strcat(archs, ezxml_txt(arch));
		}
		ezxml_free(xml);
		free(r);
		if ((archs[0] == 0) || !AddLink(f, archs, link)) {
			SetError(f, "Could not read the architectures of the UEFI Shell");
			return -1;
		}
		return 1;
	}

	for (i = 0; i < l->nb_skus; i++) {
		static_sprintf(url, MS_DOWNLOAD_URL "/GetProductDownloadLinksBySku?profile=%s&productEditionId=undefined"
			"&SKU=%s&friendlyFileName=undefined&Locale=%s&sessionID=%s", f->profile_id, l->skus[i].id, f->query_locale,
			f->session_id[l->skus[i].session]);
		// Must add a referer for this request, else Microsoft's servers may deny it
		r = Query(f, url, MS_REFERER, TRUE);
		doc = (r == NULL) ? NULL : JsonParse(r, strlen(r));
		if (doc == NULL) {
			if (f->error[0] == 0)
				SetError(f, "Could not retrieve architectures from server");
			goto error;
		}
		errors = JsonGet(doc, "Errors");
		if ((errors != NULL) && (errors->child != NULL)) {
			type = JsonGet(errors->child, "Type");
			if ((type != NULL) && (type->type == JSON_NUMBER) && (type->number == 9))
				SetError(f, "Your IP address has been banned by Microsoft for issuing too many ISO download requests "
					"or for belonging to a region of the world where sanctions currently apply. Please try again later. "
					"If you believe this ban to be in error, you can try contacting Microsoft by referring to message "
					"code 715-123130 and session ID %s.", f->session_id[l->skus[i].session]);
			else
				SetError(f, "%s", (JsonGetString(errors->child, "Value") != NULL) ?
					JsonGetString(errors->child, "Value") : "Could not retrieve ISO download links");
			goto error;
		}
		options = JsonGet(doc, "ProductDownloadOptions");
		for (option = (options == NULL) ? NULL : options->child; option != NULL; option = option->next) {
			type = JsonGet(option, "DownloadType");
			uri = JsonGetString(option, "Uri");
			if ((type == NULL) || (type->type != JSON_NUMBER) || (uri == NULL))
				continue;
			if (!AddLink(f, ((type->number >= 0) && (type->number < ARRAYSIZE(arch_names))) ?
				arch_names[(int)type->number] : "Unknown", uri))
				goto error;
		}
		JsonFree(doc);
		doc = NULL;
		safe_free(r);
	}
	if (f->nb_links == 0) {
		SetError(f, "Could not retrieve ISO download links");
		return -1;
	}
	for (i = 0; i < f->nb_links; i++) {
		if (strcmp(f->links[i].arch, PlatformArch()) == 0)
			*preferred = i;
	}
	return f->nb_links;

error:
	JsonFree(doc);
	free(r);
	FreeLinks(f);
	return -1;
}

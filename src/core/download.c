/*
 * Rufus: The Reliable USB Formatting Utility
 * Downloads
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
 * The downloads of net.c, with libcurl instead of WinInet: the bootloader
 * files of the boot check (signed with the key of pki.c), diskcopy.dll, the
 * Secure Boot revocation data and the UEFI DBX updates. The update check of
 * upstream is not ported (see LINUX_PORT.md §5.7), and Fido is in fido.c.
 *
 * Errors are reported through errno (for WindowsErrorString()), as the WinInet
 * calls of upstream do through the last error.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <curl/curl.h>

#include "rufus.h"
#include "resource.h"
#include "ui_iface.h"
#include "settings.h"
#include "localization.h"
#include "download.h"
#include "../res/dbx/dbx_info.h"

DWORD DownloadStatus = 0;
extern const char* efi_archname[ARCH_MAX];
extern char *sbat_level_txt, *sb_active_txt, *sb_revoked_txt;

/* How long a transfer may stay below 1 byte/s before it is aborted, in seconds */
#define DOWNLOAD_STALL_TIMEOUT      30
/* How often the remote Secure Boot data and the DBX updates get checked (1 day) */
#define DBX_CHECK_INTERVAL          (24 * 3600)

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_init_status = CURLE_FAILED_INIT;

static void InitCurl(void)
{
	curl_init_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

// The errno that best describes a failed transfer (for WindowsErrorString())
static int CurlErrno(CURLcode code)
{
	switch (code) {
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
		return EHOSTUNREACH;
	case CURLE_COULDNT_CONNECT:
		return ECONNREFUSED;
	case CURLE_OPERATION_TIMEDOUT:
		return ETIMEDOUT;
	case CURLE_WRITE_ERROR:
		return (errno != 0) ? errno : EIO;
	case CURLE_OUT_OF_MEMORY:
		return ENOMEM;
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_PEER_FAILED_VERIFICATION:
		return EPROTO;
	case CURLE_ABORTED_BY_CALLBACK:
		return ECANCELED;
	case CURLE_TOO_MANY_REDIRECTS:
		return ELOOP;
	default:
		return EIO;
	}
}

static int HttpErrno(long status)
{
	switch (status) {
	case 401:
	case 403:
		return EACCES;
	case 404:
	case 410:
		return ENOENT;
	default:
		return EIO;
	}
}

// The default user agent: "Rufus/4.16.0 (Linux; x86_64)"
static const char* DefaultUserAgent(void)
{
	static char agent[64];
	struct utsname name;

	if (agent[0] == 0) {
		if (uname(&name) != 0)
			static_strcpy(name.machine, "unknown");
		static_sprintf(agent, APPLICATION_NAME "/%d.%d.%d (Linux; %s)",
			rufus_version[0], rufus_version[1], rufus_version[2], name.machine);
	}
	return agent;
}

/*
 * Test hook: with $RUFUS_TEST_URL_PREFIX set (to "http://127.0.0.1:8080" for
 * instance), "https://rufus.ie/files/x" is downloaded from
 * "http://127.0.0.1:8080/rufus.ie/files/x", and likewise for every server.
 * Signatures are still checked with the key of pki.c.
 */
static const char* TestUrl(const char* url, char* buf, size_t len)
{
	const char* prefix = getenv("RUFUS_TEST_URL_PREFIX");
	const char* p = strstr(url, "://");

	if (prefix == NULL || prefix[0] == 0 || p == NULL)
		return url;
	snprintf(buf, len, "%s/%s", prefix, &p[3]);
	return buf;
}

/*
 * A libcurl handle with the options of the WinInet session of upstream: the user
 * agent, the timeouts, no compression (so that the length of the file is known),
 * and the redirections (only to HTTPS, unless the URL itself is HTTP).
 */
static CURL* NewTransfer(const char* url, const char* user_agent, struct curl_slist** headers)
{
	char test_url[1024];
	CURL* curl;

	pthread_once(&curl_once, InitCurl);
	if (curl_init_status != CURLE_OK)
		return NULL;
	curl = curl_easy_init();
	if (curl == NULL)
		return NULL;
	url = TestUrl(url, test_url, sizeof(test_url));
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, (user_agent == NULL) ? DefaultUserAgent() : user_agent);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, (strncmp(url, "https://", 8) == 0) ? "https" : "http,https");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)NET_SESSION_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)DOWNLOAD_STALL_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
	// Must use "Accept-Encoding: identity" to get the file size
	*headers = curl_slist_append(NULL, "Accept-Encoding: identity");
	// If we are querying the GitHub API, we need to enable raw content
	if (strstr(url, "api.github.com") != NULL)
		*headers = curl_slist_append(*headers, "Accept: application/vnd.github.v3.raw");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
	return curl;
}

typedef struct {
	CURL* curl;
	const char* url;
	const char* short_name;
	int fd;						// The file being downloaded, or -1
	BYTE** buffer;				// The buffer being downloaded, if fd is -1
	uint64_t size, total_size, buffer_size;
	BOOL started, progress;
	int error;					// errno of a failed write
} transfer_t;

// The headers have been received: check the status and the length, as upstream does
static BOOL StartTransfer(transfer_t* t)
{
	long status = 0;
	curl_off_t length = -1;

	t->started = TRUE;
	curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
	DownloadStatus = (DWORD)status;
	if (status != 200) {
		uprintf("%s '%s': %ld", (status == 404) ? "File not found" : "Unable to access file", t->url, status);
		t->error = HttpErrno(status);
		return FALSE;
	}
	curl_easy_getinfo(t->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
	// NB: Upstream refuses a download without a length, which HTTP/2 servers may omit
	t->total_size = (length >= 0) ? (uint64_t)length : 0;
	if (t->progress) {
		char msg[128];
		uprintf("File length: %s", (length >= 0) ? SizeToHumanReadable(t->total_size, FALSE, FALSE) : "unknown");
		if (right_to_left_mode)
			static_sprintf(msg, "(%s) %s", SizeToHumanReadable(t->total_size, FALSE, FALSE), t->short_name);
		else
			static_sprintf(msg, "%s (%s)", t->short_name, SizeToHumanReadable(t->total_size, FALSE, FALSE));
		PrintStatus(5000, MSG_085, msg);
	}
	if (t->fd < 0) {
		// Allocate two extra bytes, so that the caller can rely on NUL-terminated text if needed
		t->buffer_size = (t->total_size != 0) ? t->total_size : 64 * KB;
		*t->buffer = calloc((size_t)t->buffer_size + 2, 1);
		if (*t->buffer == NULL) {
			uprintf("Could not allocate buffer for download");
			t->error = ENOMEM;
			return FALSE;
		}
	}
	return TRUE;
}

static size_t WriteData(char* data, size_t size, size_t nmemb, void* param)
{
	transfer_t* t = (transfer_t*)param;
	size_t len = size * nmemb, written;
	ssize_t r;
	BYTE* buf;

	if (!t->started && !StartTransfer(t))
		return CURL_WRITEFUNC_ERROR;
	// The user may have cancelled the download
	if (IS_ERROR(ErrorStatus)) {
		t->error = ECANCELED;
		return CURL_WRITEFUNC_ERROR;
	}
	if (t->fd >= 0) {
		for (written = 0; written < len; written += (size_t)r) {
			r = write(t->fd, &data[written], len - written);
			if (r < 0) {
				if (errno == EINTR) {
					r = 0;
					continue;
				}
				t->error = errno;
				uprintf("Error writing file '%s': %s", t->short_name, WindowsErrorString());
				return CURL_WRITEFUNC_ERROR;
			}
		}
	} else {
		if (t->size + len > t->buffer_size) {
			// More data than announced: only possible for a download of unknown length
			if (t->total_size != 0) {
				t->error = EIO;
				return CURL_WRITEFUNC_ERROR;
			}
			while (t->size + len > t->buffer_size)
				t->buffer_size *= 2;
			buf = realloc(*t->buffer, (size_t)t->buffer_size + 2);
			if (buf == NULL) {
				t->error = ENOMEM;
				return CURL_WRITEFUNC_ERROR;
			}
			*t->buffer = buf;
			memset(&buf[t->size], 0, (size_t)(t->buffer_size + 2 - t->size));
		}
		memcpy(&(*t->buffer)[t->size], data, len);
	}
	t->size += len;
	if (t->progress)
		UpdateProgressWithInfo(OP_NOOP, MSG_241, t->size, t->total_size);
	return len;
}

// Called regularly during the transfer, also when no data arrives: lets a cancellation through
static int TransferProgress(void* param, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	transfer_t* t = (transfer_t*)param;

	if (IS_ERROR(ErrorStatus)) {
		t->error = ECANCELED;
		return 1;
	}
	return 0;
}

/*
 * Download a file or fill a buffer from an URL
 * If file is NULL, a buffer is allocated for the download (that needs to be freed by the caller)
 * If hProgressDialog is not NULL, the progress is reported on the progress bar of that
 * dialog (MAIN_WINDOW_PROGRESS for the one of the main window).
 * Note that when a buffer is used, the actual size of the buffer is two more than its reported
 * size (with the extra bytes set to 0) to accommodate for calls that need NUL-terminated data.
 */
uint64_t DownloadToFileOrBufferEx(const char* url, const char* file, const char* user_agent,
	BYTE** buffer, void* hProgressDialog, BOOL bTaskBarProgress)
{
	transfer_t t = { 0 };
	struct curl_slist* headers = NULL;
	char *bak_file = NULL, curl_error[CURL_ERROR_SIZE] = "";
	CURLcode code;
	BOOL r = FALSE;
	int error = 0;

	ErrorStatus = 0;
	DownloadStatus = 404;
	if (hProgressDialog != NULL)
		UpdateProgressWithInfoInit(hProgressDialog, FALSE);

	if_assert_fails(url != NULL) {
		errno = EINVAL;
		return 0;
	}
	if (buffer != NULL)
		*buffer = NULL;
	t.url = url;
	t.fd = -1;
	t.buffer = buffer;
	t.progress = (hProgressDialog != NULL);
	t.short_name = GetDownloadShortName((file != NULL) ? file : url);

	if (hProgressDialog != NULL) {
		PrintInfo(5000, MSG_085, t.short_name);
		uprintf("Downloading %s", url);
	}

	if (file == NULL && buffer == NULL) {
		uprintf("No buffer pointer provided for download");
		errno = EINVAL;
		return 0;
	}

	// If an old file with the same name exists, rename it so we can restore it in case of failure
	if (file != NULL && access(file, F_OK) == 0 && (bak_file = calloc(1, strlen(file) + 6)) != NULL) {
		strcpy(bak_file, file);
		strcat(bak_file, ".bak");
		if (rename(file, bak_file) != 0)
			safe_free(bak_file);
	}

	t.curl = NewTransfer(url, user_agent, &headers);
	if (t.curl == NULL) {
		uprintf("Could not open Internet session");
		error = ENOMEM;
		goto out;
	}
	if (file != NULL) {
		t.fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (t.fd < 0) {
			error = errno;
			uprintf("Unable to create file '%s': %s", t.short_name, WindowsErrorString());
			goto out;
		}
	}
	curl_easy_setopt(t.curl, CURLOPT_WRITEFUNCTION, WriteData);
	curl_easy_setopt(t.curl, CURLOPT_WRITEDATA, &t);
	curl_easy_setopt(t.curl, CURLOPT_XFERINFOFUNCTION, TransferProgress);
	curl_easy_setopt(t.curl, CURLOPT_XFERINFODATA, &t);
	curl_easy_setopt(t.curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(t.curl, CURLOPT_ERRORBUFFER, curl_error);

	code = curl_easy_perform(t.curl);
	// An empty file never calls WriteData()
	if (code == CURLE_OK && !t.started && !StartTransfer(&t))
		code = CURLE_WRITE_ERROR;
	if (code == CURLE_PARTIAL_FILE) {
		uprintf("Could not download complete file - read: %" PRIu64 " bytes, expected: %" PRIu64 " bytes", t.size, t.total_size);
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		error = EIO;
		goto out;
	}
	if (code != CURLE_OK) {
		error = (t.error != 0) ? t.error : CurlErrno(code);
		if (error == ECANCELED) {
			if (!IS_ERROR(ErrorStatus))
				ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
		} else if (code != CURLE_WRITE_ERROR) {
			uprintf("Could not download '%s': %s", url, (curl_error[0] != 0) ? curl_error : curl_easy_strerror(code));
		}
		goto out;
	}

	if (t.total_size != 0 && t.size != t.total_size) {
		uprintf("Could not download complete file - read: %" PRIu64 " bytes, expected: %" PRIu64 " bytes", t.size, t.total_size);
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		error = EIO;
		goto out;
	}
	if (t.fd >= 0 && fsync(t.fd) != 0) {
		error = errno;
		uprintf("Error writing file '%s': %s", t.short_name, WindowsErrorString());
		goto out;
	}
	DownloadStatus = 200;
	r = TRUE;
	if (hProgressDialog != NULL) {
		UpdateProgressWithInfo(OP_NOOP, MSG_241, t.size, t.size);
		uprintf("Successfully downloaded '%s'", t.short_name);
	}

out:
	if (t.fd >= 0)
		close(t.fd);
	if (!r) {
		if (file != NULL && t.fd >= 0)
			unlink(file);
		if (bak_file != NULL)
			rename(bak_file, file);
		if (buffer != NULL)
			safe_free(*buffer);
	} else if (bak_file != NULL) {
		unlink(bak_file);
	}
	free(bak_file);
	if (t.curl != NULL)
		curl_easy_cleanup(t.curl);
	curl_slist_free_all(headers);
	errno = error;
	return r ? t.size : 0;
}

// Download and validate a signed file. The file must have a corresponding '.sig' on the server.
DWORD DownloadSignedFile(const char* url, const char* file, void* hProgressDialog, BOOL bPromptOnError)
{
	char* url_sig = NULL;
	BYTE *buf = NULL, *sig = NULL;
	DWORD buf_len = 0, sig_len = 0;
	DWORD ret = 0;
	int error = 0;

	if_assert_fails(url != NULL)
		return 0;

	url_sig = malloc(strlen(url) + 5);
	if (url_sig == NULL) {
		uprintf("Could not allocate signature URL");
		goto out;
	}
	strcpy(url_sig, url);
	strcat(url_sig, ".sig");

	buf_len = (DWORD)DownloadToFileOrBuffer(url, NULL, &buf, hProgressDialog, FALSE);
	if (buf_len == 0) {
		error = errno;
		goto out;
	}
	sig_len = (DWORD)DownloadToFileOrBuffer(url_sig, NULL, &sig, NULL, FALSE);
	if ((sig_len != RSA_SIGNATURE_SIZE) || (!ValidateOpensslSignature(buf, buf_len, sig, sig_len))) {
		uprintf("FATAL: Download signature is invalid ✗");
		DownloadStatus = 403;	// Forbidden
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BAD_SIGNATURE));
		goto out;
	}

	uprintf("Download signature is valid ✓");
	DownloadStatus = 206;	// Partial content
	ret = write_file(file, buf, buf_len);
	if (ret != buf_len) {
		error = errno;
		uprintf("Error writing file '%s': %s", GetDownloadShortName(file), WindowsErrorString());
		ret = 0;
		goto out;
	}
	DownloadStatus = 200;	// Full content

out:
	if (hProgressDialog != NULL)
		ui_event(UM_PROGRESS_EXIT, (intptr_t)ret);
	if ((bPromptOnError) && (DownloadStatus != 200)) {
		PrintInfo(0, MSG_242);
		errno = error;
		Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_044), IS_ERROR(ErrorStatus) ? StrError(ErrorStatus, FALSE) : WindowsErrorString());
	}
	free(url_sig);
	free(buf);
	free(sig);
	errno = error;
	return ret;
}

// Whether a file can be downloaded from url: the server has it, with a length
BOOL IsDownloadable(const char* url)
{
	struct curl_slist* headers = NULL;
	curl_off_t length = -1;
	long status = 0;
	CURL* curl;

	if (url == NULL)
		return FALSE;

	ErrorStatus = 0;
	DownloadStatus = 404;
	curl = NewTransfer(url, NULL, &headers);
	if (curl == NULL)
		goto out;
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	if (curl_easy_perform(curl) != CURLE_OK)
		goto out;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	DownloadStatus = (DWORD)status;
	if (status == 200)
		curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);

out:
	if (curl != NULL)
		curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	return (length > 0);
}

// The name of the file of a path or URL, without the query or fragment of the URL
const char* GetDownloadShortName(const char* url)
{
	static _Thread_local char short_name[128];
	const char* p;
	char* q;
	size_t i, len = safe_strlen(url);

	if (len == 0)
		return "";
	// Skip a trailing slash
	for (i = len - 1; i > 0 && url[i - 1] != '/'; i--);
	p = &url[i];
	static_strcpy(short_name, p);
	// If the URL is followed by a query, remove that part
	// Make sure we detect escaped queries too
	q = strcasestr(short_name, "%3F");
	if (q != NULL)
		*q = 0;
	short_name[strcspn(short_name, "?#")] = 0;
	return short_name;
}

/*
 * The Secure Boot revocation data, and the UEFI DBX updates
 */

static pthread_mutex_t sb_data_lock = PTHREAD_MUTEX_INITIALIZER;

void LockSecureBootData(void)
{
	pthread_mutex_lock(&sb_data_lock);
}

void UnlockSecureBootData(void)
{
	pthread_mutex_unlock(&sb_data_lock);
}

/*
 * Get the latest sbat_level.txt, sb_active.txt and sb_revoked.txt from the server
 * (CheckForFidoThread() of upstream does it while it checks for Fido). The data only
 * replaces the embedded defaults if no image scan used them yet.
 */
void FetchRemoteSecureBootData(void)
{
	char *sbat_txt = NULL, *active_txt = NULL, *revoked_txt = NULL;
	sbat_entry_t* sbat = NULL;
	thumbprint_list_t *active = NULL, *revoked = NULL;
	uint64_t len;
	uint32_t i;

	len = DownloadToFileOrBuffer(RUFUS_URL "/sbat_level.txt", NULL, (BYTE**)&sbat_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		sbat = GetSbatEntries(sbat_txt);
		if (sbat != NULL) {
			for (i = 0; sbat[i].product != NULL; i++);
			if (i > 0)
				uprintf("Found %d additional UEFI revocation filters from remote SBAT", i);
		}
	}
	len = DownloadToFileOrBuffer(RUFUS_URL "/sb_active.txt", NULL, (BYTE**)&active_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		active = GetThumbprintEntries(active_txt);
		if (active != NULL)
			uprintf("Found %d active Secure Boot certificate entries from remote", active->count);
	}
	len = DownloadToFileOrBuffer(RUFUS_URL "/sb_revoked.txt", NULL, (BYTE**)&revoked_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		revoked = GetThumbprintEntries(revoked_txt);
		if (revoked != NULL)
			uprintf("Found %d revoked Secure Boot certificate entries from remote", revoked->count);
	}

	LockSecureBootData();
	if (sbat != NULL && sbat_entries == NULL) {
		sbat_level_txt = sbat_txt;
		sbat_entries = sbat;
		sbat_txt = NULL;
		sbat = NULL;
	}
	if (active != NULL && sb_active_certs == NULL) {
		sb_active_txt = active_txt;
		sb_active_certs = active;
		active_txt = NULL;
		active = NULL;
	}
	if (revoked != NULL && sb_revoked_certs == NULL) {
		sb_revoked_txt = revoked_txt;
		sb_revoked_certs = revoked;
		revoked_txt = NULL;
		revoked = NULL;
	}
	UnlockSecureBootData();
	free(sbat_txt);
	free(sbat);
	free(active_txt);
	free(active);
	free(revoked_txt);
	free(revoked);
}

// The path of the DBX of an architecture in app_data_dir
static void GetLocalDbxPath(int arch, char* path, size_t len)
{
	snprintf(path, len, "%s/%s/dbx_%s.bin", app_data_dir, FILES_DIR, efi_archname[arch]);
}

// Whether a downloaded DBX, more recent than the embedded one, exists for an architecture
BOOL UseLocalDbx(int arch)
{
	char reg_name[32], path[MAX_PATH];

	if (arch <= ARCH_UNKNOWN || arch > (int)ARRAYSIZE(dbx_info))
		return FALSE;
	static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[arch]);
	GetLocalDbxPath(arch, path, sizeof(path));
	return ((uint64_t)ReadSetting64(reg_name) > dbx_info[arch - 1].timestamp) && (access(path, R_OK) == 0);
}

/*
 * Check the GitHub repositories of the DBX files for more recent ones than the
 * embedded ones, and offer to download them (CheckForDBXUpdates() of upstream).
 * Returns the number of DBX files downloaded.
 */
int CheckForDBXUpdates(void)
{
	int i, r, downloaded = 0;
	char reg_name[32], timestamp_url[256], path[MAX_PATH];
	char *p, *c, *rep, *buf = NULL;
	struct tm t = { 0 };
	uint64_t size, timestamp;
	BOOL already_prompted = FALSE;
	int verbose = ReadSetting32(SETTING_VERBOSE_UPDATES);

	for (i = 0; i < (int)ARRAYSIZE(dbx_info); i++) {
		// Get the epoch of the last commit
		timestamp = 0;
		static_strcpy(timestamp_url, dbx_info[i].url);
		p = strstr(timestamp_url, "contents/");
		if (p == NULL)
			continue;
		*p = 0;
		rep = replace_char(&p[9], '/', "%2F");
		static_strcat(timestamp_url, "commits?path=");
		static_strcat(timestamp_url, rep);
		free(rep);
		static_strcat(timestamp_url, "&page=1&per_page=1");
		vuprintf("Querying %s for DBX update timestamp", timestamp_url);
		size = DownloadToFileOrBuffer(timestamp_url, NULL, (BYTE**)&buf, NULL, FALSE);
		if (size == 0)
			continue;
		// Assumes that the GitHub JSON commit dates are of the form:
		// "date":[ ]*"2025-02-24T20:20:22Z"
		p = strstr(buf, "\"date\":");
		if (p == NULL) {
			safe_free(buf);
			continue;
		}
		c = &p[7];
		while (*c == ' ' || *c == '"')
			c++;
		p = c;
		while (*c != '"' && *c != '\0')
			c++;
		*c = 0;
		memset(&t, 0, sizeof(t));
		r = sscanf(p, "%d-%d-%dT%d:%d:%dZ", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec);
		safe_free(buf);
		if (r != 6)
			continue;
		t.tm_year -= 1900;
		t.tm_mon -= 1;
		timestamp = (uint64_t)timegm(&t);
		vuprintf("DBX update timestamp is %" PRIu64, timestamp);
		static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[i + 1]);
		// Check if we have an external DBX that is newer than embedded/last downloaded
		if (timestamp <= dbx_info[i].timestamp)
			continue;
		GetLocalDbxPath(i + 1, path, sizeof(path));
		if (access(path, F_OK) == 0 && timestamp <= (uint64_t)ReadSetting64(reg_name))
			continue;
		if (!already_prompted) {
			r = Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_353), lmprintf(MSG_354));
			already_prompted = TRUE;
			if (r != IDYES)
				break;
			static_sprintf(path, "%s/%s", app_data_dir, FILES_DIR);
			IGNORE_RETVAL(mkdir_p(path, 0755));
			GetLocalDbxPath(i + 1, path, sizeof(path));
		}
		if (DownloadToFileOrBuffer(dbx_info[i].url, path, NULL, NULL, FALSE) != 0) {
			WriteSetting64(reg_name, timestamp);
			uprintf("Saved %s as 'dbx_%s.bin'", dbx_info[i].url, efi_archname[i + 1]);
			downloaded++;
		} else {
			uprintf("WARNING: Failed to download %s", dbx_info[i].url);
		}
	}
	return downloaded;
}

/*
 * The DBX part of CheckForUpdatesThread() of upstream: check for DBX updates once a
 * day, as the update check of upstream does. As the AppStore version of upstream,
 * this does not depend on an update setting, as there is no update check.
 */
void CheckForDBXUpdatesDaily(void)
{
	int64_t last_check = ReadSetting64(SETTING_LAST_DBX_CHECK), now = (int64_t)time(NULL);
	int verbose = ReadSetting32(SETTING_VERBOSE_UPDATES);

	if ((now >= last_check) && (now < last_check + DBX_CHECK_INTERVAL)) {
		vuprintf("Next DBX update check in %" PRId64 " seconds.", last_check + DBX_CHECK_INTERVAL - now);
		return;
	}
	PrintInfoDebug(3000, MSG_352);
	CheckForDBXUpdates();
	WriteSetting64(SETTING_LAST_DBX_CHECK, now);
}

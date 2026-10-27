/*
 * Rufus: The Reliable USB Formatting Utility
 * Standard User I/O Routines (logging, status, error, etc.)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2020 Mattiwatti <mattiwatti@gmail.com>
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
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "ui_iface.h"
#include "localization.h"
#include "bled/bled.h"

/*
 * Globals
 */
size_t ubuffer_pos = 0;
char ubuffer[UBUFFER_SIZE];	// Buffer for ubpushf() messages we don't log right away

void uprintf(const char *format, ...)
{
	char buf[4096];
	char* p = buf;
	va_list args;
	int n;

	va_start(args, format);
	n = safe_vsnprintf(p, sizeof(buf)-2, format, args); // buf-2 is room for LF/NUL
	va_end(args);

	p += ((n < 0) || (n >= (int)sizeof(buf)-2)) ? (int)sizeof(buf)-3 : n;

	while((p>buf) && (isspace((unsigned char)p[-1])))
		*--p = '\0';

	*p++ = '\n';
	*p   = '\0';

	ui_log(buf);
}

void uprintfs(const char* str)
{
	if (str != NULL)
		ui_log(str);
}

void uprint_progress(uint64_t cur_value, uint64_t max_value)
{
	static uint64_t last_value = 0;
	if (cur_value == 0) {
		last_value = 0;
		return;
	}
	assert(max_value != 0);
	cur_value = (uint64_t)(((float)cur_value / (float)max_value) * min(MAX_MARKER, (float)max_value));
	for (; cur_value > last_value && last_value < 80; last_value++)
		uprintfs("+");
}

uint32_t read_file(const char* path, uint8_t** buf)
{
	FILE* fd = fopen(path, "rbe");
	if (fd == NULL) {
		uprintf("Error: Can't open file '%s'", path);
		return 0;
	}

	fseek(fd, 0L, SEEK_END);
	uint32_t size = (uint32_t)ftell(fd);
	fseek(fd, 0L, SEEK_SET);

	// +1 so we can add an extra NUL
	*buf = malloc(size + 1);
	if (*buf == NULL) {
		uprintf("Error: Can't allocate %d bytes buffer for file '%s'", size, path);
		size = 0;
		goto out;
	}
	if (fread(*buf, 1, size, fd) != size) {
		uprintf("Error: Can't read '%s'", path);
		size = 0;
	}
	// Always NUL terminate the file
	(*buf)[size] = 0;

out:
	fclose(fd);
	if (size == 0) {
		free(*buf);
		*buf = NULL;
	}
	return size;
}

uint32_t write_file(const char* path, const uint8_t* buf, const uint32_t size)
{
	uint32_t written;
	FILE* fd = fopen(path, "wbe");
	if (fd == NULL) {
		uprintf("Error: Can't create '%s'", path);
		return 0;
	}
	written = (uint32_t)fwrite(buf, 1, size, fd);
	if (written != size)
		uprintf("Error: Can't write '%s'", path);
	if (fclose(fd) != 0)
		written = 0;
	return written;
}

// Prints a bitstring of a number of any size, with or without leading zeroes.
// See also the printbits() and printbitslz() helper macros in rufus.h
char *_printbits(size_t const size, void const * const ptr, int leading_zeroes)
{
	// sizeof(uintmax_t) so that we have enough space to store whatever is thrown at us
	static char str[sizeof(uintmax_t) * 8 + 3];
	size_t i;
	uint8_t* b = (uint8_t*)ptr;
	uintmax_t mask, lzmask = 0, val = 0;

	// Little endian, the SCOURGE of any rational computing
	for (i = 0; i < size; i++)
		val |= ((uintmax_t)b[i]) << (8 * i);

	str[0] = '0';
	str[1] = 'b';
	if (leading_zeroes)
		lzmask = 1ULL << (size * 8 - 1);
	for (i = 2, mask = 1ULL << (sizeof(uintmax_t) * 8 - 1); mask != 0; mask >>= 1) {
		if ((i > 2) || (lzmask & mask))
			str[i++] = (val & mask) ? '1' : '0';
		else if (val & mask)
			str[i++] = '1';
	}
	str[i] = '\0';
	return str;
}

// Display an hex dump of buffer 'buf'
void DumpBufferHex(void *buf, size_t size)
{
	unsigned char* buffer = (unsigned char*)buf;
	size_t i, j, k;
	char line[80] = "";

	for (i=0; i<size; i+=16) {
		if (i!=0)
			uprintf("%s\n", line);
		line[0] = 0;
		sprintf(&line[strlen(line)], "  %08x  ", (unsigned int)i);
		for(j = 0,k = 0; k < 16; j++,k++) {
			if (i+j < size) {
				sprintf(&line[strlen(line)], "%02x", buffer[i+j]);
			} else {
				sprintf(&line[strlen(line)], "  ");
			}
			sprintf(&line[strlen(line)], " ");
		}
		sprintf(&line[strlen(line)], " ");
		for(j = 0,k = 0; k < 16; j++,k++) {
			if (i+j < size) {
				if ((buffer[i+j] < 32) || (buffer[i+j] > 126)) {
					sprintf(&line[strlen(line)], ".");
				} else {
					sprintf(&line[strlen(line)], "%c", buffer[i+j]);
				}
			}
		}
	}
	uprintf("%s\n", line);
}

/*
 * Map between errno values and the Win32 error codes that Rufus reports in
 * its status (see RUFUS_ERROR()). The first match is used in either direction.
 */
static const struct {
	int errnum;
	DWORD error;
} error_map[] = {
	{ 0, ERROR_SUCCESS },
	{ ENOENT, ERROR_FILE_NOT_FOUND },
	{ ENOTDIR, ERROR_PATH_NOT_FOUND },
	{ EACCES, ERROR_ACCESS_DENIED },
	{ EPERM, ERROR_ACCESS_DENIED },
	{ EBADF, ERROR_INVALID_HANDLE },
	{ ENOMEM, ERROR_NOT_ENOUGH_MEMORY },
	{ EROFS, ERROR_WRITE_PROTECT },
	{ ENOMEDIUM, ERROR_NO_MEDIA_IN_DRIVE },
	{ EIO, ERROR_GEN_FAILURE },
	{ EBUSY, ERROR_DEVICE_IN_USE },
	{ ETXTBSY, ERROR_SHARING_VIOLATION },
	{ ENOTSUP, ERROR_NOT_SUPPORTED },
	{ ENODEV, ERROR_DEV_NOT_EXIST },
	{ ENXIO, ERROR_DEV_NOT_EXIST },
	{ EINVAL, ERROR_INVALID_PARAMETER },
	{ ENOSPC, ERROR_DISK_FULL },
	{ ENAMETOOLONG, ERROR_LABEL_TOO_LONG },
	{ ENOTEMPTY, ERROR_DIR_NOT_EMPTY },
	{ EEXIST, ERROR_ALREADY_EXISTS },
	{ EFBIG, ERROR_FILE_TOO_LARGE },
	{ ECANCELED, ERROR_CANCELLED },
	{ EINTR, ERROR_OPERATION_ABORTED },
	{ ETIMEDOUT, ERROR_TIMEOUT },
	{ EAGAIN, ERROR_NOT_READY },
	{ EILSEQ, ERROR_INVALID_DATA },
	{ EBADMSG, ERROR_FILE_CORRUPT },
};

DWORD WindowsErrorFromErrno(int errnum)
{
	size_t i;

	for (i = 0; i < ARRAYSIZE(error_map); i++) {
		if (error_map[i].errnum == errnum)
			return error_map[i].error;
	}
	return ERROR_GEN_FAILURE;
}

// Describe a Win32 error code, through the matching errno value
static const char* Win32ErrorString(DWORD error_code)
{
	static _Thread_local char err_string[256];
	size_t i;

	for (i = 0; i < ARRAYSIZE(error_map); i++) {
		if (error_map[i].error == SCODE_CODE(error_code)) {
			static_sprintf(err_string, "[0x%08X] %s", error_code, strerror(error_map[i].errnum));
			return err_string;
		}
	}
	static_sprintf(err_string, "[0x%08X] Undetermined error", error_code);
	return err_string;
}

/*
 * Convert the last error to human readable string.
 * On Linux, the last error is errno, which this call preserves.
 */
const char *WindowsErrorString(void)
{
	static _Thread_local char err_string[256];
	int errnum = errno;
	const char* name = strerrorname_np(errnum);

	if (name != NULL)
		static_sprintf(err_string, "[%s] %s", name, strerror(errnum));
	else
		static_sprintf(err_string, "[errno %d] %s", errnum, strerror(errnum));
	errno = errnum;
	return err_string;
}

char* GuidToString(const GUID* guid, BOOL bDecorated)
{
	static char guid_string[MAX_GUID_STRING_LENGTH];

	if (guid == NULL) return NULL;
	sprintf(guid_string, bDecorated ? "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}" :
		"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
		(uint32_t)guid->Data1, guid->Data2, guid->Data3,
		guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
		guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
	return guid_string;
}

GUID StringToGuid(const char* str)
{
	GUID guid = { 0 };
	uint32_t d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7;
	if (str != NULL && sscanf(str[0] == '{' ? &str[1] : str, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
		&d1, &d2, &d3, &b0, &b1, &b2, &b3, &b4, &b5, &b6, &b7) == 11) {
		guid.Data1 = d1;
		guid.Data2 = (uint16_t)d2;
		guid.Data3 = (uint16_t)d3;
		guid.Data4[0] = (uint8_t)b0;
		guid.Data4[1] = (uint8_t)b1;
		guid.Data4[2] = (uint8_t)b2;
		guid.Data4[3] = (uint8_t)b3;
		guid.Data4[4] = (uint8_t)b4;
		guid.Data4[5] = (uint8_t)b5;
		guid.Data4[6] = (uint8_t)b6;
		guid.Data4[7] = (uint8_t)b7;
	}
	return guid;
}

// Find upper power of 2
static __inline uint16_t upo2(uint16_t v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v++;
	return v;
}

// Convert a size to human readable
char* SizeToHumanReadable(uint64_t size, BOOL copy_to_log, BOOL fake_units)
{
	int suffix;
	static char str_size[32];
	const char* dir = ((right_to_left_mode) && (!copy_to_log)) ? LEFT_TO_RIGHT_MARK : "";
	double hr_size = (double)size;
	double t;
	uint16_t i_size;
	char **_msg_table = copy_to_log ? default_msg_table : msg_table;
	const double divider = fake_units ? 1000.0 : 1024.0;

	for (suffix = 0; suffix < MAX_SIZE_SUFFIXES - 1; suffix++) {
		if (hr_size < divider)
			break;
		hr_size /= divider;
	}
	if (suffix == 0) {
		static_sprintf(str_size, "%s%d%s %s", dir, (int)hr_size, dir, _msg_table[MSG_020 - MSG_000]);
	} else if (fake_units) {
		if (hr_size < 8) {
			static_sprintf(str_size, (fabs((hr_size * 10.0) - (floor(hr_size + 0.5) * 10.0)) < 0.5) ?
				"%0.0f%s":"%0.1f%s", hr_size, _msg_table[MSG_020 + suffix - MSG_000]);
		} else {
			t = (double)upo2((uint16_t)hr_size);
			i_size = (uint16_t)((fabs(1.0f - (hr_size / t)) < 0.05f) ? t : hr_size);
			static_sprintf(str_size, "%s%d%s %s", dir, i_size, dir, _msg_table[MSG_020 + suffix - MSG_000]);
		}
	} else {
		static_sprintf(str_size, (hr_size * 10.0 - (floor(hr_size) * 10.0)) < 0.5?
			"%s%0.0f%s %s":"%s%0.1f%s %s", dir, hr_size, dir, _msg_table[MSG_020 + suffix - MSG_000]);
	}
	return str_size;
}

// Convert a YYYYMMDDHHMMSS UTC timestamp to a more human readable version
char* TimestampToHumanReadable(uint64_t ts)
{
	uint64_t rem = ts, divisor = 10000000000ULL;
	uint16_t data[6];
	int i;
	static char str[64];

	for (i = 0; i < 6; i++) {
		data[i] = (uint16_t) ((divisor == 0)?rem:(rem / divisor));
		rem %= divisor;
		divisor /= 100ULL;
	}
	static_sprintf(str, "%04d.%02d.%02d %02d:%02d:%02d (UTC)", data[0], data[1], data[2], data[3], data[4], data[5]);
	return str;
}

// Convert custom error code to messages
const char* _StrError(DWORD error_code)
{
	if ( (!IS_ERROR(error_code)) || (SCODE_CODE(error_code) == ERROR_SUCCESS)) {
		return lmprintf(MSG_050);
	}
	if (SCODE_FACILITY(error_code) != FACILITY_STORAGE)
		return Win32ErrorString(error_code);
	switch (SCODE_CODE(error_code)) {
	case ERROR_GEN_FAILURE:
		return lmprintf(MSG_051);
	case ERROR_INCOMPATIBLE_FS:
		return lmprintf(MSG_052);
	case ERROR_ACCESS_DENIED:
		return lmprintf(MSG_053);
	case ERROR_WRITE_PROTECT:
		return lmprintf(MSG_054);
	case ERROR_DEVICE_IN_USE:
		return lmprintf(MSG_055);
	case ERROR_CANT_QUICK_FORMAT:
		return lmprintf(MSG_056);
	case ERROR_LABEL_TOO_LONG:
		return lmprintf(MSG_057);
	case ERROR_INVALID_HANDLE:
		return lmprintf(MSG_058);
	case ERROR_INVALID_CLUSTER_SIZE:
		return lmprintf(MSG_059);
	case ERROR_INVALID_VOLUME_SIZE:
		return lmprintf(MSG_060);
	case ERROR_NO_MEDIA_IN_DRIVE:
		return lmprintf(MSG_061);
	case ERROR_NOT_SUPPORTED:
		return lmprintf(MSG_062);
	case ERROR_NOT_ENOUGH_MEMORY:
		return lmprintf(MSG_063);
	case ERROR_READ_FAULT:
		return lmprintf(MSG_064);
	case ERROR_WRITE_FAULT:
		return lmprintf(MSG_065);
	case ERROR_INSTALL_FAILURE:
		return lmprintf(MSG_066);
	case ERROR_OPEN_FAILED:
		return lmprintf(MSG_067);
	case ERROR_PARTITION_FAILURE:
		return lmprintf(MSG_068);
	case ERROR_CANNOT_COPY:
		return lmprintf(MSG_069);
	case ERROR_CANCELLED:
		return lmprintf(MSG_070);
	case ERROR_CANT_START_THREAD:
		return lmprintf(MSG_071);
	case ERROR_BADBLOCKS_FAILURE:
		return lmprintf(MSG_072);
	case ERROR_ISO_SCAN:
		return lmprintf(MSG_073);
	case ERROR_ISO_EXTRACT:
		return lmprintf(MSG_074);
	case ERROR_CANT_REMOUNT_VOLUME:
		return lmprintf(MSG_075);
	case ERROR_CANT_PATCH:
		return lmprintf(MSG_076);
	case ERROR_CANT_ASSIGN_LETTER:
		return lmprintf(MSG_077);
	case ERROR_CANT_MOUNT_VOLUME:
		return lmprintf(MSG_078);
	case ERROR_NOT_READY:
		return lmprintf(MSG_079);
	case ERROR_BAD_SIGNATURE:
		return lmprintf(MSG_172);
	case ERROR_CANT_DOWNLOAD:
		return lmprintf(MSG_242);
	default:
		return Win32ErrorString(error_code);
	}
}

const char* StrError(DWORD error_code, BOOL use_default_locale)
{
	const char* ret;
	if (use_default_locale)
		toggle_default_locale();
	ret = _StrError(error_code);
	if (use_default_locale)
		toggle_default_locale();
	return ret;
}

// A write() equivalent, with up to nNumRetries write attempts on error.
// As with WriteFile(), the data is written at the current file offset, which then gets advanced.
BOOL WriteFileWithRetry(int fd, const void* lpBuffer, DWORD nNumberOfBytesToWrite,
	DWORD* lpNumberOfBytesWritten, DWORD nNumRetries)
{
	DWORD nTry, NumberOfBytesWritten;
	ssize_t written;
	off_t offset;

	if (lpNumberOfBytesWritten == NULL)
		lpNumberOfBytesWritten = &NumberOfBytesWritten;
	*lpNumberOfBytesWritten = 0;

	// Need to get the current file pointer for retry
	offset = lseek(fd, 0, SEEK_CUR);
	if (offset < 0) {
		uprintf("ERROR: Could not get file offset %s", WindowsErrorString());
		return FALSE;
	}

	if (nNumRetries == 0)
		nNumRetries = 1;
	errno = 0;
	for (nTry = 1; nTry <= nNumRetries && (SCODE_CODE(ErrorStatus) != ERROR_CANCELLED); nTry++) {
		written = pwrite(fd, lpBuffer, nNumberOfBytesToWrite, offset);
		if (written >= 0) {
			*lpNumberOfBytesWritten = (DWORD)written;
			LastWriteError = 0;
			if (nNumberOfBytesToWrite == *lpNumberOfBytesWritten) {
				lseek(fd, offset + written, SEEK_SET);
				return TRUE;
			}
			uprintf("Wrote %d bytes but requested %d", *lpNumberOfBytesWritten, nNumberOfBytesToWrite);
		} else {
			uprintf("Write error %s", WindowsErrorString());
			LastWriteError = RUFUS_ERROR(WindowsErrorFromErrno(errno));
			if (LastWriteError == RUFUS_ERROR(ERROR_DISK_FULL) || SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)
				break;
		}
		if (nTry < nNumRetries) {
			uprintf("Retrying in %d seconds...", WRITE_TIMEOUT / 1000);
			usleep(WRITE_TIMEOUT * 1000);
		}
	}
	if (errno == 0 && SCODE_CODE(ErrorStatus) != ERROR_CANCELLED)
		errno = EIO;
	return FALSE;
}

// Create a file with its space allocated on disk, so that it gets a single extent
// on NTFS. Some UEFI NTFS drivers (AMI) return garbage for reads that match the
// size of an extent exactly, which this avoids.
// See https://github.com/pbatard/AmiNtfsBug for more details.
// Returns a file descriptor, positioned at the start of the file, or -1 on error.
int CreatePreallocatedFile(const char* path, int flags, mode_t mode, int64_t size)
{
	int fd = open(path, flags | O_CLOEXEC, mode);
	if (fd < 0)
		return fd;
	// No big deal if this fails
	if ((size > 0) && (posix_fallocate(fd, 0, (off_t)size) != 0))
		IGNORE_RETVAL(ftruncate(fd, (off_t)size));
	return fd;
}

static void print_extracted_file(const char* file_path, uint64_t file_length)
{
	char str[MAX_PATH];

	if (file_path == NULL)
		return;
	static_sprintf(str, "%s (%s)", file_path, SizeToHumanReadable(file_length, TRUE, FALSE));
	uprintf("Extracting: %s", str);
	PrintStatus(0, MSG_000, str);	// MSG_000 is "%s"
}

static void update_progress(const int64_t processed_bytes)
{
	static uint64_t total_bytes = 0;

	if (processed_bytes < 0) {
		total_bytes = -processed_bytes;
		UpdateProgressWithInfo(OP_EXTRACT_ZIP, MSG_348, 0, total_bytes);
	} else
		UpdateProgressWithInfo(OP_EXTRACT_ZIP, MSG_348, processed_bytes, total_bytes);
}

// Extract content from a zip archive onto the designated directory or drive
BOOL ExtractZip(const char* src_zip, const char* dest_dir)
{
	int64_t extracted_bytes = 0;

	if (src_zip == NULL)
		return FALSE;
	if (bled_init(256 * KB, NULL, NULL, NULL, update_progress, print_extracted_file, &ErrorStatus) != 0)
		return FALSE;
	uprintf("● Copying files from '%s'", src_zip);
	extracted_bytes = bled_uncompress_to_dir(src_zip, dest_dir, BLED_COMPRESSION_ZIP);
	bled_exit();
	return (extracted_bytes > 0);
}

// Returns a list of all the files or folders from a directory
// Directories are listed with a trailing '/'. Returns ERROR_FILE_NOT_FOUND if
// no files were found, NO_ERROR otherwise, or an error code.
DWORD ListDirectoryContent(StrArray* arr, char* dir, uint8_t type)
{
	DIR* d;
	struct dirent* entry;
	struct stat st;
	DWORD dwError, dwResult;
	char path[MAX_PATH + 1];
	size_t len;

	if (arr == NULL || dir == NULL || (type & 0x03) == 0)
		return ERROR_INVALID_PARAMETER;

	d = opendir(dir);
	if (d == NULL)
		return WindowsErrorFromErrno(errno);

	len = strlen(dir);
	dwResult = ERROR_FILE_NOT_FOUND;
	while ((entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		static_sprintf(path, "%s%s%s", dir, (len > 0 && dir[len - 1] == '/') ? "" : "/", entry->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if ((type & LIST_DIR_TYPE_RECURSIVE) == 0)
				continue;
			// Append a trailing slash to directories
			static_strcat(path, "/");
			if (type & LIST_DIR_TYPE_DIRECTORY)
				StrArrayAdd(arr, path, TRUE);
			dwError = ListDirectoryContent(arr, path, type);
			if (dwError != NO_ERROR && dwError != ERROR_FILE_NOT_FOUND) {
				closedir(d);
				return dwError;
			}
		} else {
			if (type & LIST_DIR_TYPE_FILE)
				StrArrayAdd(arr, path, TRUE);
			dwResult = NO_ERROR;
		}
	}
	closedir(d);
	return dwResult;
}

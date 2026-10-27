/*
 * Rufus: The Reliable USB Formatting Utility
 * DOS boot file extraction, from the FAT12 floppy image in diskcopy.dll
 * (MS WinME DOS) or from the embedded FreeDOS resource files
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
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
 * On Linux, the files get written to the mounted volume, with their FAT
 * attributes set through the FAT ioctls.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rufus.h"
#include "options.h"
#include "missing.h"
#include "resource.h"

#include "dos.h"

static BYTE* DiskImage = NULL;
static DWORD DiskImageSize;

/*
 * FAT time conversion. FAT has local times, which the vfat driver converts
 * with the time zone of the kernel (UTC unless the RTC keeps local time).
 */
static struct timespec FatDateTimeToTimespec(FAT_DATE Date, FAT_TIME Time)
{
	struct tm tm = { 0 };
	struct timespec ts = { 0 };

	tm.tm_year = Date.Year + 80;
	tm.tm_mon = Date.Month - 1;
	tm.tm_mday = Date.Day;
	tm.tm_hour = Time.Hour;
	tm.tm_min = Time.Minute;
	tm.tm_sec = Time.DoubleSeconds * 2;
	if ((Date.Month >= 1) && (Date.Month <= 12) && (Date.Day >= 1))
		ts.tv_sec = timegm(&tm);
	return ts;
}

// Write a file in one go
static BOOL WriteDOSFile(const char* filename, const uint8_t* data, size_t size, DWORD attributes,
	const struct timespec* times)
{
	int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	size_t written = 0;
	ssize_t w;

	if (fd < 0) {
		uprintf("Unable to create file '%s': %s.", filename, WindowsErrorString());
		return FALSE;
	}
	while (written < size) {
		w = write(fd, &data[written], size - written);
		if (w <= 0) {
			uprintf("Could not write file '%s': %s.", filename, WindowsErrorString());
			close(fd);
			return FALSE;
		}
		written += (size_t)w;
	}
	if ((times != NULL) && (futimens(fd, times) != 0))
		uprintf("Could not set timestamps: %s", WindowsErrorString());
	SetFileAttributesFd(fd, attributes);
	if (close(fd) != 0) {
		uprintf("Could not write file '%s': %s.", filename, WindowsErrorString());
		return FALSE;
	}
	return TRUE;
}

/* http://www.multiboot.ru/msdos8.htm & http://en.wikipedia.org/wiki/Windows_Me#Real_mode_DOS
 * COMMAND.COM and IO.SYS from diskcopy.dll are from the WinME crippled version
 * that removed real mode DOS => they must be patched:
 * IO.SYS            000003AA          75 -> EB
 * COMMAND.COM       00006510          75 -> EB
 */
static BOOL Patch_COMMAND_COM(size_t filestart, size_t filesize)
{
	const BYTE expected[8] = { 0x15, 0x80, 0xFA, 0x03, 0x75, 0x10, 0xB8, 0x0E };

	uprintf("Patching COMMAND.COM...");
	if (filesize != 93040) {
		uprintf("  unexpected file size");
		return FALSE;
	}
	if (memcmp(&DiskImage[filestart+0x650c], expected, sizeof(expected)) != 0) {
		uprintf("  unexpected binary data");
		return FALSE;
	}
	DiskImage[filestart+0x6510] = 0xeb;
	return TRUE;
}

static BOOL Patch_IO_SYS(size_t filestart, size_t filesize)
{
	const BYTE expected[8] = { 0xFA, 0x80, 0x75, 0x09, 0x8D, 0xB6, 0x99, 0x00 };

	uprintf("Patching IO.SYS...");
	if (filesize != 116736) {
		uprintf("  unexpected file size");
		return FALSE;
	}
	if (memcmp(&DiskImage[filestart+0x3a8], expected, sizeof(expected)) != 0) {
		uprintf("  unexpected binary data");
		return FALSE;
	}
	DiskImage[filestart+0x3aa] = 0xeb;
	return TRUE;
}

/* Extract the file identified by FAT RootDir index 'entry' to 'path' */
static BOOL ExtractFAT(int entry, const char* path)
{
	char filename[MAX_PATH];
	size_t i, pos, fnamepos;
	size_t filestart, filesize;
	struct timespec times[2];
	FAT_TIME midnight = { .Value = 0 };
	PDIR_ENTRY dir_entry = (PDIR_ENTRY)&DiskImage[FAT12_ROOTDIR_OFFSET + entry*FAT_BYTES_PER_DIRENT];

	if ((path == NULL) || ((safe_strlen(path) + 14) > sizeof(filename))) {
		uprintf("invalid path supplied for MS-DOS FAT extraction");
		return FALSE;
	}
	static_sprintf(filename, "%s/", path);
	pos = strlen(filename);
	fnamepos = pos;

	for(i=0; i<8; i++) {
		if (dir_entry->FileName[i] == ' ')
			break;
		filename[pos++] = dir_entry->FileName[i];
	}
	filename[pos++] = '.';
	for (i=8; i<11; i++) {
		if (dir_entry->FileName[i] == ' ')
			break;
		filename[pos++] = dir_entry->FileName[i];
	}
	filename[pos] = 0;
	filestart = (dir_entry->FirstCluster + FAT12_CLUSTER_OFFSET)*FAT12_CLUSTER_SIZE;
	filesize = dir_entry->FileSize;
	if ((filestart + filesize) > DiskImageSize) {
		uprintf("FAT File %s would be out of bounds: %zX, %zX", filename, filestart, filesize);
		uprintf("%X, %X", dir_entry->FirstCluster, dir_entry->FileSize);
		return FALSE;
	}

	/* WinME DOS files need to be patched */
	if (strcmp(&filename[fnamepos], "COMMAND.COM") == 0) {
		Patch_COMMAND_COM(filestart, filesize);
	} else if (strcmp(&filename[fnamepos], "IO.SYS") == 0) {
		Patch_IO_SYS(filestart, filesize);
	}

	/* Create a file, using the same attributes and timestamps as found in the FAT */
	// NB: The creation time can't be set on Linux
	times[0] = FatDateTimeToTimespec(dir_entry->LastAccessDate, midnight);
	times[1] = FatDateTimeToTimespec(dir_entry->LastWriteDateTime.Date, dir_entry->LastWriteDateTime.Time);
	if (!WriteDOSFile(filename, &DiskImage[filestart], filesize, dir_entry->Attributes, times))
		return FALSE;
	uprintf("Successfully wrote '%s' (%zu bytes)", filename, filesize);

	return TRUE;
}

/* Extract the MS-DOS files contained in the FAT12 1.4MB floppy
   image included as resource "BINFILE" in diskcopy.dll */
static BOOL ExtractMSDOS(const char* path)
{
	int i, j;
	BOOL r = FALSE;
	FILE* dll_file = NULL;
	size_t size = 0;
	uint8_t* diskcopy_buffer;
	char locale_path[MAX_PATH];
	char diskcopy_dll_path[MAX_PATH];
	char* extractlist[] = { "MSDOS   SYS", "COMMAND COM", "IO      SYS", "MODE    COM",
		"KEYB    COM", "KEYBOARDSYS", "KEYBRD2 SYS", "KEYBRD3 SYS", "KEYBRD4 SYS",
		"DISPLAY SYS", "EGA     CPI", "EGA2    CPI", "EGA3    CPI" };

	if (path == NULL || (diskcopy_buffer = malloc(DISKCOPY_SIZE)) == NULL)
		return FALSE;

	// There should be a diskcopy.dll in the user's AppData directory.
	// Since we're working with a known copy of diskcopy.dll, just load it in memory
	// (after locking and validating it) and point to the known disk image resource buffer.
	static_sprintf(diskcopy_dll_path, "%s/%s/diskcopy.dll", app_data_dir, FILES_DIR);
	dll_file = fopen(diskcopy_dll_path, "rbe");
	if (dll_file != NULL)
		size = fread(diskcopy_buffer, 1, DISKCOPY_SIZE, dll_file);
	if ((size != DISKCOPY_SIZE) || !BufferMatchesHash(diskcopy_buffer, size, DISKCOPY_HASH)) {
		uprintf("'diskcopy.dll' was either not found or is invalid");
		goto out;
	}
	DiskImage = &diskcopy_buffer[DISKCOPY_IMAGE_OFFSET];
	DiskImageSize = DISKCOPY_IMAGE_SIZE;

	// Reduce the visible mess by placing all the locale files into a subdir
	static_sprintf(locale_path, "%s/LOCALE", path);
	if ((mkdir(locale_path, 0755) != 0) && (errno != EEXIST)) {
		uprintf("Could not create '%s': %s", locale_path, WindowsErrorString());
		goto out;
	}

	for (i = 0, r = TRUE; r && i < FAT_FN_DIR_ENTRY_LAST; i++) {
		if (DiskImage[FAT12_ROOTDIR_OFFSET + i * FAT_BYTES_PER_DIRENT] == FAT_DIRENT_DELETED)
			continue;
		for (j = 0; r && j < ARRAYSIZE(extractlist); j++) {
			if (memcmp(extractlist[j], &DiskImage[FAT12_ROOTDIR_OFFSET + i * FAT_BYTES_PER_DIRENT], 8 + 3) == 0) {
				r = ExtractFAT(i, (j < 3) ? path : locale_path);
				if ((j == 2) || (j == 7) || (j == 12))
					UpdateProgress(OP_FILE_COPY, -1.0f);
			}
		}
	}
	if (r)
		r = SetDOSLocale(path, FALSE);

out:
	if (dll_file != NULL)
		fclose(dll_file);
	safe_free(diskcopy_buffer);
	return r;
}

/* Extract the FreeDOS files embedded in the app */
BOOL ExtractFreeDOS(const char* path)
{
	const char* res_name[] = { "COMMAND.COM", "KERNEL.SYS", "DISPLAY.EXE", "KEYB.EXE",
		"MODE.COM", "KEYBOARD.SYS", "KEYBRD2.SYS", "KEYBRD3.SYS", "KEYBRD4.SYS", "EGA.CPX",
		"EGA2.CPX", "EGA3.CPX", "EGA4.CPX", "EGA5.CPX", "EGA6.CPX",
		"EGA7.CPX", "EGA8.CPX", "EGA9.CPX", "EGA10.CPX", "EGA11.CPX",
		"EGA12.CPX", "EGA13.CPX", "EGA14.CPX", "EGA15.CPX", "EGA16.CPX",
		"EGA17.CPX", "EGA18.CPX" };
	const int res_id[ARRAYSIZE(res_name)] = { IDR_FD_COMMAND_COM, IDR_FD_KERNEL_SYS, IDR_FD_DISPLAY_EXE, IDR_FD_KEYB_EXE,
		IDR_FD_MODE_COM, IDR_FD_KB1_SYS, IDR_FD_KB2_SYS, IDR_FD_KB3_SYS, IDR_FD_KB4_SYS, IDR_FD_EGA1_CPX,
		IDR_FD_EGA2_CPX, IDR_FD_EGA3_CPX, IDR_FD_EGA4_CPX, IDR_FD_EGA5_CPX, IDR_FD_EGA6_CPX,
		IDR_FD_EGA7_CPX, IDR_FD_EGA8_CPX, IDR_FD_EGA9_CPX, IDR_FD_EGA10_CPX, IDR_FD_EGA11_CPX,
		IDR_FD_EGA12_CPX, IDR_FD_EGA13_CPX, IDR_FD_EGA14_CPX, IDR_FD_EGA15_CPX, IDR_FD_EGA16_CPX,
		IDR_FD_EGA17_CPX, IDR_FD_EGA18_CPX };
	char filename[MAX_PATH], locale_path[MAX_PATH];
	BYTE* res_data;
	DWORD res_size;
	int i;

	if ((path == NULL) || ((safe_strlen(path) + 14) > sizeof(filename))) {
		uprintf("invalid path supplied for FreeDOS extraction");
		return FALSE;
	}

	// Reduce the visible mess by placing all the locale files into a subdir
	static_sprintf(locale_path, "%s/LOCALE", path);
	if ((mkdir(locale_path, 0755) != 0) && (errno != EEXIST)) {
		uprintf("Could not create '%s': %s", locale_path, WindowsErrorString());
		return FALSE;
	}

	for (i = 0; i < ARRAYSIZE(res_name); i++) {
		res_data = GetResource(res_id[i], res_name[i], &res_size, FALSE);
		if (res_data == NULL)
			return FALSE;

		static_sprintf(filename, "%s/%s", (i<2) ? path : locale_path, res_name[i]);

		// We don't restore timestamps for FreeDOS, as there's no metadata in the files,
		// thus we would need to have a separate header with each file's timestamps
		if (!WriteDOSFile(filename, res_data, res_size,
			(i<2)?(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM):FILE_ATTRIBUTE_NORMAL, NULL))
			return FALSE;
		uprintf("Successfully wrote '%s' (%d bytes)", filename, res_size);

		if ((i == 4) || (i == 10) || (i == 16) || (i == 22) || (i == ARRAYSIZE(res_name)-1))
			UpdateProgress(OP_FILE_COPY, -1.0f);
	}

	return SetDOSLocale(path, TRUE);
}

BOOL ExtractDOS(const char* path)
{
	switch(opts.boot_type) {
	case BT_MSDOS:
		return ExtractMSDOS(path);
	case BT_FREEDOS:
		return ExtractFreeDOS(path);
	}
	return FALSE;
}

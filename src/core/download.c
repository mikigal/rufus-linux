/*
 * Rufus: The Reliable USB Formatting Utility
 * Downloads
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
 * The downloads of the boot check (other Syslinux and GRUB versions,
 * Grub4DOS, diskcopy.dll), which net.c does with WinInet on Windows. They
 * come back with libcurl in stage 4 (see LINUX_PORT.md §5.7). Until then, they
 * fail as they do upstream without a network connection, and the files have
 * to be placed in app_data_dir by hand.
 */

#include "rufus.h"

DWORD DownloadStatus = 0;

uint64_t DownloadToFileOrBufferEx(const char* url, const char* file, const char* user_agent,
	BYTE** buffer, void* hProgressDialog, BOOL bTaskBarProgress)
{
	uprintf("Downloading %s", url);
	uprintf("  Downloads are not supported yet");
	// As upstream, when the connection can't be established
	DownloadStatus = 404;
	return 0;
}

DWORD DownloadSignedFile(const char* url, const char* file, void* hProgressDialog, BOOL bPromptOnError)
{
	return (DWORD)DownloadToFileOrBufferEx(url, file, NULL, NULL, hProgressDialog, FALSE);
}

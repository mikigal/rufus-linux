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

#pragma once

#include "rufus.h"

/*
 * DownloadToFileOrBufferEx(), DownloadSignedFile() and IsDownloadable() are
 * declared in rufus.h, as upstream does.
 */

/*
 * A GET request (to a web API) into an allocated buffer, NUL terminated, with an
 * optional Referer, following the redirections or not. Returns the size of the
 * reply (0 on a transfer error, with errno), and the HTTP status in *status,
 * whatever it is. Unlike DownloadToFileOrBufferEx(), ErrorStatus is not reset.
 */
extern uint64_t HttpGet(const char* url, const char* referer, BOOL follow_redirects, BYTE** buffer, long* status);

// The name of the file of a path or URL, without the query of the URL
extern const char* GetDownloadShortName(const char* url);

/*
 * The Secure Boot revocation data (sbat_entries, sb_active_certs and
 * sb_revoked_certs), which the front end gets from the server while image scans
 * may fall back to the embedded data: whoever sets them holds this lock.
 */
extern void LockSecureBootData(void);
extern void UnlockSecureBootData(void);
extern void FetchRemoteSecureBootData(void);

// Whether a downloaded DBX, more recent than the embedded one, exists for an architecture
extern BOOL UseLocalDbx(int arch);
extern int CheckForDBXUpdates(void);

// CheckForDBXUpdates(), if it did not run in the last day (it asks the user before downloading)
extern void CheckForDBXUpdatesDaily(void);

/*
 * Rufus: The Reliable USB Formatting Utility
 * Fido: the downloads of Windows and UEFI Shell ISOs
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
 * Upstream runs Fido.ps1, a PowerShell script with a WPF user interface, which
 * sends the URL of the chosen ISO back to Rufus. Here, the steps of the script
 * are in C, and the user interface is the one of the front end. The script is
 * still downloaded (signed with the key of pki.c), for its data: the versions,
 * releases and editions it offers, and the IDs of the Microsoft download site.
 *
 * The network steps block, and can be cancelled with ErrorStatus.
 */
typedef struct fido fido_t;

/*
 * Whether the download feature is available: the URL of Fido.ps1 that rufus.ie
 * gives this version of Rufus (fido_url), and its presence on GitHub (the
 * Fido part of CheckForFidoThread() upstream).
 */
extern BOOL FidoCheck(void);
extern BOOL fido_available;

/*
 * Download Fido.ps1 (from fido_url), check its signature, and parse its data. NULL
 * on error. locale is the one of the user interface ("pl-PL"), for the languages.
 */
extern fido_t* FidoOpen(const char* locale);
// The data of a script, as loaded (for the tests)
extern fido_t* FidoOpenScript(const char* script, size_t len, const char* locale);
extern void FidoClose(fido_t* f);
// The error of the last network step that failed, for the user
extern const char* FidoError(const fido_t* f);

// The Windows versions (and UEFI Shells), their releases, and their editions
extern int FidoNbVersions(const fido_t* f);
extern const char* FidoVersionName(const fido_t* f, int version);
extern BOOL FidoIsUefiShell(const fido_t* f, int version);
extern int FidoNbReleases(const fido_t* f, int version);
extern const char* FidoReleaseName(const fido_t* f, int version, int release);
extern int FidoNbEditions(const fido_t* f, int version, int release);
extern const char* FidoEditionName(const fido_t* f, int version, int release, int edition);

/*
 * The languages of an edition, from the Microsoft download site: returns their
 * number (-1 on error, see FidoError()), and the one of the user in *preferred.
 */
extern int FidoGetLanguages(fido_t* f, int version, int release, int edition, int* preferred);
extern const char* FidoLanguageName(const fido_t* f, int language);

/*
 * The download links of the language of an edition, one per architecture:
 * returns their number (-1 on error), and the one of this machine in *preferred.
 */
extern int FidoGetLinks(fido_t* f, int version, int release, int edition, int language, int* preferred);
extern const char* FidoLinkArch(const fido_t* f, int link);
extern const char* FidoLinkUrl(const fido_t* f, int link);

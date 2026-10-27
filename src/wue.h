/*
 * Rufus: The Reliable USB Formatting Utility
 * Windows User Experience
 * Copyright © 2022 Pete Batard <pete@akeo.ie>
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

extern int unattend_xml_mask;	// unattend_xml_flags is in options.h
extern int wintogo_index, wininst_index;
extern char* unattend_xml_path;
// Set by the front end, from the selection dialog of the Windows User Experience
extern char unattend_username[MAX_USERNAME_LENGTH];
extern int unattend_edition_index;
// Offsets of the RunSynchronous section that ApplyWindowsCustomization() may remove from unattend.xml
extern uint32_t removable_section[2];

char* GetUILanguage(void);
char* CreateUnattendXml(int arch, int flags);
int GetEditions(StrArray* version_name, StrArray* version_index);
int SetWinToGoIndex(BOOL is_fixed);
BOOL PopulateWindowsVersion(void);

// Not ported yet (wue_apply.c)
BOOL ApplyWindowsCustomization(char drive_letter, int flags);
BOOL SetupWinPE(char drive_letter);
BOOL SetupWinToGo(DWORD DriveIndex, const char* drive_name, BOOL use_esp);

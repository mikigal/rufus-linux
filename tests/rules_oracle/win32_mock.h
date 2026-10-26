/*
 * Rufus: The Reliable USB Formatting Utility
 * Minimal mock of the Win32 controls used by the main window rules
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
 * Just enough of the combo box, button and window APIs, with their Win32
 * semantics, for the upstream rule functions to run unmodified.
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rufus.h"
#include "oracle.h"

typedef intptr_t INT_PTR;
typedef intptr_t LPARAM;
typedef uintptr_t WPARAM;

#define CB_ERR                      (-1)
#define BST_UNCHECKED               0
#define BST_CHECKED                 1
#define SW_HIDE                     0
#define SW_SHOW                     5
#define WM_COMMAND                  0x0111
#define WM_NEXTDLGCTL               0x0028
#define CBN_SELCHANGE               1
#define CBN_SELCHANGE_INTERNAL      (CBN_SELCHANGE + 256)
#define EN_CHANGE                   0x0300
#define EN_KILLFOCUS                0x0200
#define TB_ENABLEBUTTON             0x0401
#define TBM_SETPOS                  0x0405
#define TBM_SETRANGEMIN             0x0407
#define TBM_SETRANGEMAX             0x0408
#define UM_UPDATE_CSM_TOOLTIP       0x8005
#define LOWORD(l)                   ((WORD)(((uintptr_t)(l)) & 0xffff))
#define HIWORD(l)                   ((WORD)((((uintptr_t)(l)) >> 16) & 0xffff))
#define IDCANCEL                    2
#define MB_OK                       0
#define MB_ICONINFORMATION          0x40
#define MB_ICONERROR                0x10

typedef enum { Unknown, RemovableMedia = 11, FixedMedia = 12 } MEDIA_TYPE;

#define MOCK_MAX_ITEMS              SNAP_MAX_ITEMS

typedef struct mock_ctrl {
	int id;
	BOOL enabled;
	BOOL visible;
	int checked;
	char text[SNAP_TEXT];
	int count;
	int sel;
	struct {
		char text[SNAP_TEXT];
		intptr_t data;
	} item[MOCK_MAX_ITEMS];
	intptr_t range_min, range_max, pos;
} mock_ctrl_t;
typedef mock_ctrl_t* HWND;

extern HWND GetDlgItem(HWND hDlg, int id);
extern void mock_reset(void);

/* Combo boxes */
static __inline int ComboBox_ResetContent(HWND h) { h->count = 0; h->sel = -1; return 0; }
static __inline int ComboBox_GetCount(HWND h) { return h->count; }
static __inline int ComboBox_GetCurSel(HWND h) { return h->sel; }
static __inline int ComboBox_SetCurSel(HWND h, int i) {
	h->sel = ((i >= 0) && (i < h->count)) ? i : -1;
	return (h->sel == i) ? i : CB_ERR;
}
static __inline intptr_t ComboBox_GetItemData(HWND h, int i) {
	return ((i >= 0) && (i < h->count)) ? h->item[i].data : CB_ERR;
}
static __inline int ComboBox_SetItemData(HWND h, int i, intptr_t data) {
	if ((i < 0) || (i >= h->count))
		return CB_ERR;
	h->item[i].data = data;
	return 0;
}
static __inline int ComboBox_InsertStringU(HWND h, int i, const char* str) {
	int j;
	if (i < 0)
		i = h->count;
	if ((i > h->count) || (h->count >= MOCK_MAX_ITEMS))
		return CB_ERR;
	for (j = h->count; j > i; j--)
		h->item[j] = h->item[j - 1];
	static_strcpy(h->item[i].text, str);
	h->item[i].data = 0;
	h->count++;
	if (h->sel >= i)
		h->sel++;
	return i;
}
static __inline int ComboBox_AddStringU(HWND h, const char* str) { return ComboBox_InsertStringU(h, -1, str); }
static __inline int ComboBox_DeleteString(HWND h, int i) {
	int j;
	if ((i < 0) || (i >= h->count))
		return CB_ERR;
	for (j = i; j < h->count - 1; j++)
		h->item[j] = h->item[j + 1];
	h->count--;
	if (h->sel == i)
		h->sel = -1;
	else if (h->sel > i)
		h->sel--;
	return h->count;
}
#define ComboBox_GetCurItemData(h) ComboBox_GetItemData(h, ComboBox_GetCurSel(h))

/* Windows and buttons */
static __inline BOOL EnableWindow(HWND h, BOOL enable) { BOOL r = !h->enabled; h->enabled = (enable != 0); return r; }
static __inline BOOL IsWindowEnabled(HWND h) { return h->enabled; }
static __inline BOOL ShowWindow(HWND h, int cmd) { BOOL r = h->visible; h->visible = (cmd != SW_HIDE); return r; }
extern BOOL SetWindowTextU(HWND h, const char* str);
static __inline int GetWindowTextU(HWND h, char* str, int size) { safe_strcpy(str, size, h->text); return (int)strlen(str); }
#define GetWindowTextA GetWindowTextU
static __inline BOOL CheckDlgButton(HWND hDlg, int id, UINT state) { GetDlgItem(hDlg, id)->checked = state; return TRUE; }
static __inline UINT IsDlgButtonChecked(HWND hDlg, int id) { return GetDlgItem(hDlg, id)->checked; }
static __inline BOOL SetDlgItemTextU(HWND hDlg, int id, const char* str) { return SetWindowTextU(GetDlgItem(hDlg, id), str); }
#define IsChecked(CheckBox_ID)      (IsDlgButtonChecked(hMainDialog, CheckBox_ID) == BST_CHECKED)

/* Messages to the main dialog are dispatched to its handlers; the others are applied to the mock controls */
extern intptr_t SendMessage(HWND h, UINT msg, WPARAM w, LPARAM l);

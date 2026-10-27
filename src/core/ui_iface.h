/*
 * Rufus: The Reliable USB Formatting Utility
 * Interface between the core and the user interface
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
 * The core never touches a widget. Everything it needs from the user
 * interface goes through the callbacks below, which the front end (the Qt
 * GUI, the privileged helper, the CLI or a test) registers at startup with
 * ui_set_iface(). The callbacks may be invoked from any thread: the front end
 * is responsible for marshalling them to its own UI thread (the Qt GUI uses
 * queued connections, and blocking queued ones for the questions, which gives
 * the same semantics as SendMessage() had on Windows).
 */

#pragma once

#include "rufus.h"

/*
 * Notification types and answers. The values are the ones of the Windows
 * MessageBox() flags that upstream passes to Notification(), so that the
 * logic code can keep calling it unchanged.
 */
#define MB_OK                       0x00000000
#define MB_OKCANCEL                 0x00000001
#define MB_ABORTRETRYIGNORE         0x00000002
#define MB_YESNOCANCEL              0x00000003
#define MB_YESNO                    0x00000004
#define MB_RETRYCANCEL              0x00000005
#define MB_CLOSE                    0x0000000F		// Rufus specific: a single "Close" button
#define MB_TYPEMASK                 0x0000000F
#define MB_ICONERROR                0x00000010
#define MB_ICONQUESTION             0x00000020
#define MB_ICONWARNING              0x00000030
#define MB_ICONINFORMATION          0x00000040
#define MB_ICONMASK                 0x000000F0

#define IDOK                        1
#define IDCANCEL                    2
#define IDABORT                     3
#define IDRETRY                     4
#define IDIGNORE                    5
#define IDYES                       6
#define IDNO                        7
#define IDCLOSE                     8
#define IDHELP                      9

/* Selection styles (the values of the Windows button styles), for selection_dialog_options_t */
#define BS_AUTOCHECKBOX             0x00000003		// Check boxes
#define BS_AUTORADIOBUTTON          0x00000009		// Radio buttons (the default, when the style is 0)

/* To provide a "More information" button on notifications */
typedef struct {
	WORD id;					// MORE_INFO_URL, or the IDD_* of a dialog that the front end knows how to open
	const char* url;			// Used when id is MORE_INFO_URL
} notification_info;

/* Events the core sends to the front end. These replace the UM_* window messages. */
enum ui_event_type {
	UM_FORMAT_COMPLETED = 1,	// param: TRUE if the operation completed (see ErrorStatus for the result)
	UM_MEDIA_CHANGE,			// The device list needs to be refreshed
	UM_PROGRESS_INIT,			// Reset a progress bar before an operation (param: dialog handle, 0 for the main window)
	UM_PROGRESS_EXIT,			// Restore the progress bar after an operation
	UM_PROGRESS_MARQUEE,		// Display an indeterminate progress on the main bar (e.g. during a scan)
	UM_NO_UPDATE,				// The update check found nothing
	UM_UPDATE_CSM_TOOLTIP,		// The target system changed
	UM_ENABLE_CONTROLS,			// param: TRUE to enable the main window controls
	UM_SELECT_ISO,				// Open the image selection
	UM_TIMER_START,				// Start the elapsed time counter on the status bar
	UM_FORMAT_START,			// param: result of the boot check (see enum bootcheck_return)
	UM_HASH_COMPLETED,			// HashThread() succeeded: display hash_str[] (IDD_HASH)
	UM_IMAGE_SCAN_COMPLETED,	// param: enum rules_scan_result of StartImageScan(), for rules_on_image_scan_end()
};

/* Kinds of question the core can ask */
enum ui_question_type {
	UI_QUESTION_NOTIFICATION = 0,	// A message with buttons (Notification())
	UI_QUESTION_SELECTION,			// Radio buttons or check boxes (SelectionDialog())
	UI_QUESTION_LIST,				// A message with a list of items (ListDialog())
};

typedef struct {
	enum ui_question_type type;
	const char* title;
	const char* message;
	union {
		struct {
			int flags;							// MB_* button and icon flags
			const char* dont_display_setting;	// If not NULL, offer a "Don't show this again" check box
			const notification_info* more_info;	// If not NULL, offer a "More information" button
		} notification;
		// For the Windows User Experience, the front end also sets unattend_username and
		// unattend_edition_index (see wue.h, and GetEditions() for the editions to offer)
		selection_dialog_options_t* selection;
		struct {
			char** items;
			int size;
		} list;
	};
} ui_question_t;

typedef struct {
	/* Append UTF-8 text to the log. uprintf() passes complete lines terminated with '\n'. */
	void (*log)(const char* text);
	/*
	 * Display a message on the status bar (info == FALSE) or on the progress
	 * bar (info == TRUE). If duration is not zero, the message is transient
	 * and the previous one must be restored after duration milliseconds.
	 * A NULL text clears the area.
	 */
	void (*status)(BOOL info, unsigned int duration, const char* text);
	/*
	 * Set a progress bar to pos, out of MAX_PROGRESS. 'dialog' is NULL for the
	 * main window, or the opaque handle that the front end passed to
	 * UpdateProgressWithInfoInit() for a dialog with its own progress bar.
	 * If 'taskbar' is set, the progress should also be reported to the desktop.
	 */
	void (*progress)(void* dialog, uint32_t pos, BOOL taskbar);
	/*
	 * Ask the user something and block until they answer. Returns an ID*
	 * value for notifications, the selected mask for selections (or -1 if
	 * cancelled) and is ignored for lists.
	 */
	int (*ask)(ui_question_t* question);
	/* Notify the front end of an event (UM_*) */
	void (*event)(enum ui_event_type type, intptr_t param);
} ui_iface_t;

/*
 * Register the front end callbacks. Any callback may be NULL, in which case
 * the core falls back to a headless default: the log goes to stderr, status
 * messages and progress are dropped, and questions get IDNO/IDCANCEL (or -1
 * for selections), so that nothing destructive is ever implied by default.
 */
extern void ui_set_iface(const ui_iface_t* iface);

/* Wrappers used by the core */
extern void ui_log(const char* text);
extern void ui_status(BOOL info, unsigned int duration, const char* text);
extern void ui_progress(void* dialog, uint32_t pos, BOOL taskbar);
extern int ui_ask(ui_question_t* question);
extern void ui_event(enum ui_event_type type, intptr_t param);

/*
 * Upstream entry points, now implemented on top of ui_ask().
 * NB: No format attribute on NotificationEx(), as upstream mostly passes an
 * already formatted lmprintf() string as the format.
 */
extern int NotificationEx(int type, const char* dont_display_setting, const notification_info* more_info,
	const char* title, const char* format, ...);
#define Notification(type, title, ...) NotificationEx(type, NULL, NULL, title, __VA_ARGS__)
extern int SelectionDialog(char* title, char* message, selection_dialog_options_t* options);
extern void ListDialog(char* title, char* message, char** items, int size);

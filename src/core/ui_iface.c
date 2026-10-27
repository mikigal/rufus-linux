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

#include <stdio.h>
#include <stdlib.h>

#include "rufus.h"
#include "ui_iface.h"

static ui_iface_t ui = { 0 };

void ui_set_iface(const ui_iface_t* iface)
{
	if (iface == NULL)
		memset(&ui, 0, sizeof(ui));
	else
		ui = *iface;
}

void ui_log(const char* text)
{
	if (ui.log != NULL)
		ui.log(text);
	else
		fputs(text, stderr);
}

void ui_status(BOOL info, unsigned int duration, const char* text)
{
	if (ui.status != NULL)
		ui.status(info, duration, text);
}

void ui_progress(void* dialog, uint32_t pos, BOOL taskbar)
{
	if (ui.progress != NULL)
		ui.progress(dialog, min(pos, MAX_PROGRESS), taskbar);
}

int ui_ask(ui_question_t* question)
{
	if (ui.ask != NULL)
		return ui.ask(question);
	// Headless: never imply consent
	switch (question->type) {
	case UI_QUESTION_NOTIFICATION:
		switch (question->notification.flags & MB_TYPEMASK) {
		case MB_YESNO:
		case MB_YESNOCANCEL:
			return IDNO;
		case MB_ABORTRETRYIGNORE:
			return IDABORT;
		case MB_OKCANCEL:
		case MB_RETRYCANCEL:
			return IDCANCEL;
		default:
			return IDOK;
		}
	case UI_QUESTION_SELECTION:
		return -1;
	default:
		return 0;
	}
}

void ui_event(enum ui_event_type type, intptr_t param)
{
	if (ui.event != NULL)
		ui.event(type, param);
}

int NotificationEx(int type, const char* dont_display_setting, const notification_info* more_info,
	const char* title, const char* format, ...)
{
	char* message = NULL;
	ui_question_t q = { .type = UI_QUESTION_NOTIFICATION, .title = title };
	va_list args;
	int r;

	va_start(args, format);
	if (vasprintf(&message, format, args) < 0)
		message = NULL;
	va_end(args);
	q.message = (message == NULL) ? format : message;
	q.notification.flags = type;
	q.notification.dont_display_setting = dont_display_setting;
	q.notification.more_info = more_info;
	r = ui_ask(&q);
	free(message);
	return r;
}

int SelectionDialog(char* title, char* message, selection_dialog_options_t* options)
{
	ui_question_t q = { .type = UI_QUESTION_SELECTION, .title = title, .message = message };

	q.selection = options;
	return ui_ask(&q);
}

void ListDialog(char* title, char* message, char** items, int size)
{
	ui_question_t q = { .type = UI_QUESTION_LIST, .title = title, .message = message };

	q.list.items = items;
	q.list.size = size;
	ui_ask(&q);
}

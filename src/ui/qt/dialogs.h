/*
 * Rufus: The Reliable USB Formatting Utility
 * Standard dialogs
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

#include <QString>
#include <QWidget>

#include "core.h"

/*
 * A dialog with a progress bar of its own: the opaque handle that the core
 * gets for UM_PROGRESS_INIT and passes to ui_progress().
 */
class ProgressReceiver
{
public:
	virtual ~ProgressReceiver() = default;
	virtual void setProgress(uint32_t pos) = 0;
};

/* IDD_NOTIFICATION: returns an ID* value */
int RunNotification(QWidget* parent, const ui_question_t* q);
/* Close the notifications with this title, as if their last button was pressed */
void CloseNotifications(const QString& title);
/* IDD_SELECTION: returns the mask of the selected choices, or -1 if cancelled */
int RunSelection(QWidget* parent, ui_question_t* q);
/* IDD_LIST */
void RunList(QWidget* parent, const ui_question_t* q);
/* IDD_HASH, with hash_str[] */
void ShowHashDialog(QWidget* parent);
/* IDD_ABOUTBOX, and IDD_LICENSE from it */
void ShowAboutDialog(QWidget* parent);
void ShowLicenseDialog(QWidget* parent);

/* The simple RTF of the .loc and license.h, as HTML */
QString RtfToHtml(const char* rtf);

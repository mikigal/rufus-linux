/*
 * Rufus: The Reliable USB Formatting Utility
 * The ui_iface_t of the Qt user interface
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

#include <QCoreApplication>
#include <QPointer>
#include <QThread>

#include "ui_bridge.h"
#include "log_window.h"
#include "main_window.h"

static QPointer<LogWindow> log_window;
static QPointer<MainWindow> main_window;
static bool to_stderr = false;

static bool onUiThread()
{
	return QThread::currentThread() == QCoreApplication::instance()->thread();
}

// Run f on the thread of the user interface, without waiting for it
template <typename F> static void post(F f)
{
	if (onUiThread())
		f();
	else
		QMetaObject::invokeMethod(QCoreApplication::instance(), f, Qt::QueuedConnection);
}

static void bridge_log(const char* text)
{
	QString s = QString::fromUtf8(text);

	if (to_stderr)
		fputs(text, stderr);
	post([s] {
		if (!log_window.isNull())
			log_window->append(s);
	});
}

static void bridge_status(BOOL info, unsigned int duration, const char* text)
{
	QString s = (text == nullptr) ? QString() : QString::fromUtf8(text);

	post([info, duration, s] {
		if (!main_window.isNull())
			main_window->setStatus(info, duration, s);
	});
}

static void bridge_progress(void* dialog, uint32_t pos, BOOL taskbar)
{
	post([dialog, pos, taskbar] {
		if (!main_window.isNull())
			main_window->setProgress(dialog, pos, taskbar);
	});
}

static void bridge_event(enum ui_event_type type, intptr_t param)
{
	post([type, param] {
		if (!main_window.isNull())
			main_window->onEvent(type, param);
	});
}

static int bridge_ask(ui_question_t* q)
{
	int r = -2;

	if (onUiThread()) {
		if (!main_window.isNull())
			return main_window->ask(q);
	} else {
		QMetaObject::invokeMethod(QCoreApplication::instance(), [q, &r] {
			if (!main_window.isNull())
				r = main_window->ask(q);
		}, Qt::BlockingQueuedConnection);
		if (r != -2)
			return r;
	}
	// No window: the headless answers of ui_ask(), which never imply consent
	if (q->type == UI_QUESTION_SELECTION)
		return -1;
	if (q->type != UI_QUESTION_NOTIFICATION)
		return 0;
	switch (q->notification.flags & MB_TYPEMASK) {
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
}

void InstallUiBridge(LogWindow* log, bool log_to_stderr)
{
	static const ui_iface_t iface = { .log = bridge_log, .status = bridge_status, .progress = bridge_progress,
		.ask = bridge_ask, .event = bridge_event };

	log_window = log;
	to_stderr = log_to_stderr;
	ui_set_iface(&iface);
}

void SetBridgeWindow(MainWindow* window)
{
	main_window = window;
}

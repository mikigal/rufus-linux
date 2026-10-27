/*
 * Rufus: The Reliable USB Formatting Utility
 * Hooks for the GUI tests (T4)
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
#include <unistd.h>

#include <QApplication>
#include <QDropEvent>
#include <QFile>
#include <QKeyEvent>
#include <QMenu>
#include <QToolButton>
#include <QMimeData>
#include <QPointer>
#include <QSocketNotifier>
#include <QUrl>

#include "test_hooks.h"
#include "main_window.h"

static QPointer<MainWindow> test_window;

static void answer(const QString& line)
{
	fprintf(stdout, "%s\n", line.toUtf8().constData());
	fflush(stdout);
}

static int keyFromName(const QString& name)
{
	static const struct { const char* name; int key; } keys[] = {
		{ "period", Qt::Key_Period }, { "comma", Qt::Key_Comma }, { "plus", Qt::Key_Plus },
		{ "minus", Qt::Key_Minus }, { "equal", Qt::Key_Equal }, { "return", Qt::Key_Return },
		{ "escape", Qt::Key_Escape }, { "tab", Qt::Key_Tab }, { "space", Qt::Key_Space },
		{ "home", Qt::Key_Home }, { "end", Qt::Key_End }, { "up", Qt::Key_Up }, { "down", Qt::Key_Down },
	};

	if ((name.size() == 1) && name[0].isLetter())
		return Qt::Key_A + (name[0].toLower().unicode() - 'a');
	if ((name.size() == 1) && name[0].isDigit())
		return Qt::Key_0 + (name[0].unicode() - '0');
	for (const auto& k : keys) {
		if (name == QLatin1StringView(k.name))
			return k.key;
	}
	return 0;
}

// The file that the next file save dialog chooses
static QString test_save_file;

QString TakeTestSaveFile()
{
	QString file = test_save_file;

	test_save_file.clear();
	return file;
}

// Press and release keys, as the window system would deliver them to the widget that has the focus, or to 'name'
static bool sendKeys(const QString& spec, const QString& name)
{
	QStringList parts = spec.toLower().split(QLatin1Char('+'), Qt::SkipEmptyParts);
	Qt::KeyboardModifiers mods = Qt::NoModifier;
	QWidget* target = QApplication::focusWidget();
	QWidget* top = QApplication::activeModalWidget();
	int key = 0;

	if (!name.isEmpty()) {
		target = (top != nullptr) ? top->findChild<QWidget*>(name) : nullptr;
		for (QWidget* w : QApplication::topLevelWidgets()) {
			if ((target == nullptr) && w->isVisible())
				target = w->findChild<QWidget*>(name);
		}
		if (target == nullptr)
			return false;
	}
	if (target == nullptr)
		target = QApplication::activeWindow();
	if (target == nullptr)
		target = test_window;
	if ((target == nullptr) || parts.isEmpty())
		return false;
	for (const QString& p : parts) {
		if (p == QLatin1StringView("alt"))
			mods |= Qt::AltModifier;
		else if (p == QLatin1StringView("ctrl"))
			mods |= Qt::ControlModifier;
		else if (p == QLatin1StringView("shift"))
			mods |= Qt::ShiftModifier;
		else if ((key = keyFromName(p)) == 0)
			return false;
	}
	// A bare Alt
	if ((key == 0) && (mods == Qt::AltModifier)) {
		QKeyEvent press(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
		QKeyEvent release(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
		QApplication::sendEvent(target, &press);
		QApplication::sendEvent(target, &release);
		return true;
	}
	if (key == 0)
		return false;
	QKeyEvent shortcut(QEvent::ShortcutOverride, key, mods);
	QApplication::sendEvent(target, &shortcut);
	QKeyEvent press(QEvent::KeyPress, key, mods);
	QApplication::sendEvent(target, &press);
	QKeyEvent release(QEvent::KeyRelease, key, mods);
	QApplication::sendEvent(target, &release);
	return true;
}

static void runCommand(const QString& line)
{
	QString command = line.section(QLatin1Char(' '), 0, 0), arg = line.section(QLatin1Char(' '), 1).trimmed();

	if (command == QLatin1StringView("key")) {
		answer(sendKeys(arg.section(QLatin1Char(' '), 0, 0), arg.section(QLatin1Char(' '), 1).trimmed()) ?
			QStringLiteral("OK") : QStringLiteral("ERROR unknown keys or widget"));
	} else if (command == QLatin1StringView("capture")) {
		QWidget* w = QApplication::activeModalWidget();
		if (w == nullptr)
			w = test_window;
		if (w == nullptr) {
			answer(QStringLiteral("ERROR no window"));
			return;
		}
		QPixmap pixmap = w->grab();
		if (!pixmap.save(arg))
			answer(QStringLiteral("ERROR could not save"));
		else
			answer(QStringLiteral("OK %1 %2x%3").arg(pixmap.devicePixelRatio()).arg(pixmap.width()).arg(pixmap.height()));
	} else if (command == QLatin1StringView("drop")) {
		QMimeData mime;
		QPointF pos(10, 10);
		if (test_window == nullptr) {
			answer(QStringLiteral("ERROR no window"));
			return;
		}
		mime.setUrls({ QUrl::fromLocalFile(arg) });
		// A QPoint, which is all that QDragEnterEvent takes before Qt 6.12 (which deprecates it)
		QT_WARNING_PUSH
		QT_WARNING_DISABLE_DEPRECATED
		QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
		QT_WARNING_POP
		QApplication::sendEvent(test_window, &enter);
		if (!enter.isAccepted()) {
			answer(QStringLiteral("ERROR drop refused"));
			return;
		}
		QDropEvent drop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(test_window, &drop);
		answer(QStringLiteral("OK"));
	} else if (command == QLatin1StringView("menu")) {
		// The menu of a tool button, which a headless compositor does not pop up without an input event
		QString name = arg.section(QLatin1Char(' '), 0, 0), text = arg.section(QLatin1Char(' '), 1).trimmed();
		QToolButton* button = (test_window == nullptr) ? nullptr : test_window->findChild<QToolButton*>(name);
		QStringList entries;
		if ((button == nullptr) || (button->menu() == nullptr)) {
			answer(QStringLiteral("ERROR no such menu"));
			return;
		}
		for (QAction* a : button->menu()->actions()) {
			if (!text.isEmpty() && a->text().startsWith(text)) {
				answer(QStringLiteral("OK"));
				a->trigger();
				return;
			}
			entries.append(a->text() + (a->isChecked() ? QStringLiteral(" [x]") : QString()));
		}
		if (!text.isEmpty())
			answer(QStringLiteral("ERROR no such entry"));
		else
			answer(QStringLiteral("OK %1").arg(entries.join(QLatin1Char('|'))));
	} else if (command == QLatin1StringView("savefile")) {
		test_save_file = arg;
		answer(QStringLiteral("OK"));
	} else if (command == QLatin1StringView("language")) {
		// The language menu, which a headless compositor can't show without an input event: its
		// entries without an argument, or the choice of the entry that starts with arg
		QMenu menu;
		QStringList entries;
		if (test_window == nullptr) {
			answer(QStringLiteral("ERROR no window"));
			return;
		}
		test_window->fillLanguageMenu(&menu);
		for (QAction* a : menu.actions()) {
			if (!arg.isEmpty() && a->text().startsWith(arg)) {
				answer(QStringLiteral("OK"));
				test_window->selectLanguage(static_cast<loc_cmd*>(a->data().value<void*>()));
				return;
			}
			entries.append(a->text() + (a->isChecked() ? QStringLiteral(" [x]") : QString()));
		}
		if (!arg.isEmpty())
			answer(QStringLiteral("ERROR no such language"));
		else
			answer(QStringLiteral("OK %1").arg(entries.join(QLatin1Char('|'))));
	} else if (command == QLatin1StringView("popup")) {
		// The popup menu that is open, and its entries
		QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
		QStringList entries;
		if (menu == nullptr) {
			answer(QStringLiteral("ERROR no popup menu"));
			return;
		}
		for (QAction* a : menu->actions())
			entries.append(a->text() + (a->isChecked() ? QStringLiteral(" [x]") : QString()));
		answer(QStringLiteral("OK %1 %2").arg(menu->objectName(), entries.join(QLatin1Char('|'))));
	} else if (command == QLatin1StringView("choose")) {
		// Choose the entry of the open popup menu that starts with arg, with the keyboard
		QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
		QAction* action = nullptr;
		if (menu != nullptr) {
			for (QAction* a : menu->actions()) {
				if ((action == nullptr) && a->text().startsWith(arg))
					action = a;
			}
		}
		if (action == nullptr) {
			answer(QStringLiteral("ERROR no such entry"));
			return;
		}
		menu->setActiveAction(action);
		answer(QStringLiteral("OK"));
		QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
		QApplication::sendEvent(menu, &press);
	} else if (command == QLatin1StringView("quit")) {
		answer(QStringLiteral("OK"));
		if (test_window != nullptr)
			test_window->close();
	} else {
		answer(QStringLiteral("ERROR unknown command"));
	}
}

void InstallTestControl(MainWindow* window)
{
	static QSocketNotifier* notifier = nullptr;
	static QByteArray pending;

	if (getenv("RUFUS_TEST_CONTROL") == nullptr)
		return;
	test_window = window;
	if (notifier != nullptr)
		return;
	notifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, qApp);
	QObject::connect(notifier, &QSocketNotifier::activated, qApp, [] {
		char buf[4096];
		ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
		int end;

		if (r <= 0) {
			notifier->setEnabled(false);
			return;
		}
		pending.append(buf, (int)r);
		while ((end = (int)pending.indexOf('\n')) >= 0) {
			QString line = QString::fromUtf8(pending.left(end)).trimmed();
			pending.remove(0, end + 1);
			if (!line.isEmpty())
				runCommand(line);
		}
	});
	answer(QStringLiteral("READY"));
}

int GetTestDevices(rufus_device_t* devs, int max_devs, const device_filter_t* filter)
{
	const char* path = getenv("RUFUS_TEST_DEVICES");
	QFile file;
	int n = 0;

	if (path == nullptr)
		return -1;
	file.setFileName(QString::fromUtf8(path));
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return 0;
	while (!file.atEnd() && (n < max_devs)) {
		QStringList f = QString::fromUtf8(file.readLine()).trimmed().split(QLatin1Char('|'));
		rufus_device_t* d = &devs[n];
		if ((f.size() < 8) || f[0].startsWith(QLatin1Char('#')))
			continue;
		memset(d, 0, sizeof(*d));
		static_strcpy(d->devnode, f[0].toUtf8().constData());
		d->diskseq = f[1].toULongLong();
		d->size = f[2].toULongLong();
		d->sector_size = 512;
		static_strcpy(d->name, f[3].toUtf8().constData());
		static_strcpy(d->label, f[4].isEmpty() ? STR_NO_LABEL : f[4].toUtf8().constData());
		static_strcpy(d->fs_name, f[5].toUtf8().constData());
		d->nb_volumes = f[5].isEmpty() ? 0 : 1;
		d->partition_style = f[5].isEmpty() ? PARTITION_STYLE_RAW : PARTITION_STYLE_MBR;
		d->props.vid = f[6].isEmpty() ? -1 : f[6].section(QLatin1Char(':'), 0, 0).toInt(nullptr, 16);
		d->props.pid = f[6].isEmpty() ? -1 : f[6].section(QLatin1Char(':'), 1, 1).toInt(nullptr, 16);
		d->props.is_USB = f[7].contains(QLatin1StringView("usb"));
		d->props.is_UASP = f[7].contains(QLatin1StringView("uas"));
		d->props.is_Removable = f[7].contains(QLatin1StringView("removable"));
		d->props.is_CARD = f[7].contains(QLatin1StringView("card"));
		d->props.is_VHD = f[7].contains(QLatin1StringView("vhd"));
		d->props.is_SCSI = !d->props.is_USB;
		d->props.current_speed = d->props.max_speed = d->props.is_USB ? 4 : 0;
		if (ShouldListDevice(d, filter)) {
			SetDeviceDisplayName(d, filter);
			n++;
		}
	}
	SortDevices(devs, n);
	return n;
}

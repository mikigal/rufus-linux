/*
 * Rufus: The Reliable USB Formatting Utility
 * Qt 6 user interface entry point
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

#include <QApplication>
#include <QLabel>
#include <QMainWindow>

#include "version.h"

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);

	QApplication::setApplicationName(QStringLiteral("Rufus"));
	QApplication::setApplicationVersion(QStringLiteral(RUFUS_VERSION_STRING));
	QApplication::setOrganizationName(QStringLiteral("Rufus"));
	// Under Wayland this becomes the xdg-shell app_id, which the compositor uses
	// to match the window with rufus.desktop (icon, taskbar progress, ...)
	QGuiApplication::setDesktopFileName(QStringLiteral("rufus"));

	// Placeholder until the real main dialog is ported (stage 3)
	QMainWindow window;
	window.setWindowTitle(QStringLiteral("Rufus " RUFUS_VERSION_STRING));
	window.setCentralWidget(new QLabel(QStringLiteral("Rufus for Linux: user interface not ported yet")));
	window.show();

	return app.exec();
}

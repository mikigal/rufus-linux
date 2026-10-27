/*
 * Rufus: The Reliable USB Formatting Utility
 * Progress on the taskbar
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
#include <QVariant>

/*
 * ITaskbarList3: the progress is sent with the com.canonical.Unity.LauncherEntry
 * D-Bus signal, which the Plasma task manager, Dash to Dock and most docks
 * display on the icon of the application (matched by its .desktop file).
 */
class Taskbar
{
public:
	// 0 to 1, or a negative value to hide the progress
	void setProgress(double progress);
	// There is no indeterminate state: the progress gets hidden
	void setIndeterminate() { setProgress(-1); }
	// FlashWindowEx()
	void setUrgent(bool urgent);

private:
	void send(const char* property, const QVariant& value, const char* property2 = nullptr,
		const QVariant& value2 = QVariant());

	double last_ = -2;
};

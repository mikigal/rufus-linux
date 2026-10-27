/*
 * Rufus: The Reliable USB Formatting Utility
 * Hotplug of the drives
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

#include <QObject>
#include <QSocketNotifier>
#include <QTimer>

struct udev;
struct udev_monitor;

/*
 * WM_DEVICECHANGE: the udev events of the disks (added, removed, or media
 * changed in a card reader) are turned into a refresh of the drive list, a
 * second after the first of them, as the Windows version does with
 * DBT_DEVNODES_CHANGED.
 * udev only sends its events once it has processed them, so the udev database
 * is up to date when the list gets refreshed.
 */
class DeviceMonitor : public QObject
{
	Q_OBJECT

public:
	explicit DeviceMonitor(QObject* parent = nullptr);
	~DeviceMonitor() override;
	bool isValid() const { return monitor_ != nullptr; }
	// The drive list was refreshed by other means: no need for the pending refresh
	void refreshed();

Q_SIGNALS:
	void changed();

private:
	void onEvent();

	struct udev* udev_ = nullptr;
	struct udev_monitor* monitor_ = nullptr;
	QSocketNotifier* notifier_ = nullptr;
	QTimer timer_;
};

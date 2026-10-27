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

#include <libudev.h>

#include "device_monitor.h"

DeviceMonitor::DeviceMonitor(QObject* parent) : QObject(parent)
{
	timer_.setSingleShot(true);
	timer_.setInterval(1000);
	connect(&timer_, &QTimer::timeout, this, &DeviceMonitor::changed);

	udev_ = udev_new();
	if (udev_ == nullptr)
		return;
	monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
	if ((monitor_ == nullptr) || (udev_monitor_filter_add_match_subsystem_devtype(monitor_, "block", "disk") < 0) ||
		(udev_monitor_enable_receiving(monitor_) < 0)) {
		if (monitor_ != nullptr)
			udev_monitor_unref(monitor_);
		monitor_ = nullptr;
		return;
	}
	notifier_ = new QSocketNotifier(udev_monitor_get_fd(monitor_), QSocketNotifier::Read, this);
	connect(notifier_, &QSocketNotifier::activated, this, &DeviceMonitor::onEvent);
}

DeviceMonitor::~DeviceMonitor()
{
	delete notifier_;
	if (monitor_ != nullptr)
		udev_monitor_unref(monitor_);
	if (udev_ != nullptr)
		udev_unref(udev_);
}

void DeviceMonitor::refreshed()
{
	timer_.stop();
}

void DeviceMonitor::onEvent()
{
	struct udev_device* dev;

	while ((dev = udev_monitor_receive_device(monitor_)) != nullptr)
		udev_device_unref(dev);
	// Arm the refresh timer, unless it already is. Unlike the Windows version, which
	// drops the events that come within a second of a refresh, the last event of a
	// burst always gets a refresh: udev sends the events of the partitions after the
	// one of the disk.
	if (!timer_.isActive())
		timer_.start();
}

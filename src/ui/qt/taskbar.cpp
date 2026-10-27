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

#include <cmath>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QVariantMap>

#include "taskbar.h"

void Taskbar::send(const char* property, const QVariant& value, const char* property2, const QVariant& value2)
{
	QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/ie/rufus/Rufus"),
		QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
	QVariantMap properties;

	properties.insert(QLatin1StringView(property), value);
	if (property2 != nullptr)
		properties.insert(QLatin1StringView(property2), value2);
	message << QStringLiteral("application://%1.desktop").arg(QGuiApplication::desktopFileName()) << properties;
	QDBusConnection::sessionBus().send(message);
}

void Taskbar::setProgress(double progress)
{
	// Only send what changes visibly, as the progress gets updated very often
	if (progress >= 0)
		progress = std::round(std::min(progress, 1.0) * 1000) / 1000;
	else
		progress = -1;
	if (progress == last_)
		return;
	if ((progress < 0) != (last_ < 0))
		send("progress-visible", progress >= 0, "progress", std::max(progress, 0.0));
	else
		send("progress", progress);
	last_ = progress;
}

void Taskbar::setUrgent(bool urgent)
{
	send("urgent", urgent);
}

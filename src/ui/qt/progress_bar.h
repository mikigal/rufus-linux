/*
 * Rufus: The Reliable USB Formatting Utility
 * Progress bar with text, states and marquee (IDC_PROGRESS)
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

#include <QProgressBar>

/*
 * The progress bar of ui.c (ProgressCallback()), drawn by the Qt style: the
 * status text is the text of the bar (where the style puts it), the marquee is
 * the busy indicator of the style, and the bar turns yellow when an operation
 * is cancelled and red when it fails.
 */
class ProgressBar : public QProgressBar
{
	Q_OBJECT

public:
	enum class State { Normal, Paused, Error };

	explicit ProgressBar(QWidget* parent = nullptr);

	void setState(State state);
	State state() const { return state_; }
	void setMarquee(bool marquee);
	bool isMarquee() const { return marquee_; }
	// The text displayed with the bar
	void setInfo(const QString& text);
	QString info() const { return info_; }
	QString text() const override { return info_; }

private:
	State state_ = State::Normal;
	bool marquee_ = false;
	// The range and value to restore at the end of the marquee
	int saved_minimum_ = 0, saved_maximum_ = 100, saved_value_ = 0;
	QString info_;
};

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
#include <QTimer>

/*
 * The progress bar of ui.c (ProgressCallback()): the status text is drawn over
 * the bar, inverted where the bar is filled, and the bar turns yellow when an
 * operation is cancelled and red when it fails.
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
	// The text displayed over the bar
	void setInfo(const QString& text);
	QString info() const { return info_; }
	QString text() const override { return info_; }

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	State state_ = State::Normal;
	bool marquee_ = false;
	int marquee_pos_ = 0;
	QTimer marquee_timer_;
	QString info_;
};

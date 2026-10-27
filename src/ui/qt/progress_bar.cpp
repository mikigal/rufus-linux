/*
 * Rufus: The Reliable USB Formatting Utility
 * Progress bar with text, states and marquee (IDC_PROGRESS)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
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

#include <QPainter>
#include <QPalette>

#include "progress_bar.h"

// The colours of ui.h
static const QColor normal_color(0x06, 0xB0, 0x25);
static const QColor paused_color(0xDA, 0xCB, 0x26);
static const QColor error_color(0xDA, 0x26, 0x26);
// MARQUEE_TIMER_REFRESH
static constexpr int marquee_refresh = 10;

ProgressBar::ProgressBar(QWidget* parent) : QProgressBar(parent)
{
	setTextVisible(true);
	marquee_timer_.setInterval(marquee_refresh);
	connect(&marquee_timer_, &QTimer::timeout, this, [this] {
		int span = maximum() - minimum();
		marquee_pos_ += std::max(span / (1000 / marquee_refresh), 1);
		if ((marquee_pos_ > maximum()) || (marquee_pos_ < minimum()))
			marquee_pos_ = minimum();
		update();
	});
}

void ProgressBar::setState(State state)
{
	state_ = state;
	update();
}

void ProgressBar::setMarquee(bool marquee)
{
	if (marquee == marquee_)
		return;
	marquee_ = marquee;
	marquee_pos_ = minimum();
	if (marquee) {
		state_ = State::Normal;
		marquee_timer_.start();
	} else {
		marquee_timer_.stop();
	}
	update();
}

void ProgressBar::setInfo(const QString& text)
{
	if (text == info_)
		return;
	info_ = text;
	setAccessibleDescription(text);
	update();
}

void ProgressBar::paintEvent(QPaintEvent*)
{
	QPainter p(this);
	// From the palette, as the colour scheme of the desktop may have been overridden (Ctrl-Alt-D)
	const bool dark = (palette().color(QPalette::Window).lightness() < 128);
	const QColor background = dark ? palette().color(QPalette::Base) : QColor(0xE6, 0xE6, 0xE6);
	const QColor border = dark ? palette().color(QPalette::Mid) : QColor(0xBC, 0xBC, 0xBC);
	const QColor text_color = dark ? Qt::white : Qt::black;
	const QColor inverted_text_color = dark ? Qt::black : Qt::white;
	const QColor bar = (state_ == State::Error) ? error_color : ((state_ == State::Paused) ? paused_color : normal_color);
	const QRect frame = rect().adjusted(0, 0, -1, -1);
	const QRect inner = rect().adjusted(1, 1, -1, -1);
	const double span = std::max(maximum() - minimum(), 1);
	// The parts of the bar that are filled
	QList<QRect> filled;

	auto x_of = [&](double pos) { return inner.left() + (int)(inner.width() * pos / span); };
	if (marquee_) {
		double start = marquee_pos_ - minimum(), end = start + span / 5;
		filled.append(QRect(QPoint(x_of(start), inner.top()), QPoint(x_of(std::min(end, span)) - 1, inner.bottom())));
		// Wrap around
		if (end > span)
			filled.append(QRect(QPoint(inner.left(), inner.top()), QPoint(x_of(end - span) - 1, inner.bottom())));
	} else if (value() > minimum()) {
		filled.append(QRect(inner.topLeft(), QPoint(x_of(value() - minimum()) - 1, inner.bottom())));
	}

	p.fillRect(inner, background);
	p.setPen(text_color);
	p.drawText(inner, Qt::AlignCenter, info_);
	for (const QRect& r : filled) {
		if (r.width() <= 0)
			continue;
		p.save();
		p.setClipRect(r);
		p.fillRect(r, bar);
		p.setPen(inverted_text_color);
		p.drawText(inner, Qt::AlignCenter, info_);
		p.restore();
	}
	p.setPen(border);
	p.drawRect(frame);
}

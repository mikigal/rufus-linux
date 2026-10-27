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

#include <QPalette>

#include "progress_bar.h"

// The colours of the paused and error states of ui.h
static const QColor paused_color(0xDA, 0xCB, 0x26);
static const QColor error_color(0xDA, 0x26, 0x26);

ProgressBar::ProgressBar(QWidget* parent) : QProgressBar(parent)
{
	setTextVisible(true);
	// In the middle, as on Windows, for the styles that draw the text on the bar
	setAlignment(Qt::AlignCenter);
}

void ProgressBar::setState(State state)
{
	if (state == state_)
		return;
	state_ = state;
	// The styles fill the bar with the highlight colour (Fusion) or the accent
	// colour (Breeze). The other roles stay inherited, so that the bar follows
	// the colour scheme (Ctrl-Alt-D included).
	QPalette p;
	if (state != State::Normal) {
		const QColor& color = (state == State::Paused) ? paused_color : error_color;
		p.setColor(QPalette::Highlight, color);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
		p.setColor(QPalette::Accent, color);
#endif
		// For the text over the bar (Fusion), which is unreadable in white on yellow
		if (state == State::Paused)
			p.setColor(QPalette::HighlightedText, Qt::black);
	}
	setPalette(p);
}

void ProgressBar::setMarquee(bool marquee)
{
	if (marquee == marquee_)
		return;
	marquee_ = marquee;
	if (marquee) {
		saved_minimum_ = minimum();
		saved_maximum_ = maximum();
		saved_value_ = value();
		// An empty range makes the bar the busy indicator of the style
		setRange(0, 0);
	} else {
		setRange(saved_minimum_, saved_maximum_);
		setValue(saved_value_);
	}
}

void ProgressBar::setInfo(const QString& text)
{
	if (text == info_)
		return;
	info_ = text;
	setAccessibleDescription(text);
	update();
}

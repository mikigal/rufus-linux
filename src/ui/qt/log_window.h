/*
 * Rufus: The Reliable USB Formatting Utility
 * Log window (IDD_LOG)
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

#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

/*
 * The log is a window of its own: Wayland does not let it stick to the side
 * of the main window, as it does on Windows. It exists from the start, to
 * collect what gets logged before the main window does.
 */
class LogWindow : public QWidget
{
	Q_OBJECT

public:
	explicit LogWindow(QWidget* parent = nullptr);

	void append(const QString& text);
	QString text() const { return edit_->toPlainText(); }
	// IDC_LOG (and Ctrl-L): display or hide the log
	void toggle();
	void retranslate();
	// Save (or append, for a persistent log) the log to $XDG_STATE_HOME/rufus/rufus.log, on exit
	void saveToStateDir();

protected:
	void keyPressEvent(QKeyEvent* event) override;

private:
	void save();

	QPlainTextEdit* edit_ = nullptr;
	QPushButton* clear_ = nullptr;
	QPushButton* save_ = nullptr;
	QPushButton* close_ = nullptr;
	// The text of the last line, which uprintf() completes with '\n'
	QString pending_;
};

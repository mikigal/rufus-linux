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

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "log_window.h"
#include "app.h"
#include "core.h"

#define SET_NAME(widget, id) (widget)->setObjectName(QStringLiteral(#id))

LogWindow::LogWindow(QWidget* parent) : QWidget(parent, Qt::Window)
{
	QVBoxLayout* root = new QVBoxLayout(this);
	QHBoxLayout* buttons = new QHBoxLayout;

	setObjectName(QStringLiteral("IDD_LOG"));
	edit_ = new QPlainTextEdit;
	SET_NAME(edit_, IDC_LOG_EDIT);
	edit_->setReadOnly(true);
	edit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	edit_->setLineWrapMode(QPlainTextEdit::NoWrap);
	// Dots, parentheses and columns are a mess in an RTL context
	edit_->setLayoutDirection(Qt::LeftToRight);
	clear_ = new QPushButton;
	SET_NAME(clear_, IDC_LOG_CLEAR);
	save_ = new QPushButton;
	SET_NAME(save_, IDC_LOG_SAVE);
	close_ = new QPushButton;
	SET_NAME(close_, IDCANCEL);
	close_->setDefault(true);
	buttons->addStretch(1);
	buttons->addWidget(clear_);
	buttons->addWidget(save_);
	buttons->addWidget(close_);
	root->addWidget(edit_, 1);
	root->addLayout(buttons);
	resize(fontMetrics().averageCharWidth() * 100, fontMetrics().height() * 30);

	connect(clear_, &QPushButton::clicked, edit_, &QPlainTextEdit::clear);
	connect(save_, &QPushButton::clicked, this, &LogWindow::save);
	connect(close_, &QPushButton::clicked, this, &QWidget::hide);
	setWindowTitle(QStringLiteral("Log"));
	clear_->setText(QStringLiteral("Clear"));
	save_->setText(QStringLiteral("Save"));
	close_->setText(QStringLiteral("Close"));
}

void LogWindow::retranslate()
{
	apply_localization(IDD_LOG, [](void* ctx, int ctrl_id, const char* text) -> BOOL {
		LogWindow* log = static_cast<LogWindow*>(ctx);
		QString name = QString::fromUtf8(get_name_from_id(ctrl_id));
		if (name == log->objectName()) {
			log->setWindowTitle(QString::fromUtf8(text));
			return TRUE;
		}
		if (auto* b = log->findChild<QPushButton*>(name)) {
			b->setText(QString::fromUtf8(text));
			return TRUE;
		}
		return FALSE;
	}, this);
}

void LogWindow::append(const QString& text)
{
	QScrollBar* bar = edit_->verticalScrollBar();
	const bool at_end = (bar->value() == bar->maximum());
	QString lines = pending_ + text;
	int end = (int)lines.lastIndexOf(QLatin1Char('\n'));

	// QPlainTextEdit::appendPlainText() adds whole paragraphs
	if (end < 0) {
		pending_ = lines;
		return;
	}
	pending_ = lines.mid(end + 1);
	lines.truncate(end);
	lines.remove(QLatin1Char('\r'));
	edit_->appendPlainText(lines);
	if (at_end)
		bar->setValue(bar->maximum());
}

void LogWindow::toggle()
{
	if (isVisible()) {
		hide();
		return;
	}
	show();
	raise();
	activateWindow();
	close_->setFocus();
}

void LogWindow::keyPressEvent(QKeyEvent* event)
{
	// Ctrl-L closes the log too
	if ((event->modifiers() & Qt::ControlModifier) && (event->key() == Qt::Key_L)) {
		hide();
		return;
	}
	QWidget::keyPressEvent(event);
}

void LogWindow::save()
{
	QString path = QFileDialog::getSaveFileName(this, QString(), QDir::home().filePath(QStringLiteral("rufus.log")),
		QStringLiteral("Rufus log (*.log)"));
	QFile file(path);

	if (path.isEmpty())
		return;
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || (file.write(text().toUtf8() + "\n") < 0))
		uprintf("Could not save the log to '%s': %s", path.toUtf8().constData(), file.errorString().toUtf8().constData());
}

void LogWindow::saveToStateDir()
{
	QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/rufus");
	QFile file(dir + QStringLiteral("/rufus.log"));
	QByteArray contents = text().toUtf8();

	if (contents.isEmpty() || user_deleted_rufus_dir || !QDir().mkpath(dir))
		return;
	contents += '\n';
	// Add an extra line for persistent logs
	if (persistent_log)
		contents += '\n';
	if (file.open(QIODevice::WriteOnly | (persistent_log ? QIODevice::Append : QIODevice::Truncate)))
		file.write(contents);
}

/*
 * Rufus: The Reliable USB Formatting Utility
 * The dialog of Fido (DOWNLOAD)
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

#include <memory>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "core.h"

/*
 * The window of Fido.ps1 (a WPF form upstream): a version, then its release, its
 * edition, its language (from the Microsoft download site) and its architecture,
 * each one added below the previous one with Continue, and removed with Back.
 * The last step can open the link in a browser instead of downloading it.
 */
class FidoDialog : public QDialog
{
public:
	FidoDialog(std::shared_ptr<fido_t> fido, QWidget* parent);
	~FidoDialog() override;

	// The URL of the ISO that was chosen, and whether to open it in a browser
	QString url() const { return url_; }
	bool useBrowser() const { return use_browser_; }

protected:
	void reject() override;

private:
	QComboBox* addStage(const QString& title);
	void onContinue();
	void onBack();
	void setBusy(bool busy);
	void showError(const QString& message);

	std::shared_ptr<fido_t> fido_;
	QVBoxLayout* stages_;
	QList<QWidget*> rows_;
	QList<QComboBox*> combos_;
	QCheckBox* browser_;
	QPushButton *continue_, *back_;
	QString url_;
	bool use_browser_ = false, busy_ = false;
	int version_ = 0, release_ = 0, edition_ = 0;
};

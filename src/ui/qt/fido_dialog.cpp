/*
 * Rufus: The Reliable USB Formatting Utility
 * The dialog of Fido (DOWNLOAD)
 * Copyright © 2019-2026 Pete Batard <pete@akeo.ie>
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

#include <thread>

#include <QApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPointer>

#include "fido_dialog.h"

#define SET_NAME(w, id) (w)->setObjectName(QStringLiteral(#id))

/*
 * The stages of Fido.ps1: 0 the version, 1 its release, 2 its edition, 3 its
 * language and 4 its architecture, then the download (5).
 */
enum { STAGE_VERSION, STAGE_RELEASE, STAGE_EDITION, STAGE_LANGUAGE, STAGE_ARCH, STAGE_MAX };

FidoDialog::FidoDialog(std::shared_ptr<fido_t> fido, QWidget* parent) : QDialog(parent), fido_(fido)
{
	QVBoxLayout* root = new QVBoxLayout(this);
	QHBoxLayout* buttons = new QHBoxLayout;
	QComboBox* versions;
	int i;

	setObjectName(QStringLiteral("IDD_FIDO"));
	setWindowTitle(lmstr(MSG_149));
	stages_ = new QVBoxLayout;
	root->addLayout(stages_);
	browser_ = new QCheckBox(lmstr(MSG_143));
	SET_NAME(browser_, IDC_FIDO_BROWSER);
	browser_->setVisible(false);
	root->addWidget(browser_);
	continue_ = new QPushButton(lmstr(MSG_140));
	SET_NAME(continue_, IDC_FIDO_CONTINUE);
	back_ = new QPushButton(lmstr(MSG_006));
	SET_NAME(back_, IDC_FIDO_BACK);
	continue_->setDefault(true);
	buttons->addWidget(continue_);
	buttons->addWidget(back_);
	root->addLayout(buttons);
	root->setSizeConstraint(QLayout::SetFixedSize);

	versions = addStage(lmstr(MSG_135));
	for (i = 0; i < FidoNbVersions(fido_.get()); i++)
		versions->addItem(QString::fromUtf8(FidoVersionName(fido_.get(), i)));
	connect(continue_, &QPushButton::clicked, this, &FidoDialog::onContinue);
	connect(back_, &QPushButton::clicked, this, &FidoDialog::onBack);
}

FidoDialog::~FidoDialog()
{
}

// A new stage: its title and its combo, below the previous ones (Add-Entry)
QComboBox* FidoDialog::addStage(const QString& title)
{
	static const char* names[STAGE_MAX] = { "IDC_FIDO_VERSION", "IDC_FIDO_RELEASE", "IDC_FIDO_EDITION",
		"IDC_FIDO_LANGUAGE", "IDC_FIDO_ARCH" };
	QWidget* row = new QWidget;
	QVBoxLayout* col = new QVBoxLayout(row);
	QLabel* label = new QLabel(title);
	QComboBox* combo = new QComboBox;
	int stage = (int)combos_.size();

	col->setContentsMargins(0, 0, 0, 0);
	combo->setObjectName(QString::fromUtf8(names[stage]));
	combo->setMinimumContentsLength(32);
	label->setBuddy(combo);
	col->addWidget(label);
	col->addWidget(combo);
	stages_->addWidget(row);
	// The previous choices are fixed until Back
	for (QComboBox* c : combos_)
		c->setEnabled(false);
	rows_.append(row);
	combos_.append(combo);
	combo->setFocus();
	return combo;
}

void FidoDialog::setBusy(bool busy)
{
	busy_ = busy;
	setWindowTitle(busy ? lmstr(MSG_142) : lmstr(MSG_149));
	continue_->setEnabled(!busy);
	back_->setEnabled(!busy);
	if (!combos_.isEmpty())
		combos_.last()->setEnabled(!busy);
	if (busy)
		setCursor(Qt::BusyCursor);
	else
		unsetCursor();
}

// Error(): the step goes back to its choice
void FidoDialog::showError(const QString& message)
{
	QMessageBox box(QMessageBox::Critical, lmstr(MSG_042), message, QMessageBox::Ok, this);

	box.setObjectName(QStringLiteral("IDD_FIDO_ERROR"));
	box.exec();
}

void FidoDialog::onContinue()
{
	int stage = (int)combos_.size() - 1, i;
	QComboBox* combo;
	QPointer<FidoDialog> self(this);
	std::shared_ptr<fido_t> fido = fido_;

	if (busy_ || (combos_.last()->currentIndex() < 0))
		return;
	switch (stage) {
	case STAGE_VERSION:
		version_ = combos_.last()->currentIndex();
		combo = addStage(lmstr(MSG_136));
		for (i = 0; i < FidoNbReleases(fido_.get(), version_); i++)
			combo->addItem(QString::fromUtf8(FidoReleaseName(fido_.get(), version_, i)));
		back_->setText(lmstr(MSG_141));
		break;
	case STAGE_RELEASE:
		release_ = combos_.last()->currentIndex();
		combo = addStage(lmstr(MSG_137));
		for (i = 0; i < FidoNbEditions(fido_.get(), version_, release_); i++)
			combo->addItem(QString::fromUtf8(FidoEditionName(fido_.get(), version_, release_, i)));
		break;
	case STAGE_EDITION:
	case STAGE_LANGUAGE:
		// The steps that query the download site, in a thread
		if (stage == STAGE_EDITION)
			edition_ = combos_.last()->currentIndex();
		setBusy(true);
		std::thread([self, fido, stage, v = version_, r = release_, e = edition_, l = combos_.last()->currentIndex()] {
			int pref = 0;
			int n = (stage == STAGE_EDITION) ? FidoGetLanguages(fido.get(), v, r, e, &pref) :
				FidoGetLinks(fido.get(), v, r, e, l, &pref);
			QString error = QString::fromUtf8(FidoError(fido.get()));
			QMetaObject::invokeMethod(qApp, [self, fido, stage, n, pref, error] {
				QComboBox* c;
				int j;

				if (self.isNull())
					return;
				self->setBusy(false);
				if (n <= 0) {
					self->showError(error);
					return;
				}
				c = self->addStage((stage == STAGE_EDITION) ? lmstr(MSG_138) : lmstr(MSG_139));
				for (j = 0; j < n; j++)
					c->addItem(QString::fromUtf8((stage == STAGE_EDITION) ? FidoLanguageName(fido.get(), j) :
						FidoLinkArch(fido.get(), j)));
				c->setCurrentIndex(pref);
				if (stage == STAGE_LANGUAGE) {
					self->browser_->setVisible(true);
					self->continue_->setText(lmstr(MSG_040));
				}
			}, Qt::QueuedConnection);
		}).detach();
		break;
	case STAGE_ARCH:
		url_ = QString::fromUtf8(FidoLinkUrl(fido_.get(), combos_.last()->currentIndex()));
		use_browser_ = browser_->isChecked();
		uprintf("Selected: %s", url_.toUtf8().constData());
		accept();
		break;
	}
}

void FidoDialog::onBack()
{
	if (busy_)
		return;
	if (combos_.size() <= 1) {
		reject();
		return;
	}
	delete rows_.takeLast();
	combos_.removeLast();
	combos_.last()->setEnabled(true);
	combos_.last()->setFocus();
	browser_->setVisible(false);
	continue_->setText(lmstr(MSG_140));
	if (combos_.size() == 1)
		back_->setText(lmstr(MSG_006));
}

void FidoDialog::reject()
{
	// A query of the download site gets cancelled
	if (busy_)
		CancelOperation();
	QDialog::reject();
}

/*
 * Rufus: The Reliable USB Formatting Utility
 * Standard dialogs
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

#include <pwd.h>
#include <unistd.h>

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStyle>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include "dialogs.h"

extern "C" {
#include "wue.h"
extern BOOL enable_extra_hashes;
}
// The texts of the about and license dialogs
#include "license.h"

#define SET_NAME(widget, id) (widget)->setObjectName(QStringLiteral(#id))

// The 't' commands of a section of the .loc, for the widgets of a dialog
static void localize(QWidget* dlg, int dlg_id)
{
	apply_localization(dlg_id, [](void* ctx, int ctrl_id, const char* text) -> BOOL {
		QWidget* d = static_cast<QWidget*>(ctx);
		QString name = QString::fromUtf8(get_name_from_id(ctrl_id));
		QString s = QString::fromUtf8(text);
		if (name == d->objectName()) {
			d->setWindowTitle(s);
			return TRUE;
		}
		QWidget* w = d->findChild<QWidget*>(name);
		if (auto* l = qobject_cast<QLabel*>(w))
			l->setText(s);
		else if (auto* b = qobject_cast<QAbstractButton*>(w))
			b->setText(s);
		else
			return FALSE;
		return TRUE;
	}, dlg);
}

static QIcon messageIcon(int flags)
{
	QStyle* style = QApplication::style();

	switch (flags & MB_ICONMASK) {
	case MB_ICONERROR:
		return style->standardIcon(QStyle::SP_MessageBoxCritical);
	case MB_ICONWARNING:
		return style->standardIcon(QStyle::SP_MessageBoxWarning);
	case MB_ICONQUESTION:
		return style->standardIcon(QStyle::SP_MessageBoxQuestion);
	default:
		return style->standardIcon(QStyle::SP_MessageBoxInformation);
	}
}

// The top part of the notification, selection and list dialogs: an icon and a message
static QHBoxLayout* messageRow(QDialog* dlg, const QIcon& icon, const char* message, const char* text_id)
{
	QHBoxLayout* row = new QHBoxLayout;
	QLabel* icon_label = new QLabel;
	QLabel* text = new QLabel(QString::fromUtf8(message));
	const int size = dlg->style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, dlg);

	icon_label->setPixmap(icon.pixmap(size, size));
	icon_label->setAlignment(Qt::AlignTop);
	text->setObjectName(QLatin1StringView(text_id));
	text->setWordWrap(true);
	text->setTextFormat(Qt::PlainText);
	text->setTextInteractionFlags(Qt::TextSelectableByMouse);
	text->setMinimumWidth(dlg->fontMetrics().averageCharWidth() * 60);
	row->addWidget(icon_label, 0, Qt::AlignTop);
	row->addWidget(text, 1);
	return row;
}

static QFrame* separator()
{
	QFrame* line = new QFrame;
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	return line;
}

/*
 * IDD_NOTIFICATION
 */

static QList<QPointer<QDialog>> notifications;

int RunNotification(QWidget* parent, const ui_question_t* q)
{
	QDialog dlg(parent);
	QVBoxLayout* root = new QVBoxLayout(&dlg);
	QHBoxLayout* buttons = new QHBoxLayout;
	QCheckBox* dont_display = nullptr;
	const int type = q->notification.flags & MB_TYPEMASK;
	struct { QString text; int id; } spec[3];
	int nb_buttons = 0, r;

	dlg.setObjectName(QStringLiteral("IDD_NOTIFICATION"));
	dlg.setWindowTitle(QString::fromUtf8((q->title != nullptr) ? q->title : APPLICATION_NAME));
	root->addLayout(messageRow(&dlg, messageIcon(q->notification.flags), q->message, "IDC_NOTIFICATION_TEXT"));
	if (q->notification.dont_display_setting != nullptr) {
		dont_display = new QCheckBox(lmstr(MSG_127));
		SET_NAME(dont_display, IDC_DONT_DISPLAY_AGAIN);
		root->addWidget(dont_display);
	}
	root->addWidget(separator());

	// The buttons, from left to right, and what they return
	switch (type) {
	case MB_OKCANCEL:
		spec[nb_buttons++] = { QStringLiteral("OK"), IDOK };
		spec[nb_buttons++] = { lmstr(MSG_007), IDCANCEL };
		break;
	case MB_YESNO:
		spec[nb_buttons++] = { lmstr(MSG_008), IDYES };
		spec[nb_buttons++] = { lmstr(MSG_009), IDNO };
		break;
	case MB_YESNOCANCEL:
		spec[nb_buttons++] = { lmstr(MSG_008), IDYES };
		spec[nb_buttons++] = { lmstr(MSG_009), IDNO };
		spec[nb_buttons++] = { lmstr(MSG_007), IDCANCEL };
		break;
	case MB_OK:
		spec[nb_buttons++] = { QStringLiteral("OK"), IDOK };
		break;
	case MB_ABORTRETRYIGNORE:
		spec[nb_buttons++] = { QApplication::translate("QPlatformTheme", "Abort"), IDABORT };
		spec[nb_buttons++] = { QApplication::translate("QPlatformTheme", "Retry"), IDRETRY };
		spec[nb_buttons++] = { QApplication::translate("QPlatformTheme", "Ignore"), IDIGNORE };
		break;
	case MB_RETRYCANCEL:
		spec[nb_buttons++] = { QApplication::translate("QPlatformTheme", "Retry"), IDRETRY };
		spec[nb_buttons++] = { lmstr(MSG_007), IDCANCEL };
		break;
	default:	// One single 'Close' button
		spec[nb_buttons++] = { lmstr(MSG_006), IDCLOSE };
		break;
	}

	if ((q->notification.more_info != nullptr) && ((q->notification.more_info->id == MORE_INFO_URL) ||
		(q->notification.more_info->id == IDD_LICENSE))) {
		QPushButton* more = new QPushButton(QStringLiteral("More information"));
		const notification_info* info = q->notification.more_info;
		SET_NAME(more, IDC_MORE_INFO);
		more->setAutoDefault(false);
		QObject::connect(more, &QPushButton::clicked, &dlg, [&dlg, info] {
			if (info->id == MORE_INFO_URL)
				QDesktopServices::openUrl(QUrl(QString::fromUtf8(info->url)));
			else
				ShowLicenseDialog(&dlg);
		});
		buttons->addWidget(more);
	}
	buttons->addStretch(1);
	for (int i = 0; i < nb_buttons; i++) {
		QPushButton* b = new QPushButton(spec[i].text);
		const int id = spec[i].id;
		b->setObjectName(QString::fromUtf8(get_name_from_id(id)));
		// The last button is the default, as on Windows (IDNO)
		b->setDefault(i == nb_buttons - 1);
		b->setAutoDefault(true);
		QObject::connect(b, &QPushButton::clicked, &dlg, [&dlg, id] { dlg.done(id); });
		buttons->addWidget(b);
	}
	root->addLayout(buttons);
	root->setSizeConstraint(QLayout::SetFixedSize);
	localize(&dlg, IDD_NOTIFICATION);

	notifications.append(&dlg);
	r = dlg.exec();
	notifications.removeAll(&dlg);
	// Closing the dialog is the last button
	if ((r <= 0) || (r == QDialog::Accepted))
		r = spec[nb_buttons - 1].id;
	if ((dont_display != nullptr) && dont_display->isChecked())
		WriteSettingBool(q->notification.dont_display_setting, TRUE);
	return r;
}

void CloseNotifications(const QString& title)
{
	for (const QPointer<QDialog>& dlg : notifications) {
		if (!dlg.isNull() && (dlg->windowTitle() == title))
			dlg->reject();
	}
}

/*
 * IDD_SELECTION
 */

class SelectionDialogImpl : public QDialog
{
public:
	SelectionDialogImpl(QWidget* parent, ui_question_t* q) : QDialog(parent), opts_(q->selection)
	{
		QVBoxLayout* root = new QVBoxLayout(this);
		QVBoxLayout* choices = new QVBoxLayout;
		QHBoxLayout* buttons = new QHBoxLayout;
		const bool checkboxes = (opts_->style == BS_AUTOCHECKBOX);
		StrArray edition_name = {}, edition_index = {};
		int n = std::min((int)opts_->choices.Index, IDC_SELECTION_CHOICEMAX - IDC_SELECTION_CHOICE1 + 1);

		setObjectName(QStringLiteral("IDD_SELECTION"));
		setWindowTitle(QString::fromUtf8(q->title));
		group_.setExclusive(!checkboxes);
		// NB: GetEditions() may cause SelectionDialog() to be re-entered!
		StrArrayCreate(&edition_name, 16);
		StrArrayCreate(&edition_index, 16);
		if (opts_->edition_index > 0)
			GetEditions(&edition_name, &edition_index);

		root->addLayout(messageRow(this, QApplication::style()->standardIcon((opts_->flags & SELECTION_USE_WARNING_ICON) ?
			QStyle::SP_MessageBoxWarning : QStyle::SP_MessageBoxQuestion), q->message, "IDC_SELECTION_TEXT"));
		for (int i = 0; i < n; i++) {
			QAbstractButton* b = checkboxes ? static_cast<QAbstractButton*>(new QCheckBox) :
				static_cast<QAbstractButton*>(new QRadioButton);
			QHBoxLayout* row = new QHBoxLayout;
			b->setObjectName(QString::fromUtf8(get_name_from_id(IDC_SELECTION_CHOICE1 + i)));
			b->setText(QString::fromUtf8(opts_->choices.String[i]));
			if (i < (int)opts_->tooltips.Index)
				b->setToolTip(QString::fromUtf8(opts_->tooltips.String[i]));
			// The default selection
			b->setChecked(((opts_->style == BS_AUTORADIOBUTTON) && (opts_->mask == 0) && (i == 0)) ||
				((opts_->mask != 0) && ((1 << i) & opts_->mask)));
			group_.addButton(b, i);
			choice_.append(b);
			row->addWidget(b);
			if (i == opts_->username_index - 1) {
				struct passwd* pw = getpwuid(getuid());
				username_ = new QLineEdit((pw != nullptr) && (pw->pw_name[0] != 0) ? QString::fromUtf8(pw->pw_name) :
					QStringLiteral("User"));
				SET_NAME(username_, IDC_SELECTION_USERNAME);
				username_->setMaxLength(MAX_USERNAME_LENGTH - 1);
				row->addWidget(username_);
			}
			if ((i == opts_->edition_index - 1) && (edition_name.Index > 0)) {
				edition_ = new QComboBox;
				SET_NAME(edition_, IDC_SELECTION_EDITION);
				for (uint32_t j = 0; j < edition_name.Index; j++)
					edition_->addItem(QString::fromUtf8(edition_name.String[j]), atoi(edition_index.String[j]));
				edition_->setCurrentIndex(unattend_edition_index);
				row->addWidget(edition_);
			}
			row->addStretch(1);
			choices->addLayout(row);
		}
		StrArrayDestroy(&edition_name);
		StrArrayDestroy(&edition_index);
		root->addLayout(choices);
		root->addWidget(separator());

		ok_ = new QPushButton(QStringLiteral("OK"));
		SET_NAME(ok_, IDOK);
		ok_->setDefault(true);
		cancel_ = new QPushButton(lmstr(MSG_007));
		SET_NAME(cancel_, IDCANCEL);
		buttons->addStretch(1);
		buttons->addWidget(ok_);
		buttons->addWidget(cancel_);
		root->addLayout(buttons);
		root->setSizeConstraint(QLayout::SetFixedSize);
		localize(this, IDD_SELECTION);

		connect(ok_, &QPushButton::clicked, this, &SelectionDialogImpl::onOk);
		connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
		connect(&group_, &QButtonGroup::idClicked, this, &SelectionDialogImpl::onChoice);
		if (opts_->flags & SELECTION_NEEDS_ALL_TO_PROCEED)
			ok_->setEnabled(false);
		// Hide the silent option if any of the username/regional/privacy checkboxes are unchecked
		if (opts_->edition_index > 0)
			showSilentOption(allChecked({ opts_->username_index, opts_->regional_index, opts_->privacy_index }));
	}

	int result_mask() const { return result_; }

private:
	bool allChecked(std::initializer_list<int> indexes) const
	{
		for (int index : indexes) {
			if ((index <= 0) || (index > choice_.size()) || !choice_[index - 1]->isChecked())
				return false;
		}
		return true;
	}

	void showSilentOption(bool show)
	{
		if ((opts_->edition_index <= 0) || (opts_->edition_index > choice_.size()))
			return;
		choice_[opts_->edition_index - 1]->setVisible(show);
		if (edition_ != nullptr)
			edition_->setVisible(show);
	}

	void onChoice(int i)
	{
		if (opts_->flags & SELECTION_NEEDS_ALL_TO_PROCEED) {
			bool enable = true;
			for (QAbstractButton* b : choice_)
				enable = enable && b->isChecked();
			ok_->setEnabled(enable);
		} else if ((opts_->edition_index > 0) && (opts_->username_index > 0) && (opts_->regional_index > 0) &&
			((i == opts_->username_index - 1) || (i == opts_->regional_index - 1) || (i == opts_->privacy_index - 1))) {
			// Check if local account + regional settings + data collection are checked to show the silent install option
			showSilentOption(allChecked({ opts_->username_index, opts_->regional_index, opts_->privacy_index }));
		}
	}

	void onOk()
	{
		int r = 0;

		// Produce a big scary warning if the silent install option was selected
		if ((opts_->edition_index > 0) && (opts_->edition_index <= choice_.size()) &&
			choice_[opts_->edition_index - 1]->isVisible() && choice_[opts_->edition_index - 1]->isChecked()) {
			selection_dialog_options_t selection = {};
			int i;
			selection.style = BS_AUTOCHECKBOX;
			selection.flags = SELECTION_NEEDS_ALL_TO_PROCEED | SELECTION_USE_WARNING_ICON;
			StrArrayCreate(&selection.choices, 4);
			StrArrayAdd(&selection.choices, lmprintf(MSG_372), FALSE);
			StrArrayAdd(&selection.choices, lmprintf(MSG_373), FALSE);
			StrArrayAdd(&selection.choices, lmprintf(MSG_374), FALSE);
			i = SelectionDialog((char*)APPLICATION_NAME, lmprintf(MSG_356), &selection);
			StrArrayDestroy(&selection.choices);
			if (i != 7)
				return;
		}
		for (int i = 0; i < choice_.size(); i++) {
			if (choice_[i]->isChecked() && !choice_[i]->isHidden())
				r |= 1 << i;
		}
		if (username_ != nullptr)
			static_strcpy(unattend_username, username_->text().toUtf8().constData());
		if (edition_ != nullptr)
			unattend_edition_index = edition_->currentData().toInt();
		result_ = r;
		accept();
	}

	selection_dialog_options_t* opts_;
	QButtonGroup group_;
	QList<QAbstractButton*> choice_;
	QLineEdit* username_ = nullptr;
	QComboBox* edition_ = nullptr;
	QPushButton* ok_ = nullptr;
	QPushButton* cancel_ = nullptr;
	int result_ = -1;
};

int RunSelection(QWidget* parent, ui_question_t* q)
{
	if (q->selection->style == 0)
		q->selection->style = BS_AUTORADIOBUTTON;
	SelectionDialogImpl dlg(parent, q);
	return (dlg.exec() == QDialog::Accepted) ? dlg.result_mask() : -1;
}

/*
 * IDD_LIST
 */

void RunList(QWidget* parent, const ui_question_t* q)
{
	QDialog dlg(parent);
	QVBoxLayout* root = new QVBoxLayout(&dlg);
	QHBoxLayout* buttons = new QHBoxLayout;
	QPushButton* ok = new QPushButton(QStringLiteral("OK"));
	int n = std::min(q->list.size, IDC_LIST_ITEMMAX - IDC_LIST_ITEM1 + 1);

	dlg.setObjectName(QStringLiteral("IDD_LIST"));
	dlg.setWindowTitle(QString::fromUtf8(q->title));
	root->addLayout(messageRow(&dlg, QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning), q->message,
		"IDC_LIST_TEXT"));
	for (int i = 0; i < n; i++) {
		QLabel* item = new QLabel(QStringLiteral("• ") + QString::fromUtf8(q->list.items[i]));
		item->setObjectName(QString::fromUtf8(get_name_from_id(IDC_LIST_ITEM1 + i)));
		item->setTextFormat(Qt::PlainText);
		item->setTextInteractionFlags(Qt::TextSelectableByMouse);
		root->addWidget(item);
	}
	root->addWidget(separator());
	SET_NAME(ok, IDOK);
	ok->setDefault(true);
	QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
	buttons->addStretch(1);
	buttons->addWidget(ok);
	root->addLayout(buttons);
	root->setSizeConstraint(QLayout::SetFixedSize);
	dlg.exec();
}

/*
 * IDD_HASH
 */

void ShowHashDialog(QWidget* parent)
{
	static const struct { const char* name; const char* label; } fields[HASH_MAX] = {
		{ "IDC_MD5", "MD5:" }, { "IDC_SHA1", "SHA1:" }, { "IDC_SHA256", "SHA256:" }, { "IDC_SHA512", "SHA512:" },
	};
	QDialog dlg(parent);
	QGridLayout* grid = new QGridLayout(&dlg);
	QPushButton* ok = new QPushButton(QStringLiteral("OK"));
	const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	const QFontMetrics fm(mono);

	dlg.setObjectName(QStringLiteral("IDD_HASH"));
	dlg.setWindowTitle((image_path != nullptr) ? QString::fromUtf8(image_path).section(QLatin1Char('/'), -1) :
		QStringLiteral("Checksums"));
	for (int i = 0; i < HASH_MAX; i++) {
		QLineEdit* edit = new QLineEdit;
		edit->setObjectName(QLatin1StringView(fields[i].name));
		edit->setReadOnly(true);
		edit->setFont(mono);
		if ((i < HASH_MAX - 1) || enable_extra_hashes) {
			edit->setText(QString::fromUtf8(hash_str[i]));
		} else {
			edit->setFont(dlg.font());
			edit->setText(lmstr(MSG_311, "<Alt>-<H>"));
		}
		edit->setMinimumWidth(fm.horizontalAdvance(QString(64, QLatin1Char('0'))) + 16);
		edit->setCursorPosition(0);
		grid->addWidget(new QLabel(QLatin1StringView(fields[i].label)), i, 0);
		grid->addWidget(edit, i, 1);
	}
	SET_NAME(ok, IDOK);
	ok->setDefault(true);
	QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
	grid->addWidget(ok, HASH_MAX, 1, Qt::AlignRight);
	grid->setSizeConstraint(QLayout::SetFixedSize);
	localize(&dlg, IDD_HASH);
	ok->setFocus();
	dlg.exec();
}

/*
 * IDD_ABOUTBOX and IDD_LICENSE
 */

// Enough of RTF for the strings of the .loc (MSG_RTF) and of license.h: \line, \b, \b0, \fsN, \uN and \'XX
QString RtfToHtml(const char* rtf)
{
	static const QRegularExpression url(QStringLiteral("((?:https?://|mailto:)[^\\s<]+)"));
	QString html, text = QString::fromUtf8(rtf);
	bool bold = false;
	int i = 0;

	while (i < text.size()) {
		QChar c = text[i];
		if ((c == QLatin1Char('{')) || (c == QLatin1Char('}'))) {
			i++;
		} else if (c == QLatin1Char('\\')) {
			int j = i + 1;
			if ((j < text.size()) && !text[j].isLetter()) {
				if (text[j] == QLatin1Char('\'')) {
					html += QChar(text.mid(j + 1, 2).toInt(nullptr, 16));
					i = j + 3;
				} else {
					html += QString(text[j]).toHtmlEscaped();
					i = j + 1;
				}
				continue;
			}
			while ((j < text.size()) && text[j].isLetter())
				j++;
			QString word = text.mid(i + 1, j - i - 1);
			int k = j;
			while ((k < text.size()) && ((text[k] == QLatin1Char('-')) || text[k].isDigit()))
				k++;
			QString param = text.mid(j, k - j);
			// A control word ends with an optional space
			if ((k < text.size()) && (text[k] == QLatin1Char(' ')))
				k++;
			if (word == QLatin1StringView("line") || word == QLatin1StringView("par")) {
				html += QStringLiteral("<br>");
			} else if (word == QLatin1StringView("b")) {
				bool on = (param != QLatin1StringView("0"));
				if (on != bold)
					html += on ? QStringLiteral("<b>") : QStringLiteral("</b>");
				bold = on;
			} else if (word == QLatin1StringView("u")) {
				html += QChar((char16_t)param.toInt());
				// The ANSI replacement character
				if ((k < text.size()) && (text[k] == QLatin1Char('?')))
					k++;
			}
			i = k;
		} else if ((c == QLatin1Char('\n')) || (c == QLatin1Char('\r'))) {
			i++;
		} else {
			html += QString(c).toHtmlEscaped();
			i++;
		}
	}
	if (bold)
		html += QStringLiteral("</b>");
	html.replace(url, QStringLiteral("<a href=\"\\1\">\\1</a>"));
	return html;
}

void ShowLicenseDialog(QWidget* parent)
{
	QDialog dlg(parent);
	QVBoxLayout* root = new QVBoxLayout(&dlg);
	QPlainTextEdit* text = new QPlainTextEdit(QString::fromUtf8(gplv3));
	QDialogButtonBox* box = new QDialogButtonBox;
	QPushButton* close = box->addButton(lmstr(MSG_006), QDialogButtonBox::RejectRole);

	dlg.setObjectName(QStringLiteral("IDD_LICENSE"));
	dlg.setWindowTitle(QStringLiteral("Rufus License"));
	SET_NAME(text, IDC_LICENSE_TEXT);
	text->setReadOnly(true);
	// The license is in English, whatever the language
	text->setLayoutDirection(Qt::LeftToRight);
	text->setMinimumSize(dlg.fontMetrics().averageCharWidth() * 80, dlg.fontMetrics().height() * 25);
	SET_NAME(close, IDCANCEL);
	root->addWidget(text);
	root->addWidget(box);
	QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	localize(&dlg, IDD_LICENSE);
	dlg.exec();
}

void ShowAboutDialog(QWidget* parent)
{
	QDialog dlg(parent);
	QHBoxLayout* top = new QHBoxLayout;
	QVBoxLayout* root = new QVBoxLayout(&dlg);
	QVBoxLayout* col = new QVBoxLayout;
	QLabel* icon = new QLabel;
	QTextBrowser* blurb = new QTextBrowser;
	QTextBrowser* copyrights = new QTextBrowser;
	QHBoxLayout* buttons = new QHBoxLayout;
	QPushButton* license = new QPushButton;
	QPushButton* ok = new QPushButton(QStringLiteral("OK"));
	char about_blurb[2048];

	dlg.setObjectName(QStringLiteral("IDD_ABOUTBOX"));
	dlg.setWindowTitle(QStringLiteral("About Rufus"));
	icon->setPixmap(QIcon(QStringLiteral(":/icons/rufus-64.png")).pixmap(64, 64));
	icon->setAlignment(Qt::AlignTop);
	SET_NAME(icon, IDC_ABOUT_ICON);
	static_sprintf(about_blurb, about_blurb_format, lmprintf(MSG_174 | MSG_RTF),
		lmprintf(MSG_175 | MSG_RTF, rufus_version[0], rufus_version[1], rufus_version[2]),
		"Copyright © 2011-2026 Pete Batard\\line\nLinux port: Copyright © 2026 Rufus for Linux contributors",
		lmprintf(MSG_176 | MSG_RTF), lmprintf(MSG_177 | MSG_RTF), lmprintf(MSG_178 | MSG_RTF));
	for (QTextBrowser* b : { blurb, copyrights }) {
		b->setOpenExternalLinks(true);
		b->setFrameShape(QFrame::NoFrame);
		b->viewport()->setAutoFillBackground(false);
		b->setMinimumWidth(dlg.fontMetrics().averageCharWidth() * 70);
	}
	SET_NAME(blurb, IDC_ABOUT_BLURB);
	blurb->setHtml(RtfToHtml(about_blurb));
	blurb->setFixedHeight((int)blurb->document()->size().height() + 8);
	SET_NAME(copyrights, IDC_ABOUT_COPYRIGHTS);
	copyrights->setHtml(RtfToHtml(additional_copyrights));
	copyrights->setFrameShape(QFrame::StyledPanel);
	copyrights->setMinimumHeight(dlg.fontMetrics().height() * 10);
	col->addWidget(blurb);
	col->addWidget(copyrights, 1);
	top->addWidget(icon, 0, Qt::AlignTop);
	top->addLayout(col, 1);
	root->addLayout(top);
	SET_NAME(license, IDC_ABOUT_LICENSE);
	license->setText(QStringLiteral("License"));
	SET_NAME(ok, IDOK);
	ok->setDefault(true);
	buttons->addWidget(license);
	buttons->addStretch(1);
	buttons->addWidget(ok);
	root->addLayout(buttons);
	QObject::connect(license, &QPushButton::clicked, &dlg, [&dlg] { ShowLicenseDialog(&dlg); });
	QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
	localize(&dlg, IDD_ABOUTBOX);
	dlg.exec();
}

/*
 * IDD_UPDATE_POLICY
 */

void ShowUpdatePolicyDialog(QWidget* parent)
{
	QDialog dlg(parent);
	QVBoxLayout* root = new QVBoxLayout(&dlg);
	QHBoxLayout* top = new QHBoxLayout;
	QHBoxLayout* bottom = new QHBoxLayout;
	QLabel* icon = new QLabel;
	QTextBrowser* policy = new QTextBrowser;
	QGroupBox* settings = new QGroupBox(QStringLiteral("Settings"));
	QGridLayout* grid = new QGridLayout(settings);
	QLabel* frequency_txt = new QLabel(QStringLiteral("Check for updates"));
	QLabel* betas_txt = new QLabel(QStringLiteral("Include beta versions"));
	QComboBox* frequency = new QComboBox;
	QComboBox* betas = new QComboBox;
	QVBoxLayout* buttons = new QVBoxLayout;
	QPushButton* check_now = new QPushButton(QStringLiteral("Check Now"));
	QPushButton* close = new QPushButton(QStringLiteral("Close"));
	char policy_text[4096];
	int32_t freq;

	dlg.setObjectName(QStringLiteral("IDD_UPDATE_POLICY"));
	dlg.setWindowTitle(QStringLiteral("Update policy and settings"));
	icon->setPixmap(QIcon(QStringLiteral(":/icons/rufus-64.png")).pixmap(48, 48));
	SET_NAME(icon, IDC_ABOUT_ICON);
	SET_NAME(policy, IDC_POLICY);
	static_sprintf(policy_text, update_policy, lmprintf(MSG_179 | MSG_RTF), lmprintf(MSG_180 | MSG_RTF),
		lmprintf(MSG_181 | MSG_RTF), lmprintf(MSG_182 | MSG_RTF), lmprintf(MSG_183 | MSG_RTF),
		lmprintf(MSG_184 | MSG_RTF), lmprintf(MSG_185 | MSG_RTF), lmprintf(MSG_186 | MSG_RTF));
	policy->setHtml(RtfToHtml(policy_text));
	policy->setOpenExternalLinks(true);
	policy->setMinimumSize(dlg.fontMetrics().averageCharWidth() * 70, dlg.fontMetrics().height() * 14);
	top->addWidget(icon, 0, Qt::AlignTop);
	top->addWidget(policy, 1);
	root->addLayout(top);

	SET_NAME(settings, IDS_UPDATE_SETTINGS_GRP);
	SET_NAME(frequency_txt, IDS_UPDATE_FREQUENCY_TXT);
	SET_NAME(betas_txt, IDS_INCLUDE_BETAS_TXT);
	SET_NAME(frequency, IDC_UPDATE_FREQUENCY);
	SET_NAME(betas, IDC_INCLUDE_BETAS);
	frequency_txt->setBuddy(frequency);
	betas_txt->setBuddy(betas);
	frequency->addItem(lmstr(MSG_013), -1);
	frequency->addItem(lmstr(MSG_030, lmprintf(MSG_014)), 86400);
	frequency->addItem(lmstr(MSG_015), 604800);
	frequency->addItem(lmstr(MSG_016), 2629800);
	freq = ReadSetting32(SETTING_UPDATE_INTERVAL);
	switch (freq) {
	case -1:
		frequency->setCurrentIndex(0);
		break;
	case 0:
	case 86400:
		frequency->setCurrentIndex(1);
		break;
	case 604800:
		frequency->setCurrentIndex(2);
		break;
	case 2629800:
		frequency->setCurrentIndex(3);
		break;
	default:
		frequency->addItem(lmstr(MSG_017), freq);
		frequency->setCurrentIndex(4);
		break;
	}
	betas->addItem(lmstr(MSG_008));
	betas->addItem(lmstr(MSG_009));
	betas->setCurrentIndex(ReadSettingBool(SETTING_INCLUDE_BETAS) ? 0 : 1);
	betas->setEnabled(freq >= 0);
	grid->addWidget(frequency_txt, 0, 0);
	grid->addWidget(frequency, 0, 1);
	grid->addWidget(betas_txt, 1, 0);
	grid->addWidget(betas, 1, 1);
	SET_NAME(check_now, IDC_CHECK_NOW);
	// The update check comes with the network code (stage 4 of the port)
	check_now->setEnabled(false);
	SET_NAME(close, IDCANCEL);
	close->setDefault(true);
	buttons->addWidget(check_now);
	buttons->addWidget(close);
	buttons->addStretch(1);
	bottom->addWidget(settings, 1);
	bottom->addLayout(buttons);
	root->addLayout(bottom);

	QObject::connect(frequency, &QComboBox::currentIndexChanged, &dlg, [frequency, betas] {
		int32_t f = frequency->currentData().toInt();
		WriteSetting32(SETTING_UPDATE_INTERVAL, (DWORD)f);
		betas->setEnabled(f >= 0);
	});
	QObject::connect(betas, &QComboBox::currentIndexChanged, &dlg, [betas] {
		WriteSettingBool(SETTING_INCLUDE_BETAS, betas->currentIndex() == 0);
	});
	QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::reject);
	localize(&dlg, IDD_UPDATE_POLICY);
	dlg.exec();
}

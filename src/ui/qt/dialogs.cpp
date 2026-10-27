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
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScreen>
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
	int nb_buttons = 0, clicked = 0, r;

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
		}, Qt::QueuedConnection);
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
		QObject::connect(b, &QPushButton::clicked, &dlg, [&dlg, &clicked, id] {
			clicked = id;
			dlg.accept();
		});
		buttons->addWidget(b);
	}
	root->addLayout(buttons);
	root->setSizeConstraint(QLayout::SetFixedSize);
	localize(&dlg, IDD_NOTIFICATION);

	notifications.append(&dlg);
	// NB: Not the result of exec(), as IDOK is QDialog::Accepted
	dlg.exec();
	notifications.removeAll(&dlg);
	// Closing the dialog is the last button
	r = (clicked != 0) ? clicked : spec[nb_buttons - 1].id;
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

		// onOk() may open another selection
		connect(ok_, &QPushButton::clicked, this, &SelectionDialogImpl::onOk, Qt::QueuedConnection);
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
	// Not into the &gt; of the <mailto:...> of the translators
	static const QRegularExpression url(QStringLiteral("((?:https?://|mailto:)[^\\s<&]+)"));
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
	QLabel* blurb = new QLabel;
	QTextBrowser* copyrights = new QTextBrowser;
	QHBoxLayout* buttons = new QHBoxLayout;
	QPushButton* license = new QPushButton;
	QPushButton* ok = new QPushButton(QStringLiteral("OK"));
	char about_blurb[4 * LOC_MESSAGE_SIZE], translators[LOC_MESSAGE_SIZE] = "";
	const char* msg_176 = lmprintf(MSG_176 | MSG_RTF);
	QFontMetrics fm = dlg.fontMetrics();

	dlg.setObjectName(QStringLiteral("IDD_ABOUTBOX"));
	dlg.setWindowTitle(QStringLiteral("About Rufus"));
	icon->setPixmap(QIcon(QStringLiteral(":/icons/rufus-64.png")).pixmap(64, 64));
	icon->setAlignment(Qt::AlignTop);
	SET_NAME(icon, IDC_ABOUT_ICON);
	// MSG_176 is the support address of upstream in English, and the credits of the translation otherwise
	if (strstr(msg_176, "support@akeo.ie") == NULL)
		static_sprintf(translators, "%s\\line\n\\line\n", msg_176);
	static_sprintf(about_blurb, about_blurb_format, lmprintf(MSG_174 | MSG_RTF),
		lmprintf(MSG_175 | MSG_RTF, rufus_version[0], rufus_version[1], rufus_version[2]),
		translators, lmprintf(MSG_177 | MSG_RTF), lmprintf(MSG_178 | MSG_RTF));
	// A label, which takes the height of its text (the lines of the .loc are short enough not to wrap)
	SET_NAME(blurb, IDC_ABOUT_BLURB);
	blurb->setTextFormat(Qt::RichText);
	blurb->setText(RtfToHtml(about_blurb));
	blurb->setOpenExternalLinks(true);
	blurb->setTextInteractionFlags(Qt::TextBrowserInteraction);
	SET_NAME(copyrights, IDC_ABOUT_COPYRIGHTS);
	copyrights->setOpenExternalLinks(true);
	copyrights->viewport()->setAutoFillBackground(false);
	copyrights->setHtml(RtfToHtml(additional_copyrights));
	copyrights->setFrameShape(QFrame::StyledPanel);
	copyrights->setMinimumSize(fm.averageCharWidth() * 80, fm.height() * 8);
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
	QObject::connect(license, &QPushButton::clicked, &dlg, [&dlg] { ShowLicenseDialog(&dlg); }, Qt::QueuedConnection);
	QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
	localize(&dlg, IDD_ABOUTBOX);
	// Room for more of the additional copyrights than their minimum, as far as the screen allows
	QSize size = dlg.sizeHint() + QSize(0, fm.height() * 10);
	if (QScreen* screen = (parent != nullptr) ? parent->screen() : QGuiApplication::primaryScreen())
		size = size.boundedTo(screen->availableGeometry().size() * 0.9);
	dlg.resize(size.expandedTo(dlg.minimumSizeHint()));
	dlg.exec();
}

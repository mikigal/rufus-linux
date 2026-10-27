/*
 * Rufus: The Reliable USB Formatting Utility
 * Main window (IDD_DIALOG)
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

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <thread>

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QStyleHints>
#include <QHBoxLayout>
#include <QMenu>
#include <QMimeData>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>

#include "main_window.h"
#include "app.h"
extern "C" {
#include "disk.h"
}
#include "device_monitor.h"
#include "dialogs.h"
#include "log_window.h"
#include "ui_bridge.h"
#include "test_hooks.h"

extern "C" {
extern char embedded_sl_version_str[2][12];
extern BOOL enable_iso, enable_joliet, enable_rockridge, enable_extra_hashes, ignore_boot_marker;
#include <cdio/logging.h>
}

// The width of the window, in average characters (232 dialog units on Windows)
static constexpr int window_chars = 58;

static helper_t session_helper = { -1, 0 };

void StopSessionHelper()
{
	StopHelper(&session_helper);
}

#define SET_NAME(widget, id) (widget)->setObjectName(QStringLiteral(#id))

static QIcon toolbarIcon(const char* name)
{
	QIcon icon;
	for (int size : { 16, 24, 32 })
		icon.addFile(QStringLiteral(":/icons/%1-%2.png").arg(QLatin1StringView(name)).arg(size), QSize(size, size));
	return icon;
}

// A section header: bold text followed by a line (SetSectionHeaders())
static QWidget* sectionHeader(QLabel** label, const char* name)
{
	QWidget* w = new QWidget;
	QHBoxLayout* h = new QHBoxLayout(w);
	QFrame* line = new QFrame;
	QFont font;

	h->setContentsMargins(0, 0, 0, 0);
	*label = new QLabel;
	(*label)->setObjectName(QLatin1StringView(name));
	font = (*label)->font();
	font.setBold(true);
	font.setPointSizeF(font.pointSizeF() * 1.3);
	(*label)->setFont(font);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	h->addWidget(*label);
	h->addWidget(line, 1);
	return w;
}

// A label above a control
static QLabel* fieldLabel(QWidget* buddy, const char* name)
{
	QLabel* label = new QLabel;
	label->setObjectName(QLatin1StringView(name));
	label->setBuddy(buddy);
	return label;
}

static QComboBox* newCombo(const char* name)
{
	QComboBox* combo = new QComboBox;
	combo->setObjectName(QLatin1StringView(name));
	// The window does not grow with long entries, which get elided
	combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	combo->setMinimumContentsLength(4);
	combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	return combo;
}

MainWindow::MainWindow(LogWindow* log, QWidget* parent) : QMainWindow(parent), log_(log)
{
	rules_init(&m_);
	m_.advanced_mode_device = advanced_mode_device;
	m_.advanced_mode_format = advanced_mode_format;
	m_.allow_dual_uefi_bios = allow_dual_uefi_bios;
	m_.force_large_fat32 = force_large_fat32;
	m_.preselected_fs = preselected_fs;
	m_.selected_fs = preselected_fs;
	// Windows To Go needs stage 5, and there is no mkfs for ReFS
	m_.wintogo_available = FALSE;
	m_.refs_available = FALSE;
	filter_.enable_HDDs = enable_HDDs;
	filter_.list_non_usb_removable_drives = list_non_usb_removable_drives;
	filter_.use_fake_units = use_fake_units;
	filter_.enable_VHDs = enable_VHDs;
	filter_.enable_vmdk = enable_vmdk;
	memcpy(filter_.ignore_vid_pid, ignore_vid_pid, sizeof(filter_.ignore_vid_pid));

	setObjectName(QStringLiteral("IDD_DIALOG"));
	setWindowTitle(QStringLiteral(APPLICATION_NAME " %1.%2.%3").arg(rufus_version[0]).arg(rufus_version[1])
		.arg(rufus_version[2]));
	setAcceptDrops(true);
	createWidgets();
	retranslate();
	// The core can talk to the window from now on
	SetBridgeWindow(this);

	status_timer_.setSingleShot(true);
	connect(&status_timer_, &QTimer::timeout, this, [this] { status_->setText(status_text_); });
	clock_.setInterval(1000);
	connect(&clock_, &QTimer::timeout, this, [this] {
		elapsed_++;
		timer_label_->setText(QString::asprintf("%02d:%02d:%02d", elapsed_ / 3600, (elapsed_ % 3600) / 60, elapsed_ % 60));
	});

	monitor_ = new DeviceMonitor(this);
	if (!monitor_->isValid())
		uprintf("Could not monitor the drives: the list will not be refreshed when one is plugged in");
	connect(monitor_, &DeviceMonitor::changed, this, [this] {
		if (format_running_)
			return;
		if (m_.op_in_progress) {
			queued_hotplug_event_ = true;
			return;
		}
		queued_hotplug_event_ = false;
		refreshDevices(false);
	});

	// The cheat codes, ahead of the mnemonics of the widgets
	qApp->installEventFilter(this);
	InstallTestControl(this);

	// InitDialog() + GetDevices() + EnableControls()
	refreshDevices(true);
	PrintInfo(0, MSG_210);
	// Set 'START' as the selected button if it's enabled, otherwise use 'SELECT', instead
	if (start_->isEnabled())
		start_->setFocus();
	else
		select_->setFocus();
}

MainWindow::~MainWindow()
{
	SetBridgeWindow(nullptr);
}

void MainWindow::createWidgets()
{
	QWidget* central = new QWidget;
	QVBoxLayout* root = new QVBoxLayout(central);
	QGridLayout* grid;
	QHBoxLayout* row;
	QVBoxLayout* col;

	root->setSizeConstraint(QLayout::SetFixedSize);
	// The minimum width of the window, which longer texts can widen
	root->addSpacerItem(new QSpacerItem(fontMetrics().averageCharWidth() * window_chars, 0, QSizePolicy::Fixed,
		QSizePolicy::Minimum));

	/* Drive Properties */
	root->addWidget(sectionHeader(&drive_properties_txt_, "IDS_DRIVE_PROPERTIES_TXT"));
	device_ = newCombo("IDC_DEVICE");
	save_ = new QToolButton;
	SET_NAME(save_, IDC_SAVE);
	save_->setIcon(toolbarIcon("save"));
	save_->setAutoRaise(true);
	root->addWidget(device_txt_ = fieldLabel(device_, "IDS_DEVICE_TXT"));
	row = new QHBoxLayout;
	row->addWidget(device_, 1);
	row->addWidget(save_);
	root->addLayout(row);

	boot_ = newCombo("IDC_BOOT_SELECTION");
	hash_ = new QToolButton;
	SET_NAME(hash_, IDC_HASH);
	hash_->setIcon(toolbarIcon("hash"));
	hash_->setAutoRaise(true);
	select_ = new QToolButton;
	SET_NAME(select_, IDC_SELECT);
	select_->setToolButtonStyle(Qt::ToolButtonTextOnly);
	select_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	root->addWidget(boot_txt_ = fieldLabel(boot_, "IDS_BOOT_SELECTION_TXT"));
	row = new QHBoxLayout;
	row->addWidget(boot_, 1);
	row->addWidget(hash_);
	row->addWidget(select_);
	root->addLayout(row);

	// Image option: Windows To Go, or the persistence size
	image_option_row_ = new QWidget;
	col = new QVBoxLayout(image_option_row_);
	col->setContentsMargins(0, 0, 0, 0);
	image_option_ = newCombo("IDC_IMAGE_OPTION");
	persistence_slider_ = new QSlider(Qt::Horizontal);
	SET_NAME(persistence_slider_, IDC_PERSISTENCE_SLIDER);
	persistence_size_ = new QLineEdit;
	SET_NAME(persistence_size_, IDC_PERSISTENCE_SIZE);
	// ES_NUMBER, and EM_LIMITTEXT of 7
	persistence_size_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{0,7}")),
		persistence_size_));
	persistence_units_ = newCombo("IDC_PERSISTENCE_UNITS");
	persistence_units_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	col->addWidget(image_option_txt_ = fieldLabel(image_option_, "IDS_IMAGE_OPTION_TXT"));
	row = new QHBoxLayout;
	row->addWidget(image_option_, 1);
	row->addWidget(persistence_slider_, 1);
	row->addWidget(persistence_size_, 1);
	row->addWidget(persistence_units_);
	col->addLayout(row);
	root->addWidget(image_option_row_);

	partition_ = newCombo("IDC_PARTITION_TYPE");
	target_ = newCombo("IDC_TARGET_SYSTEM");
	csm_help_ = new QLabel(QStringLiteral("?"));
	SET_NAME(csm_help_, IDS_CSM_HELP_TXT);
	csm_help_->setStyleSheet(QStringLiteral("color: #2980B9; font-weight: bold;"));
	grid = new QGridLayout;
	grid->addWidget(fieldLabel(partition_, "IDS_PARTITION_TYPE_TXT"), 0, 0);
	grid->addWidget(fieldLabel(target_, "IDS_TARGET_SYSTEM_TXT"), 0, 1);
	grid->addWidget(partition_, 1, 0);
	row = new QHBoxLayout;
	row->addWidget(target_, 1);
	row->addWidget(csm_help_);
	grid->addLayout(row, 1, 1);
	grid->setColumnStretch(0, 1);
	grid->setColumnStretch(1, 1);
	root->addLayout(grid);

	advanced_device_ = new QToolButton;
	SET_NAME(advanced_device_, IDC_ADVANCED_DRIVE_PROPERTIES);
	advanced_device_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	advanced_device_->setAutoRaise(true);
	root->addWidget(advanced_device_);
	advanced_device_box_ = new QWidget;
	col = new QVBoxLayout(advanced_device_box_);
	col->setContentsMargins(0, 0, 0, 0);
	list_usb_hdd_ = new QCheckBox;
	SET_NAME(list_usb_hdd_, IDC_LIST_USB_HDD);
	old_bios_fixes_ = new QCheckBox;
	SET_NAME(old_bios_fixes_, IDC_OLD_BIOS_FIXES);
	uefi_validation_ = new QCheckBox;
	SET_NAME(uefi_validation_, IDC_UEFI_MEDIA_VALIDATION);
	col->addWidget(list_usb_hdd_);
	col->addWidget(old_bios_fixes_);
	col->addWidget(uefi_validation_);
	root->addWidget(advanced_device_box_);

	/* Format Options */
	root->addWidget(sectionHeader(&format_options_txt_, "IDS_FORMAT_OPTIONS_TXT"));
	label_ = new QLineEdit;
	SET_NAME(label_, IDC_LABEL);
	root->addWidget(fieldLabel(label_, "IDS_LABEL_TXT"));
	root->addWidget(label_);
	fs_ = newCombo("IDC_FILE_SYSTEM");
	cluster_ = newCombo("IDC_CLUSTER_SIZE");
	grid = new QGridLayout;
	grid->addWidget(fieldLabel(fs_, "IDS_FILE_SYSTEM_TXT"), 0, 0);
	grid->addWidget(fieldLabel(cluster_, "IDS_CLUSTER_SIZE_TXT"), 0, 1);
	grid->addWidget(fs_, 1, 0);
	grid->addWidget(cluster_, 1, 1);
	grid->setColumnStretch(0, 1);
	grid->setColumnStretch(1, 1);
	root->addLayout(grid);

	advanced_format_ = new QToolButton;
	SET_NAME(advanced_format_, IDC_ADVANCED_FORMAT_OPTIONS);
	advanced_format_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	advanced_format_->setAutoRaise(true);
	root->addWidget(advanced_format_);
	advanced_format_box_ = new QWidget;
	col = new QVBoxLayout(advanced_format_box_);
	col->setContentsMargins(0, 0, 0, 0);
	quick_format_ = new QCheckBox;
	SET_NAME(quick_format_, IDC_QUICK_FORMAT);
	extended_label_ = new QCheckBox;
	SET_NAME(extended_label_, IDC_EXTENDED_LABEL);
	bad_blocks_ = new QCheckBox;
	SET_NAME(bad_blocks_, IDC_BAD_BLOCKS);
	nb_passes_ = newCombo("IDC_NB_PASSES");
	col->addWidget(quick_format_);
	col->addWidget(extended_label_);
	row = new QHBoxLayout;
	row->addWidget(bad_blocks_, 1);
	row->addWidget(nb_passes_, 1);
	col->addLayout(row);
	root->addWidget(advanced_format_box_);

	/* Status */
	root->addWidget(sectionHeader(&status_txt_, "IDS_STATUS_TXT"));
	progress_ = new ProgressBar;
	SET_NAME(progress_, IDC_PROGRESS);
	progress_->setRange(0, MAX_PROGRESS);
	progress_->setValue(0);
	progress_->setMinimumHeight(fontMetrics().height() + 8);
	root->addWidget(progress_);

	row = new QHBoxLayout;
	lang_ = new QToolButton;
	SET_NAME(lang_, IDC_LANG);
	lang_->setIcon(toolbarIcon("lang"));
	about_ = new QToolButton;
	SET_NAME(about_, IDC_ABOUT);
	about_->setIcon(toolbarIcon("info"));
	settings_ = new QToolButton;
	SET_NAME(settings_, IDC_SETTINGS);
	settings_->setIcon(toolbarIcon("settings"));
	log_button_ = new QToolButton;
	SET_NAME(log_button_, IDC_LOG);
	log_button_->setIcon(toolbarIcon("log"));
	for (QToolButton* b : { lang_, about_, settings_, log_button_ }) {
		b->setAutoRaise(true);
		row->addWidget(b);
	}
	row->addStretch(1);
	start_ = new QPushButton;
	SET_NAME(start_, IDC_START);
	start_->setDefault(true);
	close_ = new QPushButton;
	SET_NAME(close_, IDCANCEL);
	row->addWidget(start_);
	row->addWidget(close_);
	root->addLayout(row);

	setCentralWidget(central);

	/* Status bar */
	status_ = new QLabel;
	SET_NAME(status_, IDC_STATUS);
	status_->setMinimumWidth(1);
	status_->setTextFormat(Qt::PlainText);
	timer_label_ = new QLabel(QStringLiteral("00:00:00"));
	timer_label_->setObjectName(QStringLiteral("IDC_STATUS_TIMER"));
	timer_label_->setEnabled(false);
	statusBar()->addWidget(status_, 1);
	statusBar()->addPermanentWidget(timer_label_);
	statusBar()->setSizeGripEnabled(false);

	/* User actions */
	connect(device_, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceSelected);
	connect(boot_, &QComboBox::currentIndexChanged, this, &MainWindow::onBootSelection);
	connect(image_option_, &QComboBox::currentIndexChanged, this, [this](int i) {
		rules_on_image_option(&m_, i);
		render();
	});
	connect(partition_, &QComboBox::currentIndexChanged, this, [this](int i) {
		rules_on_partition_scheme(&m_, i);
		render();
	});
	connect(target_, &QComboBox::currentIndexChanged, this, [this](int i) {
		rules_on_target_system(&m_, i);
		render();
	});
	connect(fs_, &QComboBox::currentIndexChanged, this, [this](int i) {
		rules_on_file_system(&m_, i);
		render();
	});
	connect(cluster_, &QComboBox::currentIndexChanged, this, [this](int i) {
		rules_on_cluster_size(&m_, i);
		render();
	});
	connect(nb_passes_, &QComboBox::currentIndexChanged, this, [this](int i) {
		rules_on_nb_passes(&m_, i);
		render();
	});
	connect(persistence_units_, &QComboBox::currentIndexChanged, this, [this](int i) {
		rules_on_persistence_units(&m_, i);
		render();
	});
	connect(persistence_slider_, &QSlider::valueChanged, this, [this](int pos) {
		rules_on_persistence_slider(&m_, (uint64_t)pos);
		render();
	});
	connect(persistence_size_, &QLineEdit::editingFinished, this, &MainWindow::onPersistenceSizeEdited);
	// Not textEdited(), so that a label set through accessibility counts too: render() blocks the signals
	connect(label_, &QLineEdit::textChanged, this, [this](const QString& text) {
		rules_on_label(&m_, text.toUtf8().constData());
	});
	auto connect_check = [this](QCheckBox* box, rules_check_t* check) {
		connect(box, &QCheckBox::clicked, this, [this, check] {
			rules_on_check(&m_, check);
			render();
		});
	};
	connect_check(quick_format_, &m_.quick_format);
	connect_check(extended_label_, &m_.extended_label);
	connect_check(old_bios_fixes_, &m_.old_bios_fixes);
	connect_check(uefi_validation_, &m_.uefi_validation);
	connect_check(bad_blocks_, &m_.bad_blocks);
	connect(list_usb_hdd_, &QCheckBox::clicked, this, [this] {
		filter_.enable_HDDs = enable_HDDs = !enable_HDDs;
		PrintStatusTimeout(lmprintf(MSG_253), enable_HDDs);
		refreshDevices(false);
	});
	connect(advanced_device_, &QToolButton::clicked, this, &MainWindow::toggleAdvancedDevice);
	connect(advanced_format_, &QToolButton::clicked, this, &MainWindow::toggleAdvancedFormat);
	// The buttons that open a modal dialog do it once their click has returned, so that an
	// accessibility client that pressed them (a screen reader, or the tests) gets its answer
	connect(select_, &QToolButton::clicked, this, [this] { onSelect(); }, Qt::QueuedConnection);
	connect(hash_, &QToolButton::clicked, this, &MainWindow::onHash);
	connect(start_, &QPushButton::clicked, this, [this] { onStart(); });
	connect(close_, &QPushButton::clicked, this, &MainWindow::onClose, Qt::QueuedConnection);
	connect(log_button_, &QToolButton::clicked, this, [this] { log_->toggle(); });
	connect(about_, &QToolButton::clicked, this, [this] { ShowAboutDialog(this); }, Qt::QueuedConnection);
	connect(settings_, &QToolButton::clicked, this, [this] { ShowUpdatePolicyDialog(this); }, Qt::QueuedConnection);
	connect(lang_, &QToolButton::clicked, this, &MainWindow::showLanguageMenu, Qt::QueuedConnection);
}

void MainWindow::retranslate()
{
	// The texts of the dialog template (rufus.rc), which the .loc only overrides for other languages
	static const struct { const char* name; const char* text; } defaults[] = {
		{ "IDS_DRIVE_PROPERTIES_TXT", "Drive Properties" },
		{ "IDS_DEVICE_TXT", "Device" },
		{ "IDS_BOOT_SELECTION_TXT", "Boot selection" },
		{ "IDC_SELECT", "Select" },
		{ "IDS_IMAGE_OPTION_TXT", "Image Option" },
		{ "IDS_PARTITION_TYPE_TXT", "Partition scheme" },
		{ "IDS_TARGET_SYSTEM_TXT", "Target system" },
		{ "IDC_LIST_USB_HDD", "List USB Hard Drives" },
		{ "IDC_OLD_BIOS_FIXES", "Add fixes for old BIOSes (extra partition, align, etc.)" },
		{ "IDC_UEFI_MEDIA_VALIDATION", "Enable runtime UEFI media validation" },
		{ "IDS_FORMAT_OPTIONS_TXT", "Format Options" },
		{ "IDS_FILE_SYSTEM_TXT", "File system" },
		{ "IDS_CLUSTER_SIZE_TXT", "Cluster size" },
		{ "IDS_LABEL_TXT", "Volume label" },
		{ "IDC_QUICK_FORMAT", "Quick format" },
		{ "IDC_BAD_BLOCKS", "Check device for bad blocks" },
		{ "IDC_EXTENDED_LABEL", "Create extended label and icon files" },
		{ "IDS_STATUS_TXT", "Status" },
		{ "IDCANCEL", "Close" },
		{ "IDC_START", "Start" },
	};

	for (const auto& d : defaults) {
		QWidget* w = findChild<QWidget*>(QLatin1StringView(d.name));
		if (auto* l = qobject_cast<QLabel*>(w))
			l->setText(QString::fromUtf8(d.text));
		else if (auto* b = qobject_cast<QAbstractButton*>(w))
			b->setText(QString::fromUtf8(d.text));
	}
	const std::pair<QWidget*, int> tooltips[] = {
		{ fs_, MSG_157 }, { cluster_, MSG_158 }, { label_, MSG_159 }, { advanced_device_, MSG_160 },
		{ advanced_format_, MSG_160 }, { bad_blocks_, MSG_161 }, { quick_format_, MSG_162 }, { boot_, MSG_164 },
		{ select_, MSG_165 }, { extended_label_, MSG_166 }, { uefi_validation_, MSG_167 },
		{ old_bios_fixes_, MSG_169 }, { list_usb_hdd_, MSG_170 }, { start_, MSG_171 }, { partition_, MSG_163 },
		{ target_, MSG_150 }, { image_option_, MSG_305 }, { persistence_slider_, MSG_125 },
		{ persistence_size_, MSG_125 }, { persistence_units_, MSG_126 }, { about_, MSG_302 },
		{ settings_, MSG_301 }, { lang_, MSG_273 }, { log_button_, MSG_303 }, { save_, MSG_304 }, { hash_, MSG_272 },
	};

	// The 't' commands of the IDD_DIALOG section of the .loc
	apply_localization(IDD_DIALOG, [](void* ctx, int ctrl_id, const char* text) -> BOOL {
		QWidget* w = static_cast<MainWindow*>(ctx)->findChild<QWidget*>(QString::fromUtf8(get_name_from_id(ctrl_id)));
		QString s = QString::fromUtf8(text);
		if (auto* l = qobject_cast<QLabel*>(w))
			l->setText(s);
		else if (auto* b = qobject_cast<QAbstractButton*>(w))
			b->setText(s);
		else
			return FALSE;
		return TRUE;
	}, this);
	image_option_text_ = image_option_txt_->text();
	// The main buttons are in uppercase (with a hardcoded exception for German, which Qt handles)
	start_->setText(start_->text().toUpper());
	close_->setText(close_->text().toUpper());
	uppercase_close_ = close_->text();
	uppercase_cancel_ = lmstr(MSG_007).toUpper();
	uppercase_select_[0] = select_->text().toUpper();
	uppercase_select_[1] = lmstr(MSG_040).toUpper();
	select_->setText(uppercase_select_[select_index_]);

	for (const auto& [widget, msg] : tooltips)
		widget->setToolTip(lmstr(msg));
	nb_passes_->setAccessibleName(lmstr(MSG_316));
	save_->setAccessibleName(lmstr(MSG_313));
	hash_->setAccessibleName(lmstr(MSG_314));
	advanced_device_->setAccessibleName(lmstr(MSG_119));
	advanced_format_->setAccessibleName(lmstr(MSG_120));
	for (QToolButton* b : { lang_, about_, settings_, log_button_ })
		b->setAccessibleName(b->toolTip());

	// The passes of the bad blocks check
	{
		QSignalBlocker block(nb_passes_);
		char text[128];
		nb_passes_->clear();
		for (int i = 0; i < 5; i++) {
			rules_passes_text(i, text, sizeof(text));
			nb_passes_->addItem(QString::fromUtf8(text));
		}
	}
}

/*
 * Rendering of the rules state
 */

void MainWindow::renderCombo(QComboBox* combo, const rules_combo_t* rc, bool enabled)
{
	QSignalBlocker block(combo);
	char text[512];
	bool same = (combo->count() == rc->count);

	for (int i = 0; same && (i < rc->count); i++) {
		rules_entry_text(&m_, rc, i, text, sizeof(text));
		same = (combo->itemData(i).toInt() == rc->entry[i].data) && (combo->itemText(i) == QString::fromUtf8(text));
	}
	if (!same) {
		combo->clear();
		for (int i = 0; i < rc->count; i++) {
			rules_entry_text(&m_, rc, i, text, sizeof(text));
			combo->addItem(QString::fromUtf8(text), rc->entry[i].data);
		}
	}
	combo->setCurrentIndex(rc->sel);
	combo->setEnabled(enabled);
}

void MainWindow::renderCheck(QCheckBox* box, const rules_check_t* rc)
{
	QSignalBlocker block(box);
	box->setEnabled(rc->enabled);
	box->setChecked(rc->checked);
}

void MainWindow::renderPersistence()
{
	QSignalBlocker block1(persistence_slider_), block2(persistence_size_), block3(persistence_units_);
	const bool persistence = (m_.image_options & IMOP_PERSISTENCE);
	QString text;

	persistence_slider_->setVisible(persistence);
	persistence_size_->setVisible(persistence);
	persistence_units_->setVisible(persistence && m_.persistence_units_visible);
	if (!persistence)
		return;
	persistence_slider_->setRange(0, (int)std::min<uint64_t>(m_.persistence_max, INT_MAX));
	persistence_slider_->setValue((int)std::min<uint64_t>(m_.persistence_pos, INT_MAX));
	persistence_slider_->setEnabled(m_.persistence_slider_enabled);
	// SetPersistencePos()
	if ((m_.boot_type == BT_IMAGE) && (m_.persistence_pos != 0))
		text = QString::number(m_.persistence_pos);
	else
		text = QStringLiteral("0 (%1)").arg(lmstr(MSG_124));
	// Not while the user is typing a size in
	if (!persistence_size_->hasFocus())
		persistence_size_->setText(text);
	persistence_size_->setEnabled(m_.persistence_size_enabled);
	persistence_size_->setAlignment(m_.persistence_units_visible ? Qt::AlignRight : Qt::AlignLeft);
	renderCombo(persistence_units_, &m_.persistence_units, m_.persistence_units_enabled);
}

// UM_UPDATE_CSM_TOOLTIP
void MainWindow::updateCsmHelp()
{
	const int target_type = rules_combo_data(&m_.target);

	csm_help_->setVisible((target_type == TT_UEFI) || m_.has_uefi_csm);
	csm_help_->setToolTip(lmstr((target_type == TT_UEFI) ? MSG_152 : MSG_151));
	csm_help_->setEnabled(m_.partition_enabled);
}

void MainWindow::render()
{
	QSignalBlocker block(label_);

	device_->setEnabled(m_.device_enabled);
	list_usb_hdd_->setEnabled(m_.device_enabled);
	list_usb_hdd_->setChecked(filter_.enable_HDDs);
	save_->setVisible(m_.advanced_mode_device);
	save_->setEnabled(m_.save_enabled);
	renderCombo(boot_, &m_.boot, m_.boot_enabled);
	select_->setEnabled(m_.boot_enabled);
	hash_->setEnabled(m_.hash_enabled);
	for (QToolButton* b : { lang_, about_, settings_, log_button_ })
		b->setEnabled(m_.boot_enabled);

	// ToggleImageOptions()
	image_option_row_->setVisible(m_.image_options != 0);
	image_option_txt_->setText((m_.image_options & IMOP_PERSISTENCE) ? lmstr(MSG_123) : image_option_text_);
	image_option_->setVisible(m_.image_options & IMOP_WINTOGO);
	renderCombo(image_option_, &m_.image_option, m_.image_option_enabled);
	renderPersistence();

	renderCombo(partition_, &m_.partition, m_.partition_enabled);
	renderCombo(target_, &m_.target, m_.target_enabled);
	updateCsmHelp();

	advanced_device_->setText(lmstr(m_.advanced_mode_device ? MSG_122 : MSG_121, lmprintf(MSG_119)));
	advanced_device_->setArrowType(m_.advanced_mode_device ? Qt::UpArrow : Qt::DownArrow);
	advanced_device_box_->setVisible(m_.advanced_mode_device);
	renderCheck(old_bios_fixes_, &m_.old_bios_fixes);
	renderCheck(uefi_validation_, &m_.uefi_validation);

	if (label_->text() != QString::fromUtf8(m_.label))
		label_->setText(QString::fromUtf8(m_.label));
	label_->setEnabled(m_.label_enabled);
	renderCombo(fs_, &m_.fs, m_.fs_enabled);
	renderCombo(cluster_, &m_.cluster, m_.cluster_enabled);

	advanced_format_->setText(lmstr(m_.advanced_mode_format ? MSG_122 : MSG_121, lmprintf(MSG_120)));
	advanced_format_->setArrowType(m_.advanced_mode_format ? Qt::UpArrow : Qt::DownArrow);
	advanced_format_box_->setVisible(m_.advanced_mode_format);
	renderCheck(quick_format_, &m_.quick_format);
	renderCheck(extended_label_, &m_.extended_label);
	renderCheck(bad_blocks_, &m_.bad_blocks);
	{
		QSignalBlocker block_passes(nb_passes_);
		nb_passes_->setCurrentIndex(m_.nb_passes_sel);
		nb_passes_->setEnabled(m_.bad_blocks.enabled);
		nb_passes_->setToolTip(nb_passes_->currentText());
	}

	start_->setEnabled(m_.start_enabled && !format_running_ && !cycling_);
}

/*
 * The drive list
 */

void MainWindow::refreshDevices(bool startup)
{
	QSignalBlocker block(device_);
	QString previous_node;
	uint64_t previous_seq = 0;
	rules_drive_t drive;
	const rufus_device_t* dev;
	int i, sel = 0;

	if ((dev = selectedDevice()) != nullptr) {
		previous_node = QString::fromUtf8(dev->devnode);
		previous_seq = dev->diskseq;
	}
	monitor_->refreshed();
	nb_devs_ = GetTestDevices(devs_, MAX_DRIVES, &filter_);
	if (nb_devs_ < 0)
		nb_devs_ = GetDevices(devs_, MAX_DRIVES, &filter_);
	if (nb_devs_ < 0)
		nb_devs_ = 0;
	device_->clear();
	for (i = 0; i < nb_devs_; i++) {
		device_->addItem(QString::fromUtf8(devs_[i].display_name), i);
		if ((QString::fromUtf8(devs_[i].devnode) == previous_node) && (devs_[i].diskseq == previous_seq))
			sel = i;
	}
	device_->setCurrentIndex((nb_devs_ == 0) ? -1 : sel);

	if (startup) {
		dev = selectedDevice();
		if (dev != nullptr)
			GetRulesDrive(dev, &drive);
		rules_on_startup(&m_, (dev != nullptr) ? &drive : nullptr);
		updateDeviceTooltip();
		render();
		PrintStatus(0, (nb_devs_ == 1) ? MSG_208 : MSG_209, nb_devs_);
	} else {
		// The device refresh ends with the selection of a device (IDC_DEVICE)
		dev = selectedDevice();
		if (dev != nullptr)
			GetRulesDrive(dev, &drive);
		rules_on_device_refresh(&m_, (dev != nullptr) ? &drive : nullptr);
		updateDeviceTooltip();
		render();
		PrintStatus(0, (nb_devs_ == 1) ? MSG_208 : MSG_209, nb_devs_);
	}
}

const rufus_device_t* MainWindow::selectedDevice() const
{
	int i = device_->currentIndex();
	return ((i < 0) || (i >= nb_devs_)) ? nullptr : &devs_[i];
}

// The tooltip of the device list, with the size of the device (PopulateProperties())
void MainWindow::updateDeviceTooltip()
{
	const rufus_device_t* dev = selectedDevice();
	QString size;

	if (dev == nullptr) {
		device_->setToolTip(QString());
		return;
	}
	size = QString::fromUtf8(SizeToHumanReadable(dev->size, FALSE, FALSE));
	if (right_to_left_mode)
		device_->setToolTip(QStringLiteral("(%1) %2").arg(size, QString::fromUtf8(dev->name)));
	else
		device_->setToolTip(QStringLiteral("%1 (%2)").arg(QString::fromUtf8(dev->name), size));
}

void MainWindow::onDeviceSelected(int)
{
	const rufus_device_t* dev = selectedDevice();
	rules_drive_t drive;

	PrintStatus(0, (nb_devs_ == 1) ? MSG_208 : MSG_209, nb_devs_);
	if (dev != nullptr)
		GetRulesDrive(dev, &drive);
	rules_on_device(&m_, (dev != nullptr) ? &drive : nullptr);
	updateDeviceTooltip();
	render();
}

/*
 * User actions
 */

void MainWindow::onBootSelection(int index)
{
	rules_on_boot_selection(&m_, index);
	render();
}

void MainWindow::onPersistenceSizeEdited()
{
	uint64_t size = persistence_size_->text().toULongLong() * MB;

	for (int i = 0; i < m_.persistence_units.sel; i++)
		size *= 1024;
	rules_on_persistence_size(&m_, size);
	persistence_size_->clearFocus();
	render();
}

void MainWindow::toggleAdvancedDevice()
{
	advanced_mode_device = !advanced_mode_device;
	WriteSettingBool(SETTING_ADVANCED_MODE_DEVICE, advanced_mode_device);
	rules_on_toggle_advanced_device(&m_);
	render();
}

void MainWindow::toggleAdvancedFormat()
{
	advanced_mode_format = !advanced_mode_format;
	WriteSettingBool(SETTING_ADVANCED_MODE_FORMAT, advanced_mode_format);
	rules_on_toggle_advanced_format(&m_);
	render();
}

void MainWindow::PrintStatusTimeout(const char* str, BOOL val)
{
	PrintStatus(STATUS_MSG_TIMEOUT, val ? MSG_250 : MSG_251, str);
}

// IDC_SELECT, a dropped file, or an image given on the command line
void MainWindow::onSelect(const QString& provided)
{
	QString path = provided;
	char* old_image_path = image_path;

	// Ctrl-SELECT is used to select an additional archive of files to extract
	if (provided.isEmpty() && (QGuiApplication::keyboardModifiers() & Qt::ControlModifier)) {
		path = QFileDialog::getOpenFileName(this, QString(), QDir::homePath(),
			QStringLiteral("%1 (*.zip)").arg(lmstr(MSG_309)));
		if (path.isEmpty())
			return;
		safe_free(archive_path);
		archive_path = strdup(path.toUtf8().constData());
		uprintf("Using archive: %s (%s)", QFileInfo(path).fileName().toUtf8().constData(),
			SizeToHumanReadable(QFileInfo(path).size(), FALSE, FALSE));
		return;
	}
	if (provided.isEmpty()) {
		const QString extensions = QStringLiteral("*.iso *.img *.vhd *.vhdx *.usb *.bz2 *.bzip2 *.gz *.lzma *.xz *.Z "
			"*.zip *.zst *.wic *.wim *.esd *.vtsi");
		path = QFileDialog::getOpenFileName(this, QString(), last_image_dir_.isEmpty() ? QDir::homePath() : last_image_dir_,
			QStringLiteral("%1 (%2);;%3 (*)").arg(lmstr(MSG_280), extensions, lmstr(MSG_109)));
		if (path.isEmpty()) {
			if (old_image_path == nullptr) {
				select_->setToolTip(lmstr(MSG_173));
				PrintStatus(0, MSG_086);
			}
			return;
		}
		last_image_dir_ = QFileInfo(path).absolutePath();
	} else {
		uprintf("\nImage provided: '%s'", provided.toUtf8().constData());
	}
	safe_free(archive_path);
	image_path = strdup(QFileInfo(path).absoluteFilePath().toUtf8().constData());
	free(old_image_path);
	startImageScan(false);
}

void MainWindow::startImageScan(bool keep_image_name)
{
	keep_image_name_ = keep_image_name;
	ErrorStatus = 0;
	rules_on_image_scan_start(&m_);
	render();
	if (!StartImageScan(keep_image_name)) {
		uprintf("Unable to start ISO scanning thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		rules_on_image_scan_end(&m_, &img_report, RULES_SCAN_FAILED, keep_image_name);
		render();
	}
}

// IDC_HASH
void MainWindow::onHash()
{
	pthread_t thread;

	if (format_running_ || (image_path == nullptr))
		return;
	ErrorStatus = 0;
	no_confirmation_on_cancel_ = true;
	onEvent(UM_PROGRESS_INIT, 0);
	// Disable all controls except cancel
	EnableControls(&m_, FALSE, FALSE);
	render();
	InitProgress(FALSE);
	if (pthread_create(&thread, nullptr, HashThread, nullptr) != 0) {
		uprintf("Unable to start hash thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		onEvent(UM_FORMAT_COMPLETED, FALSE);
		return;
	}
	pthread_detach(thread);
	format_running_ = true;
	PrintInfo(0, -1);
	onEvent(UM_TIMER_START, 0);
}

// IDC_START
void MainWindow::onStart(bool zero_drive)
{
	const rufus_device_t* dev = selectedDevice();
	bootcheck_drive_t bcd = {};

	if (format_running_ || cycling_ || (zero_drive ? (dev == nullptr) : !m_.start_enabled))
		return;
	zero_drive_ = zero_drive;
	rules_on_start(&m_);
	render();
	ErrorStatus = 0;
	LastWriteError = 0;
	no_confirmation_on_cancel_ = false;
	onEvent(UM_PROGRESS_INIT, 0);
	// The options of the operation: the selections of the window, then the settings and cheat modes
	SetOptionsFromSettings(&opts);
	rules_get_options(&m_, &opts);
	opts.zero_drive = zero_drive;
	opts.fast_zeroing = zero_drive && fast_zeroing;
	if (dev != nullptr) {
		opts.device_index = device_->currentIndex();
		opts.disk_size = dev->size;
		start_device_ = *dev;
		bcd.path = start_device_.devnode;
		bcd.is_fixed = !dev->props.is_Removable;
	}
	// A thread validates the options and downloads what they need, and reports UM_FORMAT_START
	if (!StartBootCheck(&bcd)) {
		uprintf("Unable to start boot check thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		onEvent(UM_FORMAT_COMPLETED, FALSE);
	}
}

// IDCANCEL: cancel the operation in progress, or close the application
void MainWindow::onClose()
{
	close_->setEnabled(false);
	if (format_running_) {
		if (no_confirmation_on_cancel_ || (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_049),
			lmprintf(MSG_105)) == IDYES)) {
			// The operation may have completed in the meantime
			if (format_running_) {
				CancelOperation();
				PrintInfo(0, MSG_201);
				uprintf("Cancelling");
			}
		} else {
			close_->setEnabled(true);
		}
		no_confirmation_on_cancel_ = false;
		return;
	} else if (m_.op_in_progress) {
		// The user might be trying to cancel during the preliminary checks
		CancelOperation();
		PrintInfo(0, MSG_201);
		close_->setEnabled(true);
		return;
	}
	close();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	if (m_.op_in_progress || format_running_) {
		close_->setEnabled(true);
		event->ignore();
		return;
	}
	if (!relaunch_) {
		log_->saveToStateDir();
		log_->close();
	}
	event->accept();
	QCoreApplication::exit(0);
}

// The entries of the language menu, with the selected locale checked
void MainWindow::fillLanguageMenu(QMenu* menu)
{
	loc_cmd* lcmd = nullptr;

	menu->setObjectName(QStringLiteral("IDC_LANG_MENU"));
	list_for_each_entry(lcmd, &locale_list, loc_cmd, list) {
		QString name = QString::fromUtf8(lcmd->txt[1]);
		// The appearance of LTR languages must be fixed for RTL menus
		if (right_to_left_mode && !(lcmd->ctrl_id & LOC_RIGHT_TO_LEFT)) {
			int open = (int)name.indexOf(QLatin1Char('('));
			if (open > 0)
				name = QString::fromUtf8(LEFT_TO_RIGHT_EMBEDDING "%1 " POP_DIRECTIONAL_FORMATTING "%2")
					.arg(name.mid(open), name.left(open).trimmed());
		}
		QAction* a = menu->addAction(name);
		a->setCheckable(true);
		a->setChecked(lcmd == selected_locale);
		a->setData(QVariant::fromValue((void*)lcmd));
	}
}

// IDC_LANG
void MainWindow::showLanguageMenu()
{
	QMenu menu(this);
	QAction* chosen;

	fillLanguageMenu(&menu);
	// Show the menu such that it doesn't overlap the button
	chosen = menu.exec(lang_->mapToGlobal(QPoint(lang_->width() / 2, lang_->height())));
	if (chosen != nullptr)
		selectLanguage(static_cast<loc_cmd*>(chosen->data().value<void*>()));
}

void MainWindow::selectLanguage(loc_cmd* lcmd)
{
	if ((lcmd == nullptr) || (lcmd == selected_locale) || m_.op_in_progress || format_running_)
		return;
	selected_locale = lcmd;
	// Avoid the file system being reset on language change
	preselected_fs = rules_combo_data(&m_.fs);
	relaunch_ = true;
	close();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if (!format_running_ && !m_.op_in_progress && event->mimeData()->hasUrls() &&
		event->mimeData()->urls().first().isLocalFile())
		event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
	if (format_running_ || m_.op_in_progress || !event->mimeData()->hasUrls())
		return;
	event->acceptProposedAction();
	onSelect(event->mimeData()->urls().first().toLocalFile());
}

/*
 * The format operation
 */

// UM_FORMAT_START: the boot check is done
void MainWindow::onFormatStart(int r)
{
	const rufus_device_t* dev = &start_device_;
	char title[256];
	StrArray processes;
	struct stat st;

	if (r != BOOTCHECK_PROCEED)
		goto aborted_start;
	// All subsequent aborts below translate to a user cancellation
	r = BOOTCHECK_CANCEL;

	if ((opts.partition_type == PARTITION_STYLE_MBR) && (dev->size > 2 * TB)) {
		if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_128, "MBR"),
			lmprintf(MSG_134, SizeToHumanReadable(dev->size - 2 * TB, FALSE, FALSE))) != IDYES)
			goto aborted_start;
	}

	if (!zero_drive_ && (opts.fs_type == FS_UDF)) {
		// Display a warning about UDF formatting times
		dur_secs = (uint32_t)(((double)dev->size) / 1073741824.0f / UDF_FORMAT_SPEED);
		if (dur_secs > UDF_FORMAT_WARN) {
			dur_mins = dur_secs / 60;
			dur_secs -= dur_mins * 60;
			Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_113), lmprintf(MSG_112, dur_mins, dur_secs));
		} else {
			dur_secs = 0;
			dur_mins = 0;
		}
	}

	// The processes that use the drive (the ones of this user: the helper checks again as root)
	PrintStatus(0, MSG_278);
	if (stat(dev->devnode, &st) == 0) {
		StrArrayCreate(&processes, 8);
		if (SearchProcesses(st.st_rdev, &processes) > 0) {
			for (uint32_t i = 0; i < processes.Index; i++)
				uprintf("Process using the drive: %s", processes.String[i]);
			static_strcpy(title, dev->display_name);
			if (Notification(MB_ICONWARNING | MB_YESNO, title, lmprintf(MSG_132)) != IDYES) {
				StrArrayDestroy(&processes);
				goto aborted_start;
			}
		}
		StrArrayDestroy(&processes);
	}
	PrintStatus(0, MSG_142);

	if (Notification(MB_OKCANCEL | MB_ICONWARNING, APPLICATION_NAME, lmprintf(MSG_003, dev->display_name)) != IDOK)
		goto aborted_start;
	// NB: Windows counts the partitions, and the udev database the volumes
	if ((dev->nb_volumes > 1) && (Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_094), lmprintf(MSG_093)) != IDOK))
		goto aborted_start;
	if (!zero_drive_ && (opts.boot_type != BT_NON_BOOTABLE) && (dev->sector_size != 512) &&
		(Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_197), lmprintf(MSG_196, dev->sector_size)) != IDOK))
		goto aborted_start;

	if (!startFormatThread()) {
		uprintf("Unable to start formatting thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		onEvent(UM_FORMAT_COMPLETED, FALSE);
		return;
	}
	uprintf("\nFormat operation started");
	onEvent(UM_TIMER_START, 0);
	// Set focus to the Cancel button
	close_->setFocus();
	return;

aborted_start:
	zero_drive_ = false;
	if (queued_hotplug_event_)
		onEvent(UM_MEDIA_CHANGE, 0);
	if (r == BOOTCHECK_CANCEL) {
		PrintStatus(0, (nb_devs_ == 1) ? MSG_208 : MSG_209, nb_devs_);
		PrintStatus(5000, MSG_041);
		if (unattend_xml_path != nullptr) {
			unlink(unattend_xml_path);
			safe_free(unattend_xml_path);
		}
		rules_on_operation_end(&m_);
		render();
		return;
	}
	ErrorStatus = RUFUS_ERROR((r == BOOTCHECK_DOWNLOAD_ERROR) ? APPERR(ERROR_CANT_DOWNLOAD) : ERROR_GEN_FAILURE);
	onEvent(UM_FORMAT_COMPLETED, FALSE);
}

/*
 * The format runs in rufus-helper, which a thread starts (the first time) and
 * drives, as both wait: for the polkit prompt, and then for the helper.
 */
bool MainWindow::startFormatThread()
{
	struct Job {
		std::string devnode, image_name, bb_log_path;
		uint64_t diskseq;
		int image_fd = -1, archive_fd = -1, bb_log_fd = -1;
	};
	auto job = std::make_shared<Job>();
	char date[32], path[MAX_PATH];
	struct tm lt;
	time_t now;

	job->devnode = start_device_.devnode;
	job->diskseq = start_device_.diskseq;
	if ((opts.boot_type == BT_IMAGE) && (image_path != nullptr) && !opts.zero_drive) {
		job->image_fd = open(image_path, O_RDONLY | O_CLOEXEC);
		if (job->image_fd < 0) {
			uprintf("Could not open '%s': %s", image_path, strerror(errno));
			ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
			return false;
		}
		job->image_name = image_path;
	}
	if ((archive_path != nullptr) && !opts.zero_drive) {
		job->archive_fd = open(archive_path, O_RDONLY | O_CLOEXEC);
		if (job->archive_fd < 0)
			uprintf("Could not open '%s': %s", archive_path, strerror(errno));
	}
	if (opts.bad_blocks && !opts.zero_drive) {
		// The report of the check goes to the home directory, as upstream does with %USERPROFILE%
		now = time(nullptr);
		localtime_r(&now, &lt);
		strftime(date, sizeof(date), "%Y%m%d_%H%M%S", &lt);
		static_sprintf(path, "%s/rufus_%s.log", QDir::homePath().toUtf8().constData(), date);
		job->bb_log_fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (job->bb_log_fd < 0)
			uprintf("Could not create '%s': %s", path, strerror(errno));
		else
			job->bb_log_path = path;
	}

	format_running_ = true;
	render();
	std::thread([this, job] {
		helper_job_t hj = {};
		struct stat st;

		if ((session_helper.sock < 0) && !StartHelper(&session_helper, selected_locale_name())) {
			uprintf("Could not start the helper");
			if (!IS_ERROR(ErrorStatus))
				ErrorStatus = RUFUS_ERROR(ERROR_ACCESS_DENIED);
		} else {
			hj.devnode = job->devnode.c_str();
			hj.diskseq = job->diskseq;
			hj.image_fd = job->image_fd;
			hj.image_name = job->image_name.empty() ? nullptr : job->image_name.c_str();
			hj.archive_fd = job->archive_fd;
			hj.bb_log_fd = job->bb_log_fd;
			hj.bb_log_path = job->bb_log_path.c_str();
			HelperFormat(&session_helper, &hj);
		}
		if (job->bb_log_fd >= 0) {
			// No bad blocks: no report
			if ((fstat(job->bb_log_fd, &st) == 0) && (st.st_size == 0))
				unlink(job->bb_log_path.c_str());
			::close(job->bb_log_fd);
		}
		if (job->image_fd >= 0)
			::close(job->image_fd);
		if (job->archive_fd >= 0)
			::close(job->archive_fd);
		ui_event(UM_FORMAT_COMPLETED, TRUE);
	}).detach();
	return true;
}

// UM_FORMAT_COMPLETED: param is TRUE when the drive was written to
void MainWindow::onFormatCompleted(bool written)
{
	zero_drive_ = false;
	format_running_ = false;
	if (unattend_xml_path != nullptr) {
		unlink(unattend_xml_path);
		safe_free(unattend_xml_path);
	}
	clock_.stop();
	// Close the cancel confirmation, if it is displayed
	CloseNotifications(lmstr(MSG_049));
	close_->setEnabled(true);
	rules_on_operation_end(&m_);
	render();
	if (written) {
		uprintf("\n");
		refreshDevices(false);
	}
	// When the window is in the background, the desktop tells the user
	if (written && !isActiveWindow() && !IS_ERROR(ErrorStatus))
		taskbar_.notify(QStringLiteral(APPLICATION_NAME), lmstr(MSG_210), false);
	else if (written && !isActiveWindow() && (SCODE_CODE(ErrorStatus) != ERROR_CANCELLED))
		taskbar_.notify(lmstr(MSG_042), lmstr(MSG_043, StrError(ErrorStatus, FALSE)), true);
	if (!IS_ERROR(ErrorStatus)) {
		progress_->setValue(MAX_PROGRESS);
		taskbar_.setProgress(-1);
		PrintInfo(0, MSG_210);
		QApplication::beep();
		QApplication::alert(this);
	} else if (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED) {
		progress_->setState(ProgressBar::State::Paused);
		PrintInfo(0, MSG_211);
		Notification(MB_ICONINFORMATION | MB_CLOSE, lmprintf(MSG_211), lmprintf(MSG_041));
	} else {
		progress_->setState(ProgressBar::State::Error);
		PrintInfo(0, MSG_212);
		QApplication::beep();
		QApplication::alert(this);
		// A port cycle usually helps with a device not ready
		if (SCODE_CODE(ErrorStatus) == ERROR_NOT_READY)
			cyclePort(true);
		Notification(MB_ICONERROR | MB_CLOSE, lmprintf(MSG_042), lmprintf(MSG_043, StrError(ErrorStatus, FALSE)));
	}
	ErrorStatus = 0;
	LastWriteError = 0;
}

/*
 * The ui_iface_t callbacks
 */

void MainWindow::setStatus(bool info, unsigned int duration, const QString& text)
{
	if (info) {
		progress_->setInfo(text);
		return;
	}
	if (duration == 0) {
		status_text_ = text;
		if (!status_timer_.isActive())
			status_->setText(text);
		return;
	}
	// A transient message, after which the previous one gets restored
	status_->setText(text);
	status_timer_.start((int)duration);
}

void MainWindow::setProgress(void* dialog, uint32_t pos, bool taskbar)
{
	if (dialog != nullptr) {
		static_cast<ProgressReceiver*>(dialog)->setProgress(pos);
		return;
	}
	progress_->setValue((int)pos);
	if (taskbar)
		taskbar_.setProgress((double)pos / MAX_PROGRESS);
}

int MainWindow::ask(ui_question_t* q)
{
	switch (q->type) {
	case UI_QUESTION_NOTIFICATION:
		return RunNotification(this, q);
	case UI_QUESTION_SELECTION:
		return RunSelection(this, q);
	case UI_QUESTION_LIST:
		RunList(this, q);
		return 0;
	}
	return -1;
}

void MainWindow::onEvent(enum ui_event_type type, intptr_t param)
{
	switch (type) {
	case UM_FORMAT_COMPLETED:
		onFormatCompleted(param != 0);
		break;
	case UM_MEDIA_CHANGE:
		if (format_running_)
			break;
		if (m_.op_in_progress) {
			queued_hotplug_event_ = true;
			break;
		}
		queued_hotplug_event_ = false;
		refreshDevices(false);
		break;
	case UM_PROGRESS_INIT:
		if (param != 0) {
			static_cast<ProgressReceiver*>((void*)param)->setProgress(0);
			break;
		}
		progress_->setMarquee(false);
		progress_->setState(ProgressBar::State::Normal);
		progress_->setValue(0);
		taskbar_.setProgress(0);
		break;
	case UM_PROGRESS_MARQUEE:
		progress_->setState(ProgressBar::State::Normal);
		progress_->setMarquee(true);
		taskbar_.setIndeterminate();
		break;
	case UM_PROGRESS_EXIT:
		if (progress_->isMarquee()) {
			progress_->setMarquee(false);
			taskbar_.setProgress(-1);
		} else if (!IS_ERROR(ErrorStatus)) {
			taskbar_.setProgress(1);
		} else if (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED) {
			progress_->setState(ProgressBar::State::Paused);
		} else {
			progress_->setState(ProgressBar::State::Error);
			QApplication::beep();
			QApplication::alert(this);
		}
		break;
	case UM_NO_UPDATE:
		Notification(MB_ICONINFORMATION | MB_CLOSE, lmprintf(MSG_243), lmprintf(MSG_247));
		break;
	case UM_UPDATE_CSM_TOOLTIP:
		updateCsmHelp();
		break;
	case UM_ENABLE_CONTROLS:
		clock_.stop();
		if (!IS_ERROR(ErrorStatus))
			PrintInfo(0, MSG_210);
		else if (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)
			PrintInfo(0, MSG_211);
		else if (SCODE_CODE(ErrorStatus) == ERROR_BAD_SIGNATURE)
			PrintInfo(0, MSG_283);
		else
			PrintInfo(0, MSG_212);
		rules_on_operation_end(&m_);
		render();
		break;
	case UM_SELECT_ISO:
		select_index_ = 0;
		select_->setText(uppercase_select_[0]);
		if (image_path != nullptr)
			onSelect(QString::fromUtf8(image_path));
		break;
	case UM_TIMER_START:
		PrintInfo(0, -1);
		elapsed_ = 0;
		timer_label_->setText(QStringLiteral("00:00:00"));
		timer_label_->setEnabled(true);
		clock_.start();
		break;
	case UM_FORMAT_START:
		onFormatStart((int)param);
		break;
	case UM_HASH_COMPLETED:
		ShowHashDialog(this);
		break;
	case UM_IMAGE_SCAN_COMPLETED:
		rules_on_image_scan_end(&m_, &img_report, (enum rules_scan_result)param, keep_image_name_);
		render();
		if (queued_hotplug_event_)
			onEvent(UM_MEDIA_CHANGE, 0);
		break;
	}
}

/*
 * The cheat codes (the message loop of WinMain() in rufus.c)
 */

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
	QKeyEvent* key;

	if ((event->type() != QEvent::KeyPress) && (event->type() != QEvent::KeyRelease) &&
		(event->type() != QEvent::ShortcutOverride))
		return QMainWindow::eventFilter(watched, event);
	// Only for the main window and the log, not for the dialogs (which are modal anyway)
	if (!watched->isWidgetType() || ((static_cast<QWidget*>(watched)->window() != this) &&
		(static_cast<QWidget*>(watched)->window() != log_)))
		return QMainWindow::eventFilter(watched, event);
	key = static_cast<QKeyEvent*>(event);

	// A standalone 'Alt' key toggles the progress report between percent, rate and remaining time
	if (key->key() == Qt::Key_Alt) {
		if ((event->type() == QEvent::KeyPress) && !key->isAutoRepeat()) {
			alt_pressed_ = true;
			alt_command_ = false;
		} else if ((event->type() == QEvent::KeyRelease) && alt_pressed_) {
			if (!alt_command_)
				update_progress_type = (update_progress_type + 1) % UPT_MAX;
			alt_pressed_ = false;
		}
		return QMainWindow::eventFilter(watched, event);
	}
	if (key->modifiers() & Qt::AltModifier)
		alt_command_ = true;
	if (event->type() == QEvent::KeyRelease)
		return QMainWindow::eventFilter(watched, event);
	// Accepting the override keeps the cheat codes from being taken as mnemonics or shortcuts
	if (event->type() == QEvent::ShortcutOverride) {
		if (cheatCode(key->key(), key->modifiers(), true)) {
			event->accept();
			return true;
		}
		return QMainWindow::eventFilter(watched, event);
	}
	if (!key->isAutoRepeat() && cheatCode(key->key(), key->modifiers(), false))
		return true;
	return QMainWindow::eventFilter(watched, event);
}

// Returns true if the key is a cheat code, which gets enacted unless check_only is set
bool MainWindow::cheatCode(int key, Qt::KeyboardModifiers modifiers, bool check_only)
{
	const Qt::KeyboardModifiers mods = modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
	const bool ctrl = (mods == Qt::ControlModifier), alt = (mods == Qt::AltModifier);
	const bool ctrl_alt = (mods == (Qt::ControlModifier | Qt::AltModifier));
	const bool op = m_.op_in_progress || format_running_ || cycling_;
	char path[MAX_PATH];

	// Ctrl-L => Open/Close the log
	if (ctrl && (key == Qt::Key_L)) {
		if (!check_only)
			log_->toggle();
		return true;
	}
	// Ctrl-P => Persistent log
	if (ctrl && (key == Qt::Key_P)) {
		if (!check_only) {
			persistent_log = !persistent_log;
			WriteSettingBool(SETTING_PERSISTENT_LOG, persistent_log);
			PrintStatusTimeout(lmprintf(MSG_336), persistent_log);
		}
		return true;
	}
	// Alt +/- => Increase or decrease the priority of the format (from the next one on)
	if (alt && ((key == Qt::Key_Plus) || (key == Qt::Key_Minus) || (key == Qt::Key_Equal))) {
		if (!check_only) {
			int delta = (key == Qt::Key_Minus) ? -1 : +1;
			if (((delta == +1) && (default_thread_priority < THREAD_PRIORITY_HIGHEST)) ||
				((delta == -1) && (default_thread_priority > THREAD_PRIORITY_LOWEST))) {
				default_thread_priority += delta;
				WriteSetting32(SETTING_DEFAULT_THREAD_PRIORITY, default_thread_priority - THREAD_PRIORITY_ABOVE_NORMAL);
			}
			PrintStatus(STATUS_MSG_TIMEOUT, MSG_318, default_thread_priority);
		}
		return true;
	}

	// The following cheat modes should not be enacted when an operation is in progress
	if (!alt && !ctrl_alt)
		return false;
	if (alt) {
		switch (key) {
		case Qt::Key_Period: case Qt::Key_Comma: case Qt::Key_A: case Qt::Key_B: case Qt::Key_C: case Qt::Key_D:
		case Qt::Key_E: case Qt::Key_F: case Qt::Key_G: case Qt::Key_H: case Qt::Key_I: case Qt::Key_J:
		case Qt::Key_K: case Qt::Key_L: case Qt::Key_M: case Qt::Key_N: case Qt::Key_O: case Qt::Key_P:
		case Qt::Key_Q: case Qt::Key_R: case Qt::Key_S: case Qt::Key_T: case Qt::Key_U: case Qt::Key_V:
		case Qt::Key_W: case Qt::Key_X: case Qt::Key_Y: case Qt::Key_Z:
			break;
		default:
			return false;
		}
	} else if ((key != Qt::Key_Z) && (key != Qt::Key_D) && (key != Qt::Key_E) && (key != Qt::Key_F) &&
		(key != Qt::Key_Y)) {
		return false;
	}
	if (check_only)
		return true;
	if (op)
		return true;

	if (ctrl_alt) {
		switch (key) {
		// Ctrl-Alt-Z => Zero the drive while trying to skip empty blocks
		case Qt::Key_Z:
			fast_zeroing = TRUE;
			onStart(true);
			break;
		// Ctrl-Alt-D => Toggle dark mode (Qt does not need a restart)
		case Qt::Key_D:
			WriteSetting32(SETTING_DARK_MODE,
				(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) ? 2 : 1);
			ApplyColorScheme();
			break;
		// Ctrl-Alt-E => Expert Mode
		case Qt::Key_E:
			expert_mode = !expert_mode;
			WriteSettingBool(SETTING_EXPERT_MODE, expert_mode);
			PrintStatusTimeout(lmprintf(MSG_347), expert_mode);
			break;
		// Ctrl-Alt-F => List non USB removable drives such as eSATA, etc - CAUTION!!!
		case Qt::Key_F:
			list_non_usb_removable_drives = !list_non_usb_removable_drives;
			if (list_non_usb_removable_drives) {
				previous_enable_HDDs_ = enable_HDDs;
				enable_HDDs = TRUE;
			} else {
				enable_HDDs = previous_enable_HDDs_;
			}
			filter_.enable_HDDs = enable_HDDs;
			filter_.list_non_usb_removable_drives = list_non_usb_removable_drives;
			PrintStatusTimeout(lmprintf(MSG_287), list_non_usb_removable_drives);
			uprintf("%sListing of non-USB removable drives %s",
				(list_non_usb_removable_drives) ? "CAUTION: " : "", (list_non_usb_removable_drives) ? "enabled" : "disabled");
			if (list_non_usb_removable_drives)
				uprintf("By using this unofficial cheat mode you forfeit ANY RIGHT to complain if you lose valuable data!");
			refreshDevices(false);
			break;
		// Ctrl-Alt-Y => Force update check to be successful and ignore timestamp errors
		case Qt::Key_Y:
			force_update = (force_update > 0) ? 0 : 2;
			PrintStatusTimeout(lmprintf(MSG_259), force_update);
			break;
		}
		return true;
	}

	switch (key) {
	// Alt-. => Enable USB enumeration debug
	case Qt::Key_Period:
		usb_debug = !usb_debug;
		cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
		WriteSettingBool(SETTING_ENABLE_USB_DEBUG, usb_debug);
		PrintStatusTimeout(lmprintf(MSG_270), usb_debug);
		refreshDevices(false);
		break;
	// Alt-, => Disable physical drive locking
	case Qt::Key_Comma:
		lock_drive = !lock_drive;
		PrintStatusTimeout(lmprintf(MSG_282), lock_drive);
		break;
	// Alt-A => Toggle use of Rufus MBR for Windows boot
	case Qt::Key_A:
		use_rufus_mbr = !use_rufus_mbr;
		WriteSettingBool(SETTING_DISABLE_RUFUS_MBR, !use_rufus_mbr);
		PrintStatusTimeout(lmprintf(MSG_349), use_rufus_mbr);
		break;
	// Alt-B => Toggle fake drive detection during bad blocks check
	case Qt::Key_B:
		detect_fakes = !detect_fakes;
		WriteSettingBool(SETTING_DISABLE_FAKE_DRIVES_CHECK, !detect_fakes);
		PrintStatusTimeout(lmprintf(MSG_256), detect_fakes);
		break;
	// Alt-C => Cycle USB port for currently selected device
	case Qt::Key_C:
		cyclePort(false);
		break;
	// Alt-D => Delete the subdirectory where Rufus keeps its files
	case Qt::Key_D:
		static_sprintf(path, "%s/%s", app_data_dir, FILES_DIR);
		PrintStatusDebug(STATUS_MSG_TIMEOUT, MSG_264, path);
		QDir(QString::fromUtf8(path)).removeRecursively();
		user_deleted_rufus_dir = TRUE;
		break;
	// Alt-E => Enhanced installation mode (allow dual UEFI/BIOS mode and FAT32 for Windows)
	case Qt::Key_E:
		allow_dual_uefi_bios = !allow_dual_uefi_bios;
		WriteSettingBool(SETTING_ENABLE_WIN_DUAL_EFI_BIOS, allow_dual_uefi_bios);
		PrintStatusTimeout(lmprintf(MSG_266), allow_dual_uefi_bios);
		rules_on_toggle_dual_uefi_bios(&m_);
		render();
		break;
	// Alt-F => Toggle detection of USB HDDs
	case Qt::Key_F:
		filter_.enable_HDDs = enable_HDDs = !enable_HDDs;
		PrintStatusTimeout(lmprintf(MSG_253), enable_HDDs);
		refreshDevices(false);
		break;
	// Alt-G => Toggle detection of Virtual Disks (the loop devices of files)
	case Qt::Key_G:
		filter_.enable_VHDs = enable_VHDs = !enable_VHDs;
		WriteSettingBool(SETTING_DISABLE_VHDS, !enable_VHDs);
		PrintStatusTimeout(lmprintf(MSG_308), enable_VHDs);
		refreshDevices(false);
		break;
	// Alt-H => Toggle computation of SHA512 digest
	case Qt::Key_H:
		enable_extra_hashes = !enable_extra_hashes;
		WriteSettingBool(SETTING_ENABLE_EXTRA_HASHES, enable_extra_hashes);
		PrintStatusTimeout(lmprintf(MSG_312), enable_extra_hashes);
		break;
	// Alt-I => Toggle ISO support (to force DD mode on an ISOHybrid)
	case Qt::Key_I:
		enable_iso = !enable_iso;
		PrintStatusTimeout(lmprintf(MSG_262), enable_iso);
		if (image_path != nullptr)
			startImageScan(true);
		break;
	// Alt-J => Toggle Joliet support for ISO9660 images
	case Qt::Key_J:
		enable_joliet = !enable_joliet;
		PrintStatusTimeout(lmprintf(MSG_257), enable_joliet);
		break;
	// Alt-K => Toggle Rock Ridge support for ISO9660 images
	case Qt::Key_K:
		enable_rockridge = !enable_rockridge;
		PrintStatusTimeout(lmprintf(MSG_258), enable_rockridge);
		break;
	// Alt-L => Force Large FAT32 format to be used on < 32 GB drives
	case Qt::Key_L:
		m_.force_large_fat32 = force_large_fat32 = !force_large_fat32;
		WriteSettingBool(SETTING_FORCE_LARGE_FAT32_FORMAT, force_large_fat32);
		PrintStatusTimeout(lmprintf(MSG_254), force_large_fat32);
		refreshDevices(false);
		break;
	// Alt-M => Toggle the check for the 0x55 0xAA boot marker at offset 0x1fe
	case Qt::Key_M:
		ignore_boot_marker = !ignore_boot_marker;
		WriteSettingBool(SETTING_IGNORE_BOOT_MARKER, ignore_boot_marker);
		PrintStatusTimeout(lmprintf(MSG_319), ignore_boot_marker);
		break;
	// Alt-N => Enable NTFS compression
	case Qt::Key_N:
		enable_ntfs_compression = !enable_ntfs_compression;
		PrintStatusTimeout(lmprintf(MSG_260), enable_ntfs_compression);
		break;
	// Alt-O => Save from Optical drive to ISO
	case Qt::Key_O:
		uprintf("Saving an optical disc to an ISO image is not available yet");
		ui_status(FALSE, STATUS_MSG_TIMEOUT, "Optical disc to ISO: not available yet");
		break;
	// Alt-P => Toggle GPT ESP to and from Basic Data type: Linux mounts an ESP as any partition
	case Qt::Key_P:
		uprintf("Alt-P (ESP to Basic Data) is not needed on Linux, which can mount an ESP");
		ui_status(FALSE, STATUS_MSG_TIMEOUT, "ESP toggle: not needed on Linux");
		break;
	// Alt-Q => Enable file indexing (for file systems that support it)
	case Qt::Key_Q:
		enable_file_indexing = !enable_file_indexing;
		WriteSettingBool(SETTING_ENABLE_FILE_INDEXING, enable_file_indexing);
		PrintStatusTimeout(lmprintf(MSG_290), enable_file_indexing);
		break;
	// Alt-R => Remove the settings (the registry keys on Windows)
	case Qt::Key_R:
		if ((ini_file != nullptr) && QFile::resize(QString::fromUtf8(ini_file), 0))
			PrintStatus(STATUS_MSG_TIMEOUT, MSG_248);
		else
			PrintStatus(STATUS_MSG_TIMEOUT, MSG_249);
		break;
	// Alt-S => Disable size limit for ISOs
	case Qt::Key_S:
		size_check = !size_check;
		PrintStatusTimeout(lmprintf(MSG_252), size_check);
		refreshDevices(false);
		break;
	// Alt-T => Preserve timestamps when extracting ISO files
	case Qt::Key_T:
		preserve_timestamps = !preserve_timestamps;
		WriteSettingBool(SETTING_PRESERVE_TIMESTAMPS, preserve_timestamps);
		PrintStatusTimeout(lmprintf(MSG_269), preserve_timestamps);
		break;
	// Alt-U => Use PROPER size units, instead of this whole Kibi/Gibi nonsense
	case Qt::Key_U:
		filter_.use_fake_units = use_fake_units = !use_fake_units;
		WriteSettingBool(SETTING_USE_PROPER_SIZE_UNITS, !use_fake_units);
		PrintStatusTimeout(lmprintf(MSG_263), !use_fake_units);
		refreshDevices(false);
		break;
	// Alt-V => Use VDS facilities for formatting, which Linux does not have
	case Qt::Key_V:
		PrintStatusTimeout("VDS", FALSE);
		break;
	// Alt-W => Enable VMWare disk detection
	case Qt::Key_W:
		filter_.enable_vmdk = enable_vmdk = !enable_vmdk;
		WriteSettingBool(SETTING_ENABLE_VMDK_DETECTION, enable_vmdk);
		PrintStatusTimeout(lmprintf(MSG_265), enable_vmdk);
		refreshDevices(false);
		break;
	// Alt-X => Delete the NoDriveTypeAutorun key on exit: Linux has no such key
	case Qt::Key_X:
		uprintf("Alt-X (NoDriveTypeAutorun) is not needed on Linux, where Rufus hides the drive from udisks");
		ui_status(FALSE, STATUS_MSG_TIMEOUT, "NoDriveTypeAutorun: not needed on Linux");
		break;
	// Alt-Y => Force the update check to be successful
	case Qt::Key_Y:
		force_update = (force_update > 0) ? 0 : 1;
		PrintStatusTimeout(lmprintf(MSG_259), force_update);
		break;
	// Alt-Z => Zero the drive
	case Qt::Key_Z:
		fast_zeroing = FALSE;
		onStart(true);
		break;
	}
	return true;
}

// Alt-C, or after a "device not ready" error: through the helper, in a thread (for the polkit prompt)
void MainWindow::cyclePort(bool after_error)
{
	const rufus_device_t* dev = selectedDevice();
	std::string devnode;
	uint64_t diskseq;

	if ((dev == nullptr) || cycling_)
		return;
	if (after_error)
		uprintf("Device not ready → Trying to cycle port...");
	devnode = dev->devnode;
	diskseq = dev->diskseq;
	cycling_ = true;
	render();
	std::thread([this, devnode, diskseq] {
		if ((session_helper.sock < 0) && !StartHelper(&session_helper, selected_locale_name()))
			uprintf("Could not start the helper");
		else
			HelperCyclePort(&session_helper, devnode.c_str(), diskseq);
		QMetaObject::invokeMethod(this, [this] {
			cycling_ = false;
			render();
		}, Qt::QueuedConnection);
	}).detach();
}

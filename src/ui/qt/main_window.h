/*
 * Rufus: The Reliable USB Formatting Utility
 * Main window (IDD_DIALOG)
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

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>

#include "progress_bar.h"
#include "taskbar.h"
#include "core.h"

class DeviceMonitor;
class LogWindow;

/* rufus-helper, which stays for the session once started (a single polkit prompt) */
extern void StopSessionHelper();

/*
 * The main window holds a rules_state_t, which describes what the Windows
 * dialog displays: each user action goes to the rules (rules_on_*()), and the
 * widgets are then rendered from the state. The widgets have the IDC_* or IDS_*
 * name of their Windows counterpart as their object name, for the
 * localization and the tests.
 */
class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(LogWindow* log, QWidget* parent = nullptr);
	~MainWindow() override;

	/* The ui_iface_t callbacks, on the thread of the window */
	void setStatus(bool info, unsigned int duration, const QString& text);
	void setProgress(void* dialog, uint32_t pos, bool taskbar);
	int ask(ui_question_t* question);
	void onEvent(enum ui_event_type type, intptr_t param);

	rules_state_t& rules() { return m_; }
	// An image given on the command line
	void selectImage(const QString& path) { onSelect(path); }
	// The window was closed to be created again, in another language
	bool relaunch() const { return relaunch_; }

protected:
	void closeEvent(QCloseEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	void createWidgets();
	void retranslate();
	void render();
	void renderCombo(QComboBox* combo, const rules_combo_t* rc, bool enabled);
	void renderCheck(QCheckBox* box, const rules_check_t* rc);
	void renderPersistence();
	void updateCsmHelp();

	/* The drive list (GetDevices()) */
	void refreshDevices(bool startup);
	const rufus_device_t* selectedDevice() const;
	void updateDeviceTooltip();
	void onDeviceSelected(int index);

	/* User actions */
	void onBootSelection(int index);
	void onPersistenceSizeEdited();
	void toggleAdvancedDevice();
	void toggleAdvancedFormat();
	void onSelect(const QString& provided = QString());
	void startImageScan(bool keep_image_name);
	void onHash();
	void onStart(bool zero_drive = false);
	void onClose();
	void showLanguageMenu();
	void PrintStatusTimeout(const char* str, BOOL val);

	/* The format operation */
	void onFormatStart(int r);
	bool startFormatThread();
	void onFormatCompleted(bool written);

	rules_state_t m_;
	rufus_device_t devs_[MAX_DRIVES];
	int nb_devs_ = 0;
	device_filter_t filter_ = {};
	bool queued_hotplug_event_ = false;
	DeviceMonitor* monitor_ = nullptr;
	LogWindow* log_ = nullptr;
	Taskbar taskbar_;

	/* The operation in progress */
	bool format_running_ = false;		// format_thread != NULL: a format or a hash
	bool zero_drive_ = false;
	bool no_confirmation_on_cancel_ = false;
	bool keep_image_name_ = false;
	rufus_device_t start_device_ = {};	// The drive of the operation
	QString last_image_dir_;
	bool relaunch_ = false;

	/* Status bar */
	QLabel* status_ = nullptr;
	QLabel* timer_label_ = nullptr;
	QString status_text_;
	QTimer status_timer_;
	QTimer clock_;
	int elapsed_ = 0;

	/* Section headers */
	QLabel* drive_properties_txt_ = nullptr;
	QLabel* format_options_txt_ = nullptr;
	QLabel* status_txt_ = nullptr;

	/* Drive properties */
	QLabel* device_txt_ = nullptr;
	QComboBox* device_ = nullptr;
	QToolButton* save_ = nullptr;
	QLabel* boot_txt_ = nullptr;
	QComboBox* boot_ = nullptr;
	QToolButton* hash_ = nullptr;
	QToolButton* select_ = nullptr;
	QAction* select_action_ = nullptr;
	QAction* download_action_ = nullptr;
	QWidget* image_option_row_ = nullptr;
	QLabel* image_option_txt_ = nullptr;
	QComboBox* image_option_ = nullptr;
	QSlider* persistence_slider_ = nullptr;
	QLineEdit* persistence_size_ = nullptr;
	QComboBox* persistence_units_ = nullptr;
	QComboBox* partition_ = nullptr;
	QComboBox* target_ = nullptr;
	QLabel* csm_help_ = nullptr;
	QToolButton* advanced_device_ = nullptr;
	QWidget* advanced_device_box_ = nullptr;
	QCheckBox* list_usb_hdd_ = nullptr;
	QCheckBox* old_bios_fixes_ = nullptr;
	QCheckBox* uefi_validation_ = nullptr;

	/* Format options */
	QLineEdit* label_ = nullptr;
	QComboBox* fs_ = nullptr;
	QComboBox* cluster_ = nullptr;
	QToolButton* advanced_format_ = nullptr;
	QWidget* advanced_format_box_ = nullptr;
	QCheckBox* quick_format_ = nullptr;
	QCheckBox* extended_label_ = nullptr;
	QCheckBox* bad_blocks_ = nullptr;
	QComboBox* nb_passes_ = nullptr;

	/* Status */
	ProgressBar* progress_ = nullptr;
	QToolButton* lang_ = nullptr;
	QToolButton* about_ = nullptr;
	QToolButton* settings_ = nullptr;
	QToolButton* log_button_ = nullptr;
	QPushButton* start_ = nullptr;
	QPushButton* close_ = nullptr;

	// The image option label, which persistence replaces (image_option_txt)
	QString image_option_text_;
	// SELECT or DOWNLOAD
	int select_index_ = 0;
	QString uppercase_select_[2], uppercase_close_, uppercase_cancel_;
};

/*
 * Rufus: The Reliable USB Formatting Utility
 * Qt 6 user interface entry point
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

#include <getopt.h>
#include <stdio.h>
#include <strings.h>

#include <QApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>

#include "version.h"
#include "app.h"
#include "log_window.h"
#include "main_window.h"
#include "ui_bridge.h"

extern "C" {
extern const char* FileSystemLabel[FS_MAX];
extern char embedded_sl_version_str[2][12], embedded_sl_version_ext[2][32];
}
#include "../res/grub/grub_version.h"
#include "../res/grub2/grub2_version.h"

static void PrintUsage(const char* appname)
{
	printf("\nUsage: %s [-x] [-h] [-v] [-f FILESYSTEM] [-i PATH] [-l LOCALE]\n", appname);
	printf("  -x, --extra-devs\n");
	printf("     List extra devices, such as USB HDDs\n");
	printf("  -i PATH, --iso=PATH\n");
	printf("     Select the ISO image pointed by PATH to be used on startup\n");
	printf("  -l LOCALE, --locale=LOCALE\n");
	printf("     Select the locale to be used on startup\n");
	printf("  -f FILESYSTEM, --filesystem=FILESYSTEM\n");
	printf("     Preselect the file system to be preferred when formatting\n");
	printf("  -v, --verbose\n");
	printf("     Copy the log to the standard error\n");
	printf("  -h, --help\n");
	printf("     This usage guide.\n");
}

int main(int argc, char* argv[])
{
	static const struct option long_options[] = {
		{ "extra-devs", no_argument, NULL, 'x' },
		{ "help", no_argument, NULL, 'h' },
		{ "iso", required_argument, NULL, 'i' },
		{ "locale", required_argument, NULL, 'l' },
		{ "filesystem", required_argument, NULL, 'f' },
		{ "verbose", no_argument, NULL, 'v' },
		{ NULL, 0, NULL, 0 }
	};
	QString provided_image;
	QByteArray locale_name;
	bool verbose = false;
	int opt, i;

	while ((opt = getopt_long(argc, argv, "hxvf:i:l:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'x':
			enable_HDDs = TRUE;
			break;
		case 'i':
			if (!QFileInfo(QString::fromLocal8Bit(optarg)).isFile()) {
				printf("Could not find image '%s'\n", optarg);
				break;
			}
			provided_image = QFileInfo(QString::fromLocal8Bit(optarg)).absoluteFilePath();
			break;
		case 'l':
			locale_name = optarg;
			break;
		case 'f':
			preselected_fs = FS_UNKNOWN;
			if ((optarg[0] >= '0') && (optarg[0] <= '9')) {
				preselected_fs = (int)strtol(optarg, NULL, 0);
			} else {
				for (i = 0; i < FS_MAX; i++) {
					if (strcasecmp(optarg, FileSystemLabel[i]) == 0)
						preselected_fs = i;
				}
			}
			if ((preselected_fs < FS_UNKNOWN) || (preselected_fs >= FS_MAX))
				preselected_fs = FS_UNKNOWN;
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			PrintUsage(argv[0]);
			return 0;
		default:
			PrintUsage(argv[0]);
			return 2;
		}
	}

	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral(APPLICATION_NAME));
	QApplication::setApplicationVersion(QStringLiteral(RUFUS_VERSION_STRING));
	QApplication::setOrganizationName(QStringLiteral(APPLICATION_NAME));
	// Under Wayland this becomes the xdg-shell app_id, which the compositor uses
	// to match the window with rufus.desktop (icon, taskbar progress, ...)
	QGuiApplication::setDesktopFileName(QStringLiteral("rufus"));
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/rufus-256.png")));

	// The log collects everything from now on
	LogWindow log;
	InstallUiBridge(&log, verbose);
	uprintf("*** " APPLICATION_NAME " init ***");
	InitDirectories(argv[0]);
	LoadSettings();
	if (locale_name.isEmpty())
		locale_name = ReadSettingStr(SETTING_LOCALE);
	if (!InitLocalization() || !SelectLocale(locale_name.constData())) {
		uprintf("FATAL: Could not access locale!");
		QMessageBox::critical(nullptr, QStringLiteral("Fatal error"),
			QStringLiteral("The locale data is missing or invalid. This application will now exit."));
		return 1;
	}
	ubprintf("Localization set to '%s'", selected_locale->txt[0]);
	WriteSettingStr(SETTING_LOCALE, selected_locale->txt[0]);
	QGuiApplication::setLayoutDirection(right_to_left_mode ? Qt::RightToLeft : Qt::LeftToRight);
	log.retranslate();

	uprintf(APPLICATION_NAME " v%d.%d.%d (Linux)", rufus_version[0], rufus_version[1], rufus_version[2]);
	SetEmbeddedSyslinuxVersions();
	uprintf("Syslinux versions: %s%s, %s%s", embedded_sl_version_str[0], embedded_sl_version_ext[0],
		embedded_sl_version_str[1], embedded_sl_version_ext[1]);
	uprintf("Grub versions: %s, %s", GRUB4DOS_VERSION, GRUB2_PACKAGE_VERSION);
	uprintf("Qt version: %s (%s)", qVersion(), QGuiApplication::platformName().toUtf8().constData());
	if (selected_locale->ctrl_id & LOC_NEEDS_UPDATE) {
		uprintf("NOTE: The %s translation requires an update, but the current translator hasn't submitted "
			"one. Because of this, some messages will only be displayed in English.", selected_locale->txt[1]);
		uprintf("If you think you can help update this translation, please e-mail the author of this application");
	}

	MainWindow window(&log);
	window.show();
	// Process the image of the command line, once the window is up
	if (!provided_image.isEmpty())
		QTimer::singleShot(0, &window, [&window, provided_image] { window.selectImage(provided_image); });

	// Test hook: a capture of the main window, after it settles (RUFUS_TEST_CAPTURE=file.png)
	if (const char* capture = getenv("RUFUS_TEST_CAPTURE")) {
		QString path = QString::fromUtf8(capture);
		QTimer::singleShot(1000, &window, [&window, path] {
			window.grab().save(path);
			if (getenv("RUFUS_TEST_QUIT") != nullptr)
				QApplication::quit();
		});
	}

	int r = app.exec();
	SetBridgeWindow(nullptr);
	uprintf("*** " APPLICATION_NAME " exit ***");
	exit_localization();
	safe_free(image_path);
	safe_free(archive_path);
	return r;
}

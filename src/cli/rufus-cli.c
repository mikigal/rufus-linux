/*
 * Rufus: The Reliable USB Formatting Utility
 * Command line interface
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

/*
 * rufus-cli drives librufus-core without a user interface: listing the
 * drives for now, and writing to them once the helper exists.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "ui_iface.h"
#include "devices.h"
#include "platform.h"
#include "localization.h"

static int verbose = 0;

static void cli_log(const char* text)
{
	if (verbose)
		fputs(text, stderr);
}

static void usage(FILE* fd)
{
	fprintf(fd, "Usage: rufus-cli [-v] <command> [options]\n\n"
		"Commands:\n"
		"  list [--hdd] [--non-usb] [--json]\n"
		"      List the drives that Rufus can write to. --hdd also lists USB hard drives,\n"
		"      and --non-usb non-USB removable drives (Alt-F and Ctrl-Alt-F in the GUI).\n\n"
		"Options:\n"
		"  -v, --verbose    Display the log on stderr (twice for the USB debug messages)\n"
		"  -h, --help       Display this help\n");
}

// Load the English messages, so that the names Rufus uses for devices are not "MSG_xxx"
static void load_localization(void)
{
	const char* loc_files[] = { getenv("RUFUS_LOC"), RUFUS_LOC_INSTALLED, RUFUS_LOC_SOURCE };
	size_t i;

	init_localization();
	for (i = 0; i < ARRAYSIZE(loc_files); i++) {
		if ((loc_files[i] == NULL) || (access(loc_files[i], R_OK) != 0))
			continue;
		if (get_supported_locales(loc_files[i]) &&
			get_loc_data_file(loc_files[i], get_locale_from_name("en-US", FALSE)))
			return;
	}
}

static void json_string(const char* str)
{
	const unsigned char* p;

	putchar('"');
	for (p = (const unsigned char*)str; *p != 0; p++) {
		if ((*p == '"') || (*p == '\\'))
			printf("\\%c", *p);
		else if (*p < 0x20)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

static int cmd_list(int argc, char** argv)
{
	static const struct option long_options[] = {
		{ "hdd", no_argument, NULL, 'H' },
		{ "non-usb", no_argument, NULL, 'N' },
		{ "json", no_argument, NULL, 'j' },
		{ NULL, 0, NULL, 0 }
	};
	static rufus_device_t devs[MAX_DRIVES];
	device_filter_t filter = { 0 };
	BOOL json = FALSE;
	char vid_pid[16];
	int c, i, n;

	optind = 1;
	while ((c = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
		switch (c) {
		case 'H':
			filter.enable_HDDs = TRUE;
			break;
		case 'N':
			filter.list_non_usb_removable_drives = TRUE;
			break;
		case 'j':
			json = TRUE;
			break;
		default:
			usage(stderr);
			return 2;
		}
	}

	n = GetDevices(devs, ARRAYSIZE(devs), &filter);
	if (n < 0) {
		fprintf(stderr, "Could not list the drives\n");
		return 1;
	}
	if (json)
		printf("[");
	for (i = 0; i < n; i++) {
		if (devs[i].props.vid >= 0 && devs[i].props.pid >= 0)
			snprintf(vid_pid, sizeof(vid_pid), "%04x:%04x", devs[i].props.vid, devs[i].props.pid);
		else
			vid_pid[0] = 0;
		if (json) {
			printf("%s\n  {\"device\": ", (i == 0) ? "" : ",");
			json_string(devs[i].devnode);
			printf(", \"diskseq\": %llu, \"size\": %llu, \"sector_size\": %u, \"name\": ",
				(unsigned long long)devs[i].diskseq, (unsigned long long)devs[i].size, devs[i].sector_size);
			json_string(devs[i].name);
			printf(", \"display_name\": ");
			json_string(devs[i].display_name);
			printf(", \"label\": ");
			json_string(devs[i].label);
			printf(", \"fs\": ");
			json_string(devs[i].fs_name);
			printf(", \"vid_pid\": ");
			json_string(vid_pid);
			printf(", \"usb\": %s, \"uas\": %s, \"card\": %s, \"removable\": %s}",
				devs[i].props.is_USB ? "true" : "false", devs[i].props.is_UASP ? "true" : "false",
				devs[i].props.is_CARD ? "true" : "false", devs[i].props.is_Removable ? "true" : "false");
		} else {
			printf("%-14s %-10s %-9s %s\n", devs[i].devnode, SizeToHumanReadable(devs[i].size, FALSE, FALSE),
				vid_pid, devs[i].display_name);
			printf("%-14s %s\n", "", devs[i].name);
		}
	}
	if (json)
		printf("%s]\n", (n == 0) ? "" : "\n");
	else if (n == 0)
		printf("No drive found\n");
	return 0;
}

int main(int argc, char** argv)
{
	static const struct option long_options[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	const ui_iface_t iface = { .log = cli_log };
	int c, r;

	// Stop at the command, whose options are its own
	while ((c = getopt_long(argc, argv, "+vh", long_options, NULL)) != -1) {
		switch (c) {
		case 'v':
			verbose++;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind >= argc) {
		usage(stderr);
		return 2;
	}
	usb_debug = (verbose > 1);
	ui_set_iface(&iface);
	load_localization();

	if (strcmp(argv[optind], "list") == 0) {
		r = cmd_list(argc - optind, &argv[optind]);
	} else {
		fprintf(stderr, "Unknown command '%s'\n", argv[optind]);
		usage(stderr);
		r = 2;
	}
	exit_localization();
	return r;
}

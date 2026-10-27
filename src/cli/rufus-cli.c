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
 * rufus-cli drives librufus-core without a user interface: it lists the
 * drives, and writes to them through rufus-helper, with the options set
 * through the rules of the main window (as the GUI would), the boot check and
 * its questions (asked on the terminal, or answered from the options).
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "rules.h"
#include "devices.h"
#include "image_scan.h"
#include "boot_check.h"
#include "platform.h"
#include "helper_client.h"
#include "resource.h"
#include "localization.h"

extern const char* FileSystemLabel[FS_MAX];

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
		"  write <device> [--image FILE] [options]\n"
		"      Write to a drive (through rufus-helper, which pkexec starts as root), with\n"
		"      the defaults of the main window for what the options leave out:\n"
		"      --boot TYPE        non-bootable, image (the default with --image), freedos, msdos,\n"
		"                         syslinux4, syslinux6, reactos, grub2, grub4dos or uefi-ntfs\n"
		"      --partition STYLE  mbr, gpt or sfd\n"
		"      --target SYSTEM    bios or uefi\n"
		"      --fs NAME          FAT, FAT32, NTFS, UDF, exFAT, ext2, ext3 or ext4\n"
		"      --cluster BYTES    Cluster size\n"
		"      --label TEXT       Volume label\n"
		"      --persistence MB   Persistent partition size, for Linux images that support it\n"
		"      --archive FILE     A zip archive to extract to the drive (FAT, NTFS, exFAT, UDF)\n"
		"      --slow             Full format (zero the volume) rather than a quick one\n"
		"      --no-extended-label  No autorun.inf label and icon\n"
		"      --old-bios-fixes   Add fixes for old BIOSes\n"
		"      --bad-blocks PASSES  Check for bad blocks first: 1, 2, slc, mlc or tlc\n"
		"      --iso, --dd        How to write an ISOHybrid image (asked otherwise)\n"
		"      --yes              Answer the other questions (and the confirmation) with\n"
		"                         their first choice\n"
		"      --hdd, --non-usb   As for list\n"
		"      The helper is $RUFUS_HELPER, or " RUFUS_HELPER_INSTALLED ".\n\n"
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

	// 0 (rather than 1) makes glibc start the scan of the command afresh
	optind = 0;
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

/*
 * The user interface of a write
 */
static struct {
	BOOL yes;						// --yes
	int image_mode;					// --iso (1) or --dd (2): the answer to the ISOHybrid question
	BOOL tty;						// The questions can be asked on the terminal
	int last_percent;
	char progress_text[256];
} cli;

static void cli_status(BOOL info, unsigned int duration, const char* text)
{
	if (info) {
		static_strcpy(cli.progress_text, (text == NULL) ? "" : text);
		return;
	}
	if ((text != NULL) && (text[0] != 0) && (duration == 0))
		fprintf(stderr, "%s%s\n", cli.tty ? "\r\033[K" : "", text);
}

static void cli_progress(void* dialog, uint32_t pos, BOOL taskbar)
{
	int percent = (int)((uint64_t)pos * 100 / MAX_PROGRESS);

	if ((dialog != NULL) || (percent == cli.last_percent))
		return;
	cli.last_percent = percent;
	if (cli.tty)
		fprintf(stderr, "\r\033[K%3d%% %s", percent, cli.progress_text);
	else if (percent % 10 == 0)
		fprintf(stderr, "%3d%% %s\n", percent, cli.progress_text);
}

static void cli_event(enum ui_event_type type, intptr_t param)
{
}

// Read an answer on the terminal
static BOOL read_answer(char* buf, size_t size)
{
	fflush(stderr);
	if (fgets(buf, (int)size, stdin) == NULL)
		return FALSE;
	buf[strcspn(buf, "\r\n")] = 0;
	return TRUE;
}

static int ask_notification(ui_question_t* q)
{
	static const struct { int types; int id; const char* name; } buttons[] = {
		{ 1 << MB_OK | 1 << MB_OKCANCEL, IDOK, "ok" },
		{ 1 << MB_YESNOCANCEL | 1 << MB_YESNO, IDYES, "yes" },
		{ 1 << MB_YESNOCANCEL | 1 << MB_YESNO, IDNO, "no" },
		{ 1 << MB_ABORTRETRYIGNORE, IDABORT, "abort" },
		{ 1 << MB_ABORTRETRYIGNORE | 1 << MB_RETRYCANCEL, IDRETRY, "retry" },
		{ 1 << MB_ABORTRETRYIGNORE, IDIGNORE, "ignore" },
		{ 1 << MB_OKCANCEL | 1 << MB_YESNOCANCEL | 1 << MB_RETRYCANCEL, IDCANCEL, "cancel" },
		{ 1 << MB_CLOSE, IDCLOSE, "close" },
	};
	int type = q->notification.flags & MB_TYPEMASK, i, n = 0, first = -1, last = -1;
	char line[64];

	for (i = 0; i < ARRAYSIZE(buttons); i++) {
		if (!(buttons[i].types & (1 << type)))
			continue;
		if (first < 0)
			first = i;
		last = i;
		n++;
	}
	if (first < 0)
		return IDOK;
	if (n == 1)
		return buttons[first].id;
	if (cli.yes) {
		fprintf(stderr, "-> %s\n", buttons[first].name);
		return buttons[first].id;
	}
	while (cli.tty) {
		fprintf(stderr, "[");
		for (i = first; i <= last; i++) {
			if (buttons[i].types & (1 << type))
				fprintf(stderr, "%s%s", (i == first) ? "" : "/", buttons[i].name);
		}
		fprintf(stderr, "]? ");
		if (!read_answer(line, sizeof(line)))
			break;
		for (i = first; i <= last; i++) {
			if ((buttons[i].types & (1 << type)) && (line[0] != 0) &&
				(strncasecmp(line, buttons[i].name, strlen(line)) == 0))
				return buttons[i].id;
		}
	}
	fprintf(stderr, "-> %s (no answer)\n", buttons[last].name);
	return buttons[last].id;
}

static int ask_selection(ui_question_t* q)
{
	selection_dialog_options_t* sel = q->selection;
	BOOL checkboxes = (sel->style == BS_AUTOCHECKBOX);
	char line[64], *p, *end;
	int i, mask;
	long v;

	for (i = 0; i < (int)sel->choices.Index; i++)
		fprintf(stderr, "  %d. %s\n", i + 1, sel->choices.String[i]);
	if ((cli.image_mode != 0) && (strstr(q->title, "ISOHybrid") != NULL)) {
		fprintf(stderr, "-> %d\n", cli.image_mode);
		return cli.image_mode;
	}
	if (cli.yes) {
		fprintf(stderr, "-> %s\n", checkboxes ? "the default" : "1");
		return checkboxes ? sel->mask : 1;
	}
	while (cli.tty) {
		fprintf(stderr, checkboxes ? "Choices (e.g. 1,3)? " : "Choice? ");
		if (!read_answer(line, sizeof(line)))
			break;
		for (mask = 0, p = line; *p != 0; p = end) {
			v = strtol(p, &end, 10);
			if ((end == p) || (v < 1) || (v > (long)sel->choices.Index))
				break;
			mask |= 1 << (v - 1);
			end += strspn(end, ", ");
		}
		if ((*p == 0) && (mask != 0) && (checkboxes || ((mask & (mask - 1)) == 0)))
			return mask;
	}
	fprintf(stderr, "-> cancel (no answer)\n");
	return -1;
}

static int cli_ask(ui_question_t* q)
{
	int i;

	fprintf(stderr, "%s\n%s: %s\n", cli.tty ? "\r\033[K" : "", q->title, q->message);
	switch (q->type) {
	case UI_QUESTION_NOTIFICATION:
		return ask_notification(q);
	case UI_QUESTION_SELECTION:
		return ask_selection(q);
	case UI_QUESTION_LIST:
		for (i = 0; i < q->list.size; i++)
			fprintf(stderr, "  %s\n", q->list.items[i]);
		return 0;
	}
	return -1;
}

static void on_interrupt(int sig)
{
	// The helper gets told to cancel, and a second interrupt kills us
	CancelOperation();
	signal(sig, SIG_DFL);
}

static int find_name(const char* name, const char* const* names, const int* values, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (strcasecmp(name, names[i]) == 0)
			return values[i];
	}
	return -1;
}

// Select the entry of a dropdown that has this item data
static BOOL select_entry(rules_state_t* m, rules_combo_t* combo, int data, void (*on_select)(rules_state_t*, int))
{
	int i;

	for (i = 0; i < combo->count; i++) {
		if (combo->entry[i].data == data) {
			if (combo->sel != i)
				on_select(m, i);
			return TRUE;
		}
	}
	return FALSE;
}

static int cmd_write(int argc, char** argv)
{
	static const struct option long_options[] = {
		{ "image", required_argument, NULL, 'i' },
		{ "boot", required_argument, NULL, 'b' },
		{ "partition", required_argument, NULL, 'p' },
		{ "target", required_argument, NULL, 't' },
		{ "fs", required_argument, NULL, 'f' },
		{ "cluster", required_argument, NULL, 'c' },
		{ "label", required_argument, NULL, 'l' },
		{ "persistence", required_argument, NULL, 'P' },
		{ "archive", required_argument, NULL, 'a' },
		{ "slow", no_argument, NULL, 's' },
		{ "no-extended-label", no_argument, NULL, 'E' },
		{ "old-bios-fixes", no_argument, NULL, 'O' },
		{ "bad-blocks", required_argument, NULL, 'B' },
		{ "iso", no_argument, NULL, 'I' },
		{ "dd", no_argument, NULL, 'D' },
		{ "yes", no_argument, NULL, 'y' },
		{ "hdd", no_argument, NULL, 'H' },
		{ "non-usb", no_argument, NULL, 'N' },
		{ NULL, 0, NULL, 0 }
	};
	static const char* boot_names[] = { "non-bootable", "image", "msdos", "freedos", "syslinux4", "syslinux6",
		"reactos", "grub2", "grub4dos", "uefi-ntfs" };
	static const int boot_values[] = { BT_NON_BOOTABLE, BT_IMAGE, BT_MSDOS, BT_FREEDOS, BT_SYSLINUX_V4,
		BT_SYSLINUX_V6, BT_REACTOS, BT_GRUB2, BT_GRUB4DOS, BT_UEFI_NTFS };
	static const char* partition_names[] = { "mbr", "gpt", "sfd" };
	static const int partition_values[] = { PARTITION_STYLE_MBR, PARTITION_STYLE_GPT, PARTITION_STYLE_SFD };
	static const char* target_names[] = { "bios", "uefi" };
	static const int target_values[] = { TT_BIOS, TT_UEFI };
	static const char* passes_names[] = { "1", "2", "slc", "mlc", "tlc" };
	static const int passes_values[] = { 0, 1, 2, 3, 4 };
	static rufus_device_t devs[MAX_DRIVES];
	static rules_state_t m;
	const ui_iface_t iface = { .log = cli_log, .status = cli_status, .progress = cli_progress,
		.ask = cli_ask, .event = cli_event };
	device_filter_t filter = { 0 };
	const char *image = NULL, *archive = NULL, *label = NULL, *home;
	int c, i, n, boot = -1, partition = -1, target = -1, fs = -1, passes = -1, image_fd = -1, archive_fd = -1;
	int bb_log_fd = -1;
	int option_index = 0;
	long cluster = -1, persistence = -1;
	BOOL slow = FALSE, no_extended_label = FALSE, old_bios_fixes = FALSE, ret = FALSE;
	char bb_log_path[MAX_PATH] = "", date[32], *end;
	rufus_device_t* dev = NULL;
	rules_drive_t drive;
	bootcheck_drive_t bcd;
	helper_job_t job;
	helper_t helper;
	struct stat st;
	struct tm lt;
	time_t now;

	// 0 (rather than 1) makes glibc start the scan of the command afresh
	optind = 0;
	while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
		switch (c) {
		case 'i': image = optarg; break;
		case 'b':
			boot = find_name(optarg, boot_names, boot_values, ARRAYSIZE(boot_names));
			if (boot < 0) goto bad_value;
			break;
		case 'p':
			partition = find_name(optarg, partition_names, partition_values, ARRAYSIZE(partition_names));
			if (partition < 0) goto bad_value;
			break;
		case 't':
			target = find_name(optarg, target_names, target_values, ARRAYSIZE(target_names));
			if (target < 0) goto bad_value;
			break;
		case 'f':
			for (fs = FS_MAX - 1; (fs >= 0) && (strcasecmp(optarg, FileSystemLabel[fs]) != 0); fs--);
			if (fs < 0) goto bad_value;
			break;
		case 'c':
			cluster = strtol(optarg, &end, 10);
			if ((*end != 0) || (cluster <= 0)) goto bad_value;
			break;
		case 'l': label = optarg; break;
		case 'a': archive = optarg; break;
		case 'P':
			persistence = strtol(optarg, &end, 10);
			if ((*end != 0) || (persistence < 0)) goto bad_value;
			break;
		case 's': slow = TRUE; break;
		case 'E': no_extended_label = TRUE; break;
		case 'O': old_bios_fixes = TRUE; break;
		case 'B':
			passes = find_name(optarg, passes_names, passes_values, ARRAYSIZE(passes_names));
			if (passes < 0) goto bad_value;
			break;
		case 'I': cli.image_mode = 1; break;
		case 'D': cli.image_mode = 2; break;
		case 'y': cli.yes = TRUE; break;
		case 'H': filter.enable_HDDs = TRUE; break;
		case 'N': filter.list_non_usb_removable_drives = TRUE; break;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind != argc - 1) {
		usage(stderr);
		return 2;
	}
	cli.tty = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
	cli.last_percent = -1;
	ui_set_iface(&iface);

	// The drive, as Rufus lists it
	n = GetDevices(devs, ARRAYSIZE(devs), &filter);
	for (i = 0; i < n; i++) {
		if (strcmp(devs[i].devnode, argv[optind]) == 0)
			dev = &devs[i];
	}
	if (dev == NULL) {
		fprintf(stderr, "'%s' is not a drive that Rufus can write to (see 'rufus-cli list')\n", argv[optind]);
		return 1;
	}

	// The main window, as the user would set it
	rules_init(&m);
	m.advanced_mode_device = TRUE;
	m.advanced_mode_format = TRUE;
	static_strcpy(m.no_label_msg, lmprintf(MSG_207));
	GetRulesDrive(dev, &drive);
	rules_on_startup(&m, &drive);
	if (image != NULL) {
		image_fd = open(image, O_RDONLY | O_CLOEXEC);
		if ((image_fd < 0) || (fstat(image_fd, &st) != 0) || !S_ISREG(st.st_mode)) {
			fprintf(stderr, "Could not open '%s': %s\n", image, (image_fd < 0) ? strerror(errno) : "not a file");
			goto out;
		}
		image_path = strdup(image);
		rules_on_image_scan_start(&m);
		i = ImageScan(FALSE);
		rules_on_image_scan_end(&m, &img_report, i, FALSE);
		if (i != RULES_SCAN_OK) {
			fprintf(stderr, "'%s' is not an image that Rufus can write\n", image);
			goto out;
		}
		if (boot < 0)
			boot = BT_IMAGE;
	} else if ((boot < 0) || (boot == BT_IMAGE)) {
		if (boot == BT_IMAGE) {
			fprintf(stderr, "--boot image needs an --image\n");
			goto out;
		}
		boot = BT_NON_BOOTABLE;
	}
	if (!select_entry(&m, &m.boot, boot, rules_on_boot_selection)) {
		fprintf(stderr, "This boot selection is not available\n");
		goto out;
	}
	if ((partition >= 0) && !select_entry(&m, &m.partition, partition, rules_on_partition_scheme)) {
		fprintf(stderr, "This partition scheme is not available for this boot selection\n");
		goto out;
	}
	if ((target >= 0) && !select_entry(&m, &m.target, target, rules_on_target_system)) {
		fprintf(stderr, "This target system is not available for this partition scheme\n");
		goto out;
	}
	if ((fs >= 0) && !select_entry(&m, &m.fs, fs, rules_on_file_system)) {
		fprintf(stderr, "This file system is not available for this boot selection\n");
		goto out;
	}
	if ((cluster > 0) && !select_entry(&m, &m.cluster, (int)cluster, rules_on_cluster_size)) {
		fprintf(stderr, "This cluster size is not available for this file system\n");
		goto out;
	}
	if (label != NULL)
		rules_on_label(&m, label);
	if (persistence >= 0) {
		if (!(m.image_options & IMOP_PERSISTENCE)) {
			fprintf(stderr, "This image does not support persistence\n");
			goto out;
		}
		rules_on_persistence_size(&m, (uint64_t)persistence * MB);
	}
	if (slow && m.quick_format.checked)
		rules_on_check(&m, &m.quick_format);
	if (no_extended_label && m.extended_label.checked)
		rules_on_check(&m, &m.extended_label);
	if (old_bios_fixes && !m.old_bios_fixes.checked)
		rules_on_check(&m, &m.old_bios_fixes);
	if (passes >= 0) {
		if (!m.bad_blocks.checked)
			rules_on_check(&m, &m.bad_blocks);
		rules_on_nb_passes(&m, passes);
	}
	if (!m.start_enabled) {
		fprintf(stderr, "These options can't be used to write to the drive\n");
		goto out;
	}

	// START
	rules_on_start(&m);
	rules_get_options(&m, &opts);
	opts.disk_size = dev->size;
	bcd.path = dev->devnode;
	bcd.is_fixed = drive.is_fixed;
	if (BootCheck(&bcd) != BOOTCHECK_PROCEED) {
		fprintf(stderr, "Cancelled\n");
		goto out;
	}
	if (Notification(MB_OKCANCEL | MB_ICONWARNING, APPLICATION_NAME, lmprintf(MSG_003, dev->display_name)) != IDOK) {
		fprintf(stderr, "Cancelled\n");
		goto out;
	}
	if (archive != NULL) {
		archive_fd = open(archive, O_RDONLY | O_CLOEXEC);
		if (archive_fd < 0) {
			fprintf(stderr, "Could not open '%s': %s\n", archive, strerror(errno));
			goto out;
		}
	}
	if (opts.bad_blocks) {
		home = getenv("HOME");
		now = time(NULL);
		localtime_r(&now, &lt);
		strftime(date, sizeof(date), "%Y%m%d_%H%M%S", &lt);
		static_sprintf(bb_log_path, "%s/rufus_%s.log", (home != NULL) ? home : ".", date);
		bb_log_fd = open(bb_log_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (bb_log_fd < 0)
			fprintf(stderr, "Could not create '%s': %s\n", bb_log_path, strerror(errno));
	}

	if (!StartHelper(&helper, "en-US")) {
		fprintf(stderr, "Could not start the helper\n");
		goto out;
	}
	signal(SIGINT, on_interrupt);
	signal(SIGTERM, on_interrupt);
	job.devnode = dev->devnode;
	job.diskseq = dev->diskseq;
	job.image_fd = image_fd;
	job.image_name = image;
	job.archive_fd = archive_fd;
	job.bb_log_fd = bb_log_fd;
	job.bb_log_path = bb_log_path;
	ret = HelperFormat(&helper, &job);
	StopHelper(&helper);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	if (cli.tty)
		fprintf(stderr, "\r\033[K");
	if (ret)
		fprintf(stderr, "Done\n");
	else
		fprintf(stderr, "Failed: %s%s\n", StrError(ErrorStatus, TRUE), verbose ? "" : " (-v displays the log)");

out:
	if (bb_log_fd >= 0) {
		// No bad blocks: no report
		if ((fstat(bb_log_fd, &st) == 0) && (st.st_size == 0))
			unlink(bb_log_path);
		close(bb_log_fd);
	}
	if (image_fd >= 0)
		close(image_fd);
	if (archive_fd >= 0)
		close(archive_fd);
	safe_free(image_path);
	return ret ? 0 : 1;

bad_value:
	fprintf(stderr, "Invalid value '%s' for --%s\n", optarg, long_options[option_index].name);
	return 2;
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
	} else if (strcmp(argv[optind], "write") == 0) {
		r = cmd_write(argc - optind, &argv[optind]);
	} else {
		fprintf(stderr, "Unknown command '%s'\n", argv[optind]);
		usage(stderr);
		r = 2;
	}
	exit_localization();
	return r;
}

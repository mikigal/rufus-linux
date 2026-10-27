/*
 * Rufus: The Reliable USB Formatting Utility
 * Tests of rufus-helper, as root in a VM
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
 * This test erases a USB stick: tests/e2e/test_cli.py runs it as root in a VM,
 * with an emulated one. It refuses to run without RUFUS_TEST_VM=1, and only
 * touches a QEMU stick (VID 46f4). $RUFUS_HELPER is the helper to test.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "devices.h"
#include "platform.h"
#include "helper_client.h"
#include "ipc.h"
#include "test.h"
#include "../res/grub/grub_version.h"

#define QEMU_VID                    0x46f4

static rufus_device_t stick;
static helper_t helper;
static char log_buf[256 * 1024];
static enum { ON_LOG_NOTHING, ON_LOG_CANCEL, ON_LOG_DISCONNECT } on_bad_blocks;

static void test_log(const char* text)
{
	fputs(text, stderr);
	safe_strcat(log_buf, sizeof(log_buf), text);
	if ((on_bad_blocks != ON_LOG_NOTHING) && (strstr(text, "Bad Blocks: Writing test pattern") != NULL)) {
		if (on_bad_blocks == ON_LOG_CANCEL)
			CancelOperation();
		else
			shutdown(helper.sock, SHUT_RDWR);
		on_bad_blocks = ON_LOG_NOTHING;
	}
}

static BOOL sh(const char* cmd)
{
	const char* argv[] = { "sh", "-c", cmd, NULL };

	return (RunCommand(argv, NULL, TRUE) == 0);
}

static BOOL find_stick(void)
{
	static rufus_device_t devs[MAX_DRIVES];
	device_filter_t filter = { .enable_HDDs = TRUE };
	int i, n = GetDevices(devs, ARRAYSIZE(devs), &filter);

	for (i = 0; i < n; i++) {
		if (devs[i].props.vid == QEMU_VID) {
			stick = devs[i];
			return TRUE;
		}
	}
	fprintf(stderr, "No QEMU USB stick found\n");
	return FALSE;
}

static void reset(int boot_type)
{
	ResetOptions(&opts);
	opts.boot_type = boot_type;
	opts.partition_type = PARTITION_STYLE_MBR;
	opts.target_type = TT_BIOS;
	opts.fs_type = FS_FAT32;
	opts.quick_format = TRUE;
	opts.disk_size = stick.size;
	static_strcpy(opts.label, "HELPER");
	log_buf[0] = 0;
}

static BOOL format(const char* devnode, uint64_t diskseq)
{
	helper_job_t job = { .devnode = devnode, .diskseq = diskseq, .image_fd = -1, .archive_fd = -1, .bb_log_fd = -1 };

	return HelperFormat(&helper, &job);
}

static void start(void)
{
	CHECK(StartHelper(&helper, "en-US"));
}

// Nothing of the operation is left on the host
static void check_clean(void)
{
	CHECK(sh("! ls /run/udev/rules.d/ 2>/dev/null | grep -i rufus"));
	CHECK(sh("! grep -q /run/rufus /proc/self/mountinfo"));
	CHECK(sh("! grep -q '^/dev/sd' /proc/mounts"));
	// And udisks no longer ignores the drive
	CHECK(sh("! udevadm info --query=property /dev/sda | grep -q UDISKS_IGNORE"));
}

static void test_refused(void)
{
	char loop[64] = "", cmd[256];
	FILE* f;

	start();
	// Not a device node
	reset(BT_NON_BOOTABLE);
	CHECK(!format("/dev/../dev/sda", stick.diskseq));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_INVALID_PARAMETER);
	// Not a drive that Rufus lists
	CHECK(sh("modprobe loop; truncate -s 64M /tmp/helper.img && losetup -f --show /tmp/helper.img > /tmp/loop"));
	f = fopen("/tmp/loop", "r");
	CHECK((f != NULL) && (fgets(loop, sizeof(loop), f) != NULL));
	if (f != NULL)
		fclose(f);
	loop[strcspn(loop, "\n")] = 0;
	opts.disk_size = 64 * MB;
	CHECK(!format(loop, 0));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_ACCESS_DENIED);
	CHECK_CONTAINS(log_buf, "is not a drive that Rufus can write to");
	snprintf(cmd, sizeof(cmd), "losetup -d %s", loop);
	IGNORE_RETVAL(sh(cmd));
	// Not the drive that was selected
	reset(BT_NON_BOOTABLE);
	CHECK(!format(stick.devnode, stick.diskseq + 1));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_ACCESS_DENIED);
	CHECK_CONTAINS(log_buf, "is no longer the drive that was selected");
	opts.disk_size = stick.size / 2;
	CHECK(!format(stick.devnode, stick.diskseq));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_ACCESS_DENIED);
	// Options out of range
	reset(BT_MAX);
	CHECK(!format(stick.devnode, stick.diskseq));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_INVALID_PARAMETER);
	// An image without its file
	reset(BT_IMAGE);
	CHECK(!format(stick.devnode, stick.diskseq));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_INVALID_PARAMETER);
	CHECK_CONTAINS(log_buf, "No image to write");
	// A boot file that the format does not use
	ipc_msg_t* m = malloc(sizeof(ipc_msg_t));
	int fd = open("/proc/self/exe", O_RDONLY);
	CHECK((m != NULL) && (fd >= 0));
	ipc_init(m, IPC_FILE);
	ipc_put_str(m, "grub4dos-" GRUB4DOS_VERSION "/grldr");
	ipc_put_fd(m, fd);
	CHECK(ipc_send(helper.sock, m));
	reset(BT_NON_BOOTABLE);
	CHECK(!format(stick.devnode, stick.diskseq));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_INVALID_PARAMETER);
	CHECK_CONTAINS(log_buf, "Unexpected boot file 'grub4dos-");
	// A file name that goes elsewhere: the helper hangs up
	ipc_init(m, IPC_FILE);
	ipc_put_str(m, "../../etc/passwd");
	ipc_put_fd(m, fd);
	CHECK(ipc_send(helper.sock, m));
	CHECK(!format(stick.devnode, stick.diskseq));
	CHECK(helper.sock == -1);
	close(fd);
	free(m);
	check_clean();
}

static void test_format(void)
{
	char cmd[256], dir[] = "/tmp/rufus-appdata-XXXXXX", path[MAX_PATH];

	start();
	// A boot file of the front end ends up on the drive
	CHECK(mkdtemp(dir) != NULL);
	static_strcpy(app_data_dir, dir);
	snprintf(path, sizeof(path), "mkdir -p %s/%s/grub4dos-%s && echo 'GRLDR OF THE FRONT END' > %s/%s/grub4dos-%s/grldr",
		dir, FILES_DIR, GRUB4DOS_VERSION, dir, FILES_DIR, GRUB4DOS_VERSION);
	CHECK(sh(path));
	reset(BT_GRUB4DOS);
	CHECK(format(stick.devnode, stick.diskseq));
	CHECK(ErrorStatus == 0);
	CHECK_CONTAINS(log_buf, "Helper: writing to ");
	CHECK_CONTAINS(log_buf, "Installing: /run/rufus/");
	snprintf(cmd, sizeof(cmd), "udevadm settle; mtype -i %s1 ::/grldr | grep -qx 'GRLDR OF THE FRONT END'", stick.devnode);
	CHECK(sh(cmd));
	// The helper keeps its files to itself
	CHECK(sh("! ls -d /run/rufus/data-* 2>/dev/null"));
	check_clean();

	// The same helper can be used again
	reset(BT_NON_BOOTABLE);
	opts.fs_type = FS_EXT4;
	CHECK(format(stick.devnode, stick.diskseq));
	snprintf(cmd, sizeof(cmd), "udevadm settle; e2fsck -fn %s1 && blkid -p -o value -s LABEL %s1 | grep -qx HELPER",
		stick.devnode, stick.devnode);
	CHECK(sh(cmd));
	StopHelper(&helper);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	IGNORE_RETVAL(sh(cmd));
	static_strcpy(app_data_dir, "");
}

static void test_cancel(void)
{
	// Through IPC_CANCEL, during the bad blocks check: the helper stays usable
	start();
	reset(BT_NON_BOOTABLE);
	opts.bad_blocks = TRUE;
	opts.nb_passes_sel = 4;
	on_bad_blocks = ON_LOG_CANCEL;
	CHECK(!format(stick.devnode, stick.diskseq));
	CHECK(SCODE_CODE(ErrorStatus) == ERROR_CANCELLED);
	CHECK(helper.sock >= 0);
	// Or RunCommand() would not run anything
	ErrorStatus = 0;
	check_clean();
	reset(BT_NON_BOOTABLE);
	CHECK(format(stick.devnode, stick.diskseq));

	// The front end going away
	reset(BT_NON_BOOTABLE);
	opts.bad_blocks = TRUE;
	opts.nb_passes_sel = 4;
	on_bad_blocks = ON_LOG_DISCONNECT;
	CHECK(!format(stick.devnode, stick.diskseq));
	CHECK(helper.sock == -1);
	// HelperFormat() waited for the helper, which waited for its worker
	ErrorStatus = 0;
	check_clean();
	CHECK(sh("! pgrep -x rufus-helper"));
}

int main(int argc, char** argv)
{
	const ui_iface_t iface = { .log = test_log };

	if ((getenv("RUFUS_TEST_VM") == NULL) || (getuid() != 0) || (getenv("RUFUS_HELPER") == NULL)) {
		fprintf(stderr, "This test erases a USB stick, and only runs as root in the VM of tests/e2e\n");
		return 77;
	}
	ui_set_iface(&iface);
	if (!find_stick())
		return 1;
	if ((argc < 2) || (strcmp(argv[1], "refused") == 0))
		test_refused();
	if ((argc < 2) || (strcmp(argv[1], "format") == 0))
		test_format();
	if ((argc < 2) || (strcmp(argv[1], "cancel") == 0))
		test_cancel();
	StopHelper(&helper);
	if (test_failures == 0)
		printf("HELPER TESTS PASSED\n");
	return TEST_RESULT();
}

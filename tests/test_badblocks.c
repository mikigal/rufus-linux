/*
 * Rufus: The Reliable USB Formatting Utility
 * Tests of the bad blocks check
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
 * The check of a clean image file runs anywhere. With RUFUS_TEST_VM=1, as root
 * (in the VM of tests/e2e/test_disk.py), it also checks device-mapper devices
 * on a loop device: one with failing sectors, and a fake one, twice the size
 * of its storage, with its second half mapped onto the first.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "badblocks.h"
#include "test_files.h"

#define TEST_DM_BAD                 "rufus-test-bad"
#define TEST_DM_FAKE                "rufus-test-fake"
#define MB_SECTORS                  2048

static BOOL sh(const char* cmd)
{
	const char* argv[] = { "sh", "-c", cmd, NULL };

	return (RunCommand(argv, NULL, TRUE) == 0);
}

static BOOL check(const char* path, int flags, int sel, badblocks_report* report)
{
	FILE* log_fd = fopen(tmp_path("report.log"), "w+");
	int fd = open(path, O_RDWR | O_CLOEXEC | flags);
	off_t size;
	BOOL r;

	CHECK(log_fd != NULL);
	CHECK(fd >= 0);
	size = lseek(fd, 0, SEEK_END);
	ResetOptions(&opts);
	ErrorStatus = 0;
	r = BadBlocks(fd, (uint64_t)size, (sel >= 2) ? 4 : sel + 1, sel, report, log_fd);
	close(fd);
	fclose(log_fd);
	return r;
}

static void test_clean_file(void)
{
	badblocks_report report;
	int fd = open(tmp_path("clean.img"), O_RDWR | O_CREAT | O_TRUNC, 0644);

	CHECK(fd >= 0);
	CHECK(ftruncate(fd, 4 * MB) == 0);
	close(fd);
	CHECK(check(tmp_path("clean.img"), 0, 1, &report));
	CHECK(report.bb_count == 0);
	CHECK(report.num_read_errors + report.num_write_errors + report.num_corruption_errors == 0);
	CHECK_CONTAINS(log_buf, "Bad Blocks: Checking from block 0 to 7");
}

static void test_cancel(void)
{
	badblocks_report report;
	FILE* log_fd = fopen(tmp_path("report.log"), "w+");
	int fd = open(tmp_path("clean.img"), O_RDWR);

	CHECK((fd >= 0) && (log_fd != NULL));
	ResetOptions(&opts);
	// A cancelled check (ErrorStatus set by CancelOperation()) fails without bad blocks
	ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
	CHECK(!BadBlocks(fd, 4 * MB, 1, 0, &report, log_fd));
	CHECK(report.bb_count == 0);
	ErrorStatus = 0;
	close(fd);
	fclose(log_fd);
}

static void test_devices(void)
{
	char cmd[512], loop[64] = "";
	badblocks_report report;
	FILE* f;
	int fd;

	fd = open(tmp_path("backing.img"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(ftruncate(fd, 16 * MB) == 0);
	close(fd);
	snprintf(cmd, sizeof(cmd), "losetup -f --show %s > %s", tmp_path("backing.img"), tmp_path("loop"));
	CHECK(sh(cmd));
	f = fopen(tmp_path("loop"), "r");
	CHECK((f != NULL) && (fgets(loop, sizeof(loop), f) != NULL));
	if (f != NULL)
		fclose(f);
	loop[strcspn(loop, "\n")] = 0;

	// 16 MB with 1 MB of failing sectors at 4 MB: blocks 8 and 9 of 512 KB
	snprintf(cmd, sizeof(cmd), "printf '0 %d linear %s 0\\n%d %d error\\n%d %d linear %s %d\\n' | dmsetup create " TEST_DM_BAD,
		4 * MB_SECTORS, loop, 4 * MB_SECTORS, MB_SECTORS, 5 * MB_SECTORS, 11 * MB_SECTORS, loop, 5 * MB_SECTORS);
	CHECK(sh(cmd));
	CHECK(sh("udevadm settle"));
	CHECK(check("/dev/mapper/" TEST_DM_BAD, O_DIRECT, 0, &report));
	CHECK(report.bb_count == 2);
	CHECK(report.num_write_errors == 2);
	CHECK(report.num_read_errors == 0);
	CHECK_CONTAINS(file_content(tmp_path("report.log")), "Block 8: write error\nBlock 9: write error\n");

	// 32 MB, of which the second half is the first one again
	snprintf(cmd, sizeof(cmd), "printf '0 %d linear %s 0\\n%d %d linear %s 0\\n' | dmsetup create " TEST_DM_FAKE,
		16 * MB_SECTORS, loop, 16 * MB_SECTORS, 16 * MB_SECTORS, loop);
	CHECK(sh(cmd));
	CHECK(sh("udevadm settle"));
	CHECK(check("/dev/mapper/" TEST_DM_FAKE, O_DIRECT, 0, &report));
	// All the blocks of one of the halves no longer have their number
	CHECK(report.num_corruption_errors == 32);
	CHECK(report.num_read_errors + report.num_write_errors == 0);

	IGNORE_RETVAL(sh("dmsetup remove " TEST_DM_BAD));
	IGNORE_RETVAL(sh("dmsetup remove " TEST_DM_FAKE));
	snprintf(cmd, sizeof(cmd), "losetup -d %s", loop);
	IGNORE_RETVAL(sh(cmd));
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };

	ui_set_iface(&iface);
	if (!test_files_init())
		return 1;
	test_clean_file();
	test_cancel();
	if ((getenv("RUFUS_TEST_VM") != NULL) && (getuid() == 0)) {
		test_devices();
		if (test_failures == 0)
			printf("BAD BLOCKS TESTS PASSED\n");
	}
	test_files_cleanup();
	return TEST_RESULT();
}

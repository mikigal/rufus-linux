/*
 * Rufus: The Reliable USB Formatting Utility
 * Tests of the protocol of rufus-helper
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "boot_check.h"
#include "helper_client.h"
#include "progress.h"
#include "ipc.h"
#include "test.h"
#include "../res/grub/grub_version.h"

static ipc_msg_t a, b;

static void test_fields(void)
{
	int sv[2];
	char buf[16];
	uint8_t bytes[3];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	ipc_init(&a, IPC_STATUS);
	ipc_put_u32(&a, 0xdeadbeef);
	ipc_put_u64(&a, 0x0123456789abcdefULL);
	ipc_put_str(&a, "héllo");
	ipc_put_str(&a, NULL);
	ipc_put_str(&a, "");
	ipc_put_bytes(&a, "\x01\x02\x03", 3);
	CHECK(ipc_send(sv[0], &a));
	CHECK(ipc_recv(sv[1], &b) == 1);
	CHECK(b.type == IPC_STATUS);
	CHECK(ipc_get_u32(&b) == 0xdeadbeef);
	CHECK(ipc_get_u64(&b) == 0x0123456789abcdefULL);
	CHECK_EQ_STR(ipc_get_str(&b, buf, sizeof(buf)), "héllo");
	CHECK(ipc_get_str(&b, buf, sizeof(buf)) == NULL);
	CHECK(!b.error);
	CHECK_EQ_STR(ipc_get_str(&b, buf, sizeof(buf)), "");
	CHECK(!ipc_end(&b));
	CHECK(ipc_get_bytes(&b, bytes, 3) && (memcmp(bytes, "\x01\x02\x03", 3) == 0));
	CHECK(ipc_end(&b));
	// Reading past the end is an error, which sticks
	CHECK(ipc_get_u32(&b) == 0);
	CHECK(b.error && !ipc_end(&b));

	// Strings: too long for the buffer, or with a NUL
	ipc_init(&a, IPC_LOG);
	ipc_put_str(&a, "0123456789abcdef");
	CHECK(ipc_send(sv[0], &a));
	CHECK(ipc_recv(sv[1], &b) == 1);
	CHECK(ipc_get_str(&b, buf, 16) == NULL);
	CHECK(b.error);
	ipc_init(&a, IPC_LOG);
	ipc_put_u32(&a, 3);
	ipc_put_bytes(&a, "a\0b", 3);
	CHECK(ipc_send(sv[0], &a));
	CHECK(ipc_recv(sv[1], &b) == 1);
	CHECK(ipc_get_str(&b, buf, sizeof(buf)) == NULL);
	// A length that goes past the end
	ipc_init(&a, IPC_LOG);
	ipc_put_u32(&a, 100);
	ipc_put_bytes(&a, "abc", 3);
	CHECK(ipc_send(sv[0], &a));
	CHECK(ipc_recv(sv[1], &b) == 1);
	CHECK(ipc_get_str(&b, buf, sizeof(buf)) == NULL);

	// A message that does not fit can't be sent
	ipc_init(&a, IPC_LOG);
	ipc_put_bytes(&a, a.data, IPC_MAX_MESSAGE);
	CHECK(a.error);
	CHECK(!ipc_send(sv[0], &a));

	// The end of the socket
	close(sv[0]);
	CHECK(ipc_recv(sv[1], &b) == 0);
	close(sv[1]);
}

static void test_fds(void)
{
	int sv[2], p[2], fd;
	char c = 0;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	CHECK(pipe(p) == 0);
	ipc_init(&a, IPC_FILE);
	ipc_put_str(&a, "grub-2.12/core.img");
	ipc_put_fd(&a, p[0]);
	ipc_put_fd(&a, p[1]);
	CHECK(ipc_send(sv[0], &a));
	close(p[0]);
	close(p[1]);
	CHECK(ipc_recv(sv[1], &b) == 1);
	CHECK(b.nfds == 2);
	// Close on exec
	CHECK(fcntl(b.fds[0], F_GETFD) & FD_CLOEXEC);
	CHECK(write(b.fds[1], "x", 1) == 1);
	fd = ipc_take_fd(&b, 0);
	CHECK((fd >= 0) && (b.fds[0] == -1));
	CHECK(ipc_take_fd(&b, 0) == -1);
	CHECK(ipc_take_fd(&b, 5) == -1);
	CHECK((read(fd, &c, 1) == 1) && (c == 'x'));
	// The other one gets closed with the message
	ipc_close_fds(&b);
	CHECK(b.nfds == 0);
	close(fd);

	// Too many descriptors can't be sent
	ipc_init(&a, IPC_FILE);
	for (fd = 0; fd <= IPC_MAX_FDS; fd++)
		ipc_put_fd(&a, STDIN_FILENO);
	CHECK(a.error);
	close(sv[0]);
	close(sv[1]);
}

static void test_truncated(void)
{
	static uint8_t big[IPC_MAX_MESSAGE + 16];
	int sv[2], p[2];
	uint32_t type = IPC_LOG;
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} control;
	struct iovec iov = { big, sizeof(big) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.buf,
		.msg_controllen = sizeof(control.buf) };
	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
	int sndbuf = 4 * IPC_MAX_MESSAGE;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	CHECK(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);
	CHECK(pipe(p) == 0);
	// A message larger than what the protocol allows, with a descriptor, gets refused
	memcpy(big, &type, sizeof(type));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &p[1], sizeof(int));
	CHECK(sendmsg(sv[0], &msg, 0) == (ssize_t)sizeof(big));
	close(p[1]);
	CHECK(ipc_recv(sv[1], &b) == -1);
	CHECK(b.nfds == 0);
	// ...and its descriptor got closed: the pipe has no writer left
	CHECK(read(p[0], &type, 1) == 0);
	close(p[0]);

	// So does a message shorter than its type
	CHECK(send(sv[0], "ab", 2, 0) == 2);
	CHECK(ipc_recv(sv[1], &b) == -1);
	close(sv[0]);
	close(sv[1]);
}

static void test_check_options(void)
{
	rufus_options_t o;

	ResetOptions(&o);
	o.disk_size = 8 * GB;
	CHECK(CheckOptions(&o));
	o.cluster_size = 4096;
	CHECK(CheckOptions(&o));
	o.cluster_size = 3000;
	CHECK(!CheckOptions(&o));
	o.cluster_size = 64 * MB;
	CHECK(!CheckOptions(&o));
	ResetOptions(&o);
	o.boot_type = BT_MAX;
	CHECK(!CheckOptions(&o));
	ResetOptions(&o);
	o.fs_type = FS_MAX;
	CHECK(!CheckOptions(&o));
	ResetOptions(&o);
	o.partition_type = PARTITION_STYLE_SFD + 1;
	CHECK(!CheckOptions(&o));
	ResetOptions(&o);
	memset(o.label, 'A', sizeof(o.label));
	CHECK(!CheckOptions(&o));
	ResetOptions(&o);
	memset(o.locale, 'A', sizeof(o.locale));
	CHECK(!CheckOptions(&o));
	ResetOptions(&o);
	o.persistence_size = 1;
	CHECK(!CheckOptions(&o));
	ResetOptions(&o);
	o.use_own_c32[0] = 2;
	CHECK(!CheckOptions(&o));
}

// The helper only talks to a front end, as root
static void test_helper_refuses(void)
{
	const char* helper = getenv("RUFUS_TEST_HELPER");
	int sv[2], status;
	pid_t pid;

	if ((helper == NULL) || (getuid() == 0))
		return;
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	pid = fork();
	if (pid == 0) {
		dup2(sv[1], STDIN_FILENO);
		execl(helper, helper, NULL);
		_exit(127);
	}
	close(sv[1]);
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && (WEXITSTATUS(status) == 1));
	close(sv[0]);
	// Without the socket
	pid = fork();
	if (pid == 0) {
		int fd = open("/dev/null", O_RDONLY);
		dup2(fd, STDIN_FILENO);
		execl(helper, helper, NULL);
		_exit(127);
	}
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && (WEXITSTATUS(status) == 2));
}

/*
 * The front end side, against a fake helper in a child process: it checks what
 * it gets, and fails with a specific exit code otherwise
 */
static char log_text[4096];
static uint32_t last_progress;
static int asked_flags;
static BOOL cancel_on_log;

static void client_log(const char* text)
{
	static_strcat(log_text, text);
	if (cancel_on_log)
		CancelOperation();
}

static void client_progress(void* dialog, uint32_t pos, BOOL taskbar)
{
	last_progress = pos;
}

static int client_ask(ui_question_t* q)
{
	asked_flags = q->notification.flags;
	return ((q->type == UI_QUESTION_NOTIFICATION) && (strcmp(q->title, "Bad blocks found") == 0) &&
		(strcmp(q->message, "Retry?") == 0)) ? IDRETRY : IDABORT;
}

#define EXPECT(cond, code) do { if (!(cond)) _exit(code); } while (0)

static void fake_helper(int sock, BOOL cancel)
{
	char buf[256];
	rufus_options_t o;
	struct stat st;

	// The boot files come first
	EXPECT(ipc_recv(sock, &b) == 1 && b.type == IPC_FILE, 10);
	EXPECT(strcmp(ipc_get_str(&b, buf, sizeof(buf)), "grub4dos-" GRUB4DOS_VERSION "/grldr") == 0 && b.nfds == 1, 11);
	EXPECT(read(b.fds[0], buf, 5) == 5 && memcmp(buf, "grldr", 5) == 0, 12);
	ipc_close_fds(&b);
	// What the progress text displays (bare Alt)
	EXPECT(ipc_recv(sock, &b) == 1 && b.type == IPC_PROGRESS_TYPE, 29);
	EXPECT(ipc_get_u32(&b) == UPT_PERCENT && ipc_end(&b), 30);
	EXPECT(ipc_recv(sock, &b) == 1 && b.type == IPC_FORMAT, 13);
	EXPECT(ipc_get_u32(&b) == sizeof(o) && ipc_get_bytes(&b, &o, sizeof(o)) && o.boot_type == BT_GRUB4DOS, 14);
	EXPECT(strcmp(ipc_get_str(&b, buf, sizeof(buf)), "/dev/sdz") == 0 && ipc_get_u64(&b) == 42, 15);
	EXPECT(ipc_get_u32(&b) == (IPC_FD_IMAGE | IPC_FD_BB_LOG) && b.nfds == 2, 16);
	EXPECT(strcmp(ipc_get_str(&b, buf, sizeof(buf)), "image.iso") == 0, 17);
	EXPECT(strcmp(ipc_get_str(&b, buf, sizeof(buf)), "/home/user/rufus.log") == 0 && ipc_end(&b), 18);
	EXPECT(fstat(b.fds[0], &st) == 0 && S_ISREG(st.st_mode), 19);
	ipc_close_fds(&b);

	ipc_init(&a, IPC_LOG);
	ipc_put_str(&a, "Hello from the helper\n");
	EXPECT(ipc_send(sock, &a), 20);
	if (cancel) {
		// The front end cancels on that log message
		EXPECT(ipc_recv(sock, &b) == 1 && b.type == IPC_CANCEL, 21);
		ipc_init(&a, IPC_DONE);
		ipc_put_u32(&a, FALSE);
		ipc_put_u32(&a, RUFUS_ERROR(ERROR_CANCELLED));
		EXPECT(ipc_send(sock, &a), 22);
		_exit(0);
	}
	ipc_init(&a, IPC_PROGRESS);
	ipc_put_u32(&a, 1234);
	ipc_put_u32(&a, TRUE);
	EXPECT(ipc_send(sock, &a), 23);
	ipc_init(&a, IPC_ASK);
	ipc_put_u32(&a, UI_QUESTION_NOTIFICATION);
	ipc_put_str(&a, "Bad blocks found");
	ipc_put_str(&a, "Retry?");
	ipc_put_u32(&a, MB_ABORTRETRYIGNORE | MB_ICONWARNING);
	ipc_put_str(&a, NULL);
	ipc_put_u32(&a, 0);
	ipc_put_str(&a, NULL);
	EXPECT(ipc_send(sock, &a), 24);
	EXPECT(ipc_recv(sock, &b) == 1 && b.type == IPC_ANSWER, 25);
	EXPECT((int32_t)ipc_get_u32(&b) == IDRETRY && ipc_end(&b), 26);
	ipc_init(&a, IPC_DONE);
	ipc_put_u32(&a, TRUE);
	ipc_put_u32(&a, 0);
	EXPECT(ipc_send(sock, &a), 27);
	// Then the end of the socket
	EXPECT(ipc_recv(sock, &b) == 0, 28);
	_exit(0);
}

static void run_client(BOOL cancel)
{
	const ui_iface_t iface = { .log = client_log, .progress = client_progress, .ask = client_ask };
	char dir[] = "/tmp/rufus-test-XXXXXX", path[MAX_PATH];
	helper_job_t job = { .devnode = "/dev/sdz", .diskseq = 42, .image_name = "image.iso", .archive_fd = -1, .unattend_fd = -1,
		.bb_log_path = "/home/user/rufus.log" };
	helper_t h;
	int sv[2], status;
	pid_t pid;
	FILE* f;

	ui_set_iface(&iface);
	CHECK(mkdtemp(dir) != NULL);
	static_strcpy(app_data_dir, dir);
	snprintf(path, sizeof(path), "%s/%s/grub4dos-%s", dir, FILES_DIR, GRUB4DOS_VERSION);
	CHECK(mkdir_p(path, 0755) == 0);
	snprintf(path, sizeof(path), "%s/%s/grub4dos-%s/grldr", dir, FILES_DIR, GRUB4DOS_VERSION);
	f = fopen(path, "w");
	CHECK(f != NULL);
	fputs("grldr", f);
	fclose(f);
	ResetOptions(&opts);
	opts.boot_type = BT_GRUB4DOS;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	pid = fork();
	if (pid == 0) {
		close(sv[0]);
		fake_helper(sv[1], cancel);
	}
	close(sv[1]);
	h.sock = sv[0];
	h.pid = -1;
	job.image_fd = open(path, O_RDONLY);
	job.bb_log_fd = open("/dev/null", O_WRONLY);
	cancel_on_log = cancel;
	if (cancel) {
		CHECK(!HelperFormat(&h, &job));
		CHECK(SCODE_CODE(ErrorStatus) == ERROR_CANCELLED);
	} else {
		CHECK(HelperFormat(&h, &job));
		CHECK(ErrorStatus == 0);
		CHECK(last_progress == 1234);
		CHECK(asked_flags == (MB_ABORTRETRYIGNORE | MB_ICONWARNING));
	}
	CHECK_CONTAINS(log_text, "Hello from the helper\n");
	if (cancel)
		CHECK_CONTAINS(log_text, "Cancelling the operation of the helper");
	log_text[0] = 0;
	StopHelper(&h);
	CHECK(h.sock == -1);
	CHECK(waitpid(pid, &status, 0) == pid);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
		fprintf(stderr, "The fake helper failed with %d\n", WEXITSTATUS(status));
	CHECK(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
	close(job.image_fd);
	close(job.bb_log_fd);
	snprintf(path, sizeof(path), "rm -rf '%s'", dir);
	IGNORE_RETVAL(system(path));
	ErrorStatus = 0;
}

int main(void)
{
	test_fields();
	test_fds();
	test_truncated();
	test_check_options();
	test_helper_refuses();
	run_client(FALSE);
	run_client(TRUE);
	return TEST_RESULT();
}

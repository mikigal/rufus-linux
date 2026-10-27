/*
 * Rufus: The Reliable USB Formatting Utility
 * The privileged helper
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
 * rufus-helper runs as root (through pkexec), with no user interface: the
 * front end starts it with a SOCK_SEQPACKET socket as its standard input, and
 * asks it to format drives (see ipc.h). It does not trust the front end:
 * - the drive must be one that Rufus lists (never a system disk), with the
 *   diskseq and size the front end saw;
 * - files only come as descriptors (the image, the archive, the downloaded
 *   boot files and the bad blocks report), and the image gets scanned again;
 * - the options must be in range, and the boot files the ones they use.
 * Each format runs in a child process, in a mount namespace of its own (the
 * mounts go away with it), which the end of the socket cancels.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "devices.h"
#include "disk.h"
#include "image_scan.h"
#include "boot_check.h"
#include "format_thread.h"
#include "localization.h"
#include "ipc.h"

#define SOCK                        STDIN_FILENO
#define MAX_BOOT_FILE_SIZE          (32 * MB)

// The boot files that the front end sent for the next format
static struct {
	char name[MAX_PATH];
	int fd;
} files[MAX_BOOT_FILES];
static int nb_files = 0;

// The format to run, from IPC_FORMAT
typedef struct {
	char devnode[64];
	uint64_t diskseq;
	char syspath[MAX_PATH];
	int image_fd, archive_fd, bb_log_fd;
	char image_name[MAX_PATH];
	char bb_log_path[MAX_PATH];
} job_t;

static ipc_msg_t in, out;
static int answer_fd = -1;			// In the worker: the answers that the helper forwards

static void send_done(BOOL ret, DWORD status)
{
	ipc_init(&out, IPC_DONE);
	ipc_put_u32(&out, (uint32_t)ret);
	ipc_put_u32(&out, (uint32_t)status);
	ipc_send(SOCK, &out);
}

/*
 * The user interface of the worker: messages to the front end
 */
static void helper_log(const char* text)
{
	ipc_init(&out, IPC_LOG);
	ipc_put_str(&out, text);
	ipc_send(SOCK, &out);
}

static void helper_status(BOOL info, unsigned int duration, const char* text)
{
	ipc_init(&out, IPC_STATUS);
	ipc_put_u32(&out, (uint32_t)info);
	ipc_put_u32(&out, duration);
	ipc_put_str(&out, text);
	ipc_send(SOCK, &out);
}

static void helper_progress(void* dialog, uint32_t pos, BOOL taskbar)
{
	// The helper has no dialog of its own
	if (dialog != NULL)
		return;
	ipc_init(&out, IPC_PROGRESS);
	ipc_put_u32(&out, pos);
	ipc_put_u32(&out, (uint32_t)taskbar);
	ipc_send(SOCK, &out);
}

static void helper_event(enum ui_event_type type, intptr_t param)
{
	ipc_init(&out, IPC_EVENT);
	ipc_put_u32(&out, (uint32_t)type);
	ipc_put_u64(&out, (uint64_t)(int64_t)param);
	ipc_send(SOCK, &out);
}

static int helper_ask(ui_question_t* q)
{
	const int cancel = (q->type == UI_QUESTION_NOTIFICATION) ? IDCANCEL : -1;
	int32_t answer;
	ssize_t r;
	int i;

	if (q->type == UI_QUESTION_SELECTION) {
		uprintf("The helper can't ask for a selection: '%s'", q->title);
		return -1;
	}
	ipc_init(&out, IPC_ASK);
	ipc_put_u32(&out, (uint32_t)q->type);
	ipc_put_str(&out, q->title);
	ipc_put_str(&out, q->message);
	if (q->type == UI_QUESTION_NOTIFICATION) {
		ipc_put_u32(&out, (uint32_t)q->notification.flags);
		ipc_put_str(&out, q->notification.dont_display_setting);
		ipc_put_u32(&out, (q->notification.more_info == NULL) ? 0 : q->notification.more_info->id);
		ipc_put_str(&out, (q->notification.more_info == NULL) ? NULL : q->notification.more_info->url);
	} else {
		ipc_put_u32(&out, (uint32_t)min(q->list.size, IPC_MAX_LIST_ITEMS));
		for (i = 0; i < min(q->list.size, IPC_MAX_LIST_ITEMS); i++)
			ipc_put_str(&out, q->list.items[i]);
	}
	if (!ipc_send(SOCK, &out))
		return cancel;
	// The helper closes the pipe, or interrupts the read (SIGUSR1), to cancel
	do {
		r = read(answer_fd, &answer, sizeof(answer));
	} while ((r < 0) && (errno == EINTR) && !IS_ERROR(ErrorStatus));
	return (r == sizeof(answer)) ? answer : cancel;
}

static void on_cancel(int sig)
{
	// NB: A lock free atomic store, which is safe in a signal handler
	ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
}

/*
 * The worker
 */
static int remove_entry(const char* path, const struct stat* st, int flag, struct FTW* ftw)
{
	return remove(path);
}

// The directory of the worker, for the image and the boot files (its app_data_dir)
static BOOL CreateDataDir(char* dir, size_t dir_size)
{
	if ((mkdir("/run/rufus", 0700) != 0) && (errno != EEXIST))
		return FALSE;
	safe_strcpy(dir, dir_size, "/run/rufus/data-XXXXXX");
	if (mkdtemp(dir) == NULL) {
		dir[0] = 0;
		return FALSE;
	}
	return TRUE;
}

/*
 * The image, as a link with its name in the directory of the worker: Rufus
 * tells how an image is compressed from its extension.
 */
static char* LinkImage(const char* dir, const job_t* job)
{
	const char* name = strrchr(job->image_name, '/');
	char path[2 * MAX_PATH], target[64];

	name = (name == NULL) ? job->image_name : &name[1];
	if ((name[0] == 0) || (strcmp(name, ".") == 0) || (strcmp(name, "..") == 0) || (strcmp(name, FILES_DIR) == 0))
		name = "image";
	static_sprintf(path, "%s/%s", dir, name);
	static_sprintf(target, "/proc/self/fd/%d", job->image_fd);
	if (symlink(target, path) != 0)
		return NULL;
	return strdup(path);
}

// Copy the boot files into the directory of the worker
static BOOL SetupBootFiles(const char* dir)
{
	char path[2 * MAX_PATH], *p, buf[64 * KB];
	struct stat st;
	ssize_t r;
	int i, fd;
	BOOL ret;

	for (i = 0; i < nb_files; i++) {
		if ((fstat(files[i].fd, &st) != 0) || !S_ISREG(st.st_mode) || (st.st_size > MAX_BOOT_FILE_SIZE)) {
			uprintf("Invalid boot file '%s'", files[i].name);
			return FALSE;
		}
		static_sprintf(path, "%s/%s/%s", dir, FILES_DIR, files[i].name);
		p = strrchr(path, '/');
		*p = 0;
		if (mkdir_p(path, 0700) != 0)
			return FALSE;
		*p = '/';
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			return FALSE;
		ret = (lseek(files[i].fd, 0, SEEK_SET) == 0);
		while (ret && ((r = read(files[i].fd, buf, sizeof(buf))) != 0))
			ret = (r > 0) && (write(fd, buf, (size_t)r) == r);
		close(fd);
		if (!ret)
			return FALSE;
	}
	return TRUE;
}

// Returns the result of the format, which the helper reports once the worker is gone
static BOOL Work(const job_t* job)
{
	const ui_iface_t iface = { .log = helper_log, .status = helper_status, .progress = helper_progress,
		.ask = helper_ask, .event = helper_event };
	format_target_t target = { .devnode = job->devnode, .diskseq = job->diskseq, .syspath = job->syspath,
		.image_fd = job->image_fd, .private_mounts = TRUE, .bb_log_fd = job->bb_log_fd,
		.bb_log_path = job->bb_log_path };
	char names[MAX_BOOT_FILES][MAX_PATH], path[64], data_dir[MAX_PATH] = "";
	int i, j, n;
	BOOL ret = FALSE;

	ui_set_iface(&iface);
	ErrorStatus = 0;
	uprintf("Helper: writing to %s for the front end", job->devnode);
	if (!CreateDataDir(data_dir, sizeof(data_dir))) {
		uprintf("Could not create the directory of the helper: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_CANNOT_COPY);
		goto out;
	}

	if (job->image_fd >= 0) {
		image_path = LinkImage(data_dir, job);
		uprintf("Image: %s (from the front end)", job->image_name);
		if ((image_path == NULL) || (ImageScan(FALSE) != RULES_SCAN_OK)) {
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_SCAN));
			goto out;
		}
		if (IsSourceImageLocatedOnTargetDrive(job->devnode)) {
			uprintf("The image is located on the target drive");
			ErrorStatus = RUFUS_ERROR(ERROR_ACCESS_DENIED);
			goto out;
		}
	} else if (opts.boot_type == BT_IMAGE) {
		uprintf("No image to write");
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
		goto out;
	}
	if (job->archive_fd >= 0) {
		static_sprintf(path, "/proc/self/fd/%d", job->archive_fd);
		archive_path = strdup(path);
	}

	// Only the boot files that this format uses
	n = GetBootFiles(names, FALSE);
	for (i = 0; i < nb_files; i++) {
		for (j = 0; (j < n) && (strcmp(files[i].name, names[j]) != 0); j++);
		if (j >= n) {
			uprintf("Unexpected boot file '%s'", files[i].name);
			ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
			goto out;
		}
	}
	if (!SetupBootFiles(data_dir)) {
		uprintf("Could not set up the boot files: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_CANNOT_COPY);
		goto out;
	}
	static_strcpy(app_data_dir, data_dir);
	LoadBootFiles();

	ret = FormatDrive(&target);

out:
	if (data_dir[0] != 0)
		nftw(data_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	return ret && !IS_ERROR(ErrorStatus);
}

/*
 * The helper
 */

// A name relative to FILES_DIR, such as "grub-2.12/core.img"
static BOOL IsValidFileName(const char* name)
{
	const char* allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-";
	const char* p = name;
	size_t len;

	if (strlen(name) >= MAX_PATH / 2)
		return FALSE;
	// Components that are not empty, "." or ".."
	do {
		len = strcspn(p, "/");
		if ((len == 0) || (strspn(p, allowed) < len) || ((len == 1) && (p[0] == '.')) ||
			((len == 2) && (p[0] == '.') && (p[1] == '.')))
			return FALSE;
		p += len;
	} while (*p++ == '/');
	return TRUE;
}

static void ClearFiles(void)
{
	int i;

	for (i = 0; i < nb_files; i++)
		close(files[i].fd);
	nb_files = 0;
}

// The drive must be one that Rufus lists, as the front end saw it
static BOOL FindDrive(job_t* job)
{
	static rufus_device_t devs[MAX_DRIVES];
	device_filter_t filter = { .enable_HDDs = TRUE, .list_non_usb_removable_drives = TRUE, .enable_VHDs = TRUE,
		.enable_vmdk = TRUE };
	int i, n = GetDevices(devs, ARRAYSIZE(devs), &filter);

	for (i = 0; i < n; i++) {
		if (strcmp(devs[i].devnode, job->devnode) != 0)
			continue;
		if ((devs[i].diskseq != job->diskseq) || (devs[i].size != opts.disk_size)) {
			uprintf("%s is no longer the drive that was selected", job->devnode);
			return FALSE;
		}
		static_strcpy(job->syspath, devs[i].syspath);
		return TRUE;
	}
	uprintf("%s is not a drive that Rufus can write to", job->devnode);
	return FALSE;
}

static BOOL ParseFormat(ipc_msg_t* m, job_t* job)
{
	uint32_t opts_size = ipc_get_u32(m), fd_flags;
	int fd_index = 0;

	memset(job, 0, sizeof(*job));
	job->image_fd = job->archive_fd = job->bb_log_fd = -1;
	if ((opts_size != sizeof(opts)) || !ipc_get_bytes(m, &opts, sizeof(opts)) || !CheckOptions(&opts))
		return FALSE;
	if ((ipc_get_str(m, job->devnode, sizeof(job->devnode)) == NULL) ||
		(strncmp(job->devnode, "/dev/", 5) != 0) || (strchr(&job->devnode[5], '/') != NULL) ||
		(job->devnode[5] == 0) || (job->devnode[5] == '.'))
		return FALSE;
	job->diskseq = ipc_get_u64(m);
	fd_flags = ipc_get_u32(m);
	if (ipc_get_str(m, job->image_name, sizeof(job->image_name)) == NULL)
		job->image_name[0] = 0;
	if (ipc_get_str(m, job->bb_log_path, sizeof(job->bb_log_path)) == NULL)
		job->bb_log_path[0] = 0;
	if (!ipc_end(m) || (fd_flags & ~(IPC_FD_IMAGE | IPC_FD_ARCHIVE | IPC_FD_BB_LOG)))
		return FALSE;
	if (fd_flags & IPC_FD_IMAGE)
		job->image_fd = ipc_take_fd(m, fd_index++);
	if (fd_flags & IPC_FD_ARCHIVE)
		job->archive_fd = ipc_take_fd(m, fd_index++);
	if (fd_flags & IPC_FD_BB_LOG)
		job->bb_log_fd = ipc_take_fd(m, fd_index++);
	return (fd_index == m->nfds) && ((fd_flags & IPC_FD_IMAGE) == 0 || job->image_fd >= 0) &&
		((fd_flags & IPC_FD_ARCHIVE) == 0 || job->archive_fd >= 0) && ((fd_flags & IPC_FD_BB_LOG) == 0 || job->bb_log_fd >= 0);
}

/*
 * Run the format in a worker, and forward the answers and the cancellation to
 * it. The helper reports the result (IPC_DONE) once the worker is gone, so
 * that the next request of the front end can't arrive before. Returns FALSE
 * when the front end went away.
 */
static BOOL RunWorker(job_t* job)
{
	struct sigaction sa = { .sa_handler = on_cancel };	// No SA_RESTART: a pending question gets interrupted
	struct pollfd pfd[2];
	int answer_pipe[2], result_pipe[2], pidfd, status = 0, r;
	uint32_t result[2] = { FALSE, RUFUS_ERROR(ERROR_GEN_FAILURE) };
	BOOL connected = TRUE;
	int32_t answer;
	pid_t pid;

	if (pipe2(answer_pipe, O_CLOEXEC) != 0) {
		send_done(FALSE, RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY));
		return TRUE;
	}
	if (pipe2(result_pipe, O_CLOEXEC) != 0) {
		close(answer_pipe[0]);
		close(answer_pipe[1]);
		send_done(FALSE, RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY));
		return TRUE;
	}
	pid = fork();
	if (pid < 0) {
		close(answer_pipe[0]);
		close(answer_pipe[1]);
		close(result_pipe[0]);
		close(result_pipe[1]);
		send_done(FALSE, RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY));
		return TRUE;
	}
	if (pid == 0) {
		close(answer_pipe[1]);
		close(result_pipe[0]);
		answer_fd = answer_pipe[0];
		sigaction(SIGUSR1, &sa, NULL);
		result[0] = Work(job);
		result[1] = ErrorStatus;
		_exit((write(result_pipe[1], result, sizeof(result)) == sizeof(result)) ? 0 : 1);
	}
	close(answer_pipe[0]);
	close(result_pipe[1]);
	pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	pfd[0].fd = SOCK;
	pfd[0].events = POLLIN;
	pfd[1].fd = pidfd;
	pfd[1].events = POLLIN;
	while (pidfd >= 0) {
		r = poll(pfd, 2, -1);
		if ((r < 0) && (errno == EINTR))
			continue;
		if ((r < 0) || (pfd[1].revents != 0))
			break;
		if (pfd[0].revents == 0)
			continue;
		r = ipc_recv(SOCK, &in);
		if ((r < 0) && (errno == EINTR))
			continue;
		if ((r > 0) && (in.type == IPC_ANSWER)) {
			answer = (int32_t)ipc_get_u32(&in);
			if (!ipc_end(&in) || (write(answer_pipe[1], &answer, sizeof(answer)) != sizeof(answer)))
				kill(pid, SIGUSR1);
		} else if ((r > 0) && (in.type == IPC_CANCEL)) {
			kill(pid, SIGUSR1);
		} else {
			// The front end went away (or does not follow the protocol): cancel, and wait for the worker
			kill(pid, SIGUSR1);
			connected = FALSE;
			pfd[0].fd = -1;
		}
		ipc_close_fds(&in);
	}
	close(answer_pipe[1]);
	while ((waitpid(pid, &status, 0) < 0) && (errno == EINTR));
	if (pidfd >= 0)
		close(pidfd);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0) ||
		(read(result_pipe[0], result, sizeof(result)) != sizeof(result))) {
		// The worker crashed: its mounts went with its namespace, but not the udev rule
		uprintf("The worker of the helper died (status 0x%x)", status);
		RemoveAutomountRules();
		result[0] = FALSE;
		result[1] = RUFUS_ERROR(ERROR_GEN_FAILURE);
	}
	close(result_pipe[0]);
	if (connected)
		send_done((BOOL)result[0], result[1]);
	return connected;
}

static void LoadLanguage(const char* language)
{
	const char* loc_files[] = { RUFUS_LOC_INSTALLED, RUFUS_LOC_SOURCE };
	size_t i;

	init_localization();
	for (i = 0; i < ARRAYSIZE(loc_files); i++) {
		if (access(loc_files[i], R_OK) != 0)
			continue;
		if (get_supported_locales(loc_files[i]) &&
			get_loc_data_file(loc_files[i], get_locale_from_name((char*)language, TRUE)))
			return;
	}
}

static void stderr_log(const char* text)
{
	fputs(text, stderr);
}

int main(int argc, char** argv)
{
	ui_iface_t iface = { .log = stderr_log };
	char name[MAX_PATH], language[32];
	int type = 0, domain = 0, r;
	socklen_t len = sizeof(int);
	BOOL hello = FALSE;
	job_t job;

	if ((getsockopt(SOCK, SOL_SOCKET, SO_TYPE, &type, &len) != 0) || (type != SOCK_SEQPACKET) ||
		(getsockopt(SOCK, SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0) || (domain != AF_UNIX)) {
		fprintf(stderr, "rufus-helper is started by Rufus, and is not meant to be run directly\n");
		return 2;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "rufus-helper must run as root\n");
		return 1;
	}
	ui_set_iface(&iface);
	signal(SIGPIPE, SIG_IGN);
	umask(077);
	ResetOptions(&opts);
	// The boot check and the Syslinux installation compare with the versions we embed
	SetEmbeddedSyslinuxVersions();
	RemoveAutomountRules();

	while ((r = ipc_recv(SOCK, &in)) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (!hello && (in.type != IPC_HELLO))
			break;
		switch (in.type) {
		case IPC_HELLO:
			if ((ipc_get_u32(&in) != IPC_VERSION) || (ipc_get_u32(&in) != sizeof(rufus_options_t)) ||
				(ipc_get_str(&in, language, sizeof(language)) == NULL) || !ipc_end(&in)) {
				fprintf(stderr, "rufus-helper: the front end is not from the same build\n");
				goto out;
			}
			ipc_init(&out, IPC_HELLO);
			ipc_put_u32(&out, IPC_VERSION);
			if (!ipc_send(SOCK, &out))
				goto out;
			if (!hello) {
				// The log goes to the front end from now on
				iface.log = helper_log;
				ui_set_iface(&iface);
				LoadLanguage(language);
			}
			hello = TRUE;
			break;
		case IPC_FILE:
			if ((ipc_get_str(&in, name, sizeof(name)) == NULL) || !ipc_end(&in) || (in.nfds != 1) ||
				!IsValidFileName(name) || (nb_files >= MAX_BOOT_FILES))
				goto out;
			static_strcpy(files[nb_files].name, name);
			files[nb_files++].fd = ipc_take_fd(&in, 0);
			break;
		case IPC_FORMAT:
			ErrorStatus = 0;
			if (!ParseFormat(&in, &job)) {
				uprintf("Invalid format request");
				send_done(FALSE, RUFUS_ERROR(ERROR_INVALID_PARAMETER));
			} else if (!FindDrive(&job)) {
				send_done(FALSE, RUFUS_ERROR(ERROR_ACCESS_DENIED));
			} else if (!RunWorker(&job)) {
				goto out;
			}
			if (job.image_fd >= 0)
				close(job.image_fd);
			if (job.archive_fd >= 0)
				close(job.archive_fd);
			if (job.bb_log_fd >= 0)
				close(job.bb_log_fd);
			ClearFiles();
			ResetOptions(&opts);
			break;
		case IPC_CANCEL:
			// Nothing to cancel
			break;
		default:
			goto out;
		}
		ipc_close_fds(&in);
	}

out:
	ipc_close_fds(&in);
	ClearFiles();
	exit_localization();
	return 0;
}

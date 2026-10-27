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
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
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
#include "progress.h"
#include "platform.h"
#include "localization.h"
#include "resource.h"
#include "ipc.h"
#include "vdisk.h"
#include "helper_client.h"

#define SOCK                        STDIN_FILENO
#define MAX_BOOT_FILE_SIZE          (32 * MB)
#define MAX_UNATTEND_SIZE           (1 * MB)

// The boot files that the front end sent for the next format
static struct {
	char name[MAX_PATH];
	int fd;
} files[MAX_BOOT_FILES];
static int nb_files = 0;

// The format to run, from IPC_FORMAT, or the drive to save, from IPC_SAVE
typedef struct {
	BOOL save;
	char devnode[64];
	uint64_t diskseq;
	char syspath[MAX_PATH];
	int image_fd, archive_fd, bb_log_fd, unattend_fd;
	char image_name[MAX_PATH];
	char bb_log_path[MAX_PATH];
	uint32_t save_type;				// SAVE_* (helper_client.h)
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

// A copy of the unattend.xml of the front end in the directory of the worker, or NULL
static char* CopyUnattendXml(const char* dir, int fd)
{
	char path[MAX_PATH], buf[64 * KB];
	struct stat st;
	ssize_t r;
	int out_fd;
	BOOL ret;

	if ((fstat(fd, &st) != 0) || !S_ISREG(st.st_mode) || (st.st_size == 0) || (st.st_size > MAX_UNATTEND_SIZE))
		return NULL;
	static_sprintf(path, "%s/unattend.xml", dir);
	out_fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (out_fd < 0)
		return NULL;
	ret = (lseek(fd, 0, SEEK_SET) == 0);
	while (ret && ((r = read(fd, buf, sizeof(buf))) != 0))
		ret = (r > 0) && (write(out_fd, buf, (size_t)r) == r);
	if (close(out_fd) != 0)
		ret = FALSE;
	return ret ? strdup(path) : NULL;
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
	// The priority of the format, as Windows sets the one of its thread (5 nice levels a step)
	if (setpriority(PRIO_PROCESS, 0, -5 * opts.thread_priority) != 0)
		uprintf("Could not set the priority of the format: %s", WindowsErrorString());
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
	// The temporary files of the format (the registry hive of boot.wim...) go there too
	static_strcpy(temp_dir, data_dir);
	// unattend.xml gets modified, so the worker uses a copy of its own
	if (job->unattend_fd >= 0) {
		unattend_xml_path = CopyUnattendXml(data_dir, job->unattend_fd);
		if (unattend_xml_path == NULL) {
			uprintf("Invalid unattend.xml");
			ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
			goto out;
		}
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
 * Save a drive to an image (IDC_SAVE), to the file of the front end (job->image_fd):
 * a VHD or VHDX of the whole drive, or an ISO of the files of its first volume,
 * which gets mounted read-only for xorriso (oscdimg.exe upstream).
 */
static BOOL WorkSave(const job_t* job)
{
	const ui_iface_t iface = { .log = helper_log, .status = helper_status, .progress = helper_progress,
		.ask = helper_ask, .event = helper_event };
	char volume[64], fs_type[32], label[64], output[64], *mount_point = NULL;
	// Level 3 for the files over 4 GB (upstream makes a UDF image for that), and Joliet for Windows
	const char* xorriso[] = { "xorriso", "-outdev", output, "-volid", label, "-joliet", "on", "-rockridge", "on",
		"-compliance", "iso_9660_level=3:joliet_long_names", "-map", NULL, "/", "-commit", NULL };
	drive_t drive = { .fd = -1 };
	disk_set_t disks = { 0 };
	struct stat st;
	BOOL ret = FALSE;
	DWORD r;

	ui_set_iface(&iface);
	ErrorStatus = 0;
	uprintf("Helper: saving %s to '%s' for the front end", job->devnode, job->image_name);
	if ((fstat(job->image_fd, &st) != 0) || !S_ISREG(st.st_mode)) {
		uprintf("The image is not a file");
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
		goto out;
	}
	if (!OpenDrive(job->devnode, job->diskseq, 0, &drive))
		goto out;
	// An image of a drive, on that drive, would never end
	AddDisksOfDevice(&disks, st.st_dev);
	if (DiskSetContains(&disks, drive.devnum)) {
		uprintf("The image can not be saved on the drive that it is the image of");
		ErrorStatus = RUFUS_ERROR(ERROR_ACCESS_DENIED);
		goto out;
	}
	InitProgress(TRUE);

	if (job->save_type != SAVE_ISO) {
		ret = VdiskSave(drive.fd, drive.size, drive.sector_size, job->image_fd,
			(job->save_type == SAVE_VHD) ? VDISK_VHD : VDISK_VHDX);
		goto out;
	}
	if (!GetFirstVolume(job->syspath, volume, sizeof(volume), fs_type, sizeof(fs_type), label, sizeof(label))) {
		uprintf("The drive has no file system to save to an ISO");
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
		goto out;
	}
	CloseDrive(&drive);
	// The mounts of the worker must not be seen by the desktop
	if (!EnterPrivateMountNamespace()) {
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_MOUNT_VOLUME));
		goto out;
	}
	mount_point = MountVolume(volume, fs_type, TRUE);
	if (mount_point == NULL) {
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_MOUNT_VOLUME));
		goto out;
	}
	// ISO-9660 labels are up to 32 characters (and the defaults of upstream otherwise)
	if (label[0] == 0)
		static_strcpy(label, "CDROM");
	label[32] = 0;
	// The image, through the descriptor of the worker
	static_sprintf(output, "stdio:/proc/%d/fd/%d", (int)getpid(), job->image_fd);
	xorriso[12] = mount_point;
	uprintf("Running command: 'xorriso -outdev <image> -volid \"%s\" -joliet on -rockridge on "
		"-compliance iso_9660_level=3:joliet_long_names -map <%s> / -commit'", label, volume);
	// For the progress report of xorriso: "xorriso : UPDATE :  15.03% done"
	r = RunCommandWithProgress(xorriso, NULL, TRUE, MSG_261, ".* ([0-9\\.]+)% done.*");
	if (r != 0) {
		if (!IS_ERROR(ErrorStatus)) {
			uprintf("Failed to write ISO image: xorriso returned %d", (int)r);
			ErrorStatus = RUFUS_ERROR((r == ERROR_FILE_NOT_FOUND) ? ERROR_FILE_NOT_FOUND : ERROR_WRITE_FAULT);
		}
		goto out;
	}
	UpdateProgressWithInfo(OP_FORMAT, MSG_261, 1, 1);
	ret = TRUE;

out:
	if (mount_point != NULL) {
		UnmountVolume(mount_point);
		free(mount_point);
	}
	CloseDrive(&drive);
	DiskSetFree(&disks);
	if (ret)
		uprintf("Saved '%s'", job->image_name);
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

// A drive that Rufus lists (with its most permissive filters), as the front end saw it
static const rufus_device_t* FindDevice(const char* devnode, uint64_t diskseq)
{
	static rufus_device_t devs[MAX_DRIVES];
	device_filter_t filter = { .enable_HDDs = TRUE, .list_non_usb_removable_drives = TRUE, .enable_VHDs = TRUE,
		.enable_vmdk = TRUE };
	int i, n = GetDevices(devs, ARRAYSIZE(devs), &filter);

	for (i = 0; i < n; i++) {
		if (strcmp(devs[i].devnode, devnode) != 0)
			continue;
		if (devs[i].diskseq != diskseq) {
			uprintf("%s is no longer the drive that was selected", devnode);
			return NULL;
		}
		return &devs[i];
	}
	uprintf("%s is not a drive that Rufus can write to", devnode);
	return NULL;
}

// The drive must be one that Rufus lists, as the front end saw it
static BOOL FindDrive(job_t* job)
{
	const rufus_device_t* dev = FindDevice(job->devnode, job->diskseq);

	if (dev == NULL)
		return FALSE;
	if (dev->size != opts.disk_size) {
		uprintf("%s is no longer the drive that was selected", job->devnode);
		return FALSE;
	}
	static_strcpy(job->syspath, dev->syspath);
	return TRUE;
}

// In the worker: what the progress text displays, as the front end cycles it
static void* ProgressTypeThread(void* param)
{
	int fd = (int)(intptr_t)param;
	int32_t type;

	while (read(fd, &type, sizeof(type)) == sizeof(type)) {
		if ((type >= 0) && (type < UPT_MAX))
			update_progress_type = type;
	}
	return NULL;
}

// A device node, as GetDevices() lists them ("/dev/sdb")
static BOOL IsValidDevnode(const char* devnode)
{
	return (strncmp(devnode, "/dev/", 5) == 0) && (strchr(&devnode[5], '/') == NULL) &&
		(devnode[5] != 0) && (devnode[5] != '.');
}

static BOOL ParseSave(ipc_msg_t* m, job_t* job)
{
	memset(job, 0, sizeof(*job));
	job->image_fd = job->archive_fd = job->bb_log_fd = job->unattend_fd = -1;
	job->save = TRUE;
	if ((ipc_get_str(m, job->devnode, sizeof(job->devnode)) == NULL) || !IsValidDevnode(job->devnode))
		return FALSE;
	job->diskseq = ipc_get_u64(m);
	job->save_type = ipc_get_u32(m);
	if (ipc_get_str(m, job->image_name, sizeof(job->image_name)) == NULL)
		job->image_name[0] = 0;
	if (!ipc_end(m) || (m->nfds != 1) || (job->save_type < SAVE_VHD) || (job->save_type > SAVE_ISO))
		return FALSE;
	job->image_fd = ipc_take_fd(m, 0);
	return (job->image_fd >= 0);
}

static BOOL ParseFormat(ipc_msg_t* m, job_t* job)
{
	uint32_t opts_size = ipc_get_u32(m), fd_flags;
	int fd_index = 0;

	memset(job, 0, sizeof(*job));
	job->image_fd = job->archive_fd = job->bb_log_fd = job->unattend_fd = -1;
	if ((opts_size != sizeof(opts)) || !ipc_get_bytes(m, &opts, sizeof(opts)) || !CheckOptions(&opts))
		return FALSE;
	if ((ipc_get_str(m, job->devnode, sizeof(job->devnode)) == NULL) || !IsValidDevnode(job->devnode))
		return FALSE;
	job->diskseq = ipc_get_u64(m);
	fd_flags = ipc_get_u32(m);
	if (ipc_get_str(m, job->image_name, sizeof(job->image_name)) == NULL)
		job->image_name[0] = 0;
	if (ipc_get_str(m, job->bb_log_path, sizeof(job->bb_log_path)) == NULL)
		job->bb_log_path[0] = 0;
	if (!ipc_end(m) || (fd_flags & ~(IPC_FD_IMAGE | IPC_FD_ARCHIVE | IPC_FD_BB_LOG | IPC_FD_UNATTEND)))
		return FALSE;
	if (fd_flags & IPC_FD_IMAGE)
		job->image_fd = ipc_take_fd(m, fd_index++);
	if (fd_flags & IPC_FD_ARCHIVE)
		job->archive_fd = ipc_take_fd(m, fd_index++);
	if (fd_flags & IPC_FD_BB_LOG)
		job->bb_log_fd = ipc_take_fd(m, fd_index++);
	if (fd_flags & IPC_FD_UNATTEND)
		job->unattend_fd = ipc_take_fd(m, fd_index++);
	return (fd_index == m->nfds) && ((fd_flags & IPC_FD_IMAGE) == 0 || job->image_fd >= 0) &&
		((fd_flags & IPC_FD_ARCHIVE) == 0 || job->archive_fd >= 0) && ((fd_flags & IPC_FD_BB_LOG) == 0 || job->bb_log_fd >= 0) &&
		((fd_flags & IPC_FD_UNATTEND) == 0 || job->unattend_fd >= 0);
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
	int answer_pipe[2], result_pipe[2], type_pipe[2], pidfd, status = 0, r;
	pthread_t type_thread;
	uint32_t result[2] = { FALSE, RUFUS_ERROR(ERROR_GEN_FAILURE) };
	BOOL connected = TRUE;
	int32_t answer, type;
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
	if (pipe2(type_pipe, O_CLOEXEC) != 0) {
		close(answer_pipe[0]);
		close(answer_pipe[1]);
		close(result_pipe[0]);
		close(result_pipe[1]);
		send_done(FALSE, RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY));
		return TRUE;
	}
	pid = fork();
	if (pid < 0) {
		close(answer_pipe[0]);
		close(answer_pipe[1]);
		close(result_pipe[0]);
		close(result_pipe[1]);
		close(type_pipe[0]);
		close(type_pipe[1]);
		send_done(FALSE, RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY));
		return TRUE;
	}
	if (pid == 0) {
		close(answer_pipe[1]);
		close(result_pipe[0]);
		close(type_pipe[1]);
		answer_fd = answer_pipe[0];
		// A thread, rather than a signal, which would interrupt the system calls of the format
		if (pthread_create(&type_thread, NULL, ProgressTypeThread, (void*)(intptr_t)type_pipe[0]) == 0)
			pthread_detach(type_thread);
		sigaction(SIGUSR1, &sa, NULL);
		result[0] = job->save ? WorkSave(job) : Work(job);
		result[1] = ErrorStatus;
		_exit((write(result_pipe[1], result, sizeof(result)) == sizeof(result)) ? 0 : 1);
	}
	close(answer_pipe[0]);
	close(result_pipe[1]);
	close(type_pipe[0]);
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
		} else if ((r > 0) && (in.type == IPC_PROGRESS_TYPE)) {
			type = (int32_t)ipc_get_u32(&in);
			if (!ipc_end(&in) || (write(type_pipe[1], &type, sizeof(type)) != sizeof(type)))
				uprintf("Could not forward the progress type to the worker");
		} else {
			// The front end went away (or does not follow the protocol): cancel, and wait for the worker
			kill(pid, SIGUSR1);
			connected = FALSE;
			pfd[0].fd = -1;
		}
		ipc_close_fds(&in);
	}
	close(answer_pipe[1]);
	close(type_pipe[1]);
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
	char relative[PATH_MAX];
	const char* loc_files[] = { GetInstalledFile(RUFUS_LOC_RELATIVE, relative, sizeof(relative)) ? relative : NULL,
		RUFUS_LOC_INSTALLED, RUFUS_LOC_SOURCE };
	size_t i;

	init_localization();
	for (i = 0; i < ARRAYSIZE(loc_files); i++) {
		if ((loc_files[i] == NULL) || (access(loc_files[i], R_OK) != 0))
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
	const rufus_device_t* dev;
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
			if (job.unattend_fd >= 0)
				close(job.unattend_fd);
			ClearFiles();
			ResetOptions(&opts);
			break;
		case IPC_CANCEL:
			// Nothing to cancel
			break;
		case IPC_PROGRESS_TYPE:
			// The next worker inherits it
			type = (int)ipc_get_u32(&in);
			if (!ipc_end(&in) || (type < 0) || (type >= UPT_MAX))
				goto out;
			update_progress_type = type;
			break;
		case IPC_SAVE:
			ErrorStatus = 0;
			if (!ParseSave(&in, &job)) {
				uprintf("Invalid save request");
				send_done(FALSE, RUFUS_ERROR(ERROR_INVALID_PARAMETER));
			} else if ((dev = FindDevice(job.devnode, job.diskseq)) == NULL) {
				send_done(FALSE, RUFUS_ERROR(ERROR_ACCESS_DENIED));
			} else {
				static_strcpy(job.syspath, dev->syspath);
				if (!RunWorker(&job))
					goto out;
			}
			if (job.image_fd >= 0)
				close(job.image_fd);
			job.image_fd = -1;
			break;
		case IPC_CYCLE_PORT:
			ErrorStatus = 0;
			if (ipc_get_str(&in, job.devnode, sizeof(job.devnode)) == NULL)
				job.devnode[0] = 0;
			job.diskseq = ipc_get_u64(&in);
			if ((job.devnode[0] == 0) || !ipc_end(&in)) {
				send_done(FALSE, RUFUS_ERROR(ERROR_INVALID_PARAMETER));
				break;
			}
			dev = FindDevice(job.devnode, job.diskseq);
			if (dev == NULL)
				send_done(FALSE, RUFUS_ERROR(ERROR_ACCESS_DENIED));
			else if (!CyclePort(dev))
				send_done(FALSE, RUFUS_ERROR(ERROR_GEN_FAILURE));
			else
				send_done(TRUE, 0);
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

/*
 * Rufus: The Reliable USB Formatting Utility
 * Front end side of rufus-helper
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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "boot_check.h"
#include "helper_client.h"
#include "progress.h"
#include "ipc.h"

extern char** environ;

// How often a cancellation gets checked, in ms
#define CANCEL_CHECK_INTERVAL       100

static ipc_msg_t in, out;

static void HelperExited(helper_t* h)
{
	int status = 0;

	// First, as the helper exits at the end of the socket
	if (h->sock >= 0)
		close(h->sock);
	h->sock = -1;
	if (h->pid > 0) {
		while ((waitpid(h->pid, &status, 0) < 0) && (errno == EINTR));
		// pkexec: 126 if the user could not be authorized, 127 if they dismissed the dialog
		if (WIFEXITED(status) && ((WEXITSTATUS(status) == 126) || (WEXITSTATUS(status) == 127)))
			uprintf("The helper was not authorized");
		else if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
			uprintf("The helper exited with status 0x%x", status);
	}
	h->pid = -1;
}

BOOL StartHelper(helper_t* h, const char* language)
{
	const char* helper = getenv("RUFUS_HELPER");
	const char* argv[3];
	char text[8 * 1024];
	posix_spawn_file_actions_t actions;
	int sv[2], r;

	h->sock = -1;
	h->pid = -1;
	if ((helper == NULL) || (helper[0] == 0))
		helper = RUFUS_HELPER_INSTALLED;
	// The tests of the front ends replace the helper with one that runs as the user
	if ((getenv("RUFUS_FAKE_HELPER") != NULL) && (getenv("RUFUS_FAKE_HELPER")[0] != 0)) {
		helper = getenv("RUFUS_FAKE_HELPER");
		uprintf("WARNING: Using the fake helper '%s'", helper);
		argv[0] = helper;
		argv[1] = NULL;
	} else if (geteuid() == 0) {
		argv[0] = helper;
		argv[1] = NULL;
	} else {
		argv[0] = "pkexec";
		argv[1] = helper;
		argv[2] = NULL;
	}
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0)
		return FALSE;
	// The helper gets its end of the socket as its standard input
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, sv[1], STDIN_FILENO);
	r = posix_spawnp(&h->pid, argv[0], &actions, NULL, (char* const*)argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(sv[1]);
	if (r != 0) {
		errno = r;
		uprintf("Could not start '%s': %s", argv[0], WindowsErrorString());
		close(sv[0]);
		h->pid = -1;
		return FALSE;
	}
	h->sock = sv[0];
	uprintf("Started %s%s", (argv[1] == NULL) ? "" : "pkexec ", helper);

	ipc_init(&out, IPC_HELLO);
	ipc_put_u32(&out, IPC_VERSION);
	ipc_put_u32(&out, sizeof(rufus_options_t));
	ipc_put_str(&out, language);
	if (!ipc_send(h->sock, &out))
		goto fail;
	// The log of the helper may come first
	while (((r = ipc_recv(h->sock, &in)) > 0) && (in.type == IPC_LOG)) {
		if (ipc_get_str(&in, text, sizeof(text)) != NULL)
			ui_log(text);
		ipc_close_fds(&in);
	}
	if ((r <= 0) || (in.type != IPC_HELLO) || (ipc_get_u32(&in) != IPC_VERSION) || !ipc_end(&in))
		goto fail;
	return TRUE;

fail:
	ipc_close_fds(&in);
	uprintf("The helper did not answer");
	HelperExited(h);
	return FALSE;
}

// Forward a question of the helper to the user
static BOOL Ask(helper_t* h)
{
	char title[1024], message[16 * 1024], setting[256], url[1024];
	char* items[IPC_MAX_LIST_ITEMS], *buf = NULL;
	notification_info more_info;
	ui_question_t q = { 0 };
	int i, answer;
	BOOL ret = FALSE;

	q.type = (enum ui_question_type)ipc_get_u32(&in);
	q.title = ipc_get_str(&in, title, sizeof(title));
	q.message = ipc_get_str(&in, message, sizeof(message));
	if (q.type == UI_QUESTION_NOTIFICATION) {
		q.notification.flags = (int)ipc_get_u32(&in);
		q.notification.dont_display_setting = ipc_get_str(&in, setting, sizeof(setting));
		more_info.id = (WORD)ipc_get_u32(&in);
		more_info.url = ipc_get_str(&in, url, sizeof(url));
		q.notification.more_info = (more_info.id == 0) ? NULL : &more_info;
	} else if (q.type == UI_QUESTION_LIST) {
		q.list.size = (int)ipc_get_u32(&in);
		if ((q.list.size < 0) || (q.list.size > IPC_MAX_LIST_ITEMS))
			return FALSE;
		buf = malloc((size_t)q.list.size * MAX_PATH + 1);
		if (buf == NULL)
			return FALSE;
		for (i = 0; i < q.list.size; i++)
			items[i] = ipc_get_str(&in, &buf[i * MAX_PATH], MAX_PATH);
		q.list.items = items;
	} else {
		return FALSE;
	}
	if (ipc_end(&in) && (q.title != NULL) && (q.message != NULL)) {
		answer = ui_ask(&q);
		ipc_init(&out, IPC_ANSWER);
		ipc_put_u32(&out, (uint32_t)answer);
		ret = ipc_send(h->sock, &out);
	}
	free(buf);
	return ret;
}

BOOL HelperFormat(helper_t* h, const helper_job_t* job)
{
	char names[MAX_BOOT_FILES][MAX_PATH], path[2 * MAX_PATH], text[8 * 1024];
	struct pollfd pfd = { .fd = h->sock, .events = POLLIN };
	uint32_t fd_flags = 0, info, duration, type;
	BOOL cancel_sent = FALSE, ret = FALSE, done = FALSE;
	int i, n, fd, r = 0, progress_type = -1;

	ErrorStatus = 0;
	if (h->sock < 0) {
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_READY);
		return FALSE;
	}

	// The boot files that the boot check found (or downloaded)
	n = GetBootFiles(names, TRUE);
	for (i = 0; i < n; i++) {
		static_sprintf(path, "%s/%s/%s", app_data_dir, FILES_DIR, names[i]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			uprintf("Could not open '%s': %s", path, WindowsErrorString());
			continue;
		}
		ipc_init(&out, IPC_FILE);
		ipc_put_str(&out, names[i]);
		ipc_put_fd(&out, fd);
		r = ipc_send(h->sock, &out);
		close(fd);
		if (!r)
			goto disconnected;
	}

	// What the progress text displays, which the front end may cycle (bare Alt) during the format
	progress_type = update_progress_type;
	ipc_init(&out, IPC_PROGRESS_TYPE);
	ipc_put_u32(&out, (uint32_t)progress_type);
	if (!ipc_send(h->sock, &out))
		goto disconnected;

	ipc_init(&out, IPC_FORMAT);
	ipc_put_u32(&out, sizeof(opts));
	ipc_put_bytes(&out, &opts, sizeof(opts));
	ipc_put_str(&out, job->devnode);
	ipc_put_u64(&out, job->diskseq);
	fd_flags |= (job->image_fd >= 0) ? IPC_FD_IMAGE : 0;
	fd_flags |= (job->archive_fd >= 0) ? IPC_FD_ARCHIVE : 0;
	fd_flags |= (job->bb_log_fd >= 0) ? IPC_FD_BB_LOG : 0;
	ipc_put_u32(&out, fd_flags);
	ipc_put_str(&out, job->image_name);
	ipc_put_str(&out, job->bb_log_path);
	if (job->image_fd >= 0)
		ipc_put_fd(&out, job->image_fd);
	if (job->archive_fd >= 0)
		ipc_put_fd(&out, job->archive_fd);
	if (job->bb_log_fd >= 0)
		ipc_put_fd(&out, job->bb_log_fd);
	if (!ipc_send(h->sock, &out))
		goto disconnected;

	while (!done) {
		if (update_progress_type != progress_type) {
			progress_type = update_progress_type;
			ipc_init(&out, IPC_PROGRESS_TYPE);
			ipc_put_u32(&out, (uint32_t)progress_type);
			if (!ipc_send(h->sock, &out))
				goto disconnected;
		}
		if (!cancel_sent && IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) {
			uprintf("Cancelling the operation of the helper");
			ipc_init(&out, IPC_CANCEL);
			if (!ipc_send(h->sock, &out))
				goto disconnected;
			cancel_sent = TRUE;
		}
		r = poll(&pfd, 1, CANCEL_CHECK_INTERVAL);
		if ((r == 0) || ((r < 0) && (errno == EINTR)))
			continue;
		r = ipc_recv(h->sock, &in);
		if ((r < 0) && (errno == EINTR))
			continue;
		if (r <= 0)
			goto disconnected;
		switch (in.type) {
		case IPC_LOG:
			if (ipc_get_str(&in, text, sizeof(text)) != NULL)
				ui_log(text);
			break;
		case IPC_STATUS:
			info = ipc_get_u32(&in);
			duration = ipc_get_u32(&in);
			ui_status((BOOL)info, duration, ipc_get_str(&in, text, sizeof(text)));
			break;
		case IPC_PROGRESS:
			info = ipc_get_u32(&in);
			ui_progress(NULL, info, (BOOL)ipc_get_u32(&in));
			break;
		case IPC_EVENT:
			type = ipc_get_u32(&in);
			ui_event((enum ui_event_type)type, (intptr_t)(int64_t)ipc_get_u64(&in));
			break;
		case IPC_ASK:
			if (!Ask(h))
				goto disconnected;
			break;
		case IPC_DONE:
			ret = (BOOL)ipc_get_u32(&in);
			// Keep a cancellation that came from here
			type = ipc_get_u32(&in);
			if (!ipc_end(&in))
				goto disconnected;
			if (!cancel_sent || !IS_ERROR(ErrorStatus))
				ErrorStatus = type;
			done = TRUE;
			break;
		default:
			goto disconnected;
		}
		ipc_close_fds(&in);
	}
	return ret && !IS_ERROR(ErrorStatus);

disconnected:
	ipc_close_fds(&in);
	if (r < 0)
		uprintf("Lost the connection to the helper: %s", WindowsErrorString());
	else if (r == 0)
		uprintf("Lost the connection to the helper");
	else
		uprintf("Unexpected message %u from the helper", in.type);
	HelperExited(h);
	if (!IS_ERROR(ErrorStatus))
		ErrorStatus = RUFUS_ERROR(ERROR_GEN_FAILURE);
	return FALSE;
}

BOOL HelperCyclePort(helper_t* h, const char* devnode, uint64_t diskseq)
{
	char text[8 * 1024];
	BOOL ret = FALSE;
	int r;

	if (h->sock < 0)
		return FALSE;
	ipc_init(&out, IPC_CYCLE_PORT);
	ipc_put_str(&out, devnode);
	ipc_put_u64(&out, diskseq);
	if (!ipc_send(h->sock, &out))
		goto disconnected;
	while ((r = ipc_recv(h->sock, &in)) != 0) {
		if ((r < 0) && (errno == EINTR))
			continue;
		if (r < 0)
			goto disconnected;
		if (in.type == IPC_LOG) {
			if (ipc_get_str(&in, text, sizeof(text)) != NULL)
				ui_log(text);
		} else if (in.type == IPC_DONE) {
			ret = (BOOL)ipc_get_u32(&in);
			ipc_get_u32(&in);
			ipc_close_fds(&in);
			return ret;
		} else {
			goto disconnected;
		}
		ipc_close_fds(&in);
	}

disconnected:
	ipc_close_fds(&in);
	uprintf("Lost the connection to the helper");
	HelperExited(h);
	return FALSE;
}

void StopHelper(helper_t* h)
{
	if (h->sock >= 0) {
		// The end of the socket: the helper cancels what it does, and exits
		shutdown(h->sock, SHUT_RDWR);
		HelperExited(h);
	}
}

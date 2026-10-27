/*
 * Rufus: The Reliable USB Formatting Utility
 * A fake rufus-helper, for the tests of the front ends
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
 * Speaks the protocol of rufus-helper (ipc.h) as the user, writes nothing,
 * and records what it gets as JSON lines in $RUFUS_FAKE_HELPER_LOG. A format
 * reports some progress over $RUFUS_FAKE_HELPER_STEPS steps of
 * $RUFUS_FAKE_HELPER_DELAY ms, and can be cancelled. With
 * $RUFUS_FAKE_HELPER_ASK, it asks a question half way, and with
 * $RUFUS_FAKE_HELPER_FAIL, the format fails with a write fault.
 * The front ends start it instead of the real helper when $RUFUS_FAKE_HELPER
 * points to it.
 */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "ipc.h"

#define SOCK STDIN_FILENO

static ipc_msg_t in, out;
static FILE* log_file = NULL;

static void record(const char* format, ...)
{
	va_list args;

	if (log_file == NULL)
		return;
	va_start(args, format);
	vfprintf(log_file, format, args);
	va_end(args);
	fputc('\n', log_file);
	fflush(log_file);
}

static void json_str(char* dst, size_t size, const char* src)
{
	size_t i = 0;

	for (; (src != NULL) && (*src != 0) && (i + 7 < size); src++) {
		if ((*src == '"') || (*src == '\\')) {
			dst[i++] = '\\';
			dst[i++] = *src;
		} else if ((unsigned char)*src < 0x20) {
			i += snprintf(&dst[i], size - i, "\\u%04x", (unsigned char)*src);
		} else {
			dst[i++] = *src;
		}
	}
	dst[i] = 0;
}

static BOOL send_done(BOOL ret, DWORD status)
{
	ipc_init(&out, IPC_DONE);
	ipc_put_u32(&out, (uint32_t)ret);
	ipc_put_u32(&out, (uint32_t)status);
	return ipc_send(SOCK, &out);
}

static BOOL send_status(BOOL info, const char* text)
{
	ipc_init(&out, IPC_STATUS);
	ipc_put_u32(&out, (uint32_t)info);
	ipc_put_u32(&out, 0);
	ipc_put_str(&out, text);
	return ipc_send(SOCK, &out);
}

/* Wait for the messages of the front end for up to ms: returns FALSE if it cancelled or went away */
static BOOL wait_front_end(int ms, int32_t* answer)
{
	struct pollfd pfd = { .fd = SOCK, .events = POLLIN };
	int r;

	while (poll(&pfd, 1, ms) > 0) {
		r = ipc_recv(SOCK, &in);
		if (r <= 0)
			return FALSE;
		switch (in.type) {
		case IPC_CANCEL:
			record("{\"event\": \"cancel\"}");
			ipc_close_fds(&in);
			return FALSE;
		case IPC_PROGRESS_TYPE:
			record("{\"event\": \"progress_type\", \"type\": %u}", ipc_get_u32(&in));
			break;
		case IPC_ANSWER:
			if (answer != NULL)
				*answer = (int32_t)ipc_get_u32(&in);
			ipc_close_fds(&in);
			return TRUE;
		default:
			record("{\"event\": \"unexpected\", \"type\": %u}", in.type);
			break;
		}
		ipc_close_fds(&in);
		ms = 0;
	}
	return TRUE;
}

static BOOL format(void)
{
	const char* env;
	char devnode[64], image[MAX_PATH], bb_log[MAX_PATH], label[512];
	int steps = 20, delay = 50, i;
	uint64_t diskseq;
	uint32_t fd_flags;
	int32_t answer = -1;
	rufus_options_t o;

	if ((ipc_get_u32(&in) != sizeof(o)) || !ipc_get_bytes(&in, &o, sizeof(o)) ||
		(ipc_get_str(&in, devnode, sizeof(devnode)) == NULL))
		return send_done(FALSE, RUFUS_ERROR(ERROR_INVALID_PARAMETER));
	diskseq = ipc_get_u64(&in);
	fd_flags = ipc_get_u32(&in);
	if (ipc_get_str(&in, image, sizeof(image)) == NULL)
		image[0] = 0;
	if (ipc_get_str(&in, bb_log, sizeof(bb_log)) == NULL)
		bb_log[0] = 0;
	json_str(label, sizeof(label), o.label);
	record("{\"event\": \"format\", \"devnode\": \"%s\", \"diskseq\": %llu, \"boot_type\": %d, "
		"\"partition_type\": %d, \"target_type\": %d, \"fs_type\": %d, \"cluster_size\": %u, \"label\": \"%s\", "
		"\"quick_format\": %d, \"extended_label\": %d, \"bad_blocks\": %d, \"nb_passes_sel\": %d, "
		"\"zero_drive\": %d, \"fast_zeroing\": %d, \"persistence_size\": %llu, \"write_as_image\": %d, "
		"\"thread_priority\": %d, \"detect_fakes\": %d, \"use_rufus_mbr\": %d, \"lock_drive\": %d, "
		"\"image_fd\": %d, \"image\": \"%s\"}", devnode, (unsigned long long)diskseq, o.boot_type, o.partition_type,
		o.target_type, o.fs_type, o.cluster_size, label, o.quick_format, o.extended_label, o.bad_blocks,
		o.nb_passes_sel, o.zero_drive, o.fast_zeroing, (unsigned long long)o.persistence_size, o.write_as_image,
		o.thread_priority, o.detect_fakes, o.use_rufus_mbr, o.lock_drive, (fd_flags & IPC_FD_IMAGE) ? 1 : 0, image);
	ipc_close_fds(&in);

	if ((env = getenv("RUFUS_FAKE_HELPER_STEPS")) != NULL)
		steps = atoi(env);
	if ((env = getenv("RUFUS_FAKE_HELPER_DELAY")) != NULL)
		delay = atoi(env);
	if (!send_status(TRUE, "Fake format...") || !send_status(FALSE, "Writing nothing"))
		return FALSE;
	for (i = 0; i <= steps; i++) {
		ipc_init(&out, IPC_PROGRESS);
		ipc_put_u32(&out, (uint32_t)((uint64_t)i * MAX_PROGRESS / ((steps == 0) ? 1 : steps)));
		ipc_put_u32(&out, TRUE);
		if (!ipc_send(SOCK, &out))
			return FALSE;
		if (!wait_front_end(delay, NULL)) {
			record("{\"event\": \"done\", \"result\": \"cancelled\"}");
			return send_done(FALSE, RUFUS_ERROR(ERROR_CANCELLED));
		}
		if ((i == steps / 2) && (getenv("RUFUS_FAKE_HELPER_ASK") != NULL)) {
			ipc_init(&out, IPC_ASK);
			ipc_put_u32(&out, UI_QUESTION_NOTIFICATION);
			ipc_put_str(&out, "Fake question");
			ipc_put_str(&out, "Do you want to go on?");
			ipc_put_u32(&out, MB_YESNO | MB_ICONWARNING);
			ipc_put_str(&out, NULL);
			ipc_put_u32(&out, 0);
			ipc_put_str(&out, NULL);
			if (!ipc_send(SOCK, &out))
				return FALSE;
			while ((answer < 0) && wait_front_end(-1, &answer));
			record("{\"event\": \"answer\", \"answer\": %d}", answer);
			if (answer != IDYES) {
				record("{\"event\": \"done\", \"result\": \"cancelled\"}");
				return send_done(FALSE, RUFUS_ERROR(ERROR_CANCELLED));
			}
		}
	}
	if (getenv("RUFUS_FAKE_HELPER_FAIL") != NULL) {
		record("{\"event\": \"done\", \"result\": \"failed\"}");
		return send_done(FALSE, RUFUS_ERROR(ERROR_WRITE_FAULT));
	}
	record("{\"event\": \"done\", \"result\": \"ok\"}");
	return send_done(TRUE, 0);
}

// IPC_SAVE: a fake image of the drive, written with progress
static BOOL save(void)
{
	char devnode[64], image[MAX_PATH];
	uint64_t diskseq;
	uint32_t type;
	int i, fd;

	if (ipc_get_str(&in, devnode, sizeof(devnode)) == NULL)
		return send_done(FALSE, RUFUS_ERROR(ERROR_INVALID_PARAMETER));
	diskseq = ipc_get_u64(&in);
	type = ipc_get_u32(&in);
	if (ipc_get_str(&in, image, sizeof(image)) == NULL)
		image[0] = 0;
	fd = (in.nfds == 1) ? ipc_take_fd(&in, 0) : -1;
	ipc_close_fds(&in);
	record("{\"event\": \"save\", \"devnode\": \"%s\", \"diskseq\": %llu, \"type\": %u, \"image\": \"%s\", "
		"\"image_fd\": %d}", devnode, (unsigned long long)diskseq, type, image, (fd >= 0) ? 1 : 0);
	if (fd < 0)
		return send_done(FALSE, RUFUS_ERROR(ERROR_INVALID_PARAMETER));
	for (i = 0; i <= 10; i++) {
		ipc_init(&out, IPC_PROGRESS);
		ipc_put_u32(&out, (uint32_t)((uint64_t)i * MAX_PROGRESS / 10));
		ipc_put_u32(&out, TRUE);
		if (!ipc_send(SOCK, &out) || !wait_front_end(20, NULL)) {
			close(fd);
			record("{\"event\": \"done\", \"result\": \"cancelled\"}");
			return send_done(FALSE, RUFUS_ERROR(ERROR_CANCELLED));
		}
	}
	i = (write(fd, "Fake image", 10) == 10);
	close(fd);
	record("{\"event\": \"done\", \"result\": \"%s\"}", i ? "ok" : "failed");
	return send_done(i, i ? 0 : RUFUS_ERROR(ERROR_WRITE_FAULT));
}

int main(int argc, char** argv)
{
	char name[MAX_PATH], devnode[64];
	const char* path = getenv("RUFUS_FAKE_HELPER_LOG");
	BOOL ok = TRUE;
	int r;

	if (path != NULL)
		log_file = fopen(path, "a");
	if ((ipc_recv(SOCK, &in) != 1) || (in.type != IPC_HELLO) || (ipc_get_u32(&in) != IPC_VERSION) ||
		(ipc_get_u32(&in) != sizeof(rufus_options_t)))
		return 1;
	record("{\"event\": \"hello\"}");
	ipc_init(&out, IPC_HELLO);
	ipc_put_u32(&out, IPC_VERSION);
	if (!ipc_send(SOCK, &out))
		return 1;
	while (ok && ((r = ipc_recv(SOCK, &in)) != 0)) {
		if (r < 0)
			break;
		switch (in.type) {
		case IPC_FILE:
			record("{\"event\": \"file\", \"name\": \"%s\"}", ipc_get_str(&in, name, sizeof(name)));
			break;
		case IPC_PROGRESS_TYPE:
			record("{\"event\": \"progress_type\", \"type\": %u}", ipc_get_u32(&in));
			break;
		case IPC_FORMAT:
			ok = format();
			continue;
		case IPC_SAVE:
			ok = save();
			continue;
		case IPC_CYCLE_PORT:
			record("{\"event\": \"cycle_port\", \"devnode\": \"%s\"}", ipc_get_str(&in, devnode, sizeof(devnode)));
			ok = send_done(TRUE, 0);
			break;
		case IPC_CANCEL:
			break;
		default:
			record("{\"event\": \"unexpected\", \"type\": %u}", in.type);
			break;
		}
		ipc_close_fds(&in);
	}
	record("{\"event\": \"exit\"}");
	if (log_file != NULL)
		fclose(log_file);
	return 0;
}

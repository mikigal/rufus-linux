/*
 * Rufus: The Reliable USB Formatting Utility
 * The protocol between the front ends and rufus-helper
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
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "ipc.h"

#define IPC_NULL_STR                0xffffffff

void ipc_init(ipc_msg_t* m, uint32_t type)
{
	int i;

	m->type = type;
	m->len = 0;
	m->pos = 0;
	m->error = FALSE;
	m->nfds = 0;
	for (i = 0; i < IPC_MAX_FDS; i++)
		m->fds[i] = -1;
	ipc_put_u32(m, type);
}

void ipc_put_bytes(ipc_msg_t* m, const void* buf, size_t len)
{
	if (m->error || (len > sizeof(m->data) - m->len)) {
		m->error = TRUE;
		return;
	}
	if (len != 0)
		memcpy(&m->data[m->len], buf, len);
	m->len += len;
}

void ipc_put_u32(ipc_msg_t* m, uint32_t v)
{
	ipc_put_bytes(m, &v, sizeof(v));
}

void ipc_put_u64(ipc_msg_t* m, uint64_t v)
{
	ipc_put_bytes(m, &v, sizeof(v));
}

void ipc_put_str(ipc_msg_t* m, const char* str)
{
	size_t len = (str == NULL) ? 0 : strlen(str);

	if (len >= IPC_NULL_STR) {
		m->error = TRUE;
		return;
	}
	ipc_put_u32(m, (str == NULL) ? IPC_NULL_STR : (uint32_t)len);
	ipc_put_bytes(m, str, len);
}

void ipc_put_fd(ipc_msg_t* m, int fd)
{
	if ((fd < 0) || (m->nfds >= IPC_MAX_FDS)) {
		m->error = TRUE;
		return;
	}
	m->fds[m->nfds++] = fd;
}

BOOL ipc_send(int sock, ipc_msg_t* m)
{
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int) * IPC_MAX_FDS)];
	} control;
	struct iovec iov = { m->data, m->len };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	struct cmsghdr* cmsg;
	ssize_t r;

	if (m->error) {
		errno = EMSGSIZE;
		return FALSE;
	}
	if (m->nfds > 0) {
		memset(&control, 0, sizeof(control));
		msg.msg_control = control.buf;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * m->nfds);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * m->nfds);
		memcpy(CMSG_DATA(cmsg), m->fds, sizeof(int) * m->nfds);
	}
	do {
		r = sendmsg(sock, &msg, MSG_NOSIGNAL);
	} while ((r < 0) && (errno == EINTR));
	return (r == (ssize_t)m->len);
}

int ipc_recv(int sock, ipc_msg_t* m)
{
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int) * IPC_MAX_FDS)];
	} control;
	struct iovec iov = { m->data, sizeof(m->data) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.buf, .msg_controllen = sizeof(control.buf) };
	struct cmsghdr* cmsg;
	ssize_t r;
	int i, n;

	ipc_init(m, 0);
	m->len = 0;
	r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
	if (r <= 0)
		return (r == 0) ? 0 : -1;
	// Take the descriptors first, so that they get closed whatever happens next
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if ((cmsg->cmsg_level != SOL_SOCKET) || (cmsg->cmsg_type != SCM_RIGHTS))
			continue;
		n = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		for (i = 0; i < n; i++) {
			int fd;
			memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
			if (m->nfds < IPC_MAX_FDS)
				m->fds[m->nfds++] = fd;
			else
				close(fd);
		}
	}
	m->len = (size_t)r;
	if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || (m->len < sizeof(uint32_t))) {
		ipc_close_fds(m);
		errno = EBADMSG;
		return -1;
	}
	m->type = ipc_get_u32(m);
	return 1;
}

BOOL ipc_get_bytes(ipc_msg_t* m, void* buf, size_t len)
{
	if (m->error || (len > m->len - m->pos)) {
		m->error = TRUE;
		if (len != 0)
			memset(buf, 0, len);
		return FALSE;
	}
	if (len != 0)
		memcpy(buf, &m->data[m->pos], len);
	m->pos += len;
	return TRUE;
}

uint32_t ipc_get_u32(ipc_msg_t* m)
{
	uint32_t v;

	ipc_get_bytes(m, &v, sizeof(v));
	return v;
}

uint64_t ipc_get_u64(ipc_msg_t* m)
{
	uint64_t v;

	ipc_get_bytes(m, &v, sizeof(v));
	return v;
}

char* ipc_get_str(ipc_msg_t* m, char* buf, size_t size)
{
	uint32_t len = ipc_get_u32(m);

	if (m->error || (len == IPC_NULL_STR))
		return NULL;
	if ((len >= size) || (len > m->len - m->pos) || (memchr(&m->data[m->pos], 0, len) != NULL)) {
		m->error = TRUE;
		return NULL;
	}
	memcpy(buf, &m->data[m->pos], len);
	buf[len] = 0;
	m->pos += len;
	return buf;
}

BOOL ipc_end(const ipc_msg_t* m)
{
	return !m->error && (m->pos == m->len);
}

int ipc_take_fd(ipc_msg_t* m, int index)
{
	int fd;

	if ((index < 0) || (index >= m->nfds))
		return -1;
	fd = m->fds[index];
	m->fds[index] = -1;
	return fd;
}

void ipc_close_fds(ipc_msg_t* m)
{
	int i;

	for (i = 0; i < m->nfds; i++) {
		if (m->fds[i] >= 0)
			close(m->fds[i]);
		m->fds[i] = -1;
	}
	m->nfds = 0;
}

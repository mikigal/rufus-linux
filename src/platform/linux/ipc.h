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

#pragma once

#include "rufus.h"

/*
 * The front end (the GUI, or rufus-cli) and rufus-helper exchange messages on
 * a SOCK_SEQPACKET socket, the standard input of the helper. A message is a
 * type, then its fields (native byte order, as both ends run on the same
 * machine and come from the same build), with up to IPC_MAX_FDS file
 * descriptors (SCM_RIGHTS). The end of the socket means cancel.
 *
 * Front end → helper:
 *   IPC_HELLO    u32 version, u32 sizeof(rufus_options_t), str language (e.g. "en-US")
 *   IPC_FILE     str name (relative to FILES_DIR, see GetBootFiles()), fd
 *   IPC_FORMAT   bytes opts, str devnode, u64 diskseq, u32 fd_flags (IPC_FD_*),
 *                str image name, str bad blocks report path, then the fds of fd_flags in order
 *                (image, archive, bad blocks report, unattend.xml)
 *   IPC_ANSWER   i32 answer (to IPC_ASK)
 *   IPC_CANCEL
 *   IPC_PROGRESS_TYPE u32 type (UPT_*): what the progress text displays, at any time
 *   IPC_CYCLE_PORT str devnode, u64 diskseq: disconnect a USB drive and connect it again (Alt-C),
 *                answered by IPC_DONE
 *   IPC_SAVE     str devnode, u64 diskseq, u32 type (SAVE_*), str image name, fd image: save a
 *                drive to an image, with the messages of a format until IPC_DONE
 * Helper → front end:
 *   IPC_HELLO    u32 version
 *   IPC_LOG      str text
 *   IPC_STATUS   u32 info, u32 duration, str text (may be NULL)
 *   IPC_PROGRESS u32 pos, u32 taskbar
 *   IPC_EVENT    u32 type, i64 param
 *   IPC_ASK      u32 question type, str title, str message, then for notifications: i32 flags,
 *                str dont_display_setting, u32 more_info id (0: none), str more_info url;
 *                for lists: u32 size, then the items
 *   IPC_DONE     u32 ret, u32 ErrorStatus (the end of IPC_FORMAT or IPC_CYCLE_PORT)
 */

#define IPC_VERSION                 3
#define IPC_MAX_MESSAGE             (64 * 1024)
#define IPC_MAX_FDS                 4
#define IPC_MAX_LIST_ITEMS          64

enum ipc_type {
	IPC_HELLO = 1,
	IPC_FILE,
	IPC_FORMAT,
	IPC_ANSWER,
	IPC_CANCEL,
	IPC_LOG,
	IPC_STATUS,
	IPC_PROGRESS,
	IPC_EVENT,
	IPC_ASK,
	IPC_DONE,
	IPC_PROGRESS_TYPE,
	IPC_CYCLE_PORT,
	IPC_SAVE,
};

/* The file descriptors that come with IPC_FORMAT */
#define IPC_FD_IMAGE                0x01	// The image (ISO mode or DD mode)
#define IPC_FD_ARCHIVE              0x02	// The archive to extract to the drive (archive_path)
#define IPC_FD_BB_LOG               0x04	// The report of the bad blocks check
#define IPC_FD_UNATTEND             0x08	// The unattend.xml of the Windows User Experience (unattend_xml_path)

typedef struct {
	uint32_t type;
	size_t len;						// Of data
	size_t pos;						// Where the next field gets read
	BOOL error;						// A field did not fit, or could not be read
	int nfds;
	int fds[IPC_MAX_FDS];			// -1 once taken with ipc_take_fd()
	uint8_t data[IPC_MAX_MESSAGE];
} ipc_msg_t;

/* Writing a message: fields that don't fit set m->error, and ipc_send() then fails */
extern void ipc_init(ipc_msg_t* m, uint32_t type);
extern void ipc_put_u32(ipc_msg_t* m, uint32_t v);
extern void ipc_put_u64(ipc_msg_t* m, uint64_t v);
extern void ipc_put_bytes(ipc_msg_t* m, const void* buf, size_t len);
extern void ipc_put_str(ipc_msg_t* m, const char* str);	// NULL is allowed
// The descriptor is not duplicated: it must stay open until ipc_send()
extern void ipc_put_fd(ipc_msg_t* m, int fd);
extern BOOL ipc_send(int sock, ipc_msg_t* m);

/*
 * Reading a message: returns 1 for a message, 0 at the end of the socket and -1
 * on error (EINTR included). The descriptors of a message are close on exec,
 * and get closed by ipc_close_fds() unless taken.
 */
extern int ipc_recv(int sock, ipc_msg_t* m);
extern uint32_t ipc_get_u32(ipc_msg_t* m);
extern uint64_t ipc_get_u64(ipc_msg_t* m);
// Copies exactly len bytes
extern BOOL ipc_get_bytes(ipc_msg_t* m, void* buf, size_t len);
// A string that must fit in size (NUL included) and have no NUL; NULL if it was NULL (or on error)
extern char* ipc_get_str(ipc_msg_t* m, char* buf, size_t size);
// TRUE if the whole message was read, without error
extern BOOL ipc_end(const ipc_msg_t* m);
extern int ipc_take_fd(ipc_msg_t* m, int index);
extern void ipc_close_fds(ipc_msg_t* m);

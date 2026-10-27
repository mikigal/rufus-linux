/*
 * Rufus: The Reliable USB Formatting Utility
 * Save an optical disc to an ISO image
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
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
 * OpticalDiscSaveImageThread() of iso.c (Alt-O), on file descriptors: the front
 * end finds the disc (GetOpticalMedia() of the platform), opens the image, and
 * runs this in a thread of its own. The ISO of the files of a drive, which
 * upstream makes with oscdimg.exe, is made by the helper (with xorriso).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"

BOOL SaveOpticalDisc(const char* devnode, uint64_t disc_size, int dst_fd)
{
	BOOL ret = FALSE;
	ssize_t rSize, wSize = 0;
	uint8_t* buffer = NULL;
	uint64_t wb;
	uint32_t buf_size;
	int i, fd;

	PrintInfoDebug(0, MSG_225);
	fd = open(devnode, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open %s: %s", devnode, WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}

	// Adjust the buffer size according to the disc size so that we get a decent speed.
	for (buf_size = 32 * MB; (buf_size > 8 * MB) && (disc_size <= (uint64_t)buf_size * 64); buf_size /= 2);
	buffer = (uint8_t*)_mm_malloc(buf_size, 16);
	if (buffer == NULL) {
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
		uprintf("Could not allocate buffer");
		goto out;
	}

	uprintf("Will use a buffer size of %s", SizeToHumanReadable(buf_size, FALSE, FALSE));
	uprintf("ISO media size %s", SizeToHumanReadable(disc_size, FALSE, FALSE));

	UpdateProgressWithInfoInit(NULL, FALSE);
	for (wb = 0; ; wb += (uint64_t)wSize) {
		// NB: Optical drives do not appear to increment the sectors to read automatically on Windows
		do {
			rSize = pread(fd, buffer, (size_t)MIN(buf_size, disc_size - wb), (off_t)wb);
		} while ((rSize < 0) && (errno == EINTR));
		if (rSize < 0) {
			ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
			uprintf("Read error: %s", WindowsErrorString());
			goto out;
		}
		if (rSize == 0)
			break;
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, wb, disc_size);
		for (i = 1; i <= WRITE_RETRIES; i++) {
			CHECK_FOR_USER_CANCEL;
			wSize = pwrite(dst_fd, buffer, (size_t)rSize, (off_t)wb);
			if (wSize == rSize)
				break;
			if (wSize >= 0)
				uprintf("Write error: Wrote %zd bytes, expected %zd bytes", wSize, rSize);
			else
				uprintf("Write error: %s", WindowsErrorString());
			if (i < WRITE_RETRIES) {
				uprintf("Retrying in %d seconds...", WRITE_TIMEOUT / 1000);
				usleep(WRITE_TIMEOUT * 1000);
			} else {
				ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
				goto out;
			}
			usleep(200000);
		}
	}
	if (wb != disc_size) {
		uprintf("Error: wrote %s, expected %s", SizeToHumanReadable(wb, FALSE, FALSE),
			SizeToHumanReadable(disc_size, FALSE, FALSE));
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	if (fsync(dst_fd) != 0) {
		uprintf("Write error: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	UpdateProgressWithInfo(OP_FORMAT, MSG_261, disc_size, disc_size);
	uprintf("Operation complete (Wrote %s).", SizeToHumanReadable(wb, FALSE, FALSE));
	ret = TRUE;

out:
	safe_mm_free(buffer);
	if (fd >= 0)
		close(fd);
	return ret;
}

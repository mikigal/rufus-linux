/*
 * Rufus: The Reliable USB Formatting Utility
 * Disk image writing and drive zeroing
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
 * WriteDrive() of format.c, on file descriptors. The image is read with
 * pread() rather than asynchronously, and a VHD or VHDX is written from the
 * disk it holds (vdisk.c), as the image upstream mounts with VirtDisk would give
 * it. FFU images need DISM, which Linux does not have.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "write_drive.h"
#include "vdisk.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"
#include "bled/bled.h"

#define IMG_COMPRESSION_FFU         (BLED_COMPRESSION_MAX)
#define IMG_COMPRESSION_VHD         (BLED_COMPRESSION_MAX + 1)
#define IMG_COMPRESSION_VHDX        (BLED_COMPRESSION_MAX + 2)

static uint8_t* sec_buf = NULL;
static unsigned int sec_buf_pos = 0;
static uint32_t sec_size = 512;

static void update_progress(const int64_t processed_bytes)
{
	static uint64_t total_bytes;

	if (processed_bytes < 0) {
		total_bytes = -processed_bytes;
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, 0, total_bytes);
		uprint_progress(0, total_bytes);
	} else {
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, processed_bytes, total_bytes);
		uprint_progress(processed_bytes, total_bytes);
	}
}

// Some compressed images use streams that aren't multiple of the sector
// size and cause write failures => Use a write override that alleviates
// the problem. See GitHub issue #1422 for details.
static int sector_write(int fd, const void* _buf, unsigned int count)
{
	const uint8_t* buf = (const uint8_t*)_buf;
	int written, fill_size = 0;

	if_assert_fails(sec_size <= 64 * KB)
		return -1;
	if_assert_fails(count <= 1 * GB)
		return -1;

	// If we are on a sector boundary and count is multiple of the
	// sector size, just issue a regular write
	if ((sec_buf_pos == 0) && (count % sec_size == 0))
		return (int)write(fd, buf, count);

	// If we have an existing partial sector, fill and write it
	if (sec_buf_pos > 0) {
		if_assert_fails(sec_size >= sec_buf_pos)
			return -1;
		fill_size = min(sec_size - sec_buf_pos, count);
		memcpy(&sec_buf[sec_buf_pos], buf, fill_size);
		sec_buf_pos += fill_size;
		// If we don't have a full sector just buffer it for next call
		if (sec_buf_pos < sec_size)
			return (int)count;
		sec_buf_pos = 0;
		written = (int)write(fd, sec_buf, sec_size);
		if (written != (int)sec_size)
			return written;
	}

	// Now write as many full sectors as we can
	uint32_t sec_num = (count - fill_size) / sec_size;
	written = (int)write(fd, &buf[fill_size], sec_num * sec_size);
	if (written < 0)
		return written;
	if (written != (int)(sec_num * sec_size)) {
		// Detect overflows
		int v = fill_size + written;
		if_assert_fails(v >= fill_size)
			return -1;
		else
			return v;
	}
	sec_buf_pos = count - fill_size - written;
	if_assert_fails(sec_buf_pos < sec_size)
		return -1;

	// Keep leftover bytes, if any, in the sector buffer
	if (sec_buf_pos != 0)
		memcpy(sec_buf, &buf[fill_size + written], sec_buf_pos);
	return (int)count;
}

// Write a buffer at an offset, retrying as upstream does
static BOOL WriteWithRetries(int fd, const uint8_t* buf, uint32_t size, uint64_t offset, uint32_t sector_size)
{
	ssize_t w;
	int i;

	for (i = 1; i <= WRITE_RETRIES; i++) {
		if (IS_ERROR(ErrorStatus))
			return FALSE;
		w = pwrite(fd, buf, size, (off_t)offset);
		if (w == (ssize_t)size)
			return TRUE;
		if (w >= 0)
			uprintf("\nWrite error: Wrote %zd bytes, expected %u bytes", w, size);
		else
			uprintf("\nWrite error at sector %llu: %s", (unsigned long long)(offset / sector_size), WindowsErrorString());
		if (i < WRITE_RETRIES) {
			uprintf("Retrying in %d seconds...", WRITE_TIMEOUT / 1000);
			usleep(WRITE_TIMEOUT * 1000);
		}
	}
	ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
	return FALSE;
}

BOOL WriteDrive(int fd, uint64_t disk_size, uint32_t sector_size, int src_fd, BOOL bZeroDrive)
{
	BOOL ret = FALSE;
	uint32_t i, read_size, buf_size;
	uint64_t wb, target_size = bZeroDrive ? disk_size : MIN(disk_size, img_report.image_size);
	int64_t bled_ret;
	ssize_t rs;
	uint8_t* buffer = NULL;
	uint32_t zero_data, *cmp_buffer = NULL;
	int throttle_fast_zeroing = 0;
	vdisk_t* vd = NULL;

	if (sector_size < 512) {
		uprintf("Unexpected sector size (%d) - Aborting", sector_size);
		return FALSE;
	}
	UpdateProgressWithInfoInit(NULL, FALSE);
	// Our buffer size must be a multiple of the sector size and *ALIGNED* to the sector size
	buf_size = ((DD_BUFFER_SIZE + sector_size - 1) / sector_size) * sector_size;

	if (bZeroDrive) {
		uprintf(opts.fast_zeroing ? "Fast-zeroing drive:" : "Zeroing drive:");
		buffer = (uint8_t*)_mm_malloc(buf_size, sector_size);
		if (buffer == NULL) {
			ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
			uprintf("Could not allocate disk zeroing buffer");
			goto out;
		}

		// Clear buffer
		memset(buffer, opts.fast_zeroing ? 0xff : 0x00, buf_size);

		if (opts.fast_zeroing) {
			cmp_buffer = (uint32_t*)_mm_malloc(buf_size, sector_size);
			if (cmp_buffer == NULL) {
				ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
				uprintf("Could not allocate disk comparison buffer");
				goto out;
			}
		}

		read_size = buf_size;
		uprint_progress(0, 0);
		for (wb = 0; wb < target_size; wb += read_size) {
			UpdateProgressWithInfo(OP_FORMAT, opts.fast_zeroing ? MSG_306 : MSG_286, wb, target_size);
			uprint_progress(wb, target_size);
			CHECK_FOR_USER_CANCEL;
			// Don't overflow our projected size
			if (wb + read_size > target_size)
				read_size = (uint32_t)(target_size - wb);
			if (read_size % sector_size != 0)
				read_size = ((read_size + sector_size - 1) / sector_size) * sector_size;

			// Fast-zeroing: Depending on your hardware, reading from flash may be much faster than writing, so
			// we might speed things up by skipping empty blocks, or skipping the write if the data is the same.
			// Notes: A block is declared empty when all bits are either 0 (zeros) or 1 (flash block erased).
			// Also, a back-off strategy is used to limit reading.
			if (throttle_fast_zeroing) {
				throttle_fast_zeroing--;
			} else if (opts.fast_zeroing) {
				// Read block and compare against the block that needs to be written
				rs = pread(fd, cmp_buffer, read_size, (off_t)wb);
				if (rs != (ssize_t)read_size) {
					uprintf("\nRead error: Could not read data for fast zeroing comparison - %s", WindowsErrorString());
					ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
					goto out;
				}

				// Check for an empty block by comparing with the first element
				zero_data = cmp_buffer[0];
				// Check all bits are the same
				if ((zero_data == 0) || (zero_data == 0xffffffff)) {
					// Compare the rest of the block against the first element
					for (i = 1; (i < read_size / sizeof(uint32_t)) && (cmp_buffer[i] == zero_data); i++);
					if (i >= read_size / sizeof(uint32_t))
						// Block is empty, skip write
						continue;
				}
				// Throttle read operations
				throttle_fast_zeroing = 15;
			}
			if (!WriteWithRetries(fd, buffer, read_size, wb, sector_size))
				goto out;
		}
		uprintfs("\n");
	} else if (img_report.compression_type != BLED_COMPRESSION_NONE && img_report.compression_type < BLED_COMPRESSION_MAX) {
		uprintf("Writing compressed image:");
		sec_size = sector_size;
		sec_buf = (uint8_t*)_mm_malloc(sector_size, sector_size);
		if (sec_buf == NULL) {
			ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
			uprintf("Could not allocate disk write buffer");
			goto out;
		}
		sec_buf_pos = 0;
		// bled writes sequentially, from the start of the drive
		if ((lseek(src_fd, 0, SEEK_SET) != 0) || (lseek(fd, 0, SEEK_SET) != 0)) {
			uprintf("Could not rewind the image or the drive: %s", WindowsErrorString());
			ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
			goto out;
		}
		bled_init(256 * KB, uprintf, NULL, sector_write, update_progress, NULL, &ErrorStatus);
		bled_ret = bled_uncompress_with_fds(src_fd, fd, img_report.compression_type);
		bled_exit();
		uprintfs("\n");
		if ((bled_ret >= 0) && (sec_buf_pos != 0)) {
			// A disk image that doesn't end up on disk boundary should be a rare
			// enough case, so we dont bother checking the write operation and
			// just issue a notice about it in the log.
			uprintf("Notice: Compressed image data didn't end on block boundary.");
			memset(&sec_buf[sec_buf_pos], 0, sector_size - sec_buf_pos);
			IGNORE_RETVAL(write(fd, sec_buf, sector_size));
		}
		safe_mm_free(sec_buf);
		if ((bled_ret < 0) && (SCODE_CODE(ErrorStatus) != ERROR_CANCELLED)) {
			// Unfortunately, different compression backends return different negative error codes
			uprintf("Could not write compressed image: %lld", (long long)bled_ret);
			ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
			goto out;
		}
		if (IS_ERROR(ErrorStatus))
			goto out;
	} else {
		if (img_report.compression_type == IMG_COMPRESSION_FFU) {
			uprintf("FFU images are not supported on Linux");
			ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
			goto out;
		}
		// NB: Upstream mounts VHD and VHDX images with VirtDisk, and writes the disk they provide
		if ((img_report.compression_type == IMG_COMPRESSION_VHD) || (img_report.compression_type == IMG_COMPRESSION_VHDX)) {
			vd = VdiskOpen(src_fd, (img_report.compression_type == IMG_COMPRESSION_VHD) ? VDISK_VHD : VDISK_VHDX);
			if (vd == NULL) {
				uprintf("Could not open the virtual disk image");
				ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
				goto out;
			}
			target_size = MIN(disk_size, VdiskSize(vd));
		}

		buffer = (uint8_t*)_mm_malloc(buf_size, sector_size);
		if (buffer == NULL) {
			ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
			uprintf("Could not allocate disk write buffer");
			goto out;
		}

		uprint_progress(0, 0);
		for (wb = 0; wb < target_size; wb += read_size) {
			// 0. Update the progress
			UpdateProgressWithInfo(OP_FORMAT, MSG_261, wb, target_size);
			uprint_progress(wb, target_size);
			CHECK_FOR_USER_CANCEL;

			// 1. Read the data, without going past the source or target sizes
			if (vd != NULL) {
				rs = VdiskRead(vd, buffer, (size_t)MIN(buf_size, target_size - wb), wb);
			} else do {
				rs = pread(src_fd, buffer, (size_t)MIN(buf_size, target_size - wb), (off_t)wb);
			} while ((rs < 0) && (errno == EINTR));
			if (rs <= 0) {
				uprintf("\nRead error: %s", (rs == 0) ? "Unexpected end of image" : WindowsErrorString());
				ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
				goto out;
			}
			read_size = (uint32_t)rs;

			// 2. The writes must be a multiple of the sector size
			if (read_size % sector_size != 0) {
				memset(&buffer[read_size], 0, CEILING_ALIGN(read_size, sector_size) - read_size);
				read_size = CEILING_ALIGN(read_size, sector_size);
			}

			// 3. Write the data
			if (!WriteWithRetries(fd, buffer, read_size, wb, sector_size))
				goto out;
		}
		uprintfs("\n");
	}
	if (fsync(fd) != 0) {
		uprintf("Could not flush the drive: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	ret = TRUE;

out:
	VdiskClose(vd);
	safe_mm_free(buffer);
	safe_mm_free(cmp_buffer);
	safe_mm_free(sec_buf);
	return ret;
}

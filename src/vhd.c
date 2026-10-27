/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual Disk Handling functions
 * Copyright © 2013-2026 Pete Batard <pete@akeo.ie>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#include "rufus.h"
#include "vhd.h"
#include "missing.h"
#include "resource.h"

#include "wimlib.h"
#include "bled/bled.h"

extern BOOL ignore_boot_marker;
extern FILE* fd_md5sum;
extern uint64_t total_blocks, extra_blocks, nb_blocks, last_nb_blocks;

// NB: FFU images need DISM, which Linux does not have
static const BOOL has_ffu_support = FALSE;
static int progress_op = OP_FILE_COPY, progress_msg = MSG_267;
static size_t split_root_len = 0;
static struct wimlib_progress_info_split last_split_progress;

/*
 * Read the first sector of the disk that a VHD contains, along with its size.
 * Upstream mounts the image with VirtDisk instead. Fixed and dynamic VHDs are
 * supported (differencing ones need their parent).
 * See https://learn.microsoft.com/en-us/windows/win32/vstor/about-vhd
 */
static BOOL ReadVhdFirstSector(const char* path, uint8_t* buf, uint64_t* disk_size)
{
	uint8_t footer[512], header[1024];
	uint64_t data_offset, table_offset;
	uint32_t disk_type, block_size, bat_entry;
	off_t file_size;
	BOOL r = FALSE;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return FALSE;
	file_size = lseek(fd, 0, SEEK_END);
	// The footer is the last 512 bytes (or 511, for images created before Virtual PC 2004)
	if ((file_size < 1024) || (((pread(fd, footer, sizeof(footer), file_size - 512) != sizeof(footer)) ||
		(memcmp(footer, "conectix", 8) != 0)) && ((pread(fd, footer, sizeof(footer) - 1, file_size - 511) != sizeof(footer) - 1) ||
		(memcmp(footer, "conectix", 8) != 0)))) {
		uprintf("  Not a VHD image");
		goto out;
	}
	*disk_size = read_swap64(&footer[0x30]);
	disk_type = read_swap32(&footer[0x3C]);
	switch (disk_type) {
	case 2:	// Fixed: the disk data comes first
		r = (pread(fd, buf, MBR_SIZE, 0) == MBR_SIZE);
		break;
	case 3:	// Dynamic: locate the first block through the BAT
		data_offset = read_swap64(&footer[0x10]);
		if ((pread(fd, header, sizeof(header), (off_t)data_offset) != sizeof(header)) ||
			(memcmp(header, "cxsparse", 8) != 0))
			goto out;
		table_offset = read_swap64(&header[0x10]);
		block_size = read_swap32(&header[0x20]);
		if ((block_size < MBR_SIZE) || (pread(fd, &bat_entry, sizeof(bat_entry), (off_t)table_offset) != sizeof(bat_entry)))
			goto out;
		bat_entry = bswap_uint32(bat_entry);
		if (bat_entry == 0xFFFFFFFF) {
			// Unallocated block
			memset(buf, 0, MBR_SIZE);
			r = TRUE;
		} else {
			// The block starts with its sector bitmap, padded to a 512-byte boundary
			r = (pread(fd, buf, MBR_SIZE, (off_t)bat_entry * 512 + CEILING_ALIGN(block_size / 512 / 8, 512)) == MBR_SIZE);
		}
		break;
	default:
		uprintf("  Differencing VHDs are not supported");
		break;
	}
out:
	close(fd);
	return r;
}

typedef struct {
	const char* ext;
	uint8_t type;
} comp_assoc;

static comp_assoc file_assoc[] = {
	{ ".zip", BLED_COMPRESSION_ZIP },
	{ ".Z", BLED_COMPRESSION_LZW },
	{ ".gz", BLED_COMPRESSION_GZIP },
	{ ".lzma", BLED_COMPRESSION_LZMA },
	{ ".bz2", BLED_COMPRESSION_BZIP2 },
	{ ".xz", BLED_COMPRESSION_XZ },
	{ ".vtsi", BLED_COMPRESSION_VTSI },
	{ ".zst", BLED_COMPRESSION_ZSTD },
	{ ".ffu", BLED_COMPRESSION_MAX },
	{ ".vhd", BLED_COMPRESSION_MAX + 1 },
	{ ".vhdx", BLED_COMPRESSION_MAX + 2 },
};

// Look for a boot marker in the MBR area of the image
static int8_t IsCompressedBootableImage(const char* path)
{
	char *ext = NULL;
	unsigned char *buf = NULL;
	int i;
	FILE* fd = NULL;
	BOOL r = 0;
	int64_t dc = 0;

	img_report.compression_type = BLED_COMPRESSION_NONE;
	if (safe_strlen(path) > 4)
		for (ext = (char*)&path[safe_strlen(path) - 1]; (*ext != '.') && (ext != path); ext--);

	for (i = 0; i < ARRAYSIZE(file_assoc); i++) {
		if (safe_stricmp(ext, file_assoc[i].ext) == 0) {
			img_report.compression_type = file_assoc[i].type;
			buf = malloc(MBR_SIZE);
			if (buf == NULL)
				return 0;
			ErrorStatus = 0;
			if (img_report.compression_type < BLED_COMPRESSION_MAX) {
				bled_init(0, uprintf, NULL, NULL, NULL, NULL, &ErrorStatus);
				dc = bled_get_uncompressed_size(path, file_assoc[i].type);
				if (dc > 0)
					img_report.projected_size = dc;
				dc = bled_uncompress_to_buffer(path, (char*)buf, MBR_SIZE, file_assoc[i].type);
				bled_exit();
			} else if (img_report.compression_type == BLED_COMPRESSION_MAX) {
				// Dism, through FfuProvider.dll, can mount a .ffu as a physicaldrive, which we
				// could then use to poke the MBR as we do for VHD... Except Microsoft did design
				// dism to FAIL AND EXIT, after mounting the ffu as a virtual drive, if it doesn't
				// find something that looks like Windows at the specified image index... which it
				// usually won't in our case. So, curse Microsoft and their incredible short-
				// sightedness (or, most likely in this case, intentional malice, by BREACHING the
				// OS contract to keep useful disk APIs for their usage, and their usage only).
				// Then again, considering that .ffu's are GPT based, the marker should always be
				// present, so just check for the FFU signature and pretend there's a marker then.
				if (has_ffu_support) {
					fd = fopen(path, "rb");
					if (fd != NULL) {
						img_report.is_vhd = TRUE;
						dc = fread(buf, 1, MBR_SIZE, fd);
						fclose(fd);
						// The signature may not be constant, but since the only game in town to
						// create FFU is dism, and dism appears to use "SignedImage " always,.we
						// might as well use this to our advantage.
						if (strncmp((char*)&buf[4], "SignedImage ", 12) == 0) {
							// At this stage, the buffer is only used for marker validation.
							buf[0x1FE] = 0x55;
							buf[0x1FF] = 0xAA;
						}
					} else
						uprintf("Could not open %s: %d", path, errno);
				} else {
					uprintf("  An FFU image was selected, but this system does not have FFU support!");
				}
			} else if (img_report.compression_type == BLED_COMPRESSION_MAX + 1) {
				if (ReadVhdFirstSector(path, buf, &img_report.projected_size)) {
					img_report.is_vhd = TRUE;
					dc = MBR_SIZE;
				}
			} else {
				// TODO: VHDX (through qemu-img, see LINUX_PORT.md)
				uprintf("  VHDX images are not supported yet");
			}
			if (dc != MBR_SIZE) {
				free(buf);
				return FALSE;
			}
			if ((buf[0x1FE] == 0x55) && (buf[0x1FF] == 0xAA))
				r = 1;
			else if (ignore_boot_marker)
				r = 2;
			free(buf);
			return r;
		}
	}

	return FALSE;
}

// 0: non-bootable, 1: bootable, 2: forced bootable
int8_t IsBootableImage(const char* path)
{
	int handle = -1;
	struct stat st;
	uint64_t wim_magic = 0;
	int8_t is_bootable_img;

	uprintf("Disk image analysis:");
	handle = open(path, O_RDONLY | O_CLOEXEC);
	if (handle < 0) {
		uprintf("  Could not open image '%s'", path);
		is_bootable_img = -1;
		goto out;
	}

	is_bootable_img = IsCompressedBootableImage(path) ? 1 : 0;
	if (img_report.compression_type == BLED_COMPRESSION_NONE)
		is_bootable_img = AnalyzeMBR(handle, 512, "  Image", FALSE) ? 1 : (ignore_boot_marker ? 2 : 0);

	if (fstat(handle, &st) != 0) {
		uprintf("  Could not get image size: %s", WindowsErrorString());
		is_bootable_img = -2;
		goto out;
	}
	img_report.image_size = (uint64_t)st.st_size;
	if (img_report.projected_size == 0)
		img_report.projected_size = img_report.image_size;
	img_report.is_windows_img = (pread(handle, &wim_magic, sizeof(wim_magic), 0) == sizeof(wim_magic)) &&
		(wim_magic == WIM_MAGIC);
	if (img_report.is_windows_img)
		goto out;

out:
	safe_close(handle);
	return is_bootable_img;
}

// WIM operations progress callback
static enum wimlib_progress_status WimProgressFunc(enum wimlib_progress_msg msg_type,
	union wimlib_progress_info* info, void* progctx)
{
	static BOOL init[3] = { 0 };

	if IS_ERROR(ErrorStatus)
		return WIMLIB_PROGRESS_STATUS_ABORT;

	switch (msg_type) {
	case WIMLIB_PROGRESS_MSG_EXTRACT_IMAGE_BEGIN:
		memset(init, 0, sizeof(init));
		uprintf("Applying image %d (\"%s\") from '%s' to '%s'",
			info->extract.image, info->extract.image_name,
			info->extract.wimfile_name, info->extract.target);
		break;
	case WIMLIB_PROGRESS_MSG_EXTRACT_FILE_STRUCTURE:
		if (!init[0]) {
			uprintf("Creating file structure...");
			init[0] = TRUE;
			uprint_progress(0, 0);
		}
		UpdateProgressWithInfoUpTo(98, progress_op, progress_msg, info->extract.current_file_count, info->extract.end_file_count * 6);
		uprint_progress(info->extract.current_file_count, info->extract.end_file_count);
		break;
	case WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS:
		if (!init[1]) {
			uprintf("\nExtracting file data...");
			init[1] = TRUE;
			uprint_progress(0, 0);
		}
		UpdateProgressWithInfoUpTo(98, progress_op, progress_msg, info->extract.total_bytes + (4 * info->extract.completed_bytes), info->extract.total_bytes * 6);
		uprint_progress(info->extract.completed_bytes, info->extract.total_bytes);
		break;
	case WIMLIB_PROGRESS_MSG_EXTRACT_METADATA:
		if (!init[2]) {
			uprintf("\nApplying metadata to files...");
			init[2] = TRUE;
			uprint_progress(0, 0);
		}
		UpdateProgressWithInfoUpTo(98, progress_op, progress_msg, info->extract.current_file_count + (5 * info->extract.end_file_count), info->extract.end_file_count * 6);
		uprint_progress(info->extract.current_file_count, info->extract.end_file_count);
		if (info->extract.current_file_count >= info->extract.end_file_count)
			uprintf("\n");
		break;
	case WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART:
		last_split_progress = info->split;
		uprintf("● %s", info->split.part_name);
		break;
	case WIMLIB_PROGRESS_MSG_SPLIT_END_PART:
		if (fd_md5sum != NULL) {
			// Don't bother computing the hash at write time - just do it post creation
			uint8_t sum[MD5_HASHSIZE];
			const char* filename = info->split.part_name;
			// NB: Upstream skips 3 characters ("X:\\") for the relative path
			if ((filename != NULL) && (strlen(filename) > split_root_len)) {
				HashFile(HASH_MD5, filename, sum);
				for (int j = 0; j < MD5_HASHSIZE; j++)
					fprintf(fd_md5sum, "%02x", sum[j]);
				fprintf(fd_md5sum, "  ./%s\n", &filename[split_root_len]);
			}
		}
		break;
	case WIMLIB_PROGRESS_MSG_WRITE_STREAMS: {
		uint64_t completed_bytes = last_split_progress.completed_bytes + info->write_streams.completed_compressed_bytes;
		nb_blocks = last_nb_blocks + completed_bytes / 2048;
		UpdateProgressWithInfo(OP_FILE_COPY, MSG_231, nb_blocks, total_blocks + extra_blocks);
		break;
	}
	default:
		break;
	}

	return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

// Return the WIM version of an image
uint32_t GetWimVersion(const char* image)
{
	int r;
	WIMStruct* wim;
	struct wimlib_wim_info info;

	if (image == NULL)
		return 0;

	r = wimlib_open_wim(image, 0, &wim);
	if (r == 0) {
		r = wimlib_get_wim_info(wim, &info);
		wimlib_free(wim);
		if (r == 0)
			return info.wim_version;
	}
	uprintf("WARNING: Could not get WIM version: Error %d", r);
	return 0;
}

// Extract a file from a WIM image. Returns the allocated path of the extracted file or NULL on error.
BOOL WimExtractFile(const char* image, int index, const char* src, const char* dst)
{
	int r = 1;
	WIMStruct* wim;
	char tmp[2 * MAX_PATH] = "", dir[MAX_PATH], wim_src[MAX_PATH], *p;
	const char* src_name, *wim_paths[1] = { wim_src };

	if ((image == NULL) || (src == NULL) || (dst == NULL))
		goto out;

	// Upstream uses backslashes for the paths in the image, which wimlib only accepts on Windows
	static_strcpy(wim_src, src);
	static_repchr(wim_src, '\\', '/');
	src_name = strrchr(wim_src, '/');
	assert(src_name != NULL);
	assert(strrchr(dst, '/') != NULL);
	if (src_name == NULL || strrchr(dst, '/') == NULL)
		goto out;
	static_strcpy(dir, dst);
	p = strrchr(dir, '/');
	*p = '\0';

	wimlib_global_init(0);
	wimlib_set_print_errors(true);
	r = wimlib_open_wim(image, 0, &wim);
	if (r == 0) {
		r = wimlib_extract_paths(wim, index, dir, wim_paths, 1, WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE);
		wimlib_free(wim);
		static_sprintf(tmp, "%s/%s", dir, &src_name[1]);
		if ((r == 0) && (strcmp(tmp, dst) != 0) && (rename(tmp, dst) != 0)) {
			uprintf("  Could not rename %s to %s: %s", tmp, dst, WindowsErrorString());
			r = 1;
		}
	}
	wimlib_global_cleanup();

out:
	return (r == 0);
}

// Split an install.wim for FAT32 limits
BOOL WimSplitFile(const char* src, const char* dst, size_t root_len)
{
	int r = 1;
	WIMStruct* wim;

	if ((src == NULL) || (dst == NULL))
		goto out;

	split_root_len = root_len;
	wimlib_global_init(0);
	wimlib_set_print_errors(true);
	r = wimlib_open_wim(src, 0, &wim);
	if (r == 0) {
		wimlib_register_progress_function(wim, WimProgressFunc, NULL);
		r = wimlib_split(wim, dst, 4094ULL * MB, WIMLIB_WRITE_FLAG_FSYNC);
		wimlib_free(wim);
	}
	wimlib_global_cleanup();

out:
	return (r == 0);
}

BOOL WimApplyImage(const char* image, int index, const char* dst)
{
	int r = 1;
	WIMStruct* wim;

	wimlib_global_init(0);
	wimlib_set_print_errors(true);

	uprintf("Opening: %s:[%d]", image, index);
	r = wimlib_open_wim(image, 0, &wim);
	if (r == 0) {
		progress_op = OP_FILE_COPY;
		progress_msg = MSG_267;
		wimlib_register_progress_function(wim, WimProgressFunc, NULL);
		r = wimlib_extract_image(wim, index, dst, 0);
		wimlib_free(wim);
	} else {
		uprintf("Failed to open '%s': Wimlib error %d", image, r);
	}
	wimlib_global_cleanup();
	return (r == 0);
}

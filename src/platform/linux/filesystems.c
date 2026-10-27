/*
 * Rufus: The Reliable USB Formatting Utility
 * File systems
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
 * FormatPartition() and ToValidLabel() of format.c. On Windows, the file
 * systems are created by fmifs (FormatEx) or VDS; here, by the mkfs tools of
 * each file system, with the parameters that make their output closest to
 * the one of Windows: the 255 × 63 geometry and the hidden sectors of the
 * partition, the cluster size of the rules, and no partition table of their
 * own. A slow format zeroes the volume first, as FormatEx does.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "filesystems.h"
#include "partition.h"
#include "resource.h"
#include "localization.h"

extern const char* FileSystemLabel[FS_MAX];

// Zero a volume, as a slow format does
static BOOL ZeroVolume(const char* devnode, uint64_t size)
{
	const size_t buf_size = 1 * MB;
	uint8_t* buf = calloc(1, buf_size);
	uint64_t pos;
	size_t len;
	ssize_t w;
	int fd = open(devnode, O_WRONLY | O_CLOEXEC);
	BOOL r = FALSE;

	if ((fd < 0) || (buf == NULL)) {
		uprintf("Could not open %s: %s", devnode, WindowsErrorString());
		goto out;
	}
	UpdateProgressWithInfoInit(NULL, FALSE);
	for (pos = 0; pos < size; pos += len) {
		CHECK_FOR_USER_CANCEL;
		len = (size_t)min(size - pos, buf_size);
		w = pwrite(fd, buf, len, (off_t)pos);
		if (w != (ssize_t)len) {
			uprintf("Could not zero %s at offset %llu: %s", devnode, (unsigned long long)pos, WindowsErrorString());
			ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
			goto out;
		}
		UpdateProgressWithInfo(OP_FORMAT, MSG_217, pos + len, size);
	}
	r = (fsync(fd) == 0);

out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return r;
}

static BOOL RunMkfs(const char* const* argv, const char* fs_name)
{
	DWORD r;

	uprintf("Formatting to %s (using %s)", fs_name, argv[0]);
	r = RunCommand(argv, NULL, TRUE);
	if (r != 0) {
		uprintf("%s failed with error %u", argv[0], r);
		if (!IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR((r == ERROR_CANCELLED) ? ERROR_CANCELLED : ERROR_GEN_FAILURE);
		return FALSE;
	}
	return TRUE;
}

BOOL FormatPartition(const char* devnode, uint64_t offset, uint64_t size, uint32_t sector_size,
	uint32_t cluster_size, int fs_type, const char* label, DWORD flags)
{
	char sectors_per_cluster[16], ssz[16], hidden[16], cluster[16], label_opt[160];
	const char* fs_name;
	const char* argv[32];
	int i = 0, fd;
	BOOL r;

	if ((fs_type < 0) || (fs_type >= FS_MAX) || ((cluster_size != 0) && (!IS_POWER_OF_2(cluster_size))) ||
		(sector_size == 0)) {
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	fs_name = FileSystemLabel[fs_type];
	if ((fs_type == FS_FAT32) && ((opts.disk_size > LARGE_FAT32_SIZE) || (opts.force_large_fat32) || (flags & FP_LARGE_FAT32))) {
		fd = open(devnode, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			uprintf("Could not open %s: %s", devnode, WindowsErrorString());
			ErrorStatus = RUFUS_ERROR(WindowsErrorFromErrno(errno));
			return FALSE;
		}
		r = FormatLargeFAT32(fd, offset, size, sector_size, cluster_size, fs_name, label, flags);
		close(fd);
		return r;
	}
	if (IS_EXT(fs_type))
		return FormatExtFs(devnode, cluster_size, fs_name, label, flags);

	if ((fs_type == FS_EXFAT) && !((dur_mins == 0) && (dur_secs == 0)))
		PrintInfo(0, MSG_220, fs_name, dur_mins, dur_secs);
	else
		PrintInfo(0, MSG_222, fs_name);
	// 0 is FormatEx's value for the default cluster size, which UDF must use
	if (cluster_size < 0x200) {
		cluster_size = 0;
		uprintf("Using default cluster size");
	} else {
		uprintf("Using cluster size: %d bytes", cluster_size);
	}
	uprintf("%s format was selected", (flags & FP_QUICK) ? "Quick" : "Slow");
	// mkntfs and mkfs.exfat zero the volume themselves
	if (!(flags & FP_QUICK) && (fs_type != FS_NTFS) && (fs_type != FS_EXFAT) && !ZeroVolume(devnode, size))
		return FALSE;

	snprintf(ssz, sizeof(ssz), "%u", sector_size);
	snprintf(hidden, sizeof(hidden), "%llu", (unsigned long long)(offset / sector_size));
	snprintf(sectors_per_cluster, sizeof(sectors_per_cluster), "%u", cluster_size / sector_size);
	snprintf(cluster, sizeof(cluster), "%u", cluster_size);
	switch (fs_type) {
	case FS_FAT16:
	case FS_FAT32:
		argv[i++] = "mkfs.fat";
		argv[i++] = (fs_type == FS_FAT16) ? "-F16" : "-F32";
		argv[i++] = "-S"; argv[i++] = ssz;
		if (cluster_size != 0) {
			argv[i++] = "-s"; argv[i++] = sectors_per_cluster;
		}
		argv[i++] = "-h"; argv[i++] = hidden;
		argv[i++] = "-g"; argv[i++] = "255/63";
		// A whole drive (super floppy) is fine, and the volume must not get a fake MBR of its own
		argv[i++] = "-I";
		argv[i++] = "--mbr=n";
		if ((label != NULL) && (label[0] != 0)) {
			argv[i++] = "-n"; argv[i++] = label;
		}
		break;
	case FS_NTFS:
		argv[i++] = "mkntfs";
		argv[i++] = "-F";
		if (flags & FP_QUICK)
			argv[i++] = "-Q";
		argv[i++] = "-s"; argv[i++] = ssz;
		if (cluster_size != 0) {
			argv[i++] = "-c"; argv[i++] = cluster;
		}
		argv[i++] = "-p"; argv[i++] = hidden;
		argv[i++] = "-H"; argv[i++] = "255";
		argv[i++] = "-S"; argv[i++] = "63";
		if (flags & FP_COMPRESSION)
			argv[i++] = "-C";
		if (!opts.enable_file_indexing)
			argv[i++] = "-I";
		if ((label != NULL) && (label[0] != 0)) {
			argv[i++] = "-L"; argv[i++] = label;
		}
		break;
	case FS_EXFAT:
		argv[i++] = "mkfs.exfat";
		if (!(flags & FP_QUICK))
			argv[i++] = "-f";
		if (cluster_size != 0) {
			argv[i++] = "-c"; argv[i++] = cluster;
		}
		if ((label != NULL) && (label[0] != 0)) {
			argv[i++] = "-L"; argv[i++] = label;
		}
		break;
	case FS_UDF:
		argv[i++] = "mkudffs";
		argv[i++] = "--media-type=hd";
		snprintf(cluster, sizeof(cluster), "--blocksize=%u", sector_size);
		argv[i++] = cluster;
		argv[i++] = "--udfrev=0x0201";
		if ((label != NULL) && (label[0] != 0)) {
			snprintf(label_opt, sizeof(label_opt), "--label=%s", label);
			argv[i++] = label_opt;
		}
		break;
	default:
		uprintf("%s is not supported on Linux", fs_name);
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	argv[i++] = devnode;
	argv[i] = NULL;
	if (!RunMkfs(argv, fs_name))
		return FALSE;
	uprintf("Format completed.");
	return TRUE;
}

// The code point of a UTF-8 sequence, and its length
static uint32_t NextCodePoint(const char* s, size_t* len)
{
	const uint8_t* p = (const uint8_t*)s;

	if (p[0] < 0x80) {
		*len = 1;
		return p[0];
	} else if (((p[0] & 0xe0) == 0xc0) && ((p[1] & 0xc0) == 0x80)) {
		*len = 2;
		return ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
	} else if (((p[0] & 0xf0) == 0xe0) && ((p[1] & 0xc0) == 0x80) && ((p[2] & 0xc0) == 0x80)) {
		*len = 3;
		return ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
	} else if (((p[0] & 0xf8) == 0xf0) && ((p[1] & 0xc0) == 0x80) && ((p[2] & 0xc0) == 0x80) && ((p[3] & 0xc0) == 0x80)) {
		*len = 4;
		return ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
	}
	*len = 1;
	return 0xfffd;
}

void ToValidLabel(char* Label, BOOL bFAT, uint64_t disk_size)
{
	const char unauthorized[] = "*?,;:/\\|+=<>[]\"";
	const char to_underscore[] = "\t.";
	char label[16] = { 0 }, *out;
	size_t i, j, k, n, len;
	uint32_t c;

	// NB: Upstream works on UTF-16 code units, and this on code points
	out = calloc(1, strlen(Label) + 1);
	if (out == NULL)
		return;
	for (i = 0, k = 0, n = 0; Label[i] != 0; i += len) {
		c = NextCodePoint(&Label[i], &len);
		// NTFS does allows all the FAT unauthorized above
		if (bFAT) {
			if ((c < 0x80) && (strchr(unauthorized, (int)c) != NULL))
				continue;
			// A FAT label that contains extended chars will be rejected
			if (c >= 0x80) {
				out[k++] = '_';
				n++;
				continue;
			}
		}
		if ((c < 0x80) && (strchr(to_underscore, (int)c) != NULL)) {
			out[k++] = '_';
		} else if (c < 0x80) {
			out[k++] = bFAT ? (char)toupper((int)c) : (char)c;
		} else {
			memcpy(&out[k], &Label[i], len);
			k += len;
		}
		n++;
		// Truncate to the maximum length of the file system, in characters
		if (n >= (bFAT ? 11 : 32))
			break;
	}
	out[k] = 0;

	if (bFAT) {
		for (i = 0, j = 0; out[i] != 0; i++)
			if (out[i] == '_')
				j++;
		if (i < 2 * j) {
			// If the final label is mostly underscore, use an alternate label according to the
			// size (eg: "256 MB", "7.9 GB"). Note that we can't use SelectedDrive.proposed_label
			// here as it may contain localized character for GB or MB, so make sure that the
			// effective label we use is an English one, and also make sure we convert the dot.
			static_sprintf(label, "%s", SizeToHumanReadable(disk_size, TRUE, FALSE));
			for (i = 0; label[i] != 0; i++)
				out[i] = (label[i] == '.') ? '_' : label[i];
			out[i] = 0;
			uprintf("FAT label is mostly underscores. Using '%s' label instead.", out);
		}
	}

	// Needed for disk by label isolinux.cfg workaround
	static_strcpy(img_report.usb_label, out);
	safe_strcpy(Label, strlen(Label) + 1, img_report.usb_label);
	free(out);
}

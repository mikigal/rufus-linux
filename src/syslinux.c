/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2003 Lars Munch Christensen - All Rights Reserved
 *   Copyright 1998-2008 H. Peter Anvin - All Rights Reserved
 *   Copyright 2012-2024 Pete Batard
 *
 *   Based on the Linux installer program for SYSLINUX by H. Peter Anvin
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 53 Temple Place Ste 330,
 *   Boston MA 02111-1307, USA; either version 2 of the License, or
 *   (at your option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
/*
 * On Linux, ldlinux.sys gets written through the mounted volume, its sectors
 * get mapped with FIEMAP (or FIBMAP), whatever the file system (upstream uses
 * libfat for FAT and the NTFS extents for NTFS), and the boot sector is
 * returned to the caller, which writes it once the volume is unmounted.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#include "rufus.h"
#include "options.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"

#include "syslinux.h"
#include "syslxfs.h"
#include "libfat.h"
#include "setadv.h"

unsigned char* syslinux_ldlinux[2] = { NULL, NULL };
// NB: syslinux_ldlinux_len is now in core/globals.c
unsigned char* syslinux_mboot = NULL;
DWORD syslinux_mboot_len;

/* Workaround for 4K support */
// NB: SECTOR_SHIFT, SECTOR_SIZE and the LIBFAT_SECTOR_* variables are now in core/globals.c

// The sectors of a file on its volume, from FIEMAP, or from FIBMAP if the file system lacks it
static int MapFileSectors(int fd, libfat_sector_t* sectors, int max_sectors)
{
	const int max_extents = 256;
	struct fiemap* fm = calloc(1, sizeof(struct fiemap) + max_extents * sizeof(struct fiemap_extent));
	uint64_t off;
	uint32_t i;
	int n = 0, blk, block_size, j;

	if (fm == NULL)
		return -1;
	fm->fm_length = FIEMAP_MAX_OFFSET;
	fm->fm_flags = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = max_extents;
	if (ioctl(fd, FS_IOC_FIEMAP, fm) == 0) {
		for (i = 0; (i < fm->fm_mapped_extents) && (n < max_sectors); i++) {
			if (fm->fm_extents[i].fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
				FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE)) {
				uprintf("The extents of 'ldlinux.sys' can't be used for booting");
				n = -1;
				break;
			}
			for (off = 0; (off < fm->fm_extents[i].fe_length) && (n < max_sectors); off += SECTOR_SIZE)
				sectors[n++] = (fm->fm_extents[i].fe_physical + off) >> SECTOR_SHIFT;
		}
		free(fm);
		return n;
	}
	free(fm);
	uuprintf("FIEMAP is not available (%s) - Using FIBMAP", WindowsErrorString());
	if ((ioctl(fd, FIGETBSZ, &block_size) != 0) || (block_size < (int)SECTOR_SIZE))
		return -1;
	for (blk = 0; n < max_sectors; blk++) {
		int b = blk;
		if ((ioctl(fd, FIBMAP, &b) != 0) || (b == 0))
			return -1;
		for (j = 0; (j < block_size / (int)SECTOR_SIZE) && (n < max_sectors); j++)
			sectors[n++] = (((libfat_sector_t)b * block_size) >> SECTOR_SHIFT) + j;
	}
	return n;
}

/*
 * Extract the ldlinux.sys and ldlinux.bss from resources,
 * then patch and install them
 */
BOOL InstallSyslinux(const char* volume, const char* mount_point, int file_system, uint32_t sector_size, uint8_t* bootsect)
{
	int d_handle = -1, f_handle = -1;
	BOOL r = FALSE;
	FILE* fd;
	size_t length;

	static unsigned char* sectbuf = NULL;
	static const int resource[2][2] = {
		{ IDR_SL_LDLINUX_V4_SYS, IDR_SL_LDLINUX_V4_BSS },
		{ IDR_SL_LDLINUX_V6_SYS, IDR_SL_LDLINUX_V6_BSS } };
	const char* ldlinux = "ldlinux";
	const char* syslinux = "syslinux";
	const char* ldlinux_ext[3] = { "sys", "bss", "c32" };
	const char* mboot_c32 = "mboot.c32";
	char path[MAX_PATH], src[MAX_PATH], tmp[64];
	const char *errmsg;
	libfat_sector_t *sectors = NULL;
	int ldlinux_sectors;
	int i, w, nsectors, sl_fs_stype;
	BOOL use_v5 = (opts.boot_type == BT_SYSLINUX_V6) || ((opts.boot_type == BT_IMAGE) && (SL_MAJOR(img_report.sl_version) >= 5));

	PrintInfoDebug(0, MSG_234, (opts.boot_type == BT_IMAGE) ? img_report.sl_version_str : embedded_sl_version_str[use_v5?1:0]);

	/* 4K sector size workaround */
	SECTOR_SHIFT = 0;
	SECTOR_SIZE = sector_size;
	while (SECTOR_SIZE>>=1)
		SECTOR_SHIFT++;
	SECTOR_SIZE = sector_size;
	LIBFAT_SECTOR_SHIFT = SECTOR_SHIFT;
	LIBFAT_SECTOR_SIZE = SECTOR_SIZE;
	LIBFAT_SECTOR_MASK = SECTOR_SIZE - 1;

	/* sectbuf should be aligned to at least 8 bytes - see github #767 */
	sectbuf = _mm_malloc(SECTOR_SIZE, 16);
	if (sectbuf == NULL)
		goto out;

	/* Make sure we can read the boot sector */
	d_handle = open(volume, O_RDONLY | O_CLOEXEC);
	if ((d_handle < 0) || (pread(d_handle, sectbuf, SECTOR_SIZE, 0) != (ssize_t)SECTOR_SIZE)) {
		uprintf("Could not read VBR");
		goto out;
	}
	if ((errmsg = syslinux_check_bootsect(sectbuf, &sl_fs_stype))) {
		uprintf("Error: %s", errmsg);
		goto out;
	}

	/* Initialize the ADV -- this should be smarter */
	syslinux_reset_adv(syslinux_adv);

	/* Access a copy of the ldlinux.sys & ldlinux.bss resources (downloaded or embedded) */
	if ((syslinux_ldlinux_len[0] != 0) && (syslinux_ldlinux_len[1] != 0)) {
		for (i = 0; i < 2; i++) {
			syslinux_ldlinux[i] = (unsigned char*) malloc(syslinux_ldlinux_len[i]);
			if (syslinux_ldlinux[i] == NULL)
				goto out;
			static_sprintf(path, "%s/%s/%s-%s%s/%s.%s", app_data_dir, FILES_DIR, syslinux, img_report.sl_version_str,
				img_report.sl_version_ext, ldlinux, i==0?"sys":"bss");
			fd = fopen(path, "rb");
			if (fd == NULL) {
				uprintf("Could not open %s", path);
				goto out;
			}
			length = fread(syslinux_ldlinux[i], 1, (size_t)syslinux_ldlinux_len[i], fd);
			fclose(fd);
			if (length != (size_t)syslinux_ldlinux_len[i]) {
				uprintf("Could not read %s", path);
				goto out;
			}
			uprintf("Using existing '%s' %s", path,
				IsBufferInDB(syslinux_ldlinux[i], (size_t)syslinux_ldlinux_len[i])?"✓":"✗");
		}
	} else {
		for (i = 0; i < 2; i++) {
			DWORD len = 0;
			static_sprintf(tmp, "%s.%s", ldlinux, ldlinux_ext[i]);
			syslinux_ldlinux[i] = GetResource(resource[use_v5?1:0][i], tmp, &len, TRUE);
			syslinux_ldlinux_len[i] = len;
			if (syslinux_ldlinux[i] == NULL)
				goto out;
		}
	}

	/* Create ldlinux.sys file */
	static_sprintf(path, "%s/%s.%s", mount_point, ldlinux, ldlinux_ext[0]);
	f_handle = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
	if (f_handle < 0) {
		uprintf("Unable to create '%s': %s", path, WindowsErrorString());
		goto out;
	}

	/* Write ldlinux.sys file */
	if ((write(f_handle, syslinux_ldlinux[0], syslinux_ldlinux_len[0]) != (ssize_t)syslinux_ldlinux_len[0]) ||
		(write(f_handle, syslinux_adv, 2 * ADV_SIZE) != 2 * ADV_SIZE)) {
		uprintf("Could not write '%s': %s", path, WindowsErrorString());
		goto out;
	}
	uprintf("Successfully wrote '%s'", path);
	if (opts.boot_type != BT_IMAGE)
		UpdateProgress(OP_FILE_COPY, -1.0f);

	/* Now flush the media */
	if (fsync(f_handle) != 0) {
		uprintf("Could not flush '%s': %s", path, WindowsErrorString());
		goto out;
	}

	/* Map the file (is there a better way to do this?) */
	ldlinux_sectors = (syslinux_ldlinux_len[0] + 2 * ADV_SIZE + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
	sectors = (libfat_sector_t*) calloc(ldlinux_sectors, sizeof *sectors);
	if (sectors == NULL)
		goto out;
	// NB: Upstream uses libfat for FAT and exFAT, and the NTFS extents for NTFS
	if ((file_system != FS_FAT16) && (file_system != FS_FAT32) && (file_system != FS_EXFAT) && (file_system != FS_NTFS)) {
		uprintf("Unsupported Syslinux filesystem");
		goto out;
	}
	nsectors = MapFileSectors(f_handle, sectors, ldlinux_sectors);
	if (nsectors != ldlinux_sectors) {
		uprintf("Could not map 'ldlinux.sys' to its sectors");
		goto out;
	}

	/* Set the base directory and patch ldlinux.sys and the boot sector */
	for (i = (int)strlen(img_report.cfg_path); (i > 0) && (img_report.cfg_path[i] != '/'); i--);
	if (i > 0)
		img_report.cfg_path[i] = 0;
	w = syslinux_patch(sectors, nsectors, 0, 0, img_report.cfg_path, NULL);
	if (i > 0)
		img_report.cfg_path[i] = '/';
	if (w < 0) {
		uprintf("WARNING: Could not patch Syslinux files.");
		uprintf("This could be caused by your firewall having modified downloaded content, such as 'ldlinux.sys'...");
		goto out;
	}

	/* Rewrite the file */
	if ((pwrite(f_handle, syslinux_ldlinux[0], syslinux_ldlinux_len[0], 0) != (ssize_t)syslinux_ldlinux_len[0]) ||
		(fsync(f_handle) != 0)) {
		uprintf("Could not rewrite '%s': %s\n", path, WindowsErrorString());
		goto out;
	}
	SetFileAttributesFd(f_handle, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN);

	/* Close file */
	safe_close(f_handle);

	/* Make the syslinux boot sector, for WriteSyslinuxBootsect() once the volume is unmounted */
	syslinux_make_bootsect(sectbuf, (file_system == FS_NTFS)?NTFS:VFAT);
	memcpy(bootsect, sectbuf, SECTOR_SIZE);

	if (opts.boot_type == BT_SYSLINUX_V6) {
		static_sprintf(src, "%s/%s/%s-%s/%s.%s", app_data_dir, FILES_DIR, syslinux, embedded_sl_version_str[1], ldlinux, ldlinux_ext[2]);
		static_sprintf(path, "%s/%s.%s", mount_point, ldlinux, ldlinux_ext[2]);
		if (access(src, R_OK) != 0) {
			uprintf("Caution: No '%s' was provided. The target will be missing a required Syslinux file!", src);
		} else if (copy_file(src, path, TRUE)) {
			uprintf("Created '%s' (from '%s') %s", path, src, IsFileInDB(src)?"✓":"✗");
		} else {
			uprintf("Failed to create '%s': %s", path, WindowsErrorString());
		}
	} else if (HAS_REACTOS(img_report)) {
		uprintf("Setting up ReactOS...");
		syslinux_mboot = GetResource(IDR_SL_MBOOT_C32, "mboot.c32", &syslinux_mboot_len, FALSE);
		if (syslinux_mboot == NULL) {
			goto out;
		}
		/* Create mboot.c32 file */
		static_sprintf(path, "%s/%s", mount_point, mboot_c32);
		if (write_file(path, syslinux_mboot, syslinux_mboot_len) != syslinux_mboot_len) {
			uprintf("Could not write '%s'", path);
			goto out;
		}
		static_sprintf(path, "%s/syslinux.cfg", mount_point);
		fd = fopen(path, "w");
		if (fd == NULL) {
			uprintf("Could not create ReactOS 'syslinux.cfg'");
			goto out;
		}
		/* Write the syslinux.cfg for ReactOS */
		fprintf(fd, "DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL %s\n  APPEND %s",
			mboot_c32, img_report.reactos_path);
		fclose(fd);
	}

	if (opts.boot_type != BT_IMAGE)
		UpdateProgress(OP_FILE_COPY, -1.0f);

	r = TRUE;

out:
	safe_mm_free(sectbuf);
	safe_free(syslinux_ldlinux[0]);
	safe_free(syslinux_ldlinux[1]);
	safe_free(sectors);
	safe_close(d_handle);
	safe_close(f_handle);
	return r;
}

/*
 * Write the boot sector of InstallSyslinux() to the volume at offset on fd. The
 * boot sector gets read again, as the FAT driver flags the volume as in use in
 * it while mounted: only the boot code of Syslinux goes in.
 */
BOOL WriteSyslinuxBootsect(int fd, uint64_t offset, uint32_t sector_size, int file_system, const uint8_t* bootsect)
{
	unsigned char* boot_code = boot_sector;
	uint8_t* buf = _mm_malloc(sector_size, 16);
	int sl_fs_stype;
	BOOL r = FALSE;

	if (buf == NULL)
		return FALSE;
	if (pread(fd, buf, sector_size, (off_t)offset) != (ssize_t)sector_size) {
		uprintf("Could not read VBR: %s", WindowsErrorString());
		goto out;
	}
	if (syslinux_check_bootsect(buf, &sl_fs_stype) != NULL) {
		uprintf("The VBR changed since Syslinux was installed");
		goto out;
	}
	boot_sector = (unsigned char*)bootsect;
	syslinux_make_bootsect(buf, (file_system == FS_NTFS)?NTFS:VFAT);
	boot_sector = boot_code;
	if ((pwrite(fd, buf, sector_size, (off_t)offset) != (ssize_t)sector_size) || (fsync(fd) != 0)) {
		uprintf("Could not write Syslinux boot record: %s", WindowsErrorString());
		goto out;
	}
	uprintf("Successfully wrote Syslinux boot record");
	r = TRUE;

out:
	_mm_free(buf);
	return r;
}

// NB: GetSyslinuxVersion() is now in iso.c, with the other ISO scanning functions

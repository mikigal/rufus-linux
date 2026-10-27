/*
 * Rufus: The Reliable USB Formatting Utility
 * extfs formatting
 * Copyright © 2019-2025 Pete Batard <pete@akeo.ie>
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
 * On Linux, the file system is created through the unix_io manager of the
 * system libext2fs, on the device node of the partition (or an image file),
 * and ext4 is supported (upstream falls back to ext3). The error messages are
 * the ones of com_err.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <sys/random.h>

#include "rufus.h"
#include "filesystems.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"
#include <ext2fs/ext2fs.h>
#include <et/com_err.h>


extern const char* FileSystemLabel[FS_MAX];
static float ext2_percent_start = 0.0f, ext2_percent_share = 0.5f;

typedef struct {
	uint64_t max_size;
	uint32_t block_size;
	uint32_t inode_size;
	uint32_t inode_ratio;
} ext2fs_default_t;

// NB: Upstream has its own error_message(), which com_err provides here

errcode_t ext2fs_print_progress(int64_t cur_value, int64_t max_value)
{
	UpdateProgressWithInfo(OP_FORMAT, MSG_217, (uint64_t)((ext2_percent_start * max_value) + (ext2_percent_share * cur_value)), max_value);
	uprint_progress((uint64_t)cur_value, (uint64_t)max_value);
	return IS_ERROR(ErrorStatus) ? EXT2_ET_CANCEL_REQUESTED : 0;
}

// NB: GetExtFsLabel() is not needed, as udev provides the labels

#define SET_EXT2_FORMAT_ERROR(x)    if (!IS_ERROR(ErrorStatus)) ErrorStatus = RUFUS_ERROR(x)

static void RandomBytes(void* buf, size_t len)
{
	size_t i;

	if (getrandom(buf, len, 0) != (ssize_t)len) {
		for (i = 0; i < len; i++)
			((uint8_t*)buf)[i] = (uint8_t)rand();
	}
}

BOOL FormatExtFs(const char* devnode, DWORD BlockSize, LPCSTR FSName, LPCSTR Label, DWORD Flags)
{
	// Mostly taken from mke2fs.conf
	const float reserve_ratio = 0.05f;
	const ext2fs_default_t ext2fs_default[5] = {
		{ 3 * MB, 1024, 128, 3},	// "floppy"
		{ 512 * MB, 1024, 128, 2},	// "small"
		{ 4 * GB, 4096, 256, 2},	// "default"
		{ 16 * GB, 4096, 256, 3},	// "big"
		{ 1024 * TB, 4096, 256, 4}	// "huge"
	};

	BOOL ret = FALSE;
	char* volume_name = NULL;
	int i, count;
	struct ext2_super_block features = { 0 };
	io_manager manager = unix_io_manager;
	blk_t journal_size;
	blk64_t size = 0, cur;
	ext2_filsys ext2fs = NULL;
	errcode_t r;
	uint8_t* buf = NULL;

	volume_name = safe_strdup(devnode);
	if ((volume_name == NULL) | (strlen(FSName) != 4) || (strncmp(FSName, "ext", 3) != 0)) {
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
		goto out;
	}
	// NB: Upstream only supports ext2 and ext3, and formats ext4 as ext3
	if ((strcmp(FSName, FileSystemLabel[FS_EXT2]) != 0) && (strcmp(FSName, FileSystemLabel[FS_EXT3]) != 0) &&
		(strcmp(FSName, FileSystemLabel[FS_EXT4]) != 0)) {
		uprintf("Invalid ext file system version requested, defaulting to ext3");
		FSName = FileSystemLabel[FS_EXT3];
	}

	PrintInfoDebug(0, MSG_222, FSName);
	UpdateProgressWithInfoInit(NULL, TRUE);

	// Figure out the volume size and block size
	r = ext2fs_get_device_size2(volume_name, KB, &size);
	if ((r != 0) || (size == 0)) {
		SET_EXT2_FORMAT_ERROR(ERROR_READ_FAULT);
		uprintf("Could not read device size: %s", error_message(r));
		goto out;
	}
	size *= KB;
	for (i = 0; i < ARRAYSIZE(ext2fs_default); i++) {
		if (size < ext2fs_default[i].max_size)
			break;
	}
	assert(i < ARRAYSIZE(ext2fs_default));
	if ((BlockSize == 0) || (BlockSize < EXT2_MIN_BLOCK_SIZE))
		BlockSize = ext2fs_default[i].block_size;
	assert(IS_POWER_OF_2(BlockSize));
	for (features.s_log_block_size = 0; EXT2_BLOCK_SIZE_BITS(&features) <= EXT2_MAX_BLOCK_LOG_SIZE; features.s_log_block_size++) {
		if (EXT2_BLOCK_SIZE(&features) == BlockSize)
			break;
	}
	assert(EXT2_BLOCK_SIZE_BITS(&features) <= EXT2_MAX_BLOCK_LOG_SIZE);
	features.s_log_cluster_size = features.s_log_block_size;
	size /= BlockSize;

	// ext2 and ext3 have a can only accommodate up to Blocksize * 2^32 sized volumes
	if (((strcmp(FSName, FileSystemLabel[FS_EXT2]) == 0) || (strcmp(FSName, FileSystemLabel[FS_EXT3]) == 0)) &&
		(size >= 0x100000000ULL)) {
		SET_EXT2_FORMAT_ERROR(ERROR_INVALID_VOLUME_SIZE);
		uprintf("Volume size is too large for ext2 or ext3");
		goto out;
	}

	// Set the blocks, reserved blocks and inodes
	ext2fs_blocks_count_set(&features, size);
	ext2fs_r_blocks_count_set(&features, (blk64_t)(reserve_ratio * size));
	features.s_rev_level = 1;
	features.s_inode_size = ext2fs_default[i].inode_size;
	// ext4 needs room for the extra fields of its inodes (nanosecond timestamps...)
	if (FSName[3] == '4')
		features.s_inode_size = 256;
	features.s_inodes_count = ((ext2fs_blocks_count(&features) >> ext2fs_default[i].inode_ratio) > UINT32_MAX) ?
		UINT32_MAX : (uint32_t)(ext2fs_blocks_count(&features) >> ext2fs_default[i].inode_ratio);
	uprintf("%d possible inodes out of %lld blocks (block size = %d)", features.s_inodes_count, size, EXT2_BLOCK_SIZE(&features));
	uprintf("%lld blocks (%0.1f%%) reserved for the super user", ext2fs_r_blocks_count(&features), reserve_ratio * 100.0f);

	// Set features
	ext2fs_set_feature_dir_index(&features);
	ext2fs_set_feature_filetype(&features);
	ext2fs_set_feature_large_file(&features);
	ext2fs_set_feature_sparse_super(&features);
	ext2fs_set_feature_xattr(&features);
	if (FSName[3] != '2')
		ext2fs_set_feature_journal(&features);
	// The features that mke2fs.conf sets for ext4
	if (FSName[3] == '4') {
		ext2fs_set_feature_extents(&features);
		ext2fs_set_feature_flex_bg(&features);
		ext2fs_set_feature_huge_file(&features);
		ext2fs_set_feature_dir_nlink(&features);
		ext2fs_set_feature_extra_isize(&features);
		ext2fs_set_feature_64bit(&features);
		ext2fs_set_feature_metadata_csum(&features);
		features.s_log_groups_per_flex = 4;
		features.s_min_extra_isize = features.s_want_extra_isize = sizeof(struct ext2_inode_large) - EXT2_GOOD_OLD_INODE_SIZE;
	}
	features.s_default_mount_opts = EXT2_DEFM_XATTR_USER | EXT2_DEFM_ACL;

	// Now that we have set our base features, initialize a virtual superblock
	r = ext2fs_initialize(volume_name, EXT2_FLAG_EXCLUSIVE | EXT2_FLAG_64BITS, &features, manager, &ext2fs);
	if (r != 0) {
		SET_EXT2_FORMAT_ERROR(ERROR_INVALID_DATA);
		uprintf("Could not initialize %s features: %s", FSName, error_message(r));
		goto out;
	}

	// Zero 16 blocks of data from the start of our volume
	buf = calloc(16, ext2fs->io->block_size);
	assert(buf != NULL);
	r = io_channel_write_blk64(ext2fs->io, 0, 16, buf);
	safe_free(buf);
	if (r != 0) {
		SET_EXT2_FORMAT_ERROR(ERROR_WRITE_FAULT);
		uprintf("Could not zero %s superblock area: %s", FSName, error_message(r));
		goto out;
	}

	// Finish setting up the file system
	RandomBytes(ext2fs->super->s_uuid, sizeof(ext2fs->super->s_uuid));
	// metadata_csum needs the type of checksum before the seed
	if (ext2fs_has_feature_metadata_csum(ext2fs->super))
		ext2fs->super->s_checksum_type = EXT2_CRC32C_CHKSUM;
	ext2fs_init_csum_seed(ext2fs);
	ext2fs->super->s_def_hash_version = EXT2_HASH_HALF_MD4;
	RandomBytes(ext2fs->super->s_hash_seed, sizeof(ext2fs->super->s_hash_seed));
	ext2fs->super->s_max_mnt_count = -1;
	// NB: Upstream uses its own EXT2_OS_WINDOWS
	ext2fs->super->s_creator_os = EXT2_OS_LINUX;
	ext2fs->super->s_errors = EXT2_ERRORS_CONTINUE;
	if (Label != NULL)
		safe_strcpy((char*)ext2fs->super->s_volume_name, sizeof(ext2fs->super->s_volume_name), Label);

	r = ext2fs_allocate_tables(ext2fs);
	if (r != 0) {
		SET_EXT2_FORMAT_ERROR(ERROR_INVALID_DATA);
		uprintf("Could not allocate %s tables: %s", FSName, error_message(r));
		goto out;
	}
	r = ext2fs_convert_subcluster_bitmap(ext2fs, &ext2fs->block_map);
	if (r != 0) {
		uprintf("Could not set %s cluster bitmap: %s", FSName, error_message(r));
		goto out;
	}

	ext2_percent_start = 0.0f;
	ext2_percent_share = (FSName[3] == '2') ? 1.0f : 0.5f;
	uprintf("Creating %d inode sets: [1 marker = %0.1f set(s)]", ext2fs->group_desc_count,
		max((float)ext2fs->group_desc_count / MAX_MARKER, 1.0f));
	for (i = 0; i < (int)ext2fs->group_desc_count; i++) {
		if (ext2fs_print_progress((int64_t)i, (int64_t)ext2fs->group_desc_count))
			goto out;
		cur = ext2fs_inode_table_loc(ext2fs, i);
		count = ext2fs_div_ceil((ext2fs->super->s_inodes_per_group - ext2fs_bg_itable_unused(ext2fs, i))
			* EXT2_INODE_SIZE(ext2fs->super), EXT2_BLOCK_SIZE(ext2fs->super));
		r = ext2fs_zero_blocks2(ext2fs, cur, count, &cur, &count);
		if (r != 0) {
			SET_EXT2_FORMAT_ERROR(ERROR_WRITE_FAULT);
			uprintf("\nCould not zero inode set at position %llu (%d blocks): %s", cur, count, error_message(r));
			goto out;
		}
	}
	uprintfs("\n");

	// Create root and lost+found dirs
	r = ext2fs_mkdir(ext2fs, EXT2_ROOT_INO, EXT2_ROOT_INO, 0);
	if (r != 0) {
		SET_EXT2_FORMAT_ERROR(ERROR_FILE_CORRUPT);
		uprintf("Failed to create %s root dir: %s", FSName, error_message(r));
		goto out;
	}
	ext2fs->umask = 077;
	r = ext2fs_mkdir(ext2fs, EXT2_ROOT_INO, 0, "lost+found");
	if (r != 0) {
		SET_EXT2_FORMAT_ERROR(ERROR_FILE_CORRUPT);
		uprintf("Failed to create %s 'lost+found' dir: %s", FSName, error_message(r));
		goto out;
	}

	// Create bitmaps
	for (i = EXT2_ROOT_INO + 1; i < (int)EXT2_FIRST_INODE(ext2fs->super); i++)
		ext2fs_inode_alloc_stats(ext2fs, i, 1);
	ext2fs_mark_ib_dirty(ext2fs);

	r = ext2fs_mark_inode_bitmap2(ext2fs->inode_map, EXT2_BAD_INO);
	if (r != 0) {
		SET_EXT2_FORMAT_ERROR(ERROR_WRITE_FAULT);
		uprintf("Could not set inode bitmaps: %s", error_message(r));
		goto out;
	}
	ext2fs_inode_alloc_stats(ext2fs, EXT2_BAD_INO, 1);
	r = ext2fs_update_bb_inode(ext2fs, NULL);
	if (r != 0) {
		SET_EXT2_FORMAT_ERROR(ERROR_WRITE_FAULT);
		uprintf("Could not set inode stats: %s", error_message(r));
		goto out;
	}

	if (FSName[3] != '2') {
		// Create the journal
		ext2_percent_start = 0.5f;
		journal_size = ext2fs_default_journal_size(ext2fs_blocks_count(ext2fs->super));
		journal_size /= 2;	// That journal init is really killing us!
		uprintf("Creating %d journal blocks: [1 marker = %0.1f block(s)]", journal_size,
			max((float)journal_size / MAX_MARKER, 1.0f));
		// Even with EXT2_MKJOURNAL_LAZYINIT, this call is absolutely dreadful in terms of speed...
		r = ext2fs_add_journal_inode(ext2fs, journal_size, EXT2_MKJOURNAL_NO_MNT_CHECK | ((Flags & FP_QUICK) ? EXT2_MKJOURNAL_LAZYINIT : 0));
		uprintfs("\n");
		if (r != 0) {
			SET_EXT2_FORMAT_ERROR(ERROR_WRITE_FAULT);
			uprintf("Could not create %s journal: %s", FSName, error_message(r));
			goto out;
		}
	}

	// Create a 'persistence.conf' file if required
	if (Flags & FP_CREATE_PERSISTENCE_CONF) {
		// You *do* want the LF at the end of the "/ union" line, else Debian Live bails out...
		const char* name = "persistence.conf", data[] = "/ union\n";
		unsigned int written = 0, fsize = sizeof(data) - 1;
		ext2_file_t ext2fd;
		ext2_ino_t inode_id;
		time_t ctime = time(NULL);
		struct ext2_inode inode = { 0 };
		// Don't care about the Y2K38 problem of ext2/ext3 for a 'persistence.conf' timestamp
		if (ctime > UINT32_MAX)
			ctime = UINT32_MAX;
		inode.i_mode = 0100644;
		inode.i_links_count = 1;
		// coverity[store_truncates_time_t]
		inode.i_atime = (uint32_t)ctime;
		// coverity[store_truncates_time_t]
		inode.i_ctime = (uint32_t)ctime;
		// coverity[store_truncates_time_t]
		inode.i_mtime = (uint32_t)ctime;
		inode.i_size = fsize;

		ext2fs_namei(ext2fs, EXT2_ROOT_INO, EXT2_ROOT_INO, name, &inode_id);
		ext2fs_new_inode(ext2fs, EXT2_ROOT_INO, 010755, 0, &inode_id);
		ext2fs_link(ext2fs, EXT2_ROOT_INO, name, inode_id, EXT2_FT_REG_FILE);
		ext2fs_inode_alloc_stats(ext2fs, inode_id, 1);
		ext2fs_write_new_inode(ext2fs, inode_id, &inode);
		ext2fs_file_open(ext2fs, inode_id, EXT2_FILE_WRITE, &ext2fd);
		if ((ext2fs_file_write(ext2fd, data, fsize, &written) != 0) || (written != fsize))
			uprintf("Error: Could not create '%s' file", name);
		else
			uprintf("Created '%s' file", name);
		ext2fs_file_close(ext2fd);
	}

	// Finally we can call close() to get the file system gets created
	r = ext2fs_close(ext2fs);
	if (r == 0) {
		// Make sure ext2fs isn't freed twice
		ext2fs = NULL;
	} else {
		SET_EXT2_FORMAT_ERROR(ERROR_WRITE_FAULT);
		uprintf("Could not create %s volume: %s", FSName, error_message(r));
		goto out;
	}
	UpdateProgressWithInfo(OP_FORMAT, MSG_217, 100, 100);
	ret = TRUE;

out:
	free(volume_name);
	ext2fs_free(ext2fs);
	free(buf);
	return ret;
}

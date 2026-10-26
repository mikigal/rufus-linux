/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for ext2/3/4 formatting with the system libext2fs
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
 * Follows the same libext2fs call sequence as FormatExtFs() in format_ext.c,
 * using the stock unix_io_manager instead of the Windows nt_io_manager, to
 * make sure the system library supports everything Rufus needs (including
 * ext4, which the Windows version could not create), then checks the result
 * with e2fsck and debugfs.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ext2fs/ext2fs.h>
#include <et/com_err.h>

#include "test.h"

#define IMAGE_SIZE (64 * 1024 * 1024)

static errcode_t format(const char* path, int ext_version, const char* label)
{
	struct ext2_super_block features = { 0 };
	const char name[] = "persistence.conf", data[] = "/ union\n";
	struct ext2_inode inode = { 0 };
	ext2_filsys fs = NULL;
	ext2_file_t file;
	ext2_ino_t ino;
	unsigned int written = 0;
	unsigned int i;
	errcode_t r;
	void* buf;

	/* Rufus' "small" profile (from mke2fs.conf): 1 KB blocks, 128-byte inodes */
	features.s_log_block_size = 0;
	ext2fs_blocks_count_set(&features, IMAGE_SIZE / 1024);
	ext2fs_r_blocks_count_set(&features, IMAGE_SIZE / 1024 / 20);
	features.s_rev_level = 1;
	features.s_inode_size = 128;
	features.s_inodes_count = (IMAGE_SIZE / 1024) >> 2;
	ext2fs_set_feature_dir_index(&features);
	ext2fs_set_feature_filetype(&features);
	ext2fs_set_feature_large_file(&features);
	ext2fs_set_feature_sparse_super(&features);
	ext2fs_set_feature_xattr(&features);
	if (ext_version >= 3)
		ext2fs_set_feature_journal(&features);
	if (ext_version >= 4) {
		ext2fs_set_feature_extents(&features);
		ext2fs_set_feature_flex_bg(&features);
		ext2fs_set_feature_huge_file(&features);
		ext2fs_set_feature_dir_nlink(&features);
		ext2fs_set_feature_64bit(&features);
		ext2fs_set_feature_metadata_csum(&features);
		features.s_log_groups_per_flex = 4;
	}
	features.s_default_mount_opts = EXT2_DEFM_XATTR_USER | EXT2_DEFM_ACL;

	r = ext2fs_initialize(path, EXT2_FLAG_EXCLUSIVE | EXT2_FLAG_64BITS, &features, unix_io_manager, &fs);
	if (r != 0)
		return r;

	buf = calloc(16, fs->io->block_size);
	r = io_channel_write_blk64(fs->io, 0, 16, buf);
	free(buf);
	if (r != 0)
		goto out;

	for (i = 0; i < sizeof(fs->super->s_uuid); i++)
		fs->super->s_uuid[i] = (uint8_t)rand();
	/* Not done by FormatExtFs(), which never created ext4: metadata_csum
	 * requires the checksum type to be set before seeding the checksums */
	if (ext2fs_has_feature_metadata_csum(fs->super))
		fs->super->s_checksum_type = EXT2_CRC32C_CHKSUM;
	ext2fs_init_csum_seed(fs);
	fs->super->s_def_hash_version = EXT2_HASH_HALF_MD4;
	for (i = 0; i < sizeof(fs->super->s_hash_seed); i++)
		((uint8_t*)fs->super->s_hash_seed)[i] = (uint8_t)rand();
	fs->super->s_max_mnt_count = -1;
	fs->super->s_creator_os = EXT2_OS_LINUX;
	fs->super->s_errors = EXT2_ERRORS_CONTINUE;
	snprintf((char*)fs->super->s_volume_name, sizeof(fs->super->s_volume_name), "%s", label);

	if ((r = ext2fs_allocate_tables(fs)) != 0)
		goto out;
	if ((r = ext2fs_convert_subcluster_bitmap(fs, &fs->block_map)) != 0)
		goto out;
	for (i = 0; i < fs->group_desc_count; i++) {
		blk64_t cur = ext2fs_inode_table_loc(fs, i);
		int count = ext2fs_div_ceil((fs->super->s_inodes_per_group - ext2fs_bg_itable_unused(fs, i))
			* EXT2_INODE_SIZE(fs->super), EXT2_BLOCK_SIZE(fs->super));
		if ((r = ext2fs_zero_blocks2(fs, cur, count, &cur, &count)) != 0)
			goto out;
	}

	if ((r = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, 0)) != 0)
		goto out;
	fs->umask = 077;
	if ((r = ext2fs_mkdir(fs, EXT2_ROOT_INO, 0, "lost+found")) != 0)
		goto out;
	for (i = EXT2_ROOT_INO + 1; i < EXT2_FIRST_INODE(fs->super); i++)
		ext2fs_inode_alloc_stats(fs, i, 1);
	ext2fs_mark_ib_dirty(fs);
	if ((r = ext2fs_mark_inode_bitmap2(fs->inode_map, EXT2_BAD_INO)) != 0)
		goto out;
	ext2fs_inode_alloc_stats(fs, EXT2_BAD_INO, 1);
	if ((r = ext2fs_update_bb_inode(fs, NULL)) != 0)
		goto out;

	if (ext_version >= 3) {
		r = ext2fs_add_journal_inode(fs, ext2fs_default_journal_size(ext2fs_blocks_count(fs->super)) / 2,
			EXT2_MKJOURNAL_NO_MNT_CHECK | EXT2_MKJOURNAL_LAZYINIT);
		if (r != 0)
			goto out;
	}

	/* persistence.conf, as created for Debian live persistence */
	inode.i_mode = 0100644;
	inode.i_links_count = 1;
	inode.i_atime = inode.i_ctime = inode.i_mtime = (uint32_t)time(NULL);
	inode.i_size = sizeof(data) - 1;
	if ((r = ext2fs_new_inode(fs, EXT2_ROOT_INO, 010755, 0, &ino)) != 0)
		goto out;
	if ((r = ext2fs_link(fs, EXT2_ROOT_INO, name, ino, EXT2_FT_REG_FILE)) != 0)
		goto out;
	ext2fs_inode_alloc_stats(fs, ino, 1);
	if (ext2fs_has_feature_extents(fs->super))
		inode.i_flags |= EXT4_EXTENTS_FL;
	if ((r = ext2fs_write_new_inode(fs, ino, &inode)) != 0)
		goto out;
	if (inode.i_flags & EXT4_EXTENTS_FL) {
		ext2_extent_handle_t handle;
		if ((r = ext2fs_extent_open2(fs, ino, &inode, &handle)) != 0)
			goto out;
		ext2fs_extent_free(handle);
	}
	if ((r = ext2fs_file_open(fs, ino, EXT2_FILE_WRITE, &file)) != 0)
		goto out;
	r = ext2fs_file_write(file, data, sizeof(data) - 1, &written);
	ext2fs_file_close(file);
	if (r == 0 && written != sizeof(data) - 1)
		r = EXT2_ET_SHORT_WRITE;
	if (r != 0)
		goto out;

	r = ext2fs_close_free(&fs);

out:
	if (fs != NULL)
		ext2fs_free(fs);
	return r;
}

static void test_version(const char* dir, int ext_version)
{
	char path[4096], cmd[8192 + 128], out[256] = { 0 };
	const char* label = (ext_version == 4) ? "persistence" : "casper-rw";
	FILE* p;
	errcode_t r;
	int fd;

	snprintf(path, sizeof(path), "%s/ext%d.img", dir, ext_version);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0 && ftruncate(fd, IMAGE_SIZE) == 0);
	close(fd);

	r = format(path, ext_version, label);
	if (r != 0)
		fprintf(stderr, "ext%d: %s\n", ext_version, error_message(r));
	CHECK_EQ_INT(r, 0);

	snprintf(cmd, sizeof(cmd), "e2fsck -fn '%s' >/dev/null 2>&1", path);
	CHECK_EQ_INT(system(cmd), 0);

	snprintf(cmd, sizeof(cmd), "debugfs -R 'cat /persistence.conf' '%s' 2>/dev/null", path);
	p = popen(cmd, "r");
	if (p != NULL) {
		CHECK(fgets(out, sizeof(out), p) != NULL);
		pclose(p);
	}
	CHECK_EQ_STR(out, "/ union\n");

	snprintf(cmd, sizeof(cmd), "blkid -p -o value -s TYPE '%s'", path);
	p = popen(cmd, "r");
	out[0] = 0;
	if (p != NULL) {
		CHECK(fgets(out, sizeof(out), p) != NULL);
		pclose(p);
	}
	out[strcspn(out, "\n")] = 0;
	CHECK_EQ_STR(out, ext_version == 2 ? "ext2" : ext_version == 3 ? "ext3" : "ext4");
	unlink(path);
}

int main(void)
{
	char dir[] = "/tmp/rufus-test-ext2fs-XXXXXX";

	if (system("command -v e2fsck debugfs blkid >/dev/null 2>&1") != 0) {
		fprintf(stderr, "e2fsprogs/blkid not found, skipping\n");
		return 77;
	}
	if (mkdtemp(dir) == NULL)
		return 1;
	srand((unsigned)time(NULL));
	initialize_ext2_error_table();

	test_version(dir, 2);
	test_version(dir, 3);
	test_version(dir, 4);

	rmdir(dir);
	return TEST_RESULT();
}

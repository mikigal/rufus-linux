/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual disk tests
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
 * The VHD and VHDX images of vdisk.c, against the ones of qemu-img: the images
 * that qemu-img creates (fixed and dynamic VHD and VHDX) read as the disk they
 * were made from, and the images that VdiskSave() creates are valid for
 * qemu-img, and give that disk back.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rufus.h"
#include "ui_iface.h"
#include "missing.h"
#include "vdisk.h"
#include "test.h"
#include "test_files.h"

// Not a multiple of the 32 MB blocks of VHDX, nor of a CHS geometry
#define DISK_SIZE       (70 * MB + 7 * 512)

static uint8_t* disk = NULL;

// A disk with data, and blocks that only hold zeroes
static void make_disk(const char* path)
{
	uint64_t i;

	disk = calloc(1, DISK_SIZE);
	CHECK(disk != NULL);
	if (disk == NULL)
		return;
	for (i = 0; i < DISK_SIZE; i++) {
		// Data in the first block, the third block (a part of it), and the end of the disk
		if (i < 1 * MB || (i >= 64 * MB + 3 * KB && i < 66 * MB) || i >= DISK_SIZE - 4 * KB)
			disk[i] = (uint8_t)((i * 13) ^ (i >> 10));
	}
	disk[0x1fe] = 0x55;
	disk[0x1ff] = 0xaa;
	CHECK(write_file(path, disk, DISK_SIZE) == DISK_SIZE);
}

// Read a whole image through vdisk.c, and compare it to the disk
static void check_image(const char* path, enum vdisk_type type, const char* desc)
{
	int fd = open(path, O_RDONLY);
	uint8_t* buf = malloc(DISK_SIZE + 512);
	uint64_t pos;
	ssize_t r;
	size_t len;
	vdisk_t* vd;

	CHECK(fd >= 0 && buf != NULL);
	if (fd < 0 || buf == NULL)
		goto out;
	vd = VdiskOpen(fd, type);
	if (vd == NULL) {
		fprintf(stderr, "Could not open %s\n", desc);
		test_failures++;
		goto out;
	}
	CHECK_EQ_INT(VdiskSize(vd), DISK_SIZE);
	// In pieces that cross the blocks
	for (pos = 0; pos < DISK_SIZE; pos += len) {
		len = (size_t)MIN(3 * MB + 512, DISK_SIZE - pos);
		r = VdiskRead(vd, &buf[pos], len, pos);
		if (r != (ssize_t)len) {
			fprintf(stderr, "Could not read %s at %llu\n", desc, (unsigned long long)pos);
			test_failures++;
			break;
		}
	}
	if (memcmp(buf, disk, DISK_SIZE) != 0) {
		fprintf(stderr, "The disk of %s differs\n", desc);
		test_failures++;
	}
	// Past the end
	CHECK_EQ_INT(VdiskRead(vd, buf, 1024, DISK_SIZE - 512), 512);
	CHECK_EQ_INT(VdiskRead(vd, buf, 1024, DISK_SIZE), 0);
	VdiskClose(vd);
out:
	if (fd >= 0)
		close(fd);
	free(buf);
}

static void test_qemu_images(void)
{
	static const struct {
		const char* format;
		const char* options;
		const char* name;
		enum vdisk_type type;
	} images[] = {
		{ "vpc", "subformat=fixed,force_size=on", "fixed.vhd", VDISK_VHD },
		{ "vpc", "subformat=dynamic,force_size=on", "dynamic.vhd", VDISK_VHD },
		{ "vhdx", "subformat=dynamic", "dynamic.vhdx", VDISK_VHDX },
		{ "vhdx", "subformat=dynamic,block_size=1M", "small-blocks.vhdx", VDISK_VHDX },
		{ "vhdx", "subformat=fixed", "fixed.vhdx", VDISK_VHDX },
	};
	size_t i;

	for (i = 0; i < ARRAYSIZE(images); i++) {
		const char* argv[] = { "qemu-img", "convert", "-q", "-f", "raw", "-O", images[i].format, "-o", images[i].options,
			tmp_path("disk.raw"), tmp_path(images[i].name), NULL };
		CHECK(run(argv));
		check_image(tmp_path(images[i].name), images[i].type, images[i].name);
	}
	// Not the expected type
	{
		int fd = open(tmp_path("dynamic.vhd"), O_RDONLY);
		CHECK(VdiskOpen(fd, VDISK_VHDX) == NULL);
		close(fd);
		fd = open(tmp_path("disk.raw"), O_RDONLY);
		CHECK(VdiskOpen(fd, VDISK_VHD) == NULL);
		close(fd);
	}
}

// A drive saved to an image is valid for qemu-img, which gets the disk back
static void test_save(void)
{
	static const struct {
		const char* name;
		const char* format;
		enum vdisk_type type;
	} images[] = {
		{ "saved.vhd", "vpc", VDISK_VHD },
		{ "saved.vhdx", "vhdx", VDISK_VHDX },
	};
	struct stat st;
	size_t i;
	int src, dst;
	char* back;

	for (i = 0; i < ARRAYSIZE(images); i++) {
		src = open(tmp_path("disk.raw"), O_RDONLY);
		dst = open(tmp_path(images[i].name), O_RDWR | O_CREAT | O_TRUNC, 0644);
		CHECK(src >= 0 && dst >= 0);
		ErrorStatus = 0;
		CHECK(VdiskSave(src, DISK_SIZE, 512, dst, images[i].type));
		close(src);
		close(dst);
		check_image(tmp_path(images[i].name), images[i].type, images[i].name);

		{
			const char* check[] = { "qemu-img", "check", "-q", "-f", images[i].format, tmp_path(images[i].name), NULL };
			const char* convert[] = { "qemu-img", "convert", "-q", "-f", images[i].format, "-O", "raw",
				tmp_path(images[i].name), tmp_path("back.raw"), NULL };
			if (images[i].type == VDISK_VHDX)
				CHECK(run(check));
			CHECK(run(convert));
		}
		back = NULL;
		CHECK(read_file(tmp_path("back.raw"), (uint8_t**)&back) == DISK_SIZE);
		CHECK(back != NULL && memcmp(back, disk, DISK_SIZE) == 0);
		free(back);
		unlink(tmp_path("back.raw"));
	}

	// The VHD can be written as a DD image: the disk, then the footer
	CHECK(stat(tmp_path("saved.vhd"), &st) == 0 && st.st_size == DISK_SIZE + 512);
	back = NULL;
	CHECK(read_file(tmp_path("saved.vhd"), (uint8_t**)&back) == DISK_SIZE + 512);
	CHECK(back != NULL && memcmp(back, disk, DISK_SIZE) == 0 && memcmp(&back[DISK_SIZE], "conectix", 8) == 0);
	free(back);
	// The VHDX does not have the blocks that only hold zeroes: only the first and the last are there
	CHECK(stat(tmp_path("saved.vhdx"), &st) == 0 && st.st_size == 4 * MB + 2 * 32 * MB);
	CHECK_CONTAINS(log_buf, "2 of 3 blocks of 32 MB were saved");
}

// A drive that only holds zeroes, with 4K sectors
static void test_save_empty(void)
{
	struct stat st;
	int src = open(tmp_path("empty.raw"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	int dst = open(tmp_path("empty.vhdx"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	const char* check[] = { "qemu-img", "check", "-q", "-f", "vhdx", tmp_path("empty.vhdx"), NULL };

	CHECK(src >= 0 && dst >= 0 && ftruncate(src, 100 * MB) == 0);
	ErrorStatus = 0;
	CHECK(VdiskSave(src, 100 * MB, 4096, dst, VDISK_VHDX));
	close(src);
	close(dst);
	CHECK(stat(tmp_path("empty.vhdx"), &st) == 0 && st.st_size == 4 * MB);
	CHECK(run(check));
	// A size that is not a multiple of a sector is refused
	src = open(tmp_path("empty.raw"), O_RDONLY);
	dst = open(tmp_path("empty.vhdx"), O_RDWR);
	CHECK(!VdiskSave(src, 100 * MB + 1, 512, dst, VDISK_VHDX));
	close(src);
	close(dst);
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log };
	const char* tools[] = { "qemu-img" };

	ui_set_iface(&iface);
	if (!have_tools(tools, ARRAYSIZE(tools)))
		return 77;
	if (!test_files_init())
		return 1;
	make_disk(tmp_path("disk.raw"));

	test_qemu_images();
	test_save();
	test_save_empty();

	free(disk);
	test_files_cleanup();
	return TEST_RESULT();
}

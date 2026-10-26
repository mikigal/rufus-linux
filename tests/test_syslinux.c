/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the Syslinux libfat and libinstaller libraries
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
 * This mirrors what Rufus does to install Syslinux on a FAT32 volume: create
 * LDLINUX.SYS, locate its sectors with libfat and patch it with libinstaller.
 * The FAT32 volume is created with mkfs.fat and LDLINUX.SYS is then added by
 * hand (FAT entries + directory entry), so that no mount is required.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libfat.h"
#include "syslinux.h"
#include "test.h"

#define IMAGE_SIZE      (64 * 1024 * 1024)
#define FIRST_CLUSTER   3
#define LDLINUX_MAGIC   0x3eb202fe
#define ADV_SIZE        512

/* Globals that Rufus' syslinux.c normally provides */
unsigned char* syslinux_ldlinux[2];
unsigned long syslinux_ldlinux_len[2];
uint32_t SECTOR_SHIFT = 9;
uint32_t SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_SHIFT = 9;
uint32_t LIBFAT_SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_MASK = 511;

void uprintf(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(&p[2], (uint16_t)(v >> 16)); }

static unsigned char* load(const char* path, unsigned long* len)
{
	struct stat st;
	unsigned char* buf;
	FILE* f = fopen(path, "rb");

	if (f == NULL || fstat(fileno(f), &st) != 0) {
		if (f != NULL)
			fclose(f);
		return NULL;
	}
	buf = malloc((size_t)st.st_size);
	*len = (unsigned long)fread(buf, 1, (size_t)st.st_size, f);
	fclose(f);
	return buf;
}

static int read_sector(intptr_t fd, void* buf, size_t secsize, libfat_sector_t sector)
{
	return (int)pread((int)fd, buf, secsize, (off_t)(sector * secsize));
}

int main(void)
{
	char image[] = "/tmp/rufus-test-syslinux-XXXXXX";
	char cmd[256], path[4096];
	const char* src_dir = getenv("RUFUS_SOURCE_DIR");
	uint8_t bpb[512], dir[512], fat[512];
	uint32_t bps, spc, reserved, nfats, fat_size, root_cluster, data_start, nclusters, i, sum;
	uint64_t file_size, nsect;
	libfat_sector_t* sectors;
	libfat_sector_t s;
	struct libfat_filesystem* fs;
	int32_t cluster;
	int fd, n, slot;

	if (src_dir == NULL || system("command -v mkfs.fat >/dev/null 2>&1") != 0) {
		fprintf(stderr, "RUFUS_SOURCE_DIR or mkfs.fat missing, skipping\n");
		return 77;
	}

	/* Rufus installs Syslinux v6 from its embedded resources */
	snprintf(path, sizeof(path), "%s/res/syslinux/ldlinux_v6.sys", src_dir);
	syslinux_ldlinux[0] = load(path, &syslinux_ldlinux_len[0]);
	snprintf(path, sizeof(path), "%s/res/syslinux/ldlinux_v6.bss", src_dir);
	syslinux_ldlinux[1] = load(path, &syslinux_ldlinux_len[1]);
	CHECK(syslinux_ldlinux[0] != NULL && syslinux_ldlinux[1] != NULL);
	if (syslinux_ldlinux[0] == NULL || syslinux_ldlinux[1] == NULL)
		return TEST_RESULT();

	fd = mkstemp(image);
	if (fd < 0 || ftruncate(fd, IMAGE_SIZE) != 0)
		return 1;
	close(fd);
	snprintf(cmd, sizeof(cmd), "mkfs.fat -F 32 -n RUFUSTEST %s >/dev/null", image);
	CHECK_EQ_INT(system(cmd), 0);
	fd = open(image, O_RDWR);
	unlink(image);
	CHECK(fd >= 0);
	if (fd < 0)
		return TEST_RESULT();

	/* Parse the BPB */
	CHECK_EQ_INT(pread(fd, bpb, sizeof(bpb), 0), (ssize_t)sizeof(bpb));
	bps = le16(&bpb[0x0B]);
	spc = bpb[0x0D];
	reserved = le16(&bpb[0x0E]);
	nfats = bpb[0x10];
	fat_size = le32(&bpb[0x24]);
	root_cluster = le32(&bpb[0x2C]);
	data_start = reserved + nfats * fat_size;
	CHECK_EQ_INT(bps, 512);
	CHECK(root_cluster < FIRST_CLUSTER);

	/* Allocate a contiguous cluster chain for LDLINUX.SYS + 2 ADV sectors */
	file_size = syslinux_ldlinux_len[0] + 2 * ADV_SIZE;
	nsect = (file_size + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
	nclusters = (uint32_t)((nsect + spc - 1) / spc);
	for (n = 0; n < (int)nfats; n++) {
		uint32_t fat_start = reserved + n * fat_size;
		for (i = 0; i < nclusters; i++) {
			uint32_t c = FIRST_CLUSTER + i, sec = fat_start + (c * 4) / bps;
			CHECK_EQ_INT(pread(fd, fat, bps, (off_t)sec * bps), (ssize_t)bps);
			put32(&fat[(c * 4) % bps], (i == nclusters - 1) ? 0x0FFFFFFF : c + 1);
			CHECK_EQ_INT(pwrite(fd, fat, bps, (off_t)sec * bps), (ssize_t)bps);
		}
	}
	CHECK_EQ_INT(pwrite(fd, syslinux_ldlinux[0], syslinux_ldlinux_len[0],
		(off_t)(data_start + (FIRST_CLUSTER - 2) * spc) * bps), (ssize_t)syslinux_ldlinux_len[0]);

	/* Add the directory entry to the first free slot of the root directory */
	CHECK_EQ_INT(pread(fd, dir, sizeof(dir), (off_t)(data_start + (root_cluster - 2) * spc) * bps), (ssize_t)sizeof(dir));
	for (slot = 0; slot < 16 && dir[slot * 32] != 0; slot++);
	CHECK(slot < 16);
	memcpy(&dir[slot * 32], "LDLINUX SYS", 11);
	dir[slot * 32 + 0x0B] = 0x07;	/* read-only, hidden, system */
	put16(&dir[slot * 32 + 0x14], FIRST_CLUSTER >> 16);
	put16(&dir[slot * 32 + 0x1A], FIRST_CLUSTER & 0xFFFF);
	put32(&dir[slot * 32 + 0x1C], (uint32_t)file_size);
	CHECK_EQ_INT(pwrite(fd, dir, sizeof(dir), (off_t)(data_start + (root_cluster - 2) * spc) * bps), (ssize_t)sizeof(dir));

	/* Locate the file's sectors the same way Rufus does */
	fs = libfat_open(read_sector, (intptr_t)fd);
	CHECK(fs != NULL);
	if (fs == NULL)
		return TEST_RESULT();
	cluster = libfat_searchdir(fs, 0, "LDLINUX SYS", NULL);
	CHECK_EQ_INT(cluster, FIRST_CLUSTER);
	CHECK(libfat_searchdir(fs, 0, "NOTTHERESYS", NULL) < 0);
	sectors = calloc(nsect, sizeof(*sectors));
	n = 0;
	s = libfat_clustertosector(fs, cluster);
	while (s && n < (int)nsect) {
		sectors[n++] = s;
		s = libfat_nextsector(fs, s);
	}
	libfat_close(fs);
	CHECK_EQ_INT(n, nsect);
	CHECK_EQ_INT(sectors[0], data_start + (FIRST_CLUSTER - 2) * spc);
	for (i = 1; i < (uint32_t)n; i++)
		CHECK_EQ_INT(sectors[i], sectors[0] + i);

	/* Patch ldlinux.sys and check its self-checksum */
	CHECK(syslinux_patch(sectors, n, 0, 0, "/boot/syslinux/", NULL) > 0);
	sum = 0;
	for (i = 0; i < syslinux_ldlinux_len[0] / 4; i++)
		sum += le32(&syslinux_ldlinux[0][i * 4]);
	CHECK_EQ_INT(sum, LDLINUX_MAGIC);
	CHECK(memmem(syslinux_ldlinux[0], syslinux_ldlinux_len[0], "/boot/syslinux/", 15) != NULL);
	/* An image that is too short for the file must be rejected */
	CHECK_EQ_INT(syslinux_patch(sectors, 1, 0, 0, NULL, NULL), -1);

	free(sectors);
	free(syslinux_ldlinux[0]);
	free(syslinux_ldlinux[1]);
	close(fd);
	return TEST_RESULT();
}

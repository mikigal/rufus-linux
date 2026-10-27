/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual disks (VHD and VHDX)
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
 * What upstream does with VirtDisk: read the disk of a VHD or VHDX image (to
 * write it to a drive), and save a drive to a VHD or VHDX image. The formats
 * are the ones of the Microsoft specifications:
 * - VHD: "Virtual Hard Disk Image Format Specification" (a 512-byte footer, after
 *   the data of a fixed disk, or a block allocation table for a dynamic one);
 * - VHDX: "[MS-VHDX]: Virtual Hard Disk v2 (VHDX) File Format" (headers, region
 *   table, metadata, and a block allocation table with sector bitmap entries).
 */

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/stat.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "vdisk.h"

#define VHD_FOOTER_SIZE             512
#define VHD_TYPE_FIXED              2
#define VHD_TYPE_DYNAMIC            3
#define VHD_TYPE_DIFFERENCING       4
#define VHD_UNALLOCATED             0xffffffff
// January 1, 2000, the epoch of the timestamps of VHD
#define VHD_EPOCH                   946684800

#define VHDX_SIGNATURE              "vhdxfile"
#define VHDX_HEADER1_OFFSET         (64 * KB)
#define VHDX_HEADER2_OFFSET         (128 * KB)
#define VHDX_HEADER_SIZE            (4 * KB)
#define VHDX_REGION1_OFFSET         (192 * KB)
#define VHDX_REGION2_OFFSET         (256 * KB)
#define VHDX_REGION_SIZE            (64 * KB)
#define VHDX_LOG_OFFSET             (1 * MB)
#define VHDX_LOG_SIZE               (1 * MB)
#define VHDX_METADATA_OFFSET        (2 * MB)
#define VHDX_METADATA_SIZE          (1 * MB)
#define VHDX_BAT_OFFSET             (3 * MB)
// The size of the blocks of the VHDX images that VirtDisk creates
#define VHDX_BLOCK_SIZE             (32 * MB)
#define VHDX_PAYLOAD_FULLY_PRESENT  6
#define VHDX_PAYLOAD_PARTIALLY_PRESENT 7
#define VHDX_STATE_MASK             7
#define VHDX_MAX_BLOCK_SIZE         (256 * MB)
// Metadata entries: IsVirtualDisk and IsRequired
#define VHDX_META_VIRTUAL_DISK      0x02
#define VHDX_META_REQUIRED          0x04
// File parameters: HasParent
#define VHDX_HAS_PARENT             0x02

// GUIDs, in the byte order of the files (the first three fields are little endian)
#define GUID_BYTES(d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7) { \
	(d1) & 0xff, ((d1) >> 8) & 0xff, ((d1) >> 16) & 0xff, ((d1) >> 24) & 0xff, \
	(d2) & 0xff, ((d2) >> 8) & 0xff, (d3) & 0xff, ((d3) >> 8) & 0xff, b0, b1, b2, b3, b4, b5, b6, b7 }
static const uint8_t bat_guid[16] =
	GUID_BYTES(0x2dc27766, 0xf623, 0x4200, 0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08);
static const uint8_t metadata_guid[16] =
	GUID_BYTES(0x8b7ca206, 0x4790, 0x4b9a, 0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e);
static const uint8_t file_parameters_guid[16] =
	GUID_BYTES(0xcaa16737, 0xfa36, 0x4d43, 0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b);
static const uint8_t disk_size_guid[16] =
	GUID_BYTES(0x2fa54224, 0xcd1b, 0x4876, 0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8);
static const uint8_t page83_guid[16] =
	GUID_BYTES(0xbeca12ab, 0xb2e6, 0x4523, 0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46);
static const uint8_t logical_sector_guid[16] =
	GUID_BYTES(0x8141bf1d, 0xa96f, 0x4709, 0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f);
static const uint8_t physical_sector_guid[16] =
	GUID_BYTES(0xcda348c7, 0x445d, 0x4471, 0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56);

struct vdisk {
	int fd;
	uint64_t size;					// Of the disk
	uint32_t block_size;			// 0 for a fixed VHD, whose data is at the start of the file
	uint64_t nb_blocks;
	uint64_t* blocks;				// The offsets of the data of the blocks in the file (0: zeroes)
};

/*
 * Little and big endian fields
 */
static inline uint16_t get_le16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return le16toh(v); }
static inline uint32_t get_le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return le32toh(v); }
static inline uint64_t get_le64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return le64toh(v); }
static inline uint32_t get_be32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return be32toh(v); }
static inline uint64_t get_be64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return be64toh(v); }
static inline void put_le16(uint8_t* p, uint16_t v) { v = htole16(v); memcpy(p, &v, 2); }
static inline void put_le32(uint8_t* p, uint32_t v) { v = htole32(v); memcpy(p, &v, 4); }
static inline void put_le64(uint8_t* p, uint64_t v) { v = htole64(v); memcpy(p, &v, 8); }
static inline void put_be16(uint8_t* p, uint16_t v) { v = htobe16(v); memcpy(p, &v, 2); }
static inline void put_be32(uint8_t* p, uint32_t v) { v = htobe32(v); memcpy(p, &v, 4); }
static inline void put_be64(uint8_t* p, uint64_t v) { v = htobe64(v); memcpy(p, &v, 8); }

// CRC-32C (Castagnoli), the checksum of the VHDX headers and region tables
static uint32_t crc32c(const uint8_t* buf, size_t len)
{
	static uint32_t table[256];
	uint32_t crc = 0xffffffff, c;
	size_t i;
	int j;

	if (table[1] == 0) {
		for (i = 0; i < 256; i++) {
			for (c = (uint32_t)i, j = 0; j < 8; j++)
				c = (c & 1) ? (c >> 1) ^ 0x82f63b78 : (c >> 1);
			table[i] = c;
		}
	}
	for (i = 0; i < len; i++)
		crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

// Whether the checksum at offset 4 of a VHDX structure is valid
static BOOL vhdx_checksum_ok(uint8_t* buf, size_t len)
{
	uint32_t checksum = get_le32(&buf[4]);
	BOOL r;

	memset(&buf[4], 0, 4);
	r = (crc32c(buf, len) == checksum);
	put_le32(&buf[4], checksum);
	return r;
}

static BOOL read_at(int fd, void* buf, size_t len, uint64_t offset)
{
	ssize_t r;
	size_t done = 0;

	while (done < len) {
		r = pread(fd, (uint8_t*)buf + done, len - done, (off_t)(offset + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (r == 0)
				errno = EIO;
			return FALSE;
		}
		done += (size_t)r;
	}
	return TRUE;
}

static BOOL write_at(int fd, const void* buf, size_t len, uint64_t offset)
{
	ssize_t r;
	size_t done = 0;

	while (done < len) {
		r = pwrite(fd, (const uint8_t*)buf + done, len - done, (off_t)(offset + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (r == 0)
				errno = EIO;
			return FALSE;
		}
		done += (size_t)r;
	}
	return TRUE;
}

/*
 * Reading
 */

static BOOL OpenVhd(vdisk_t* vd, uint64_t file_size)
{
	uint8_t footer[VHD_FOOTER_SIZE], header[1024], *bat = NULL;
	uint64_t i, table_offset, entry;
	uint32_t type, max_entries, bitmap_size;

	// The footer is the last 512 bytes (or 511, for images created before Virtual PC 2004)
	if ((file_size < VHD_FOOTER_SIZE) || ((!read_at(vd->fd, footer, VHD_FOOTER_SIZE, file_size - VHD_FOOTER_SIZE) ||
		(memcmp(footer, "conectix", 8) != 0)) && (!read_at(vd->fd, footer, VHD_FOOTER_SIZE - 1, file_size - VHD_FOOTER_SIZE + 1) ||
		(memcmp(footer, "conectix", 8) != 0)))) {
		uprintf("  Not a VHD image");
		return FALSE;
	}
	vd->size = get_be64(&footer[0x30]);
	type = get_be32(&footer[0x3c]);
	switch (type) {
	case VHD_TYPE_FIXED:
		if (vd->size > file_size - VHD_FOOTER_SIZE + 1) {
			uprintf("  The VHD image is truncated");
			return FALSE;
		}
		return TRUE;
	case VHD_TYPE_DYNAMIC:
		break;
	case VHD_TYPE_DIFFERENCING:
		uprintf("  Differencing VHDs are not supported");
		return FALSE;
	default:
		uprintf("  Unknown VHD type %d", type);
		return FALSE;
	}

	if (!read_at(vd->fd, header, sizeof(header), get_be64(&footer[0x10])) || (memcmp(header, "cxsparse", 8) != 0)) {
		uprintf("  Invalid dynamic VHD header");
		return FALSE;
	}
	table_offset = get_be64(&header[0x10]);
	max_entries = get_be32(&header[0x1c]);
	vd->block_size = get_be32(&header[0x20]);
	if ((vd->block_size < 512) || (vd->block_size % 512 != 0) || (vd->block_size > VHDX_MAX_BLOCK_SIZE)) {
		uprintf("  Invalid VHD block size %d", vd->block_size);
		return FALSE;
	}
	vd->nb_blocks = (vd->size + vd->block_size - 1) / vd->block_size;
	if (vd->nb_blocks > max_entries) {
		uprintf("  The block allocation table of the VHD is too small");
		return FALSE;
	}
	bat = malloc(vd->nb_blocks * 4);
	vd->blocks = calloc(vd->nb_blocks, sizeof(uint64_t));
	if ((bat == NULL) || (vd->blocks == NULL) || !read_at(vd->fd, bat, vd->nb_blocks * 4, table_offset)) {
		uprintf("  Could not read the block allocation table of the VHD");
		free(bat);
		return FALSE;
	}
	// The data of a block comes after its sector bitmap, padded to a sector
	bitmap_size = CEILING_ALIGN(vd->block_size / 512 / 8, 512);
	for (i = 0; i < vd->nb_blocks; i++) {
		entry = get_be32(&bat[i * 4]);
		if (entry == VHD_UNALLOCATED)
			continue;
		vd->blocks[i] = entry * 512 + bitmap_size;
		if (vd->blocks[i] + vd->block_size > file_size) {
			uprintf("  Block %" PRIu64 " of the VHD is out of the file", i);
			free(bat);
			return FALSE;
		}
	}
	free(bat);
	return TRUE;
}

// The current header of a VHDX: the valid one with the highest sequence number
static BOOL ReadVhdxHeader(int fd, uint8_t* header)
{
	uint8_t h[2][VHDX_HEADER_SIZE];
	BOOL valid[2];
	int i;

	for (i = 0; i < 2; i++) {
		valid[i] = read_at(fd, h[i], VHDX_HEADER_SIZE, (i == 0) ? VHDX_HEADER1_OFFSET : VHDX_HEADER2_OFFSET) &&
			(memcmp(h[i], "head", 4) == 0) && vhdx_checksum_ok(h[i], VHDX_HEADER_SIZE);
	}
	if (!valid[0] && !valid[1])
		return FALSE;
	i = (valid[0] && (!valid[1] || get_le64(&h[0][8]) >= get_le64(&h[1][8]))) ? 0 : 1;
	memcpy(header, h[i], VHDX_HEADER_SIZE);
	return TRUE;
}

static BOOL OpenVhdx(vdisk_t* vd, uint64_t file_size)
{
	static const uint8_t zero_guid[16] = { 0 };
	uint8_t sig[8], header[VHDX_HEADER_SIZE], *region = NULL, *meta = NULL, *bat = NULL, *p;
	uint64_t bat_offset = 0, meta_offset = 0, i, entry, nb_entries, chunk_ratio;
	uint32_t bat_length = 0, meta_length = 0, n, logical_sector_size = 0, flags = 0, offset, length;
	BOOL r = FALSE, has_size = FALSE;

	if (!read_at(vd->fd, sig, sizeof(sig), 0) || (memcmp(sig, VHDX_SIGNATURE, 8) != 0)) {
		uprintf("  Not a VHDX image");
		return FALSE;
	}
	if (!ReadVhdxHeader(vd->fd, header)) {
		uprintf("  The headers of the VHDX are corrupted");
		return FALSE;
	}
	// A log that was not replayed means that the data may not be consistent
	if (memcmp(&header[0x30], zero_guid, 16) != 0) {
		uprintf("  The VHDX image needs to be repaired (its log was not replayed)");
		return FALSE;
	}

	region = malloc(VHDX_REGION_SIZE);
	if ((region == NULL) || ((!read_at(vd->fd, region, VHDX_REGION_SIZE, VHDX_REGION1_OFFSET) ||
		(memcmp(region, "regi", 4) != 0) || !vhdx_checksum_ok(region, VHDX_REGION_SIZE)) &&
		(!read_at(vd->fd, region, VHDX_REGION_SIZE, VHDX_REGION2_OFFSET) ||
		(memcmp(region, "regi", 4) != 0) || !vhdx_checksum_ok(region, VHDX_REGION_SIZE)))) {
		uprintf("  The region table of the VHDX is corrupted");
		goto out;
	}
	n = get_le32(&region[8]);
	for (i = 0; i < n && i < (VHDX_REGION_SIZE - 16) / 32; i++) {
		p = &region[16 + i * 32];
		if (memcmp(p, bat_guid, 16) == 0) {
			bat_offset = get_le64(&p[16]);
			bat_length = get_le32(&p[24]);
		} else if (memcmp(p, metadata_guid, 16) == 0) {
			meta_offset = get_le64(&p[16]);
			meta_length = get_le32(&p[24]);
		} else if (get_le32(&p[28]) & 1) {
			uprintf("  The VHDX has a region that is not supported");
			goto out;
		}
	}
	if ((bat_length == 0) || (meta_length < 64 * KB) || (meta_length > 64 * MB) ||
		(bat_offset + bat_length > file_size) || (meta_offset + meta_length > file_size)) {
		uprintf("  The VHDX has no valid metadata or block allocation table");
		goto out;
	}

	meta = malloc(meta_length);
	if ((meta == NULL) || !read_at(vd->fd, meta, meta_length, meta_offset) || (memcmp(meta, "metadata", 8) != 0)) {
		uprintf("  The metadata of the VHDX are corrupted");
		goto out;
	}
	n = get_le16(&meta[10]);
	for (i = 0; i < n && i < 2047; i++) {
		p = &meta[32 + i * 32];
		offset = get_le32(&p[16]);
		length = get_le32(&p[20]);
		if ((length == 0) || ((uint64_t)offset + length > meta_length))
			continue;
		if ((memcmp(p, file_parameters_guid, 16) == 0) && (length >= 8)) {
			vd->block_size = get_le32(&meta[offset]);
			flags = get_le32(&meta[offset + 4]);
		} else if ((memcmp(p, disk_size_guid, 16) == 0) && (length >= 8)) {
			vd->size = get_le64(&meta[offset]);
			has_size = TRUE;
		} else if ((memcmp(p, logical_sector_guid, 16) == 0) && (length >= 4)) {
			logical_sector_size = get_le32(&meta[offset]);
		} else if ((get_le32(&p[24]) & VHDX_META_REQUIRED) && (memcmp(p, page83_guid, 16) != 0) &&
			(memcmp(p, physical_sector_guid, 16) != 0)) {
			uprintf("  The VHDX has metadata that are not supported");
			goto out;
		}
	}
	if (flags & VHDX_HAS_PARENT) {
		uprintf("  Differencing VHDX images are not supported");
		goto out;
	}
	if (!has_size || ((logical_sector_size != 512) && (logical_sector_size != 4096)) || (vd->block_size < 1 * MB) ||
		(vd->block_size > VHDX_MAX_BLOCK_SIZE) || (!IS_POWER_OF_2(vd->block_size))) {
		uprintf("  Invalid VHDX parameters");
		goto out;
	}

	// The BAT has a sector bitmap entry after each chunk of blocks
	chunk_ratio = (8388608ULL * logical_sector_size) / vd->block_size;
	vd->nb_blocks = (vd->size + vd->block_size - 1) / vd->block_size;
	nb_entries = (vd->nb_blocks == 0) ? 0 : vd->nb_blocks + (vd->nb_blocks - 1) / chunk_ratio;
	if (nb_entries * 8 > bat_length) {
		uprintf("  The block allocation table of the VHDX is too small");
		goto out;
	}
	bat = malloc(nb_entries * 8 + 1);
	vd->blocks = calloc(vd->nb_blocks + 1, sizeof(uint64_t));
	if ((bat == NULL) || (vd->blocks == NULL) || !read_at(vd->fd, bat, nb_entries * 8, bat_offset)) {
		uprintf("  Could not read the block allocation table of the VHDX");
		goto out;
	}
	for (i = 0; i < vd->nb_blocks; i++) {
		entry = get_le64(&bat[(i + i / chunk_ratio) * 8]);
		switch (entry & VHDX_STATE_MASK) {
		case VHDX_PAYLOAD_FULLY_PRESENT:
			vd->blocks[i] = (entry >> 20) * MB;
			if ((vd->blocks[i] == 0) || (vd->blocks[i] + vd->block_size > file_size)) {
				uprintf("  Block %" PRIu64 " of the VHDX is out of the file", i);
				goto out;
			}
			break;
		case VHDX_PAYLOAD_PARTIALLY_PRESENT:
			uprintf("  Differencing VHDX images are not supported");
			goto out;
		default:
			// Not present, undefined, zero or unmapped: zeroes
			break;
		}
	}
	r = TRUE;

out:
	free(region);
	free(meta);
	free(bat);
	return r;
}

vdisk_t* VdiskOpen(int fd, enum vdisk_type type)
{
	vdisk_t* vd = calloc(1, sizeof(vdisk_t));
	struct stat st;
	BOOL r;

	if (vd == NULL)
		return NULL;
	vd->fd = fd;
	if (fstat(fd, &st) != 0) {
		free(vd);
		return NULL;
	}
	r = (type == VDISK_VHDX) ? OpenVhdx(vd, (uint64_t)st.st_size) : OpenVhd(vd, (uint64_t)st.st_size);
	if (!r) {
		VdiskClose(vd);
		errno = EINVAL;
		return NULL;
	}
	return vd;
}

void VdiskClose(vdisk_t* vd)
{
	if (vd == NULL)
		return;
	free(vd->blocks);
	free(vd);
}

uint64_t VdiskSize(const vdisk_t* vd)
{
	return (vd == NULL) ? 0 : vd->size;
}

ssize_t VdiskRead(vdisk_t* vd, void* buf, size_t len, uint64_t offset)
{
	uint64_t block, in_block;
	size_t n, done = 0;

	if (offset >= vd->size)
		return 0;
	len = (size_t)MIN(len, vd->size - offset);
	// A fixed VHD is the disk, followed by its footer
	if (vd->block_size == 0)
		return read_at(vd->fd, buf, len, offset) ? (ssize_t)len : -1;
	while (done < len) {
		block = (offset + done) / vd->block_size;
		in_block = (offset + done) % vd->block_size;
		n = (size_t)MIN(len - done, vd->block_size - in_block);
		if (vd->blocks[block] == 0)
			memset((uint8_t*)buf + done, 0, n);
		else if (!read_at(vd->fd, (uint8_t*)buf + done, n, vd->blocks[block] + in_block))
			return -1;
		done += n;
	}
	return (ssize_t)len;
}

/*
 * Writing
 */

static void RandomGuid(uint8_t* guid)
{
	if (getrandom(guid, 16, 0) != 16) {
		for (int i = 0; i < 16; i++)
			guid[i] = (uint8_t)rand();
	}
	// Version 4, variant 1
	guid[7] = (guid[7] & 0x0f) | 0x40;
	guid[8] = (guid[8] & 0x3f) | 0x80;
}

// The CHS geometry of a VHD, as the specification computes it
static void VhdGeometry(uint64_t size, uint16_t* cylinders, uint8_t* heads, uint8_t* sectors)
{
	uint64_t total_sectors = size / 512, cylinder_times_heads;
	uint32_t h, s;

	if (total_sectors > 65535 * 16 * 255)
		total_sectors = 65535 * 16 * 255;
	if (total_sectors >= 65535 * 16 * 63) {
		s = 255;
		h = 16;
		cylinder_times_heads = total_sectors / s;
	} else {
		s = 17;
		cylinder_times_heads = total_sectors / s;
		h = (uint32_t)((cylinder_times_heads + 1023) / 1024);
		if (h < 4)
			h = 4;
		if ((cylinder_times_heads >= h * 1024) || (h > 16)) {
			s = 31;
			h = 16;
			cylinder_times_heads = total_sectors / s;
		}
		if (cylinder_times_heads >= h * 1024) {
			s = 63;
			h = 16;
			cylinder_times_heads = total_sectors / s;
		}
	}
	*cylinders = (uint16_t)(cylinder_times_heads / h);
	*heads = (uint8_t)h;
	*sectors = (uint8_t)s;
	// A size that the geometry can't give: the maximum geometry tells the readers that use it
	// (qemu, for instance) to use the size of the footer, as VirtDisk does
	if ((uint64_t)*cylinders * h * s * 512 != size) {
		*cylinders = 65535;
		*heads = 16;
		*sectors = 255;
	}
}

static BOOL WriteVhdFooter(int fd, uint64_t size)
{
	uint8_t footer[VHD_FOOTER_SIZE] = { 0 };
	uint32_t checksum = 0;
	uint16_t cylinders;
	uint8_t heads, sectors;
	time_t now = time(NULL);
	int i;

	memcpy(footer, "conectix", 8);
	put_be32(&footer[0x08], 2);					// Features: reserved, always set
	put_be32(&footer[0x0c], 0x00010000);		// Format version
	put_be64(&footer[0x10], UINT64_MAX);		// Data offset: none, for a fixed disk
	put_be32(&footer[0x18], (uint32_t)((now > VHD_EPOCH) ? now - VHD_EPOCH : 0));
	memcpy(&footer[0x1c], "rufs", 4);			// Creator application
	put_be32(&footer[0x20], ((uint32_t)rufus_version[0] << 16) | rufus_version[1]);
	memcpy(&footer[0x24], "Wi2k", 4);			// Creator host OS, as qemu also sets it
	put_be64(&footer[0x28], size);				// Original size
	put_be64(&footer[0x30], size);				// Current size
	VhdGeometry(size, &cylinders, &heads, &sectors);
	put_be16(&footer[0x38], cylinders);
	footer[0x3a] = heads;
	footer[0x3b] = sectors;
	put_be32(&footer[0x3c], VHD_TYPE_FIXED);
	RandomGuid(&footer[0x44]);
	for (i = 0; i < VHD_FOOTER_SIZE; i++)
		checksum += footer[i];
	put_be32(&footer[0x40], ~checksum);
	return write_at(fd, footer, sizeof(footer), size);
}

static BOOL IsZero(const uint8_t* buf, size_t len)
{
	const uint64_t* p = (const uint64_t*)buf;
	size_t i;

	for (i = 0; i < len / 8; i++) {
		if (p[i] != 0)
			return FALSE;
	}
	for (i = len & ~7; i < len; i++) {
		if (buf[i] != 0)
			return FALSE;
	}
	return TRUE;
}

// The headers, region tables and metadata of a VHDX, whose BAT is at VHDX_BAT_OFFSET
static BOOL WriteVhdxMetadata(int fd, uint64_t size, uint32_t logical_sector_size, uint32_t physical_sector_size,
	uint64_t bat_length)
{
	uint8_t* buf = calloc(1, VHDX_METADATA_SIZE);
	uint8_t *p, guid[16];
	const struct {
		const uint8_t* guid;
		uint32_t length, flags;
	} items[] = {
		{ file_parameters_guid, 8, VHDX_META_REQUIRED },
		{ disk_size_guid, 8, VHDX_META_VIRTUAL_DISK | VHDX_META_REQUIRED },
		{ page83_guid, 16, VHDX_META_VIRTUAL_DISK | VHDX_META_REQUIRED },
		{ logical_sector_guid, 4, VHDX_META_VIRTUAL_DISK | VHDX_META_REQUIRED },
		{ physical_sector_guid, 4, VHDX_META_VIRTUAL_DISK | VHDX_META_REQUIRED },
	};
	uint32_t offset = 64 * KB;
	BOOL r = FALSE;
	int i;

	if (buf == NULL)
		return FALSE;

	// The file type identifier, with the creator in UTF-16
	memcpy(buf, VHDX_SIGNATURE, 8);
	for (i = 0, p = (uint8_t*)APPLICATION_NAME " for Linux"; p[i] != 0; i++)
		put_le16(&buf[8 + 2 * i], p[i]);
	if (!write_at(fd, buf, 64 * KB, 0))
		goto out;

	// The two headers, without a log
	memset(buf, 0, VHDX_HEADER_SIZE);
	memcpy(buf, "head", 4);
	RandomGuid(&buf[16]);					// FileWriteGuid
	RandomGuid(&buf[32]);					// DataWriteGuid
	put_le16(&buf[0x40], 0);				// LogVersion
	put_le16(&buf[0x42], 1);				// Version
	put_le32(&buf[0x44], VHDX_LOG_SIZE);
	put_le64(&buf[0x48], VHDX_LOG_OFFSET);
	for (i = 0; i < 2; i++) {
		put_le64(&buf[8], i + 1);			// SequenceNumber
		put_le32(&buf[4], 0);
		put_le32(&buf[4], crc32c(buf, VHDX_HEADER_SIZE));
		if (!write_at(fd, buf, VHDX_HEADER_SIZE, (i == 0) ? VHDX_HEADER1_OFFSET : VHDX_HEADER2_OFFSET))
			goto out;
	}

	// The two region tables: the BAT and the metadata
	memset(buf, 0, VHDX_REGION_SIZE);
	memcpy(buf, "regi", 4);
	put_le32(&buf[8], 2);
	memcpy(&buf[16], bat_guid, 16);
	put_le64(&buf[32], VHDX_BAT_OFFSET);
	put_le32(&buf[40], (uint32_t)bat_length);
	put_le32(&buf[44], 1);					// Required
	memcpy(&buf[48], metadata_guid, 16);
	put_le64(&buf[64], VHDX_METADATA_OFFSET);
	put_le32(&buf[72], VHDX_METADATA_SIZE);
	put_le32(&buf[76], 1);
	put_le32(&buf[4], crc32c(buf, VHDX_REGION_SIZE));
	if (!write_at(fd, buf, VHDX_REGION_SIZE, VHDX_REGION1_OFFSET) ||
		!write_at(fd, buf, VHDX_REGION_SIZE, VHDX_REGION2_OFFSET))
		goto out;

	// The metadata: a table, then the items
	memset(buf, 0, VHDX_METADATA_SIZE);
	memcpy(buf, "metadata", 8);
	put_le16(&buf[10], ARRAYSIZE(items));
	for (i = 0; i < (int)ARRAYSIZE(items); i++) {
		p = &buf[32 + 32 * i];
		memcpy(p, items[i].guid, 16);
		put_le32(&p[16], offset);
		put_le32(&p[20], items[i].length);
		put_le32(&p[24], items[i].flags);
		if (items[i].guid == file_parameters_guid) {
			put_le32(&buf[offset], VHDX_BLOCK_SIZE);
			put_le32(&buf[offset + 4], 0);	// Not LeaveBlocksAllocated, no parent
		} else if (items[i].guid == disk_size_guid) {
			put_le64(&buf[offset], size);
		} else if (items[i].guid == page83_guid) {
			RandomGuid(guid);
			memcpy(&buf[offset], guid, 16);
		} else if (items[i].guid == logical_sector_guid) {
			put_le32(&buf[offset], logical_sector_size);
		} else {
			put_le32(&buf[offset], physical_sector_size);
		}
		offset += CEILING_ALIGN(items[i].length, 8);
	}
	r = write_at(fd, buf, VHDX_METADATA_SIZE, VHDX_METADATA_OFFSET);

out:
	free(buf);
	return r;
}

BOOL VdiskSave(int src_fd, uint64_t disk_size, uint32_t sector_size, int dst_fd, enum vdisk_type type)
{
	// As VirtDisk, with the default sector size of upstream: 512-byte logical sectors, and the physical
	// ones of the drive
	uint32_t logical_sector_size = 512, physical_sector_size = (sector_size == 4096) ? 4096 : 512;
	uint64_t chunk_ratio = (8388608ULL * logical_sector_size) / VHDX_BLOCK_SIZE;
	uint64_t nb_blocks = (disk_size + VHDX_BLOCK_SIZE - 1) / VHDX_BLOCK_SIZE, nb_entries, bat_length;
	uint64_t pos, payload_offset, i, nb_present = 0;
	uint32_t buf_size = (type == VDISK_VHDX) ? VHDX_BLOCK_SIZE : DD_BUFFER_SIZE;
	uint8_t* buffer = NULL, *bat = NULL;
	size_t len;
	BOOL r = FALSE;

	if ((disk_size == 0) || (disk_size % 512 != 0)) {
		uprintf("Invalid disk size %" PRIu64, disk_size);
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	buffer = _mm_malloc(buf_size, 4096);
	if (buffer == NULL) {
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	// Start from an empty file, as the payload of a VHDX is written where it gets allocated
	if (ftruncate(dst_fd, 0) != 0) {
		uprintf("Could not truncate the image: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}

	nb_entries = nb_blocks + ((nb_blocks == 0) ? 0 : (nb_blocks - 1) / chunk_ratio);
	bat_length = CEILING_ALIGN(nb_entries * 8, 1 * MB);
	payload_offset = VHDX_BAT_OFFSET + bat_length;
	if (type == VDISK_VHDX) {
		bat = calloc(1, (size_t)bat_length);
		if (bat == NULL) {
			ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
			goto out;
		}
	}

	UpdateProgressWithInfoInit(NULL, FALSE);
	for (pos = 0, i = 0; pos < disk_size; pos += len, i++) {
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, pos, disk_size);
		CHECK_FOR_USER_CANCEL;
		len = (size_t)MIN(buf_size, disk_size - pos);
		if (!read_at(src_fd, buffer, len, pos)) {
			uprintf("Read error: %s", WindowsErrorString());
			ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
			goto out;
		}
		if (type == VDISK_VHD) {
			if (!write_at(dst_fd, buffer, len, pos))
				goto write_error;
			continue;
		}
		// VHDX: the blocks that only hold zeroes are not present
		if (IsZero(buffer, len))
			continue;
		if (len < VHDX_BLOCK_SIZE)
			memset(&buffer[len], 0, VHDX_BLOCK_SIZE - len);
		if (!write_at(dst_fd, buffer, VHDX_BLOCK_SIZE, payload_offset + nb_present * VHDX_BLOCK_SIZE))
			goto write_error;
		put_le64(&bat[(i + i / chunk_ratio) * 8],
			(((payload_offset + nb_present * VHDX_BLOCK_SIZE) / MB) << 20) | VHDX_PAYLOAD_FULLY_PRESENT);
		nb_present++;
	}

	if (type == VDISK_VHD) {
		if (!WriteVhdFooter(dst_fd, disk_size))
			goto write_error;
	} else {
		if (!write_at(dst_fd, bat, (size_t)bat_length, VHDX_BAT_OFFSET) ||
			!WriteVhdxMetadata(dst_fd, disk_size, logical_sector_size, physical_sector_size, bat_length) ||
			(ftruncate(dst_fd, (off_t)(payload_offset + nb_present * VHDX_BLOCK_SIZE)) != 0))
			goto write_error;
		uprintf("%" PRIu64 " of %" PRIu64 " blocks of %s were saved (the others are empty)", nb_present, nb_blocks,
			SizeToHumanReadable(VHDX_BLOCK_SIZE, FALSE, FALSE));
	}
	if (fsync(dst_fd) != 0)
		goto write_error;
	UpdateProgressWithInfo(OP_FORMAT, MSG_261, disk_size, disk_size);
	r = TRUE;
	goto out;

write_error:
	uprintf("Write error: %s", WindowsErrorString());
	ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
out:
	_mm_free(buffer);
	free(bat);
	return r;
}

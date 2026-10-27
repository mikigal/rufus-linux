/*
 * Rufus: The Reliable USB Formatting Utility
 * Partitioning
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
 * See partition.h. On Windows, the table itself is written by
 * IOCTL_DISK_CREATE_DISK and IOCTL_DISK_SET_DRIVE_LAYOUT_EX; here, it is
 * written directly, with the same fields: MBR entries with CHS values for a
 * 255 × 63 geometry, a GPT of 128 entries with random disk and partition GUIDs,
 * and a protective MBR.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

#include "rufus.h"
#include "options.h"
#include "partition.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"

#define GPT_ENTRIES                 128
#define GPT_ENTRY_SIZE              128
#define GPT_HEADER_SIZE             92
#define GPT_REVISION                0x00010000

/* From gpt_types.h */
static const GUID PARTITION_GENERIC_ESP =
	{ 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
static const GUID PARTITION_LINUX_DATA =
	{ 0x0FC63DAF, 0x8483, 0x4772, { 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 } };
static const GUID PARTITION_MICROSOFT_DATA =
	{ 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } };
static const GUID PARTITION_MICROSOFT_RESERVED =
	{ 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };

// A random (version 4) GUID, as CoCreateGuid() makes
static void NewGuid(GUID* guid)
{
	uint8_t b[16];
	size_t i;

	if (getrandom(b, sizeof(b), 0) != sizeof(b)) {
		srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
		for (i = 0; i < sizeof(b); i++)
			b[i] = (uint8_t)rand();
	}
	guid->Data1 = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
	guid->Data2 = (uint16_t)(b[4] | (b[5] << 8));
	guid->Data3 = (uint16_t)(((b[6] | (b[7] << 8)) & 0x0fff) | 0x4000);
	memcpy(guid->Data4, &b[8], 8);
	guid->Data4[0] = (guid->Data4[0] & 0x3f) | 0x80;
}

static void SetPartition(partition_t* p, const char* name)
{
	static_strcpy(p->name, name);
}

BOOL PlanPartitions(partition_layout_t* layout, int partition_style, int file_system,
	BOOL mbr_uefi_marker, uint8_t extra_partitions, uint64_t disk_size, uint32_t sector_size)
{
	const char* PartitionTypeName[] = { "MBR", "GPT", "SFD" };
	partition_t* part = layout->part;
	const uint64_t bytes_per_track = (uint64_t)DEFAULT_SECTORS_PER_TRACK * sector_size;
	uint64_t last_offset, uefi_ntfs_size = 0;
	// Go for a 260 MB sized ESP by default to keep everyone happy, including 4K sector users:
	// https://docs.microsoft.com/en-us/windows-hardware/manufacture/desktop/configure-uefigpt-based-hard-drive-partitions
	// and folks using MacOS: https://github.com/pbatard/rufus/issues/979
	int64_t esp_size = 260 * MB, main_size;
	uint64_t ClusterSize = (uint64_t)opts.cluster_size;
	int pi = 0, mi, i;

	memset(layout, 0, sizeof(*layout));
	layout->style = partition_style;
	layout->disk_size = disk_size;
	layout->sector_size = sector_size;
	layout->sectors_per_track = DEFAULT_SECTORS_PER_TRACK;
	PrintInfoDebug(0, MSG_238, PartitionTypeName[partition_style]);

	if (ClusterSize == 0)
		ClusterSize = 0x200;

	if (partition_style == PARTITION_STYLE_SFD)
		// Nothing to do
		return TRUE;
	if ((sector_size == 0) || (disk_size < 4 * MB)) {
		uprintf("Error: Invalid drive geometry");
		return FALSE;
	}

	if (extra_partitions & XP_UEFI_NTFS) {
		uefi_ntfs_size = GetResourceSize(IDR_UEFI_NTFS, "uefi-ntfs.img");
		if (uefi_ntfs_size == 0) {
			uprintf("Could not access embedded 'uefi-ntfs.img'");
			return FALSE;
		}
	}

	// Compute the starting offset of the first partition
	if ((partition_style == PARTITION_STYLE_GPT) || (!opts.old_bios_fixes)) {
		// Go with the MS 1 MB wastage at the beginning...
		part[pi].offset = 1 * MB;
	} else {
		// Some folks appear to think that 'Fixes for old BIOSes' is some kind of magic
		// wand and are adamant to try to apply them when creating *MODERN* VHD drives.
		// This, however, wrecks havok on MS' internal format calls because, as opposed
		// to what is the case for regular drives, VHDs require each cluster block to
		// be aligned to the cluster size, and that may not be the case with the stupid
		// CHS sizes that IBM imparted upon us. Long story short, we now align to a
		// cylinder size that is itself aligned to the cluster size.
		// If this actually breaks old systems, please send your complaints to IBM.
		part[pi].offset = CEILING_ALIGN(bytes_per_track, ClusterSize);
		// GRUB2 no longer fits in the usual 31½ KB that the above computation provides
		// so just unconditionally double that size and get on with it.
		part[pi].offset *= 2;
	}

	// Having the ESP up front may help (and is the Microsoft recommended way) but this
	// is only achievable if we can mount more than one partition at once
	// NB: Upstream also requires a fixed drive or Windows 10 1703 or later for that
	if (extra_partitions & XP_ESP) {
		assert(partition_style == PARTITION_STYLE_GPT);
		layout->index[PI_ESP] = pi;
		SetPartition(&part[pi], "EFI System Partition");
		part[pi].size = esp_size;
		part[pi + 1].offset = part[pi].offset + part[pi].size;
		// Align next partition to track and cluster
		part[pi + 1].offset = CEILING_ALIGN(part[pi + 1].offset, bytes_per_track);
		if (ClusterSize % sector_size == 0)
			part[pi + 1].offset = FLOOR_ALIGN(part[pi + 1].offset, ClusterSize);
		assert(part[pi + 1].offset >= part[pi].offset + part[pi].size);
		pi++;
		// Clear the extra partition we processed
		extra_partitions &= ~(XP_ESP);
	}

	// If required, set the MSR partition (GPT only - must be created before the data part)
	if (extra_partitions & XP_MSR) {
		assert(partition_style == PARTITION_STYLE_GPT);
		SetPartition(&part[pi], "Microsoft Reserved Partition");
		part[pi].size = 128 * MB;
		part[pi + 1].offset = part[pi].offset + part[pi].size;
		part[pi + 1].offset = CEILING_ALIGN(part[pi + 1].offset, bytes_per_track);
		if (ClusterSize % sector_size == 0)
			part[pi + 1].offset = FLOOR_ALIGN(part[pi + 1].offset, ClusterSize);
		assert(part[pi + 1].offset >= part[pi].offset + part[pi].size);
		pi++;
		extra_partitions &= ~(XP_MSR);
	}

	// Reserve an entry for the main partition
	layout->index[PI_MAIN] = pi++;
	// Shorthand for the main index.
	mi = layout->index[PI_MAIN];
	SetPartition(&part[mi], opts.write_as_esp ? "EFI System Partition" : "Main Data Partition");

	if (extra_partitions) {
		// Adjust the size according to extra partitions (which we always align to a track)
		if (extra_partitions & XP_PERSISTENCE) {
			assert(opts.persistence_size != 0);
			layout->index[PI_CASPER] = pi;
			SetPartition(&part[pi], "Linux Persistence");
			part[pi++].size = CEILING_ALIGN(opts.persistence_size, bytes_per_track);
		}
		if (extra_partitions & XP_ESP) {
			layout->index[PI_ESP] = pi;
			SetPartition(&part[pi], "EFI System Partition");
			part[pi++].size = CEILING_ALIGN(esp_size, bytes_per_track);
		} else if (extra_partitions & XP_UEFI_NTFS) {
			layout->index[PI_UEFI_NTFS] = pi;
			SetPartition(&part[pi], "UEFI:NTFS");
			part[pi++].size = CEILING_ALIGN(uefi_ntfs_size, bytes_per_track);
		} else if (extra_partitions & XP_COMPAT) {
			SetPartition(&part[pi], "BIOS Compatibility");
			part[pi++].size = bytes_per_track;	// One track for the extra partition
		}
	}
	assert(pi <= MAX_PARTITIONS);
	if (pi > MAX_PARTITIONS)
		return FALSE;

	// Compute the offsets of the extra partitions (which we always align to a track)
	last_offset = disk_size;
	if (partition_style == PARTITION_STYLE_GPT)
		last_offset -= 33ULL * sector_size;
	for (i = pi - 1; i > mi; i--) {
		if (part[i].size >= last_offset) {
			uprintf("Error: The drive is too small for the %s", part[i].name);
			return FALSE;
		}
		part[i].offset = FLOOR_ALIGN(last_offset - part[i].size, bytes_per_track);
		last_offset = part[i].offset;
	}

	// With the above, compute the main partition size (which we align to a track)
	main_size = (int64_t)last_offset - (int64_t)part[mi].offset;
	if (opts.write_as_esp)
		main_size = min(main_size, MAX_ISO_TO_ESP_SIZE);
	part[mi].size = (main_size <= 0) ? 0 : FLOOR_ALIGN((uint64_t)main_size, bytes_per_track);
	// Try to make sure that the main partition size is a multiple of the cluster size
	// This can be especially important when trying to capture an NTFS partition as FFU, as, when
	// the NTFS partition is aligned to cluster size, the FFU capture parses the NTFS allocated
	// map to only record clusters that are in use, whereas, if not aligned, the FFU capture uses
	// a full sector by sector scan of the NTFS partition and records any non-zero garbage, which
	// may include garbage leftover data from a previous reformat...
	if (ClusterSize % sector_size == 0)
		part[mi].size = FLOOR_ALIGN(part[mi].size, ClusterSize);
	if (part[mi].size == 0) {
		uprintf("Error: Invalid %s size", part[mi].name);
		return FALSE;
	}
	layout->count = pi;

	// The partition types
	for (i = 0; i < pi; i++) {
		if (partition_style == PARTITION_STYLE_MBR) {
			if (i == mi) {
				part[i].bootable = (opts.boot_type != BT_NON_BOOTABLE);
				switch (file_system) {
				case FS_FAT16:
					part[i].mbr_type = 0x0e;	// FAT16 LBA
					break;
				case FS_NTFS:
				case FS_EXFAT:
				case FS_UDF:
				case FS_REFS:
					part[i].mbr_type = 0x07;
					break;
				case FS_EXT2:
				case FS_EXT3:
				case FS_EXT4:
					part[i].mbr_type = 0x83;
					break;
				case FS_FAT32:
					part[i].mbr_type = 0x0c;	// FAT32 LBA
					break;
				default:
					uprintf("Unsupported file system");
					return FALSE;
				}
			}
			// May override the the type of main partition if write_as_esp is active
			if ((strcmp(part[i].name, "EFI System Partition") == 0) || (strcmp(part[i].name, "UEFI:NTFS") == 0))
				// UEFI:NTFS as an MBR ESP somehow seems to be okay, but we may want
				// to switch to type 0x01 (FAT21) if people report issues.
				part[i].mbr_type = 0xef;
			else if (strcmp(part[i].name, "Linux Persistence") == 0)
				part[i].mbr_type = 0x83;
			else if (strcmp(part[i].name, "BIOS Compatibility") == 0)
				part[i].mbr_type = RUFUS_EXTRA_PARTITION_TYPE;
		} else {
			assert(partition_style == PARTITION_STYLE_GPT);
			if (strcmp(part[i].name, "UEFI:NTFS") == 0) {
				// Boy do you *NOT* want the ESP of a GPT bootable drive to be declared as ESP,
				// lest the Microsoft Windows installer errors out at "Copying Windows Files"
				// because it just can't handle 2 ESPs on one system. The horror! The horror!
				part[i].gpt_type = PARTITION_MICROSOFT_DATA;
				// Prevent a drive letter from being assigned to the UEFI:NTFS partition
				part[i].gpt_attributes = GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
				// NB: We no longer make the partition read-only as we need to be able to edit
				// its label for WUE's silent install no-disk/extra-disk detection.
			} else if (strcmp(part[i].name, "EFI System Partition") == 0)
				part[i].gpt_type = PARTITION_GENERIC_ESP;
			else if (strcmp(part[i].name, "Linux Persistence") == 0)
				part[i].gpt_type = PARTITION_LINUX_DATA;
			else if (strcmp(part[i].name, "Microsoft Reserved Partition") == 0)
				part[i].gpt_type = PARTITION_MICROSOFT_RESERVED;
			else
				part[i].gpt_type = PARTITION_MICROSOFT_DATA;
			NewGuid(&part[i].gpt_id);
		}
	}

	if (partition_style == PARTITION_STYLE_MBR) {
		// If MBR+UEFI is selected, write an UEFI marker in lieu of the regular MBR signature.
		// This helps us reselect the partition scheme option that was used when creating the
		// drive in Rufus. As far as I can tell, Windows doesn't care much if this signature
		// isn't unique for USB drives.
		// NB: Upstream otherwise uses the tick count
		if (mbr_uefi_marker) {
			layout->mbr_signature = MBR_UEFI_MARKER;
		} else {
			GUID r;
			NewGuid(&r);
			layout->mbr_signature = r.Data1;
		}
	} else {
		NewGuid(&layout->disk_id);
	}
	return TRUE;
}

/* The little endian fields of the MBR and GPT */
static void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { put16(p, v & 0xffff); put16(&p[2], v >> 16); }
static void put64(uint8_t* p, uint64_t v) { put32(p, v & 0xffffffff); put32(&p[4], v >> 32); }

static void PutGuid(uint8_t* p, const GUID* guid)
{
	put32(p, guid->Data1);
	put16(&p[4], guid->Data2);
	put16(&p[6], guid->Data3);
	memcpy(&p[8], guid->Data4, 8);
}

// The CHS address of an LBA, for a 255 × 63 geometry, as the partition entries have them
static void PutChs(uint8_t* p, uint64_t lba)
{
	uint64_t c = lba / (DEFAULT_HEADS * DEFAULT_SECTORS_PER_TRACK);
	uint32_t h = (uint32_t)((lba / DEFAULT_SECTORS_PER_TRACK) % DEFAULT_HEADS);
	uint32_t s = (uint32_t)(lba % DEFAULT_SECTORS_PER_TRACK) + 1;

	// Beyond the reach of CHS, the maximum (1023, 254, 63) is used
	if (c > 1023) {
		c = 1023;
		h = 254;
		s = 63;
	}
	p[0] = (uint8_t)h;
	p[1] = (uint8_t)(s | ((c >> 2) & 0xc0));
	p[2] = (uint8_t)(c & 0xff);
}

static void PutMbrEntry(uint8_t* e, uint8_t status, uint8_t type, uint64_t lba, uint64_t sectors)
{
	e[0] = status;
	PutChs(&e[1], lba);
	e[4] = type;
	PutChs(&e[5], lba + sectors - 1);
	put32(&e[8], (uint32_t)min(lba, 0xffffffffULL));
	put32(&e[12], (uint32_t)min(sectors, 0xffffffffULL));
}

uint32_t GptCrc32(const uint8_t* buf, size_t len)
{
	static uint32_t table[256];
	static BOOL init = FALSE;
	uint32_t crc = 0xffffffff, c;
	size_t i;
	int k;

	if (!init) {
		for (i = 0; i < 256; i++) {
			c = (uint32_t)i;
			for (k = 0; k < 8; k++)
				c = (c & 1) ? (0xedb88320 ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		init = TRUE;
	}
	for (i = 0; i < len; i++)
		crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
	return crc ^ 0xffffffff;
}

static BOOL WriteAt(int fd, const void* buf, size_t len, uint64_t offset)
{
	const uint8_t* p = buf;
	ssize_t w;

	while (len > 0) {
		w = pwrite(fd, p, len, (off_t)offset);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			uprintf("Write error at offset %llu: %s", (unsigned long long)offset, WindowsErrorString());
			LastWriteError = RUFUS_ERROR(WindowsErrorFromErrno(errno));
			return FALSE;
		}
		p += w;
		len -= (size_t)w;
		offset += (uint64_t)w;
	}
	return TRUE;
}

// Zero a region of a drive
static BOOL ClearRegion(int fd, uint64_t offset, uint64_t size)
{
	uint8_t* buf = calloc(1, 1 * MB);
	uint64_t len;
	BOOL r = TRUE;

	if (buf == NULL)
		return FALSE;
	while (r && (size > 0)) {
		len = min(size, 1 * MB);
		r = WriteAt(fd, buf, (size_t)len, offset);
		offset += len;
		size -= len;
	}
	free(buf);
	return r;
}

BOOL ClearMBRGPT(int fd, uint64_t disk_size, uint32_t sector_size)
{
	uint64_t top = (uint64_t)sector_size * MAX_SECTORS_TO_CLEAR, end = (uint64_t)sector_size * (MAX_SECTORS_TO_CLEAR / 8);

	// We now unconditionally zero MAX_SECTORS_TO_CLEAR and (MAX_SECTORS_TO_CLEAR / 8)
	// at the beginning and end of the drive respectively. This should encompass the
	// MBR, GPT, backup GPT and VBR for most devices and (hopefully) get Windows and
	// other apps off our back with trying to access zombie partitions...
	PrintInfoDebug(0, MSG_224);
	if (!ClearRegion(fd, 0, min(top, disk_size)))
		return FALSE;
	uprintf("Zeroed %s at the top of the drive", SizeToHumanReadable(min(top, disk_size), FALSE, FALSE));
	if (disk_size > end) {
		if (ClearRegion(fd, disk_size - end, end))
			uprintf("Zeroed %s at the end of the drive", SizeToHumanReadable(end, FALSE, FALSE));
		else
			uprintf("WARNING: Could not to clear the backup GPT area: %s", WindowsErrorString());
	}
	return TRUE;
}

// A GPT header, for the copy that is at my_lba
static void BuildGptHeader(uint8_t* h, const partition_layout_t* layout, uint64_t my_lba, uint64_t alt_lba,
	uint64_t entries_lba, uint32_t entries_crc)
{
	const uint64_t last_lba = layout->disk_size / layout->sector_size - 1;
	const uint64_t entries_sectors = (GPT_ENTRIES * GPT_ENTRY_SIZE + layout->sector_size - 1) / layout->sector_size;

	memset(h, 0, layout->sector_size);
	memcpy(h, "EFI PART", 8);
	put32(&h[8], GPT_REVISION);
	put32(&h[12], GPT_HEADER_SIZE);
	put64(&h[24], my_lba);
	put64(&h[32], alt_lba);
	put64(&h[40], 2 + entries_sectors);					// First usable LBA
	put64(&h[48], last_lba - entries_sectors - 1);		// Last usable LBA
	PutGuid(&h[56], &layout->disk_id);
	put64(&h[72], entries_lba);
	put32(&h[80], GPT_ENTRIES);
	put32(&h[84], GPT_ENTRY_SIZE);
	put32(&h[88], entries_crc);
	put32(&h[16], GptCrc32(h, GPT_HEADER_SIZE));
}

BOOL WritePartitionTable(int fd, const partition_layout_t* layout)
{
	const uint32_t ss = layout->sector_size;
	const uint64_t last_lba = layout->disk_size / ss - 1;
	const uint64_t entries_sectors = (GPT_ENTRIES * GPT_ENTRY_SIZE + ss - 1) / ss;
	const uint64_t size_to_clear = (uint64_t)MAX_SECTORS_TO_CLEAR * ss;
	uint8_t *mbr = NULL, *header = NULL, *entries = NULL, *e, *buffer;
	uint32_t entries_crc, bufsize;
	const partition_t* p;
	BOOL r = FALSE;
	int i, j;

	if (layout->style == PARTITION_STYLE_SFD)
		return TRUE;
	mbr = calloc(1, ss);
	header = calloc(1, ss);
	entries = calloc(entries_sectors, ss);
	if ((mbr == NULL) || (header == NULL) || (entries == NULL))
		goto out;

	for (i = 0; i < layout->count; i++) {
		p = &layout->part[i];
		uprintf("● Creating %s%s (offset: %llu, size: %s)", p->name, (strstr(p->name, "Partition") == NULL) ? " Partition" : "",
			(unsigned long long)p->offset, SizeToHumanReadable(p->size, TRUE, FALSE));
		// Zero the first sectors of the partition to avoid file system caching issues
		if (!ClearRegion(fd, p->offset, min(size_to_clear, p->size)))
			uprintf("Could not zero %s: %s", p->name, WindowsErrorString());
	}

	// We need to write the UEFI:NTFS partition before we refresh the disk
	// NB: The UEFI:NTFS partition, if any, always comes after the main one, so its index can't be 0
	if (layout->index[PI_UEFI_NTFS] != 0) {
		uprintf("Writing UEFI:NTFS data...");
		buffer = GetResource(IDR_UEFI_NTFS, "uefi-ntfs.img", &bufsize, FALSE);
		if (buffer == NULL) {
			uprintf("  Could not access source image");
			goto out;
		}
		if (!WriteAt(fd, buffer, bufsize, layout->part[layout->index[PI_UEFI_NTFS]].offset))
			goto out;
	}

	if (layout->style == PARTITION_STYLE_MBR) {
		put32(&mbr[440], layout->mbr_signature);
		for (i = 0; (i < layout->count) && (i < 4); i++) {
			p = &layout->part[i];
			PutMbrEntry(&mbr[446 + 16 * i], p->bootable ? 0x80 : 0x00, p->mbr_type, p->offset / ss, p->size / ss);
		}
		mbr[510] = 0x55;
		mbr[511] = 0xaa;
		r = WriteAt(fd, mbr, ss, 0);
		goto out;
	}

	// The protective MBR, which covers the whole disk (or as much of it as it can)
	PutMbrEntry(&mbr[446], 0x00, 0xee, 1, min(last_lba, 0xffffffffULL));
	mbr[510] = 0x55;
	mbr[511] = 0xaa;

	for (i = 0; i < layout->count; i++) {
		p = &layout->part[i];
		e = &entries[i * GPT_ENTRY_SIZE];
		PutGuid(e, &p->gpt_type);
		PutGuid(&e[16], &p->gpt_id);
		put64(&e[32], p->offset / ss);
		put64(&e[40], (p->offset + p->size) / ss - 1);
		put64(&e[48], p->gpt_attributes);
		// The name is in UTF-16LE (and the names we use are ASCII)
		for (j = 0; (p->name[j] != 0) && (j < 36); j++)
			put16(&e[56 + 2 * j], (uint8_t)p->name[j]);
	}
	entries_crc = GptCrc32(entries, GPT_ENTRIES * GPT_ENTRY_SIZE);

	// The backup goes first, so that the primary is only valid once everything is written
	BuildGptHeader(header, layout, last_lba, 1, last_lba - entries_sectors, entries_crc);
	if (!WriteAt(fd, entries, entries_sectors * ss, (last_lba - entries_sectors) * ss) ||
		!WriteAt(fd, header, ss, last_lba * ss))
		goto out;
	BuildGptHeader(header, layout, 1, last_lba, 2, entries_crc);
	r = WriteAt(fd, entries, entries_sectors * ss, 2ULL * ss) && WriteAt(fd, header, ss, 1ULL * ss) &&
		WriteAt(fd, mbr, ss, 0);

out:
	if (r && (fsync(fd) != 0) && (errno != EINVAL))
		r = FALSE;
	free(mbr);
	free(header);
	free(entries);
	return r;
}

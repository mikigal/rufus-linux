/*
 * Rufus: The Reliable USB Formatting Utility
 * Large FAT32 formatting
 * Copyright © 2007-2009 Tom Thornhill/Ridgecrop
 * Copyright © 2011-2025 Pete Batard <pete@akeo.ie>
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
 * On Linux, the volume is a file descriptor on the partition (or an image file),
 * and the label gets written here, as SetVolumeLabel() does on Windows. The PBR
 * is written later, by the format pipeline, once the volume is unmounted.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "rufus.h"
#include "options.h"
#include "filesystems.h"
#include "partition.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"

#define die(msg, err) do { uprintf(msg); ErrorStatus = RUFUS_ERROR(err); goto out; } while(0)

/* Large FAT32 */
#pragma pack(push, 1)
typedef struct tagFAT_BOOTSECTOR32
{
	// Common fields.
	BYTE sJmpBoot[3];
	BYTE sOEMName[8];
	WORD wBytsPerSec;
	BYTE bSecPerClus;
	WORD wRsvdSecCnt;
	BYTE bNumFATs;
	WORD wRootEntCnt;
	WORD wTotSec16;           // if zero, use dTotSec32 instead
	BYTE bMedia;
	WORD wFATSz16;
	WORD wSecPerTrk;
	WORD wNumHeads;
	DWORD dHiddSec;
	DWORD dTotSec32;
	// Fat 32/16 only
	DWORD dFATSz32;
	WORD wExtFlags;
	WORD wFSVer;
	DWORD dRootClus;
	WORD wFSInfo;
	WORD wBkBootSec;
	BYTE Reserved[12];
	BYTE bDrvNum;
	BYTE Reserved1;
	BYTE bBootSig;           // == 0x29 if next three fields are ok
	DWORD dBS_VolID;
	BYTE sVolLab[11];
	BYTE sBS_FilSysType[8];
} FAT_BOOTSECTOR32;

typedef struct {
	DWORD dLeadSig;         // 0x41615252
	BYTE sReserved1[480];   // zeros
	DWORD dStrucSig;        // 0x61417272
	DWORD dFree_Count;      // 0xFFFFFFFF
	DWORD dNxt_Free;        // 0xFFFFFFFF
	BYTE sReserved2[12];    // zeros
	DWORD dTrailSig;        // 0xAA550000
} FAT_FSINFO;
#pragma pack(pop)

/*
 * 28.2  CALCULATING THE VOLUME SERIAL NUMBER
 *
 * For example, say a disk was formatted on 26 Dec 95 at 9:55 PM and 41.94
 * seconds.  DOS takes the date and time just before it writes it to the
 * disk.
 *
 * Low order word is calculated:               Volume Serial Number is:
 * Month & Day         12/26   0c1ah
 * Sec & Hundredths    41:94   295eh               3578:1d02
 * -----
 * 3578h
 *
 * High order word is calculated:
 * Hours & Minutes     21:55   1537h
 * Year                1995    07cbh
 * -----
 * 1d02h
 */
static DWORD GetVolumeID(void)
{
	struct timespec ts;
	struct tm s;
	DWORD d;
	WORD lo, hi, tmp;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &s);

	lo = s.tm_mday + ((s.tm_mon + 1) << 8);
	tmp = (WORD)((ts.tv_nsec / 10000000) + (s.tm_sec << 8));
	lo += tmp;

	hi = s.tm_min + (s.tm_hour << 8);
	hi += (WORD)(s.tm_year + 1900);

	d = lo + (hi << 16);
	return d;
}

static int64_t write_sectors(int fd, uint64_t SectorSize, uint64_t StartSector, uint64_t nSectors, const void* pBuf)
{
	ssize_t r;

	do {
		r = pwrite(fd, pBuf, (size_t)(nSectors * SectorSize), (off_t)(StartSector * SectorSize));
	} while ((r < 0) && (errno == EINTR));
	if (r < 0)
		uprintf("write_sectors: Write error: %s", WindowsErrorString());
	return (int64_t)r;
}

// What SetVolumeLabel() does on FAT: the label goes to the root directory (and the boot sector)
static BOOL SetFatLabel(int fd, DWORD BytesPerSect, DWORD RootSector, FAT_BOOTSECTOR32* pBootSect, const char* Label)
{
	uint8_t* entry;
	char name[11];
	struct tm t;
	time_t now = time(NULL);
	size_t i, len = safe_strlen(Label);
	uint16_t dos_time, dos_date;
	BOOL r;

	if (len == 0)
		return TRUE;
	memset(name, ' ', sizeof(name));
	memcpy(name, Label, min(len, sizeof(name)));
	localtime_r(&now, &t);
	dos_time = (uint16_t)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
	dos_date = (uint16_t)(((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);
	entry = calloc(1, BytesPerSect);
	if (entry == NULL)
		return FALSE;
	memcpy(entry, name, sizeof(name));
	entry[11] = 0x08;	// ATTR_VOLUME_ID
	entry[22] = dos_time & 0xff;
	entry[23] = dos_time >> 8;
	entry[24] = dos_date & 0xff;
	entry[25] = dos_date >> 8;
	r = (write_sectors(fd, BytesPerSect, RootSector, 1, entry) == BytesPerSect);
	free(entry);
	memcpy(pBootSect->sVolLab, name, sizeof(name));
	for (i = 0; r && (i < 2); i++)
		r = (write_sectors(fd, BytesPerSect, (i == 0) ? 0 : pBootSect->wBkBootSec, 1, pBootSect) == BytesPerSect);
	return r;
}

/*
 * Proper computation of FAT size
 * See: http://www.syslinux.org/archives/2016-February/024850.html
 * and subsequent replies.
 */
static DWORD GetFATSizeSectors(DWORD DskSize, DWORD ReservedSecCnt, DWORD SecPerClus, DWORD NumFATs, DWORD BytesPerSect)
{
	ULONGLONG Numerator, Denominator;
	ULONGLONG FatElementSize = 4;
	ULONGLONG ReservedClusCnt = 2;
	ULONGLONG FatSz;

	Numerator = DskSize - ReservedSecCnt + ReservedClusCnt * SecPerClus;
	Denominator = (ULONGLONG)SecPerClus * BytesPerSect / FatElementSize + NumFATs;
	FatSz = Numerator / Denominator + 1;	// +1 to ensure we are rounded up

	return (DWORD)FatSz;
}

/*
 * Large FAT32 volume formatting from fat32format by Tom Thornhill
 * http://www.ridgecrop.demon.co.uk/index.htm?fat32format.htm
 */
BOOL FormatLargeFAT32(int fd, uint64_t PartitionOffset, uint64_t PartitionLength, DWORD SectorSize, DWORD ClusterSize,
	LPCSTR FSName, LPCSTR Label, DWORD Flags)
{
	BOOL r = FALSE;
	DWORD i;
	// NB: Upstream gets the geometry and partition information from the volume
	const DWORD SectorsPerTrack = DEFAULT_SECTORS_PER_TRACK, TracksPerCylinder = DEFAULT_HEADS;
	const DWORD BytesPerSector = max(SectorSize, 512);
	const DWORD HiddenSectors = (DWORD)(PartitionOffset / BytesPerSector);
	// Recommended values
	DWORD ReservedSectCount = 32;
	DWORD NumFATs = 2;
	DWORD BackupBootSect = 6;
	DWORD VolumeId = 0; // calculated before format
	DWORD BurstSize = 128; // Zero in blocks of 64K typically

	// Calculated later
	DWORD FatSize = 0;
	DWORD BytesPerSect = 0;
	DWORD SectorsPerCluster = 0;
	DWORD TotalSectors = 0;
	DWORD AlignSectors = 0;
	DWORD SystemAreaSize = 0;
	DWORD UserAreaSize = 0;
	ULONGLONG qTotalSectors = 0;

	// Structures to be written to the disk
	FAT_BOOTSECTOR32* pFAT32BootSect = NULL;
	FAT_FSINFO* pFAT32FsInfo = NULL;
	DWORD* pFirstSectOfFat = NULL;
	BYTE* pZeroSect = NULL;
	char VolId[12] = "NO NAME    ";

	// Debug temp vars
	ULONGLONG FatNeeded, ClusterCount;

	if (safe_strncmp(FSName, "FAT", 3) != 0) {
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
		goto out;
	}
	if (!(Flags & FP_NO_PROGRESS)) {
		PrintInfoDebug(0, MSG_222, "Large FAT32");
		UpdateProgressWithInfoInit(NULL, TRUE);
	}
	VolumeId = GetVolumeID();

	if (fd < 0)
		die("Invalid logical volume handle", ERROR_INVALID_HANDLE);

	BytesPerSect = BytesPerSector;

	// Checks on Disk Size
	qTotalSectors = PartitionLength / BytesPerSector;
	// Low end limit - 65536 sectors
	if (qTotalSectors < 65536) {
		// Most FAT32 implementations would probably mount this volume just fine,
		// but the spec says that we shouldn't do this, so we won't
		die("This drive is too small for FAT32 - there must be at least 64K clusters", APPERR(ERROR_INVALID_CLUSTER_SIZE));
	}

	if (qTotalSectors >= 0xffffffff) {
		// This is a more fundamental limitation on FAT32 - the total sector count in the root dir
		// is 32bit. With a bit of creativity, FAT32 could be extended to handle at least 2^28 clusters
		// There would need to be an extra field in the FSInfo sector, and the old sector count could
		// be set to 0xffffffff. This is non standard though, the Windows FAT driver FASTFAT.SYS won't
		// understand this. Perhaps a future version of FAT32 and FASTFAT will handle this.
		die("This drive is too big for FAT32 - max 2TB supported", APPERR(ERROR_INVALID_VOLUME_SIZE));
	}

	// Set default cluster size
	// https://support.microsoft.com/en-us/help/140365/default-cluster-size-for-ntfs-fat-and-exfat
	if (ClusterSize == 0) {
		if (PartitionLength < 64 * MB)
			ClusterSize = 512;
		else if (PartitionLength < 128 * MB)
			ClusterSize = 1 * KB;
		else if (PartitionLength < 256 * MB)
			ClusterSize = 2 * KB;
		else if (PartitionLength < 8 * GB)
			ClusterSize = 4 * KB;
		else if (PartitionLength < 16 * GB)
			ClusterSize = 8 * KB;
		else if (PartitionLength < 32 * GB)
			ClusterSize = 16 * KB;
		else if (PartitionLength < 2 * TB)
			ClusterSize = 32 * KB;
		else
			ClusterSize = 64 * KB;
	}

	// coverity[tainted_data]
	pFAT32BootSect = (FAT_BOOTSECTOR32*)calloc(BytesPerSect, 1);
	pFAT32FsInfo = (FAT_FSINFO*)calloc(BytesPerSect, 1);
	pFirstSectOfFat = (DWORD*)calloc(BytesPerSect, 1);
	if (!pFAT32BootSect || !pFAT32FsInfo || !pFirstSectOfFat) {
		die("Failed to allocate memory", ERROR_NOT_ENOUGH_MEMORY);
	}

	// fill out the boot sector and fs info
	pFAT32BootSect->sJmpBoot[0] = 0xEB;
	pFAT32BootSect->sJmpBoot[1] = 0x58; // jmp.s $+0x5a is 0xeb 0x58, not 0xeb 0x5a. Thanks Marco!
	pFAT32BootSect->sJmpBoot[2] = 0x90;
	memcpy(pFAT32BootSect->sOEMName, "MSWIN4.1", 8);
	pFAT32BootSect->wBytsPerSec = (WORD)BytesPerSect;
	SectorsPerCluster = ClusterSize / BytesPerSect;

	pFAT32BootSect->bSecPerClus = (BYTE)SectorsPerCluster;
	pFAT32BootSect->bNumFATs = (BYTE)NumFATs;
	pFAT32BootSect->wRootEntCnt = 0;
	pFAT32BootSect->wTotSec16 = 0;
	pFAT32BootSect->bMedia = 0xF8;
	pFAT32BootSect->wFATSz16 = 0;
	pFAT32BootSect->wSecPerTrk = (WORD)SectorsPerTrack;
	pFAT32BootSect->wNumHeads = (WORD)TracksPerCylinder;
	pFAT32BootSect->dHiddSec = HiddenSectors;
	TotalSectors = (DWORD)(PartitionLength / BytesPerSector);
	pFAT32BootSect->dTotSec32 = TotalSectors;

	FatSize = GetFATSizeSectors(pFAT32BootSect->dTotSec32, pFAT32BootSect->wRsvdSecCnt,
		pFAT32BootSect->bSecPerClus, pFAT32BootSect->bNumFATs, BytesPerSect);

	// Update reserved sector count so that the start of data region is aligned to a MB boundary
	SystemAreaSize = ReservedSectCount + NumFATs * FatSize;
	AlignSectors = (1 * MB) / BytesPerSect;
	SystemAreaSize = (SystemAreaSize + AlignSectors - 1) / AlignSectors * AlignSectors;
	ReservedSectCount = SystemAreaSize - NumFATs * FatSize;

	pFAT32BootSect->wRsvdSecCnt = (WORD)ReservedSectCount;
	pFAT32BootSect->dFATSz32 = FatSize;
	pFAT32BootSect->wExtFlags = 0;
	pFAT32BootSect->wFSVer = 0;
	pFAT32BootSect->dRootClus = 2;
	pFAT32BootSect->wFSInfo = 1;
	pFAT32BootSect->wBkBootSec = (WORD)BackupBootSect;
	pFAT32BootSect->bDrvNum = 0x80;
	pFAT32BootSect->Reserved1 = 0;
	pFAT32BootSect->bBootSig = 0x29;

	pFAT32BootSect->dBS_VolID = VolumeId;
	memcpy(pFAT32BootSect->sVolLab, VolId, 11);
	memcpy(pFAT32BootSect->sBS_FilSysType, "FAT32   ", 8);
	((BYTE*)pFAT32BootSect)[510] = 0x55;
	((BYTE*)pFAT32BootSect)[511] = 0xaa;

	// FATGEN103.DOC says "NOTE: Many FAT documents mistakenly say that this 0xAA55 signature occupies the "last 2 bytes of
	// the boot sector". This statement is correct if - and only if - BPB_BytsPerSec is 512. If BPB_BytsPerSec is greater than
	// 512, the offsets of these signature bytes do not change (although it is perfectly OK for the last two bytes at the end
	// of the boot sector to also contain this signature)."
	//
	// Windows seems to only check the bytes at offsets 510 and 511. Other OSs might check the ones at the end of the sector,
	// so we'll put them there too.
	if (BytesPerSect != 512) {
		((BYTE*)pFAT32BootSect)[BytesPerSect - 2] = 0x55;
		((BYTE*)pFAT32BootSect)[BytesPerSect - 1] = 0xaa;
	}

	// FSInfo sect
	pFAT32FsInfo->dLeadSig = 0x41615252;
	pFAT32FsInfo->dStrucSig = 0x61417272;
	pFAT32FsInfo->dFree_Count = (DWORD)-1;
	pFAT32FsInfo->dNxt_Free = (DWORD)-1;
	pFAT32FsInfo->dTrailSig = 0xaa550000;

	// First FAT Sector
	pFirstSectOfFat[0] = 0x0ffffff8;  // Reserved cluster 1 media id in low byte
	pFirstSectOfFat[1] = 0x0fffffff;  // Reserved cluster 2 EOC
	pFirstSectOfFat[2] = 0x0fffffff;  // end of cluster chain for root dir

	// Write boot sector, fats
	// Sector 0 Boot Sector
	// Sector 1 FSInfo
	// Sector 2 More boot code - we write zeros here
	// Sector 3 unused
	// Sector 4 unused
	// Sector 5 unused
	// Sector 6 Backup boot sector
	// Sector 7 Backup FSInfo sector
	// Sector 8 Backup 'more boot code'
	// zeroed sectors up to ReservedSectCount
	// FAT1  ReservedSectCount to ReservedSectCount + FatSize
	// ...
	// FATn  ReservedSectCount to ReservedSectCount + FatSize
	// RootDir - allocated to cluster2

	UserAreaSize = TotalSectors - ReservedSectCount - (NumFATs * FatSize);
	assert(SectorsPerCluster > 0);
	ClusterCount = UserAreaSize / SectorsPerCluster;

	// Sanity check for a cluster count of >2^28, since the upper 4 bits of the cluster values in
	// the FAT are reserved.
	if (ClusterCount > 0x0FFFFFFF) {
		die("This drive has more than 2^28 clusters, try to specify a larger cluster size or use the default",
			ERROR_INVALID_CLUSTER_SIZE);
	}

	// Sanity check - < 64K clusters means that the volume will be misdetected as FAT16
	if (ClusterCount < 65536) {
		die("FAT32 must have at least 65536 clusters, try to specify a smaller cluster size or use the default",
			ERROR_INVALID_CLUSTER_SIZE);
	}

	// Sanity check, make sure the fat is big enough
	// Convert the cluster count into a Fat sector count, and check the fat size value we calculated
	// earlier is OK.
	FatNeeded = ClusterCount * 4;
	FatNeeded += (BytesPerSect - 1);
	FatNeeded /= BytesPerSect;
	if (FatNeeded > FatSize) {
		die("This drive is too big for large FAT32 format", APPERR(ERROR_INVALID_VOLUME_SIZE));
	}

	// Now we're committed - print some info first
	// NB: Upstream uses the format specifiers of 32-bit longs
	uprintf("Size : %s %u sectors", SizeToHumanReadable(PartitionLength, TRUE, FALSE), TotalSectors);
	uprintf("Cluster size %u bytes, %u bytes per sector", SectorsPerCluster * BytesPerSect, BytesPerSect);
	uprintf("Volume ID is %x:%x", VolumeId >> 16, VolumeId & 0xffff);
	uprintf("%u Reserved sectors, %u sectors per FAT, %u FATs", ReservedSectCount, FatSize, NumFATs);
	uprintf("%llu Total clusters", (unsigned long long)ClusterCount);

	// Fix up the FSInfo sector
	pFAT32FsInfo->dFree_Count = (UserAreaSize / SectorsPerCluster) - 1;
	pFAT32FsInfo->dNxt_Free = 3; // clusters 0-1 reserved, we used cluster 2 for the root dir

	uprintf("%u Free clusters", pFAT32FsInfo->dFree_Count);
	// Work out the Cluster count

	// First zero out ReservedSect + FatSize * NumFats + SectorsPerCluster
	SystemAreaSize = ReservedSectCount + (NumFATs * FatSize) + SectorsPerCluster;
	uprintf("Clearing out %d sectors for reserved sectors, FATs and root cluster...", SystemAreaSize);

	// Not the most effective, but easy on RAM
	pZeroSect = (BYTE*)calloc(BytesPerSect, BurstSize);
	if (!pZeroSect) {
		die("Failed to allocate memory", ERROR_NOT_ENOUGH_MEMORY);
	}

	for (i = 0; i < (SystemAreaSize + BurstSize - 1); i += BurstSize) {
		if (!(Flags & FP_NO_PROGRESS))
			UpdateProgressWithInfo(OP_FORMAT, MSG_217, (uint64_t)i, (uint64_t)SystemAreaSize + BurstSize);
		CHECK_FOR_USER_CANCEL;
		if (write_sectors(fd, BytesPerSect, i, BurstSize, pZeroSect) != (BytesPerSect * BurstSize)) {
			die("Error clearing reserved sectors", ERROR_WRITE_FAULT);
		}
	}

	uprintf ("Initializing reserved sectors and FATs...");
	// Now we should write the boot sector and fsinfo twice, once at 0 and once at the backup boot sect position
	for (i = 0; i < 2; i++) {
		int SectorStart = (i == 0) ? 0 : BackupBootSect;
		write_sectors(fd, BytesPerSect, SectorStart, 1, pFAT32BootSect);
		write_sectors(fd, BytesPerSect, SectorStart + 1, 1, pFAT32FsInfo);
	}

	// Write the first fat sector in the right places
	for (i = 0; i < NumFATs; i++) {
		int SectorStart = ReservedSectCount + (i * FatSize);
		uprintf("FAT #%d sector at address: %d", i, SectorStart);
		write_sectors(fd, BytesPerSect, SectorStart, 1, pFirstSectOfFat);
	}

	// NB: Upstream writes the PBR here (unless FP_NO_BOOT), which the format pipeline does later

	// Set the FAT32 volume label
	if (!(Flags & FP_NO_PROGRESS)) {
		PrintInfo(0, MSG_221, lmprintf(MSG_307));
		uprintf("Setting label...");
	}
	if (!SetFatLabel(fd, BytesPerSect, ReservedSectCount + NumFATs * FatSize, pFAT32BootSect, Label)) {
		uprintf("Could not set label: %s", WindowsErrorString());
		// Non fatal error
	}
	if (fsync(fd) != 0)
		die("Could not flush the volume", ERROR_WRITE_FAULT);

	uprintf("Format completed.");
	r = TRUE;

out:
	safe_free(pFAT32BootSect);
	safe_free(pFAT32FsInfo);
	safe_free(pFirstSectOfFat);
	safe_free(pZeroSect);
	return r;
}

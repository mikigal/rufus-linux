/*
 * Rufus: The Reliable USB Formatting Utility
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
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
#pragma once

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"

/* Convenient to have around */
#define KB                          1024LL
#define MB                          1048576LL
#define GB                          1073741824LL
#define TB                          1099511627776LL
#define PB                          1125899906842624LL

/*
 * Features not ready for prime time and that may *DESTROY* your data - USE AT YOUR OWN RISKS!
 */
//#define RUFUS_TEST

#define APPLICATION_NAME            "Rufus"
#if defined(__x86_64__)
#define APPLICATION_ARCH            "x64"
#elif defined(__i386__)
#define APPLICATION_ARCH            "x86"
#elif defined(__aarch64__)
#define APPLICATION_ARCH            "Arm64"
#elif defined(__arm__)
#define APPLICATION_ARCH            "Arm"
#elif defined(__riscv) && (__riscv_xlen == 64)
#define APPLICATION_ARCH            "RISC-V 64"
#elif defined(__loongarch64)
#define APPLICATION_ARCH            "LoongArch 64"
#else
#define APPLICATION_ARCH            "(Unknown Arch)"
#endif
#define COMPANY_NAME                "Akeo Consulting"
#define STR_NO_LABEL                "NO_LABEL"
#define LEFT_TO_RIGHT_MARK          "\u200e"
#define RIGHT_TO_LEFT_MARK          "\u200f"
#define LEFT_TO_RIGHT_EMBEDDING     "\u202a"
#define RIGHT_TO_LEFT_EMBEDDING     "\u202b"
#define POP_DIRECTIONAL_FORMATTING  "\u202c"
#define LEFT_TO_RIGHT_OVERRIDE      "\u202d"
#define RIGHT_TO_LEFT_OVERRIDE      "\u202e"
#define DRIVE_ACCESS_TIMEOUT        15000		// How long we should retry drive access (in ms)
#define DRIVE_ACCESS_RETRIES        150			// How many times we should retry
#define DRIVE_INDEX_MIN             0x00000080
#define DRIVE_INDEX_MAX             0x000000C0
#define MIN_DRIVE_SIZE              (8 * MB)	// Minimum size a drive must have, to be formattable
#define MIN_EXTRA_PART_SIZE         (1 * MB)	// Minimum size of the extra partition, in bytes
#define MIN_EXT_SIZE                (256 * MB)	// Minimum size we allow for ext formatting
#define MAX_DRIVES                  (DRIVE_INDEX_MAX - DRIVE_INDEX_MIN)
#define MAX_TOOLTIPS                128
#define MAX_SIZE_SUFFIXES           6			// bytes, KB, MB, GB, TB, PB
#define MAX_CLUSTER_SIZES           18
#define MAX_PROGRESS                0xFFFF
#define PATCH_PROGRESS_TOTAL        207
#define MAX_LOG_SIZE                0x7FFFFFFE
#define MAX_REFRESH                 25			// How long we should wait to refresh UI elements (in ms)
#define MARQUEE_TIMER_REFRESH       10			// Time between progress bar marquee refreshes, in ms
#define MAX_GUID_STRING_LENGTH      40
#define MAX_PARTITIONS              16			// Maximum number of partitions we handle
#define MAX_ESP_TOGGLE              8			// Maximum number of entries we record to toggle GPT ESP back and forth
#define MAX_IGNORE_USB              8			// Maximum number of USB drives we want to ignore
#define MAX_ISO_TO_ESP_SIZE         (1 * GB)	// Maximum size we allow for the ISO → ESP option
#define MAX_DEFAULT_LIST_CARD_SIZE  (500 * GB)	// Size above which we don't list a card without enable HDD or Alt-F
#define MAX_SECTORS_TO_CLEAR        (8 * MB / 512)	// Sectors to zap when clearing the MBR/GPT (must be >34)
#define MAX_USERNAME_LENGTH         128			// Maximum size we'll accept for a WUE specified username
#define MAX_WININST                 4			// Max number of install[.wim|.esd] we can handle on an image
#define MAX_MARKER                  80.0f		// Max number of '+' signs we display for progress
#define MBR_UEFI_MARKER             0x49464555	// 'U', 'E', 'F', 'I', as a 32 bit little endian longword
#define MORE_INFO_URL               0xFFFF
#define PROJECTED_SIZE_RATIO        110			// Percentage by which we inflate projected_size to prevent persistence overflow
#define STATUS_MSG_TIMEOUT          3500		// How long should cheat mode messages appear for on the status bar
#define WRITE_RETRIES               4
#define WRITE_TIMEOUT               5000		// How long we should wait between write retries (in ms)
#define SEARCH_PROCESS_TIMEOUT      5000		// How long we should wait to get the conflicting process data (in ms)
#define NET_SESSION_TIMEOUT         3500		// How long we should wait to connect, send or receive internet data (in ms)
#define FS_DEFAULT                  FS_FAT32
#define SINGLE_CLUSTERSIZE_DEFAULT  0x00000100
#define BADLOCKS_PATTERN_TYPES      5
#define BADBLOCK_PATTERN_COUNT      4
#define BADBLOCK_PATTERN_ONE_PASS   {0x55, 0x00, 0x00, 0x00}
#define BADBLOCK_PATTERN_TWO_PASSES {0x55, 0xaa, 0x00, 0x00}
#define BADBLOCK_PATTERN_SLC        {0x00, 0xff, 0x55, 0xaa}
#define BADCLOCK_PATTERN_MLC        {0x00, 0xff, 0x33, 0xcc}
#define BADBLOCK_PATTERN_TLC        {0x00, 0xff, 0x1c71c7, 0xe38e38}
#define BADBLOCK_BLOCK_SIZE         (512 * KB)
#define LARGE_FAT32_SIZE            (32 * GB)	// Size at which we need to use fat32format
#define UDF_FORMAT_SPEED            3.1f		// Speed estimate at which we expect UDF drives to be formatted (GB/s)
#define UDF_FORMAT_WARN             20			// Duration (in seconds) above which we warn about long UDF formatting times
#define MAX_FAT32_SIZE              (2 * TB)	// Threshold above which we disable FAT32 formatting
#define FAT32_CLUSTER_THRESHOLD     1.011f		// For FAT32, cluster size changes don't occur at power of 2 boundaries but slightly above
#define DD_BUFFER_SIZE              (32 * MB)	// Minimum size of buffer to use for DD operations
#define UBUFFER_SIZE                4096
#define ISO_BUFFER_SIZE             (64 * KB)	// Buffer size used for ISO data extraction
#define RSA_SIGNATURE_SIZE          256
#if defined(RUFUS_TEST)
#define RUFUS_URL                   "http://nas/~rufus"
#else
#define RUFUS_URL                   "https://rufus.ie"
#endif
#define DOWNLOAD_URL                RUFUS_URL "/downloads"
#define FILES_URL                   RUFUS_URL "/files"
#define FILES_DIR                   APPLICATION_NAME
#define FIDO_VERSION                "z1"
#define WPPRECORDER_MORE_INFO_URL   "https://github.com/pbatard/rufus/wiki/FAQ#bsods-with-windows-to-go-drives-created-from-windows-10-1809-isos"
// Generated by following https://randomascii.wordpress.com/2013/03/09/symbols-the-microsoft-way/
#define DISKCOPY_URL                "https://msdl.microsoft.com/download/symbols/diskcopy.dll/54505118173000/diskcopy.dll"
#define DISKCOPY_HASH               "95fc0786f5bc0a6db5c0604b31ac18fbed0502a2c6858e5fb02a647983ae03c7"
#define DISKCOPY_SIZE               0x16ee00
#define DISKCOPY_IMAGE_OFFSET       0x66d8
#define DISKCOPY_IMAGE_SIZE         0x168000
#define SYMBOL_SERVER_USER_AGENT    "Microsoft-Symbol-Server/10.0.22621.755"
// Per https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-useraccounts-localaccounts-localaccount-name
// and https://learn.microsoft.com/en-us/previous-versions/cc722458(v=technet.10)#user-name-policies
// Also add '&', even as it could be escaped, as it's just not worth the trouble...
#define USERNAME_INVALID_CHARS      "/\\[]:;|=,+*?<>%@&\""
#define IS_POWER_OF_2(x)            ((x != 0) && (((x) & ((x) - 1)) == 0))
#define IGNORE_RETVAL(expr)         do { (void)(expr); } while(0)
#ifndef ARRAYSIZE
#define ARRAYSIZE(A)                (sizeof(A)/sizeof((A)[0]))
#endif
#ifndef STRINGIFY
#define STRINGIFY(x)                #x
#endif
#define PERCENTAGE(percent, value)  ((1ULL * (percent) * (value)) / 100ULL)
#define CHECK_FOR_USER_CANCEL       if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) goto out
// Bit masks used for the display of additional image options in the UI
#define IMOP_WINTOGO                0x01
#define IMOP_PERSISTENCE            0x02

#define safe_free(p) do { free((void*)p); p = NULL; } while(0)
#define safe_mm_free(p) do { _mm_free((void*)p); p = NULL; } while(0)
static __inline void safe_strcp(char* dst, const size_t dst_max, const char* src, const size_t count) {
	memmove(dst, src, min(count, dst_max));
	if (dst != NULL) dst[min(count, dst_max) - 1] = 0;
}
#define safe_strcpy(dst, dst_max, src) safe_strcp(dst, dst_max, src, safe_strlen(src) + 1)
#define static_strcpy(dst, src) safe_strcpy(dst, sizeof(dst), src)
// Same semantics as strncat_s(dst, dst_max, src, _TRUNCATE): append as much as fits, always terminate
static __inline void safe_strcat(char* dst, const size_t dst_max, const char* src) {
	size_t len, n;
	if ((dst == NULL) || (dst_max == 0) || (src == NULL))
		return;
	len = strnlen(dst, dst_max);
	if (len >= dst_max) {
		dst[0] = 0;
		return;
	}
	n = min(strlen(src), dst_max - len - 1);
	memcpy(&dst[len], src, n);
	dst[len + n] = 0;
}
#define static_strcat(dst, src) safe_strcat(dst, sizeof(dst), src)
#define safe_strcmp(str1, str2) strcmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2))
#define safe_strstr(str1, str2) strstr(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2))
#define safe_stricmp(str1, str2) _stricmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2))
#define safe_strncmp(str1, str2, count) strncmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2), count)
#define safe_strnicmp(str1, str2, count) _strnicmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2), count)
#define safe_close(fd) do { if ((fd) >= 0) { close(fd); fd = -1; } } while(0)
#define safe_sprintf(dst, count, ...) do { size_t _count = count; char* _dst = dst; snprintf(_dst, _count, __VA_ARGS__); \
	if (_dst != NULL) _dst[(_count) - 1] = 0; } while(0)
#define static_sprintf(dst, ...) safe_sprintf(dst, sizeof(dst), __VA_ARGS__)
#define safe_atoi(str) ((((char*)(str))==NULL) ? 0 : atoi(str))
#define safe_strlen(str) ((((char*)(str))==NULL) ? 0 : strlen(str))
#define safe_strdup(str) ((((char*)(str))==NULL) ? NULL : _strdup(str))
#define safe_vsnprintf vsnprintf
// NB: Unlike CharLowerA()/CharUpperA(), these only convert ASCII, which is what UTF-8 data needs
static __inline void _strtolower(char* p) {
	for (; *p != 0; p++) if ((*p >= 'A') && (*p <= 'Z')) *p += 'a' - 'A';
}
static __inline void _strtoupper(char* p) {
	for (; *p != 0; p++) if ((*p >= 'a') && (*p <= 'z')) *p -= 'a' - 'A';
}
#define safe_strtolower(str) do { if (str != NULL) _strtolower(str); } while(0)
#define safe_strtoupper(str) do { if (str != NULL) _strtoupper(str); } while(0)
static __inline void static_repchr(char* p, char s, char r) {
	if (p != NULL) while (*p != 0) { if (*p == s) *p = r; p++; }
}
#define to_unix_path(str) static_repchr(str, '\\', '/')
#define to_windows_path(str) static_repchr(str, '/', '\\')
#define if_assert_fails(cond) assert(cond); if (!(cond))
#define if_assert_succeeds(cond) assert(cond); if ((cond))

extern void uprintf(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern void uprintfs(const char *str);
extern void uprint_progress(uint64_t cur_value, uint64_t max_value);
#define vuprintf(...) do { if (verbose) uprintf(__VA_ARGS__); } while(0)
#define vvuprintf(...) do { if (verbose > 1) uprintf(__VA_ARGS__); } while(0)
#define suprintf(...) do { if (!bSilent) uprintf(__VA_ARGS__); } while(0)
#define uuprintf(...) do { if (usb_debug) uprintf(__VA_ARGS__); } while(0)
#define ubprintf(...) do { safe_sprintf(&ubuffer[ubuffer_pos], UBUFFER_SIZE - ubuffer_pos - 4, __VA_ARGS__); \
	ubuffer_pos = strlen(ubuffer); ubuffer[ubuffer_pos++] = '\r'; ubuffer[ubuffer_pos++] = '\n'; \
	ubuffer[ubuffer_pos] = 0; } while(0)
#define ubflush() do { if (ubuffer_pos) uprintf("%s", ubuffer); ubuffer_pos = 0; } while(0)
#ifdef _DEBUG
#define duprintf uprintf
#else
#define duprintf(...)
#endif

/* Action type, for progress bar breakdown */
enum action_type {
	OP_NOOP_WITH_TASKBAR = -3,
	OP_NOOP = -2,
	OP_INIT = -1,
	OP_ANALYZE_MBR = 0,
	OP_BADBLOCKS,
	OP_ZERO_MBR,
	OP_PARTITION,
	OP_FORMAT,
	OP_CREATE_FS,
	OP_FIX_MBR,
	OP_FILE_COPY,
	OP_PATCH,
	OP_FINALIZE,
	OP_EXTRACT_ZIP,
	OP_MAX
};

/* File system indexes in our FS combobox */
enum fs_type {
	FS_UNKNOWN = -1,
	FS_FAT16 = 0,
	FS_FAT32,
	FS_NTFS,
	FS_UDF,
	FS_EXFAT,
	FS_REFS,
	FS_EXT2,
	FS_EXT3,
	FS_EXT4,
	FS_MAX
};

enum boot_type {
	BT_NON_BOOTABLE = 0,
	BT_MSDOS,
	BT_FREEDOS,
	BT_IMAGE,
	BT_SYSLINUX_V4,		// Start of indexes that only display in advanced mode
	BT_SYSLINUX_V6,
	BT_REACTOS,
	BT_GRUB4DOS,
	BT_GRUB2,
	BT_UEFI_NTFS,
	BT_MAX
};

enum target_type {
	TT_BIOS = 0,
	TT_UEFI,
	TT_MAX
};
// For the partition types we'll use Microsoft's PARTITION_STYLE_### constants (see compat.h)
#define PARTITION_STYLE_SFD PARTITION_STYLE_RAW

enum image_option_type {
	IMOP_WIN_STANDARD = 0,
	IMOP_WIN_EXTENDED,
	IMOP_WIN_TO_GO,
	IMOP_MAX
};

enum file_io_type {
	FILE_IO_READ = 0,
	FILE_IO_WRITE,
	FILE_IO_APPEND
};

enum EFI_BOOT_TYPE {
	EBT_MAIN = 0,
	EBT_GRUB,
	EBT_MOKMANAGER,
	EBT_BOOTMGR
};

/* Special handling for old .c32 files we need to replace */
#define NB_OLD_C32          2
#define OLD_C32_NAMES       { "menu.c32", "vesamenu.c32" }
#define OLD_C32_THRESHOLD   { 53500, 148000 }

/* ISO details that the application may want */
#define WINPE_I386          0x0007
#define WINPE_AMD64         0x0023
#define WINPE_MININT        0x01C0
#define SPECIAL_WIM_VERSION 0x000E0000
#define HAS_KOLIBRIOS(r)    (r.has_kolibrios)
#define HAS_REACTOS(r)      (r.reactos_path[0] != 0)
#define HAS_GRUB(r)         ((r.has_grub2) || (r.has_grub4dos))
#define HAS_SYSLINUX(r)     (r.sl_version != 0)
#define HAS_BOOTMGR_BIOS(r) (r.has_bootmgr)
#define HAS_BOOTMGR_EFI(r)  (r.has_bootmgr_efi)
#define HAS_BOOTMGR(r)      (HAS_BOOTMGR_BIOS(r) || HAS_BOOTMGR_EFI(r))
#define HAS_REGULAR_EFI(r)  (r.has_efi & 0x7FFE)
#define HAS_WININST(r)      (r.wininst_index != 0)
#define HAS_WINPE(r)        (((r.winpe & WINPE_I386) == WINPE_I386)||((r.winpe & WINPE_AMD64) == WINPE_AMD64)||((r.winpe & WINPE_MININT) == WINPE_MININT))
#define HAS_WINDOWS(r)      (HAS_BOOTMGR(r) || (r.uses_minint) || HAS_WINPE(r))
#define HAS_WIN7_EFI(r)     ((r.has_efi == 1) && HAS_WININST(r))
#define HAS_FATLESS_GRUB(r) ((r.has_grub2 & 0x80) && !(r.has_grub2_fs & 0x1))
#define HAS_NTFSLESS_GRUB(r)((r.has_grub2 & 0x80) && !(r.has_grub2_fs & 0x4))
#define IS_WINDOWS_1X(r)    (r.has_bootmgr_efi && (r.win_version.major >= 10))
#define IS_WINDOWS_11(r)    (r.has_bootmgr_efi && (r.win_version.major >= 11))
#define IS_FAT32_COMPAT(r)  (((r.has_4GB_file == 0 && !HAS_FATLESS_GRUB(r)) || (r.has_4GB_file == 0x11 && allow_dual_uefi_bios)) && !r.needs_ntfs)
#define HAS_EFI_IMG(r)      (r.efi_img_path[0] != 0)
#define IS_DD_BOOTABLE(r)   (r.is_bootable_img > 0)
#define IS_DD_ONLY(r)       ((r.is_bootable_img > 0) && (!r.is_iso || r.disable_iso))
#define IS_EFI_BOOTABLE(r)  (r.has_efi != 0)
#define IS_BIOS_BOOTABLE(r) (HAS_BOOTMGR(r) || HAS_SYSLINUX(r) || HAS_WINPE(r) || HAS_GRUB(r) || HAS_REACTOS(r) || HAS_KOLIBRIOS(r))
#define HAS_WINTOGO(r)      (HAS_BOOTMGR(r) && IS_EFI_BOOTABLE(r) && HAS_WININST(r))
#define HAS_PERSISTENCE(r)  ((HAS_SYSLINUX(r) || HAS_GRUB(r)) && !(HAS_WINDOWS(r) || HAS_REACTOS(r) || HAS_KOLIBRIOS(r)))
#define IS_FAT(fs)          ((fs == FS_FAT16) || (fs == FS_FAT32))
#define IS_EXT(fs)          ((fs >= FS_EXT2) && (fs <= FS_EXT4))
#define SYMLINKS_RR         0x01
#define SYMLINKS_UDF        0x02

typedef struct {
	uint16_t major;
	uint16_t minor;
	uint16_t build;
	uint16_t revision;
} winver_t;

/*
 * We can't use the Microsoft enums as we want to have RISC-V and LoongArch
 * and we also translate these to IDR_BASE + Value for resource items, which,
 * if we were to use the Microsoft IMAGE_FILE_MACHINE constants, would force
 * us to leave a 65536 sized gap between bases in order to properly map
 * 0x8664 (x86_64), 0xaa64 (ARM64) and so on...
 * NB: When editing this, make sure to also update the values in resource.h!
 */
enum ArchType {
	ARCH_UNKNOWN = 0,
	ARCH_X86_32,
	ARCH_X86_64,
	ARCH_ARM_32,
	ARCH_ARM_64,
	ARCH_IA_64,
	ARCH_RISCV_64,
	ARCH_LOONGARCH_64,
	ARCH_EBC,
	ARCH_MAX
};

typedef struct {
	uint8_t type;
	char path[64];
} efi_boot_entry_t; 

typedef struct {
	char label[192];					// 3*64 to account for UTF-8
	char usb_label[192];				// converted USB label for workaround
	char cfg_path[128];					// path to the ISO's isolinux.cfg
	char reactos_path[128];				// path to the ISO's freeldr.sys or setupldr.sys
	char wininst_path[MAX_WININST][64];	// path to the Windows install image(s)
	efi_boot_entry_t efi_boot_entry[64];// types and paths of detected UEFI bootloaders
	char efi_img_path[128];				// path to an efi.img file
	uint64_t image_size;
	uint64_t projected_size;
	int64_t mismatch_size;
	uint32_t wininst_version;
	BOOLEAN is_iso;
	int8_t is_bootable_img;
	BOOLEAN is_vhd;
	BOOLEAN is_windows_img;
	BOOLEAN disable_iso;
	BOOLEAN rh8_derivative;
	uint16_t winpe;
	uint16_t has_efi;
	uint8_t has_secureboot_bootloader;
	uint8_t has_md5sum;
	uint8_t wininst_index;
	uint8_t has_symlinks;
	uint8_t has_4GB_file;
	BOOLEAN has_long_filename;
	BOOLEAN has_deep_directories;
	BOOLEAN has_bootmgr;
	BOOLEAN has_bootmgr_efi;
	BOOLEAN has_autorun;
	BOOLEAN has_old_c32[NB_OLD_C32];
	BOOLEAN has_old_vesamenu;
	BOOLEAN has_efi_syslinux;
	BOOLEAN has_grub4dos;
	uint8_t has_grub2;
	uint8_t has_grub2_fs;
	BOOLEAN has_compatresources_dll;
	BOOLEAN has_panther_unattend;
	BOOLEAN has_kolibrios;
	BOOLEAN needs_syslinux_overwrite;
	BOOLEAN needs_ntfs;
	BOOLEAN uses_casper;
	BOOLEAN uses_minint;
	uint8_t compression_type;
	winver_t win_version;	// Windows ISO version
	uint16_t sl_version;	// Syslinux/Isolinux version
	char sl_version_str[12];
	char sl_version_ext[32];
	char grub2_version[192];
} RUFUS_IMG_REPORT;

/* Isolate the Syslinux version numbers */
#define SL_MAJOR(x) ((uint8_t)((x)>>8))
#define SL_MINOR(x) ((uint8_t)(x))

typedef struct {
	char* id;
	char* name;
	char* display_name;
	char* label;
	char* hub;
	DWORD index;
	uint32_t port;
	uint64_t size;
} RUFUS_DRIVE;

typedef struct {
	uint16_t version[3];
	uint32_t platform_min[2];		// minimum platform version required
	char* download_url;
	char* release_notes;
} RUFUS_UPDATE;

typedef struct {
	DWORD Type;
	DWORD DeviceNum;
	DWORD BufSize;
	LONGLONG DeviceSize;
	char* DevicePath;
	char* ImagePath;
	char* Label;
} IMG_SAVE;

/*
 * Structure and macros used for the extensions specification of FileDialog()
 * You can use:
 *   EXT_DECL(my_extensions, "default.std", __VA_GROUP__("*.std", "*.other"), __VA_GROUP__("Standard type", "Other Type"));
 * to define an 'ext_t my_extensions' variable initialized with the relevant attributes.
 */
typedef struct {
	size_t count;
	const char* filename;
	const char** extension;
	const char** description;
} ext_t;

/* Alignment macro */
#define ALIGNED(m) __attribute__ ((__aligned__(m)))

/* Hash definitions */
enum hash_type {
	HASH_MD5 = 0,
	HASH_SHA1,
	HASH_SHA256,
	HASH_SHA512,
	HASH_MAX
};

/* Blocksize for each hash algorithm - Must be a power of 2 */
#define MD5_BLOCKSIZE       64
#define SHA1_BLOCKSIZE      64
#define SHA256_BLOCKSIZE    64
#define SHA512_BLOCKSIZE    128
#define MAX_BLOCKSIZE       SHA512_BLOCKSIZE

/* Hashsize for each hash algorithm */
#define MD5_HASHSIZE        16
#define SHA1_HASHSIZE       20
#define SHA256_HASHSIZE     32
#define SHA512_HASHSIZE     64
#define MAX_HASHSIZE        SHA512_HASHSIZE

/* Context for the hash algorithms */
typedef struct ALIGNED(64) {
	uint8_t buf[MAX_BLOCKSIZE];
	uint64_t state[8];
	uint64_t bytecount;
} HASH_CONTEXT;

/* Certificate info */
typedef struct {
	char name[256];
	uint8_t thumbprint[SHA1_HASHSIZE];
} cert_info_t;

/* Hash functions */
typedef void hash_init_t(HASH_CONTEXT* ctx);
typedef void hash_write_t(HASH_CONTEXT* ctx, const uint8_t* buf, size_t len);
typedef void hash_final_t(HASH_CONTEXT* ctx);
extern hash_init_t* hash_init[HASH_MAX];
extern hash_write_t* hash_write[HASH_MAX];
extern hash_final_t* hash_final[HASH_MAX];

/* SBAT entry */
typedef struct {
	char* product;
	uint32_t version;
} sbat_entry_t;

/* Certificate thumbprint list */
typedef struct {
	uint32_t count;
	uint8_t list[0][SHA1_HASHSIZE];
} thumbprint_list_t;

#ifndef __VA_GROUP__
#define __VA_GROUP__(...)  __VA_ARGS__
#endif
#define EXT_X(prefix, ...) const char* _##prefix##_x[] = { __VA_ARGS__ }
#define EXT_D(prefix, ...) const char* _##prefix##_d[] = { __VA_ARGS__ }
#define EXT_DECL(var, filename, extensions, descriptions)                   \
	EXT_X(var, extensions);                                                 \
	EXT_D(var, descriptions);                                               \
	ext_t var = { ARRAYSIZE(_##var##_x), filename, _##var##_x, _##var##_d }

static __inline USHORT GetApplicationArch(void)
{
#if defined(__x86_64__)
	return IMAGE_FILE_MACHINE_AMD64;
#elif defined(__i386__)
	return IMAGE_FILE_MACHINE_I386;
#elif defined(__aarch64__)
	return IMAGE_FILE_MACHINE_ARM64;
#elif defined(__arm__)
	return IMAGE_FILE_MACHINE_ARM;
#elif defined(__riscv) && (__riscv_xlen == 64)
	return IMAGE_FILE_MACHINE_RISCV64;
#elif defined(__loongarch64)
	return IMAGE_FILE_MACHINE_LOONGARCH64;
#else
	return IMAGE_FILE_MACHINE_UNKNOWN;
#endif
}

static __inline const char* GetArchName(USHORT uArch)
{
	switch (uArch) {
	case IMAGE_FILE_MACHINE_AMD64:
		return "x64";
	case IMAGE_FILE_MACHINE_I386:
		return "x86";
	case IMAGE_FILE_MACHINE_ARM64:
		return "ARM64";
	case IMAGE_FILE_MACHINE_ARM:
		return "ARM32";
	case IMAGE_FILE_MACHINE_RISCV64:
		return "RISC-V 64";
	case IMAGE_FILE_MACHINE_LOONGARCH64:
		return "LoongArch 64";
	default:
		return "Unknown";
	}
}

typedef struct {
	WORD Major;
	WORD Minor;
	WORD Micro;
	WORD Nano;
} version_t;
static __inline uint64_t version_to_uint64(version_t* ver) {
	return (uint64_t)ver->Major << 48 | (uint64_t)ver->Minor << 32 | (uint64_t)ver->Micro << 16 | ver->Nano;
}

// Windows User Experience (unattend.xml) flags and masks
#define UNATTEND_SECUREBOOT_TPM_MINRAM      0x00001
#define UNATTEND_NO_ONLINE_ACCOUNT          0x00004
#define UNATTEND_NO_DATA_COLLECTION         0x00008
#define UNATTEND_OFFLINE_INTERNAL_DRIVES    0x00010
#define UNATTEND_DUPLICATE_LOCALE           0x00020
#define UNATTEND_SET_USER                   0x00040
#define UNATTEND_DISABLE_BITLOCKER          0x00080
#define UNATTEND_FORCE_S_MODE               0x00100
#define UNATTEND_USE_MS2023_BOOTLOADERS     0x00200
#define UNATTEND_APPLY_SKUSIPOLICY          0x00400
#define UNATTEND_SILENT_INSTALL             0x00800
#define UNATTEND_QOL_ENHANCEMENTS           0x01000
#define UNATTEND_FULL_MASK                  0x01FFF
#define UNATTEND_DEFAULT_MASK               0x016FF		// Mask of values that are persisted
#define UNATTEND_WINDOWS_TO_GO              0x10000		// Special flag for Windows To Go

#define UNATTEND_WINPE_SETUP_MASK           (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SILENT_INSTALL)
#define UNATTEND_SPECIALIZE_DEPLOYMENT_MASK (UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OOBE_SHELL_SETUP_MASK      (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE | UNATTEND_SILENT_INSTALL)
#define UNATTEND_OOBE_INTERNATIONAL_MASK    (UNATTEND_DUPLICATE_LOCALE)
#define UNATTEND_OOBE_MASK                  (UNATTEND_OOBE_SHELL_SETUP_MASK | UNATTEND_OOBE_INTERNATIONAL_MASK | UNATTEND_DISABLE_BITLOCKER | \
                                             UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OFFLINE_SERVICING_MASK     (UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_FORCE_S_MODE)
#define UNATTEND_DEFAULT_SELECTION_MASK     (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_OFFLINE_INTERNAL_DRIVES)

/* Used with ListDirectoryContent */
#define LIST_DIR_TYPE_FILE			0x01
#define LIST_DIR_TYPE_DIRECTORY		0x02
#define LIST_DIR_TYPE_RECURSIVE		0x80

/* Hash tables */
typedef struct {
	uint32_t used;
	char* str;
	void* data;
} htab_entry;
typedef struct {
	htab_entry* table;
	uint32_t size;
	uint32_t filled;
} htab_table;
#define HTAB_EMPTY { NULL, 0, 0 }
extern BOOL htab_create(uint32_t nel, htab_table* htab);
extern void htab_destroy(htab_table* htab);
extern uint32_t htab_hash(char* str, htab_table* htab);

/* Basic String Array */
typedef struct {
	char** String;
	uint32_t Index;		// Current array size
	uint32_t Max;		// Maximum array size
} StrArray;
extern void StrArrayCreate(StrArray* arr, uint32_t initial_size);
extern int32_t StrArrayAdd(StrArray* arr, const char* str, BOOL);
extern int32_t StrArrayAddUnique(StrArray* arr, const char* str, BOOL);
extern int32_t StrArrayFind(StrArray* arr, const char* str);
extern void StrArrayClear(StrArray* arr);
extern void StrArrayDestroy(StrArray* arr);
#define IsStrArrayEmpty(arr) (arr.Index == 0)

// Options for the custom selection dialog
#define SELECTION_NEEDS_ALL_TO_PROCEED 1
#define SELECTION_USE_WARNING_ICON     2
typedef struct {
	int style;
	int mask;
	int flags;
	int username_index;
	int edition_index;
	int regional_index;
	int privacy_index;
	StrArray choices;
	StrArray tooltips;
} selection_dialog_options_t;

/*
 * Globals
 */
extern RUFUS_UPDATE update;
extern RUFUS_IMG_REPORT img_report;
extern WORD selected_langid;
extern DWORD ErrorStatus, DownloadStatus, LastWriteError;
extern BOOL use_own_c32[NB_OLD_C32], detect_fakes, op_in_progress, right_to_left_mode;
extern BOOL allow_dual_uefi_bios, large_drive, usb_debug;
extern uint8_t image_options, *pe256ssp;
extern uint16_t rufus_version[3], embedded_sl_version[2];
extern uint32_t pe256ssp_size;
extern uint64_t persistence_size;
extern int64_t iso_blocking_status;
extern size_t ubuffer_pos;
extern const int nb_steps[FS_MAX];
extern sbat_entry_t* sbat_entries;
extern thumbprint_list_t *sb_active_certs, *sb_revoked_certs;
extern int force_update, fs_type, boot_type, partition_type, target_type;
extern unsigned long syslinux_ldlinux_len[2];
extern char ubuffer[UBUFFER_SIZE], embedded_sl_version_str[2][12];
extern char app_dir[MAX_PATH], temp_dir[MAX_PATH], app_data_dir[MAX_PATH], *image_path, *fido_url;
extern StrArray modified_files;

/*
 * Shared prototypes
 *
 * NB: The Linux port only declares what is portable. Win32 and GUI specific
 * functions from upstream are dropped here and come back, with POSIX types,
 * as the files that need them get ported (see LINUX_PORT.md).
 */
extern const char* GetEditionName(DWORD ProductType);
extern int GetEditions(StrArray* version_name, StrArray* version_index);
extern const char* WindowsErrorString(void);
extern DWORD WindowsErrorFromErrno(int errnum);
extern void DumpBufferHex(void *buf, size_t size);
extern void PrintStatusInfo(BOOL info, BOOL debug, unsigned int duration, int msg_id, ...);
#define PrintStatus(...) PrintStatusInfo(FALSE, FALSE, __VA_ARGS__)
#define PrintStatusDebug(...) PrintStatusInfo(FALSE, TRUE, __VA_ARGS__)
#define PrintInfo(...) PrintStatusInfo(TRUE, FALSE, __VA_ARGS__)
#define PrintInfoDebug(...) PrintStatusInfo(TRUE, TRUE, __VA_ARGS__)
extern void UpdateProgress(int op, float percent);
extern void _UpdateProgressWithInfo(int op, int msg, uint64_t processed, uint64_t total, BOOL force);
#define UpdateProgressWithInfo(op, msg, processed, total) _UpdateProgressWithInfo(op, msg, processed, total, FALSE)
#define UpdateProgressWithInfoUpTo(upto, op, msg, processed, total) _UpdateProgressWithInfo(op, msg, (processed) * (upto), (total) * 100, FALSE)
#define UpdateProgressWithInfoForce(op, msg, processed, total) _UpdateProgressWithInfo(op, msg, processed, total, TRUE)
// hProgressDialog is an opaque handle of the front end, for a dialog that has its own progress bar (NULL for none)
#define UpdateProgressWithInfoInit(hProgressDialog, bNoAltMode) UpdateProgressWithInfo(OP_INIT, (int)bNoAltMode, (uint64_t)(uintptr_t)hProgressDialog, 0)
extern const char* StrError(DWORD error_code, BOOL use_default_locale);
extern char* GuidToString(const GUID* guid, BOOL bDecorated);
extern GUID StringToGuid(const char* str);
extern char* SizeToHumanReadable(uint64_t size, BOOL copy_to_log, BOOL fake_units);
extern char* TimestampToHumanReadable(uint64_t ts);
extern BOOL ExtractDOS(const char* path);
extern BOOL ExtractISO(const char* src_iso, const char* dest_dir, BOOL scan);
extern BOOL ExtractZip(const char* src_zip, const char* dest_dir);
extern int64_t ExtractISOFile(const char* iso, const char* iso_file, const char* dest_file, DWORD attributes);
extern uint32_t ReadISOFileToBuffer(const char* iso, const char* iso_file, uint8_t** buf);
extern BOOL HasEfiImgBootLoaders(void* iso);
extern BOOL DumpFatDir(void* iso, const char* path, int32_t cluster);
extern uint16_t GetSyslinuxVersion(char* buf, size_t buf_size, char** ext);
extern BOOL SetAutorun(const char* path);
extern BOOL FileIO(enum file_io_type io_type, char* path, char** buffer, DWORD* size);
extern DWORD RunCommandWithProgress(const char* const* argv, const char* dir, BOOL log, int msg, const char* pattern);
#define RunCommand(argv, dir, log) RunCommandWithProgress(argv, dir, log, 0, NULL)
extern BOOL CompareGUID(const GUID *guid1, const GUID *guid2);
extern BOOL IsDownloadable(const char* url);
extern uint32_t read_file(const char* path, uint8_t** buf);
extern uint32_t write_file(const char* path, const uint8_t* buf, const uint32_t size);
extern char* get_token_data_file_indexed(const char* token, const char* filename, int index);
#define get_token_data_file(token, filename) get_token_data_file_indexed(token, filename, 1)
extern char* set_token_data_file(const char* token, const char* data, const char* filename);
extern char* get_token_data_buffer(const char* token, unsigned int n, const char* buffer, size_t buffer_size);
extern char* insert_section_data(const char* filename, const char* section, const char* data, BOOL dos2unix);
extern char* replace_in_token_data(const char* filename, const char* token, const char* src, const char* rep, BOOL dos2unix);
extern char* replace_char(const char* src, const char c, const char* rep);
extern void filter_chars(char* str, const char* rem, const char rep);
extern void trim(char* str);
extern char* remove_substr(const char* src, const char* sub);
extern void parse_update(char* buf, size_t len);
extern void* get_data_from_asn1(const uint8_t* buf, size_t buf_len, const char* oid_str, uint8_t asn1_type, size_t* data_len);
extern int sanitize_label(char* label);
extern int IsHDD(DWORD DriveIndex, uint16_t vid, uint16_t pid, const char* strid);
extern int GetIssuerCertificateInfo(uint8_t* cert, cert_info_t* info);
extern BOOL ValidateOpensslSignature(BYTE* pbBuffer, DWORD dwBufferLen, BYTE* pbSignature, DWORD dwSigLen);
extern BOOL DetectSHA1Acceleration(void);
extern BOOL DetectSHA256Acceleration(void);
extern BOOL HashFile(const unsigned type, const char* path, uint8_t* sum);
extern BOOL PE256Buffer(uint8_t* buf, uint32_t len, uint8_t* hash);
extern void UpdateMD5Sum(const char* dest_dir, const char* md5sum_name);
extern BOOL HashBuffer(const unsigned type, const uint8_t* buf, const size_t len, uint8_t* sum);
extern uint8_t* StringToHash(const char* str);
extern BOOL FileMatchesHash(const char* path, const char* str);
extern BOOL BufferMatchesHash(const uint8_t* buf, const size_t len, const char* str);
extern BOOL IsFileInDB(const char* path);
extern BOOL IsSignedBySecureBootAuthority(uint8_t* buf, uint32_t len);
extern int IsBootloaderRevoked(uint8_t* buf, uint32_t len);
extern BOOL IsBufferInDB(const unsigned char* buf, const size_t len);
#define printbits(x) _printbits(sizeof(x), &x, 0)
#define printbitslz(x) _printbits(sizeof(x), &x, 1)
extern char* _printbits(size_t const size, void const * const ptr, int leading_zeroes);
extern char* ToLocaleName(DWORD lang_id);
extern sbat_entry_t* GetSbatEntries(char* sbatlevel);
extern thumbprint_list_t* GetThumbprintEntries(char* thumbprints_txt);
extern uint16_t GetPeArch(uint8_t* buf);
extern uint8_t* GetPeSection(uint8_t* buf, const char* name, uint32_t* len);
extern uint8_t* GetPeSignatureData(uint8_t* buf);
extern uint8_t* RvaToPhysical(uint8_t* buf, uint32_t rva);
extern uint32_t FindResourceRva(const uint16_t* name, uint8_t* root, uint8_t* dir, uint32_t* len);
extern DWORD ListDirectoryContent(StrArray* arr, char* dir, uint8_t type);
extern BOOL WriteFileWithRetry(int fd, const void* lpBuffer, DWORD nNumberOfBytesToWrite,
	DWORD* lpNumberOfBytesWritten, DWORD nNumRetries);
extern int CreatePreallocatedFile(const char* path, int flags, mode_t mode, int64_t size);
extern enum ArchType MachineToArch(WORD machine);

/* Custom application errors */
#define FAC(f)                         ((f)<<16)
#define APPERR(err)                    (APPLICATION_ERROR_MASK|(err))
#define ERROR_INCOMPATIBLE_FS          0x1201
#define ERROR_CANT_QUICK_FORMAT        0x1202
#define ERROR_INVALID_CLUSTER_SIZE     0x1203
#define ERROR_INVALID_VOLUME_SIZE      0x1204
#define ERROR_CANT_START_THREAD        0x1205
#define ERROR_BADBLOCKS_FAILURE        0x1206
#define ERROR_ISO_SCAN                 0x1207
#define ERROR_ISO_EXTRACT              0x1208
#define ERROR_CANT_REMOUNT_VOLUME      0x1209
#define ERROR_CANT_PATCH               0x120A
#define ERROR_CANT_ASSIGN_LETTER       0x120B
#define ERROR_CANT_MOUNT_VOLUME        0x120C
#define ERROR_BAD_SIGNATURE            0x120D
#define ERROR_CANT_DOWNLOAD            0x120E

#define RUFUS_ERROR(err)               (ERROR_SEVERITY_ERROR | FAC(FACILITY_STORAGE) | (err))

/*
 * Rufus: The Reliable USB Formatting Utility
 * Windows type and CRT compatibility layer for the Linux port
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
 * This header only provides the Windows *types*, constants and C runtime
 * spellings that Rufus' platform-independent logic relies on, so that those
 * files can be kept close to upstream. It deliberately does NOT emulate any
 * Windows API (CreateFile, DeviceIoControl, SendMessage, ...): every such call
 * must be rewritten against the Linux platform layer.
 *
 * Integer widths follow the Windows LLP64 model, not the Linux LP64 one:
 * DWORD and LONG are always 32-bit.
 */

#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

/* Basic integer types */
typedef int BOOL;
typedef uint8_t BOOLEAN;
typedef uint8_t BYTE;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef int16_t SHORT;
typedef uint16_t USHORT;
typedef uint16_t WORD;
typedef int32_t INT;
typedef uint32_t UINT;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t DWORD;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORD64;
typedef int8_t INT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef size_t SIZE_T;
typedef ssize_t SSIZE_T;
typedef uintptr_t ULONG_PTR;
typedef intptr_t LONG_PTR;
typedef int32_t HRESULT;

/* Pointer types */
typedef void* PVOID;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef BYTE* LPBYTE;
typedef DWORD* LPDWORD;

typedef union {
	struct {
		DWORD LowPart;
		LONG HighPart;
	};
	LONGLONG QuadPart;
} LARGE_INTEGER;

typedef union {
	struct {
		DWORD LowPart;
		DWORD HighPart;
	};
	ULONGLONG QuadPart;
} ULARGE_INTEGER;

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t Data4[8];
} GUID;
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Linux paths are not limited to 260 characters */
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif

#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A) / sizeof((A)[0]))
#endif

#ifndef __cplusplus
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#endif

/*
 * HRESULT style status codes. Rufus stores its operation status as
 * RUFUS_ERROR(code), i.e. an HRESULT carrying a Win32 error code, and
 * decodes it with IS_ERROR() and SCODE_CODE().
 */
#define S_OK                            ((HRESULT)0)
#define E_FAIL                          ((HRESULT)0x80004005)
#define E_ACCESSDENIED                  ((HRESULT)0x80070005)
#define ERROR_SEVERITY_ERROR            0xC0000000
#define APPLICATION_ERROR_MASK          0x20000000
#define FACILITY_NULL                   0
#define FACILITY_ITF                    4
#define FACILITY_WIN32                  7
#define FACILITY_STORAGE                3
#define FACILITY_WIM                    142
#define SUCCEEDED(hr)                   (((HRESULT)(hr)) >= 0)
#define FAILED(hr)                      (((HRESULT)(hr)) < 0)
#define IS_ERROR(status)                (((uint32_t)(status)) >> 31 == 1)
#define SCODE_CODE(sc)                  ((sc) & 0xFFFF)
#define SCODE_FACILITY(sc)              (((sc) >> 16) & 0x1FFF)
#define HRESULT_FROM_WIN32(x)           ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : \
	((HRESULT)(((x) & 0x0000FFFF) | (FACILITY_WIN32 << 16) | 0x80000000)))

/* Win32 error codes used by Rufus (values match the Windows SDK) */
#define ERROR_SUCCESS                           0
#define ERROR_FILE_NOT_FOUND                    2
#define ERROR_PATH_NOT_FOUND                    3
#define ERROR_ACCESS_DENIED                     5
#define ERROR_INVALID_HANDLE                    6
#define ERROR_NOT_ENOUGH_MEMORY                 8
#define ERROR_INVALID_BLOCK                     9
#define ERROR_INVALID_DATA                      13
#define ERROR_OUTOFMEMORY                       14
#define ERROR_INVALID_DRIVE                     15
#define ERROR_NO_MORE_FILES                     18
#define ERROR_WRITE_PROTECT                     19
#define ERROR_NOT_READY                         21
#define ERROR_BAD_LENGTH                        24
#define ERROR_SECTOR_NOT_FOUND                  27
#define ERROR_WRITE_FAULT                       29
#define ERROR_READ_FAULT                        30
#define ERROR_GEN_FAILURE                       31
#define ERROR_SHARING_VIOLATION                 32
#define ERROR_HANDLE_EOF                        38
#define ERROR_NOT_SUPPORTED                     50
#define ERROR_DEV_NOT_EXIST                     55
#define ERROR_INVALID_PARAMETER                 87
#define ERROR_OPEN_FAILED                       110
#define ERROR_DISK_FULL                         112
#define ERROR_INSUFFICIENT_BUFFER               122
#define ERROR_LABEL_TOO_LONG                    154
#define ERROR_DIR_NOT_EMPTY                     145
#define ERROR_ALREADY_EXISTS                    183
#define ERROR_FILE_TOO_LARGE                    223
#define ERROR_NO_MORE_ITEMS                     259
#define ERROR_MR_MID_NOT_FOUND                  317
#define ERROR_OPERATION_ABORTED                 995
#define ERROR_IO_PENDING                        997
#define ERROR_SERVICE_DEPENDENCY_FAIL           1068
#define ERROR_NO_MEDIA_IN_DRIVE                 1112
#define ERROR_NOT_ALL_ASSIGNED                  1300
#define ERROR_FILE_CORRUPT                      1392
#define ERROR_TIMEOUT                           1460
#define ERROR_SEM_TIMEOUT                       121
#define ERROR_INSTALL_FAILURE                   1603
#define ERROR_NOT_FOUND                         1168
#define ERROR_CANCELLED                         1223
#define ERROR_UNRECOGNIZED_VOLUME               1005
#define ERROR_DEVICE_REINITIALIZATION_NEEDED    1164
#define ERROR_NO_VOLUME_ID                      1173
#define ERROR_INVALID_IMAGE_HASH                577
#define ERROR_OFFSET_ALIGNMENT_VIOLATION        327
#define ERROR_DEVICE_IN_USE                     2404
#define ERROR_OBJECT_IN_LIST                    5011
#define ERROR_MUI_FILE_NOT_FOUND                15100
#define ERROR_MUI_FILE_NOT_LOADED               15105
#define ERROR_API_UNAVAILABLE                   15841

/* Disk partition styles (values match PARTITION_STYLE from winioctl.h) */
#define PARTITION_STYLE_MBR                     0
#define PARTITION_STYLE_GPT                     1
#define PARTITION_STYLE_RAW                     2

/* PE machine types (values match the PE/COFF specification) */
#define IMAGE_FILE_MACHINE_UNKNOWN              0x0000
#define IMAGE_FILE_MACHINE_I386                 0x014c
#define IMAGE_FILE_MACHINE_ARM                  0x01c0
#define IMAGE_FILE_MACHINE_ARMNT                0x01c4
#define IMAGE_FILE_MACHINE_IA64                 0x0200
#define IMAGE_FILE_MACHINE_EBC                  0x0ebc
#define IMAGE_FILE_MACHINE_RISCV32              0x5032
#define IMAGE_FILE_MACHINE_RISCV64              0x5064
#define IMAGE_FILE_MACHINE_RISCV128             0x5128
#define IMAGE_FILE_MACHINE_LOONGARCH32          0x6232
#define IMAGE_FILE_MACHINE_LOONGARCH64          0x6264
#define IMAGE_FILE_MACHINE_AMD64                0x8664
#define IMAGE_FILE_MACHINE_ARM64                0xaa64

/* C runtime spellings */
#define _stricmp                        strcasecmp
#define _strnicmp                       strncasecmp
#define _strdup                         strdup

/* Aligned allocations (the _mm_* spelling is what Rufus uses throughout) */
static inline void* _mm_malloc(size_t size, size_t alignment)
{
	void* p = NULL;

	if (alignment < sizeof(void*))
		alignment = sizeof(void*);
	return (posix_memalign(&p, alignment, size) == 0) ? p : NULL;
}

static inline void _mm_free(void* p)
{
	free(p);
}

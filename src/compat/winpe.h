/*
 * Rufus: The Reliable USB Formatting Utility
 * PE/COFF structures, as defined in winnt.h
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
 * Rufus parses EFI bootloaders, which are PE executables, for the Secure Boot
 * and revocation checks. These are the structures it needs, with the layout
 * of the PE/COFF specification (little endian, as are all supported hosts).
 */

#pragma once

#include "compat.h"

#define IMAGE_DOS_SIGNATURE                     0x5A4D		// MZ
#define IMAGE_NT_SIGNATURE                      0x00004550	// PE\0\0
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC           0x10b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC           0x20b
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES        16
#define IMAGE_SIZEOF_SHORT_NAME                 8
#define IMAGE_DIRECTORY_ENTRY_RESOURCE          2
#define IMAGE_DIRECTORY_ENTRY_SECURITY          4
#define IMAGE_RESOURCE_NAME_IS_STRING           0x80000000
#define IMAGE_RESOURCE_DATA_IS_DIRECTORY        0x80000000
#define WIN_CERT_TYPE_PKCS_SIGNED_DATA          0x0002

typedef struct {
	WORD e_magic;
	WORD e_cblp;
	WORD e_cp;
	WORD e_crlc;
	WORD e_cparhdr;
	WORD e_minalloc;
	WORD e_maxalloc;
	WORD e_ss;
	WORD e_sp;
	WORD e_csum;
	WORD e_ip;
	WORD e_cs;
	WORD e_lfarlc;
	WORD e_ovno;
	WORD e_res[4];
	WORD e_oemid;
	WORD e_oeminfo;
	WORD e_res2[10];
	LONG e_lfanew;
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;

typedef struct {
	WORD Machine;
	WORD NumberOfSections;
	DWORD TimeDateStamp;
	DWORD PointerToSymbolTable;
	DWORD NumberOfSymbols;
	WORD SizeOfOptionalHeader;
	WORD Characteristics;
} IMAGE_FILE_HEADER;

typedef struct {
	DWORD VirtualAddress;
	DWORD Size;
} IMAGE_DATA_DIRECTORY;

typedef struct {
	WORD Magic;
	BYTE MajorLinkerVersion;
	BYTE MinorLinkerVersion;
	DWORD SizeOfCode;
	DWORD SizeOfInitializedData;
	DWORD SizeOfUninitializedData;
	DWORD AddressOfEntryPoint;
	DWORD BaseOfCode;
	DWORD BaseOfData;
	DWORD ImageBase;
	DWORD SectionAlignment;
	DWORD FileAlignment;
	WORD MajorOperatingSystemVersion;
	WORD MinorOperatingSystemVersion;
	WORD MajorImageVersion;
	WORD MinorImageVersion;
	WORD MajorSubsystemVersion;
	WORD MinorSubsystemVersion;
	DWORD Win32VersionValue;
	DWORD SizeOfImage;
	DWORD SizeOfHeaders;
	DWORD CheckSum;
	WORD Subsystem;
	WORD DllCharacteristics;
	DWORD SizeOfStackReserve;
	DWORD SizeOfStackCommit;
	DWORD SizeOfHeapReserve;
	DWORD SizeOfHeapCommit;
	DWORD LoaderFlags;
	DWORD NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32;

typedef struct __attribute__((packed, aligned(4))) {
	WORD Magic;
	BYTE MajorLinkerVersion;
	BYTE MinorLinkerVersion;
	DWORD SizeOfCode;
	DWORD SizeOfInitializedData;
	DWORD SizeOfUninitializedData;
	DWORD AddressOfEntryPoint;
	DWORD BaseOfCode;
	ULONGLONG ImageBase;
	DWORD SectionAlignment;
	DWORD FileAlignment;
	WORD MajorOperatingSystemVersion;
	WORD MinorOperatingSystemVersion;
	WORD MajorImageVersion;
	WORD MinorImageVersion;
	WORD MajorSubsystemVersion;
	WORD MinorSubsystemVersion;
	DWORD Win32VersionValue;
	DWORD SizeOfImage;
	DWORD SizeOfHeaders;
	DWORD CheckSum;
	WORD Subsystem;
	WORD DllCharacteristics;
	ULONGLONG SizeOfStackReserve;
	ULONGLONG SizeOfStackCommit;
	ULONGLONG SizeOfHeapReserve;
	ULONGLONG SizeOfHeapCommit;
	DWORD LoaderFlags;
	DWORD NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64;

typedef struct {
	DWORD Signature;
	IMAGE_FILE_HEADER FileHeader;
	IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32;

typedef struct __attribute__((packed, aligned(4))) {
	DWORD Signature;
	IMAGE_FILE_HEADER FileHeader;
	IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64;

// Only the fields up to the optional header magic are common to both
typedef IMAGE_NT_HEADERS32 IMAGE_NT_HEADERS, *PIMAGE_NT_HEADERS;

typedef struct {
	BYTE Name[IMAGE_SIZEOF_SHORT_NAME];
	union {
		DWORD PhysicalAddress;
		DWORD VirtualSize;
	} Misc;
	DWORD VirtualAddress;
	DWORD SizeOfRawData;
	DWORD PointerToRawData;
	DWORD PointerToRelocations;
	DWORD PointerToLinenumbers;
	WORD NumberOfRelocations;
	WORD NumberOfLinenumbers;
	DWORD Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct {
	DWORD Characteristics;
	DWORD TimeDateStamp;
	WORD MajorVersion;
	WORD MinorVersion;
	WORD NumberOfNamedEntries;
	WORD NumberOfIdEntries;
} IMAGE_RESOURCE_DIRECTORY;

typedef struct {
	union {
		struct {
			DWORD NameOffset : 31;
			DWORD NameIsString : 1;
		};
		DWORD Name;
		WORD Id;
	};
	union {
		DWORD OffsetToData;
		struct {
			DWORD OffsetToDirectory : 31;
			DWORD DataIsDirectory : 1;
		};
	};
} IMAGE_RESOURCE_DIRECTORY_ENTRY;

typedef struct {
	WORD Length;
	uint16_t NameString[1];			// UTF-16
} IMAGE_RESOURCE_DIR_STRING_U;

typedef struct {
	DWORD OffsetToData;
	DWORD Size;
	DWORD CodePage;
	DWORD Reserved;
} IMAGE_RESOURCE_DATA_ENTRY;

typedef struct {
	DWORD dwLength;
	WORD wRevision;
	WORD wCertificateType;
	BYTE bCertificate[1];
} WIN_CERTIFICATE;

_Static_assert(sizeof(IMAGE_DOS_HEADER) == 64, "Invalid IMAGE_DOS_HEADER size");
_Static_assert(sizeof(IMAGE_OPTIONAL_HEADER32) == 224, "Invalid IMAGE_OPTIONAL_HEADER32 size");
_Static_assert(sizeof(IMAGE_OPTIONAL_HEADER64) == 240, "Invalid IMAGE_OPTIONAL_HEADER64 size");
_Static_assert(sizeof(IMAGE_NT_HEADERS32) == 248, "Invalid IMAGE_NT_HEADERS32 size");
_Static_assert(sizeof(IMAGE_NT_HEADERS64) == 264, "Invalid IMAGE_NT_HEADERS64 size");
_Static_assert(sizeof(IMAGE_SECTION_HEADER) == 40, "Invalid IMAGE_SECTION_HEADER size");
_Static_assert(sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY) == 8, "Invalid IMAGE_RESOURCE_DIRECTORY_ENTRY size");

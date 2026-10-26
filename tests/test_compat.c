/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the Windows type/CRT compatibility layer
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

#include <stdint.h>

#include "compat.h"
#include "test.h"

/* Windows is LLP64: these must keep their Windows widths on LP64 Linux */
_Static_assert(sizeof(DWORD) == 4, "DWORD must be 32-bit");
_Static_assert(sizeof(LONG) == 4, "LONG must be 32-bit");
_Static_assert(sizeof(ULONG) == 4, "ULONG must be 32-bit");
_Static_assert(sizeof(WORD) == 2, "WORD must be 16-bit");
_Static_assert(sizeof(BYTE) == 1, "BYTE must be 8-bit");
_Static_assert(sizeof(LONGLONG) == 8, "LONGLONG must be 64-bit");
_Static_assert(sizeof(LARGE_INTEGER) == 8, "LARGE_INTEGER must be 64-bit");
_Static_assert(sizeof(GUID) == 16, "GUID must be 16 bytes");
_Static_assert(MAX_PATH >= 260, "MAX_PATH must not be smaller than on Windows");

int main(void)
{
	LARGE_INTEGER li;
	uint32_t status;
	void* p;

	/* LARGE_INTEGER halves must alias the 64-bit value (little endian) */
	li.QuadPart = 0x1122334455667788LL;
	CHECK_EQ_INT(li.LowPart, 0x55667788);
	CHECK_EQ_INT(li.HighPart, 0x11223344);

	/* Rufus encodes its status as RUFUS_ERROR(code) */
	status = ERROR_SEVERITY_ERROR | (FACILITY_STORAGE << 16) | ERROR_CANCELLED;
	CHECK(IS_ERROR(status));
	CHECK_EQ_INT(SCODE_CODE(status), ERROR_CANCELLED);
	CHECK_EQ_INT(SCODE_FACILITY(status), FACILITY_STORAGE);
	CHECK(!IS_ERROR(ERROR_SUCCESS));
	CHECK(SUCCEEDED(S_OK));
	CHECK(FAILED(E_FAIL));
	CHECK_EQ_INT((uint32_t)HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), (uint32_t)E_ACCESSDENIED);

	CHECK_EQ_INT(_stricmp("RuFuS", "rufus"), 0);
	CHECK(_strnicmp("EFI/BOOT", "efi/boot/bootx64.efi", 8) == 0);
	CHECK_EQ_INT(min(3, 7), 3);
	CHECK_EQ_INT(max(3, 7), 7);

	p = _mm_malloc(4096, 4096);
	CHECK(p != NULL);
	CHECK(((uintptr_t)p & 4095) == 0);
	_mm_free(p);

	return TEST_RESULT();
}

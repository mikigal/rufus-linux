/*
 * Rufus: The Reliable USB Formatting Utility
 * Binary payloads embedded in the executable
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
 * On Windows, the bootloaders, FreeDOS, DBX and other payloads are RCDATA
 * resources of the executable. Here they are embedded with the assembler's
 * .incbin (C23's #embed is not available on every compiler we build with),
 * and GetResource() keeps the upstream semantics, minus the module and type.
 * RUFUS_RES_DIR is the absolute path of res/, set by the build system.
 */

#include <stdlib.h>
#include <string.h>

#include "rufus.h"
#include "resource.h"

#if !defined(RUFUS_RES_DIR)
#error RUFUS_RES_DIR must be defined
#endif

#define RESOURCE(id, file)                                                       \
	__asm__(".pushsection .rodata\n"                                             \
		".global rufus_res_" #id "\n"                                            \
		".hidden rufus_res_" #id "\n"                                            \
		".global rufus_res_" #id "_end\n"                                        \
		".hidden rufus_res_" #id "_end\n"                                        \
		".balign 16\n"                                                           \
		"rufus_res_" #id ":\n"                                                   \
		".incbin \"" RUFUS_RES_DIR "/" file "\"\n"                               \
		"rufus_res_" #id "_end:\n"                                               \
		".popsection\n");                                                        \
	extern const uint8_t rufus_res_##id[] __attribute__((visibility("hidden")));     \
	extern const uint8_t rufus_res_##id##_end[] __attribute__((visibility("hidden")));
#include "resources.h"
#undef RESOURCE

static const struct {
	int id;
	const uint8_t* start;
	const uint8_t* end;
} resource[] = {
#define RESOURCE(id, file) { id, rufus_res_##id, rufus_res_##id##_end },
#include "resources.h"
#undef RESOURCE
};

/*
 * Get a resource. If needed that resource can be duplicated.
 * If duplicate is true and len is non-zero, the a zeroed buffer of 'len'
 * size is allocated for the resource. Else the buffer is allocated for
 * the resource size.
 * NB: A resource that is not duplicated is read-only.
 */
uint8_t* GetResource(int id, const char* desc, DWORD* len, BOOL duplicate)
{
	DWORD res_len;
	uint8_t* p = NULL;
	size_t i;

	for (i = 0; (i < ARRAYSIZE(resource)) && (resource[i].id != id); i++);
	if (i >= ARRAYSIZE(resource)) {
		uprintf("Could not locate resource '%s'", desc);
		goto out;
	}
	res_len = (DWORD)(resource[i].end - resource[i].start);

	if (duplicate) {
		if (*len == 0)
			*len = res_len;
		p = calloc(*len, 1);
		if (p == NULL) {
			uprintf("Could not allocate resource '%s'", desc);
			goto out;
		}
		memcpy(p, resource[i].start, min(res_len, *len));
		if (res_len > *len)
			uprintf("WARNING: Resource '%s' was truncated by %d bytes!", desc, res_len - *len);
	} else {
		p = (uint8_t*)resource[i].start;
	}
	*len = res_len;

out:
	return p;
}

DWORD GetResourceSize(int id, const char* desc)
{
	DWORD len = 0;
	return (GetResource(id, desc, &len, FALSE) == NULL) ? 0 : len;
}

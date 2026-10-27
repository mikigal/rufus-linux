/*
 * Rufus: The Reliable USB Formatting Utility
 * Registry hives for the tests
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
 * The SYSTEM hives of the boot.wim of the tests, which get the registry keys of
 * the Windows User Experience (see wue_apply.c).
 */

#pragma once

#include <string.h>
#if defined(RUFUS_HAVE_HIVEX)
#include <hivex.h>
#endif

#include "rufus.h"
#include "test.h"

/*
 * A minimal registry hive: a root key, its security descriptor, and a free cell
 * for hivex to add keys to (hivex can only open existing hives).
 */
static inline void make_hive(const char* path)
{
	uint8_t hive[0x1000 + 0x1000] = { 0 };
	uint8_t* hbin = &hive[0x1000];
	uint32_t i, sum = 0, nk = 0x20, sk = 0x80, free_cell = 0xb0;
	static const uint8_t sd[20] = { 1, 0, 0x00, 0x80 };	// Self relative, nothing else

#define PUT16(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); } while (0)
#define PUT32(p, v) do { PUT16(p, v); PUT16(&(p)[2], (v) >> 16); } while (0)
	memcpy(hive, "regf", 4);
	PUT32(&hive[0x04], 1);			// Sequence numbers
	PUT32(&hive[0x08], 1);
	PUT32(&hive[0x14], 1);			// Version 1.3
	PUT32(&hive[0x18], 3);
	PUT32(&hive[0x20], 1);			// Format
	PUT32(&hive[0x24], nk);			// Root key
	PUT32(&hive[0x28], 0x1000);		// Size of the bins
	PUT32(&hive[0x2c], 1);
	for (i = 0; i < 0x1fc; i += 4)
		sum ^= hive[i] | (hive[i + 1] << 8) | (hive[i + 2] << 16) | ((uint32_t)hive[i + 3] << 24);
	PUT32(&hive[0x1fc], sum);

	memcpy(hbin, "hbin", 4);
	PUT32(&hbin[0x08], 0x1000);
	// The root key ("nk"), allocated (negative size)
	PUT32(&hbin[nk], (uint32_t)-0x60);
	memcpy(&hbin[nk + 4], "nk", 2);
	PUT16(&hbin[nk + 6], 0x2c);		// KEY_HIVE_ENTRY | KEY_NO_DELETE | KEY_COMP_NAME
	PUT32(&hbin[nk + 0x14], 0xffffffff);	// Parent
	PUT32(&hbin[nk + 0x20], 0xffffffff);	// Subkeys
	PUT32(&hbin[nk + 0x24], 0xffffffff);	// Volatile subkeys
	PUT32(&hbin[nk + 0x2c], 0xffffffff);	// Values
	PUT32(&hbin[nk + 0x30], sk);			// Security
	PUT32(&hbin[nk + 0x34], 0xffffffff);	// Class
	PUT16(&hbin[nk + 0x4c], 4);			// Name
	memcpy(&hbin[nk + 0x50], "ROOT", 4);
	// Its security descriptor ("sk"), which links to itself
	PUT32(&hbin[sk], (uint32_t)-0x30);
	memcpy(&hbin[sk + 4], "sk", 2);
	PUT32(&hbin[sk + 0x08], sk);
	PUT32(&hbin[sk + 0x0c], sk);
	PUT32(&hbin[sk + 0x10], 1);			// References
	PUT32(&hbin[sk + 0x14], sizeof(sd));
	memcpy(&hbin[sk + 0x18], sd, sizeof(sd));
	// The rest of the bin is free
	PUT32(&hbin[free_cell], 0x1000 - free_cell);
#undef PUT32
#undef PUT16
	CHECK(write_file(path, hive, sizeof(hive)) == sizeof(hive));
}

#if defined(RUFUS_HAVE_HIVEX)
// Add a key under the root key of a hive
static inline void add_hive_key(const char* path, const char* name)
{
	hive_h* h = hivex_open(path, HIVEX_OPEN_WRITE);

	CHECK(h != NULL);
	if (h == NULL)
		return;
	CHECK(hivex_node_add_child(h, hivex_root(h), name) != 0);
	CHECK(hivex_commit(h, NULL, 0) == 0);
	hivex_close(h);
}

// The REG_DWORD value of HKLM\SYSTEM\Setup\LabConfig\name in a SYSTEM hive, or -1
static inline int lab_config_value(const char* path, const char* name)
{
	hive_h* h = hivex_open(path, 0);
	hive_node_h node = 0;
	hive_value_h value = 0;
	int r = -1;

	if (h == NULL)
		return -1;
	node = hivex_node_get_child(h, hivex_root(h), "Setup");
	if (node != 0)
		node = hivex_node_get_child(h, node, "LabConfig");
	if (node != 0)
		value = hivex_node_get_value(h, node, name);
	if (value != 0)
		r = hivex_value_dword(h, value);
	hivex_close(h);
	return r;
}
#endif


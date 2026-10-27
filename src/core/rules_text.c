/*
 * Rufus: The Reliable USB Formatting Utility
 * Text of the main window dropdown entries
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
 * The rules only store the item data of the dropdowns: what the Windows
 * version displays for each of them is rendered here, with the strings that
 * rufus.c adds in SetBootOptions(), SetPartitionSchemeAndTargetSystem(),
 * SetFileSystemAndClusterSize(), SetClusterSizes(), UpdateImage(),
 * SetPersistenceSize() and InitDialog().
 */

#include "rufus.h"
#include "rules.h"
#include "image_scan.h"
#include "resource.h"
#include "localization.h"
#include "../res/grub/grub_version.h"
#include "../res/grub2/grub2_version.h"

extern const char* FileSystemLabel[FS_MAX];
extern char embedded_sl_version_str[2][12];
static const char* sfd_name = "Super Floppy Disk";
static const char* flash_type[BADLOCKS_PATTERN_TYPES] = { "SLC", "MLC", "TLC" };

static void boot_text(int data, uint8_t flags, char* buf, size_t size)
{
	switch (data) {
	case BT_NON_BOOTABLE:
		snprintf(buf, size, "%s", lmprintf(MSG_279));
		break;
	case BT_IMAGE:
		if ((flags & RULES_ENTRY_NO_IMAGE) || (short_image_path == NULL))
			snprintf(buf, size, "%s", lmprintf(MSG_281, lmprintf(MSG_280)));
		else
			snprintf(buf, size, "%s", short_image_path);
		break;
	case BT_MSDOS:
		snprintf(buf, size, "MS-DOS");
		break;
	case BT_FREEDOS:
		snprintf(buf, size, "FreeDOS");
		break;
	case BT_SYSLINUX_V4:
		snprintf(buf, size, "Syslinux %s", embedded_sl_version_str[0]);
		break;
	case BT_SYSLINUX_V6:
		snprintf(buf, size, "Syslinux %s", embedded_sl_version_str[1]);
		break;
	case BT_REACTOS:
		snprintf(buf, size, "ReactOS");
		break;
	case BT_GRUB2:
		snprintf(buf, size, "Grub " GRUB2_PACKAGE_VERSION);
		break;
	case BT_GRUB4DOS:
		snprintf(buf, size, "Grub4DOS " GRUB4DOS_VERSION);
		break;
	case BT_UEFI_NTFS:
		snprintf(buf, size, "UEFI:NTFS");
		break;
	default:
		snprintf(buf, size, "?");
		break;
	}
}

// SetClusterSizeLabels()
static void cluster_text(uint32_t cluster, char* buf, size_t size)
{
	unsigned int i, j, msg_id;

	if (cluster == SINGLE_CLUSTERSIZE_DEFAULT) {
		snprintf(buf, size, "%s", lmprintf(MSG_029));
		return;
	}
	for (i = 512, j = 1, msg_id = MSG_026; j < MAX_CLUSTER_SIZES; i <<= 1, j++) {
		if (i > 8192) {
			i /= 1024;
			msg_id++;
		}
		if ((0x100U << j) == cluster)
			break;
	}
	snprintf(buf, size, "%u %s", i, lmprintf(msg_id));
}

void rules_entry_text(const rules_state_t* m, const rules_combo_t* combo, int index, char* buf, size_t size)
{
	char tmp[256];
	int data;
	uint8_t flags;

	if ((buf == NULL) || (size == 0))
		return;
	buf[0] = 0;
	if ((index < 0) || (index >= combo->count))
		return;
	data = combo->entry[index].data;
	flags = combo->entry[index].flags;
	if (combo == &m->boot) {
		boot_text(data, flags, tmp, sizeof(tmp));
	} else if (combo == &m->image_option) {
		snprintf(tmp, sizeof(tmp), "%s", lmprintf((data == IMOP_WIN_STANDARD) ? MSG_117 : MSG_118));
	} else if (combo == &m->partition) {
		snprintf(tmp, sizeof(tmp), "%s", (data == PARTITION_STYLE_MBR) ? "MBR" :
			((data == PARTITION_STYLE_GPT) ? "GPT" : sfd_name));
	} else if (combo == &m->target) {
		snprintf(tmp, sizeof(tmp), "%s", lmprintf((data == TT_UEFI) ? MSG_032 :
			((flags & RULES_ENTRY_DUAL_TARGET) ? MSG_033 : MSG_031)));
	} else if (combo == &m->fs) {
		snprintf(tmp, sizeof(tmp), "%s%s", (flags & RULES_ENTRY_LARGE_FAT32) ? "Large " : "",
			((data >= 0) && (data < FS_MAX)) ? FileSystemLabel[data] : "?");
	} else if (combo == &m->cluster) {
		cluster_text((uint32_t)data, tmp, sizeof(tmp));
	} else if (combo == &m->persistence_units) {
		snprintf(tmp, sizeof(tmp), "%s", lmprintf(MSG_022 + data));
	} else {
		snprintf(tmp, sizeof(tmp), "%d", data);
	}
	if (flags & RULES_ENTRY_DEFAULT)
		snprintf(buf, size, "%s", lmprintf(MSG_030, tmp));
	else
		snprintf(buf, size, "%s", tmp);
}

// InitDialog(): 1 pass, 2 passes, then 4 passes for SLC, MLC and TLC NAND
void rules_passes_text(int index, char* buf, size_t size)
{
	if (index <= 0)
		snprintf(buf, size, "%s", lmprintf(MSG_034, 1));
	else
		snprintf(buf, size, "%s", lmprintf(MSG_035, (index == 1) ? 2 : 4,
			(index == 1) ? "" : lmprintf(MSG_087, flash_type[min(index - 2, BADLOCKS_PATTERN_TYPES - 1)])));
}

/*
 * Rufus: The Reliable USB Formatting Utility
 * Main window option rules
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
 * The functions below come from rufus.c and ui.c, where they manipulated the
 * controls of the main dialog. The control operations are replaced by their
 * equivalent on rules_state_t, and the rufus.c globals by fields of the same
 * name. See rules.h.
 */

#include "rufus.h"
#include "rules.h"

const char* FileSystemLabel[FS_MAX] = { "FAT", "FAT32", "NTFS", "UDF", "exFAT", "ReFS", "ext2", "ext3", "ext4" };


/*
 * Dropdown operations, with the semantics of the Win32 combo box messages
 * (an invalid selection index clears the selection).
 */
static void ComboBox_ResetContent(rules_combo_t* c)
{
	c->count = 0;
	c->sel = -1;
}

static int ComboBox_Add(rules_combo_t* c, int data, uint8_t flags)
{
	if_assert_fails(c->count < RULES_MAX_ENTRIES)
		return -1;
	c->entry[c->count].data = data;
	c->entry[c->count].flags = flags;
	return c->count++;
}

static int ComboBox_Insert(rules_combo_t* c, int index, int data, uint8_t flags)
{
	int i;

	if (index < 0)
		return ComboBox_Add(c, data, flags);
	if ((index > c->count) || (c->count >= RULES_MAX_ENTRIES))
		return -1;
	for (i = c->count; i > index; i--)
		c->entry[i] = c->entry[i - 1];
	c->entry[index].data = data;
	c->entry[index].flags = flags;
	c->count++;
	if (c->sel >= index)
		c->sel++;
	return index;
}

static void ComboBox_Delete(rules_combo_t* c, int index)
{
	int i;

	if ((index < 0) || (index >= c->count))
		return;
	for (i = index; i < c->count - 1; i++)
		c->entry[i] = c->entry[i + 1];
	c->count--;
	if (c->sel == index)
		c->sel = -1;
	else if (c->sel > index)
		c->sel--;
}

static void ComboBox_SetCurSel(rules_combo_t* c, int index)
{
	c->sel = ((index >= 0) && (index < c->count)) ? index : -1;
}

// From ui.c: select the entry with the given data, or the first one
static void SetComboEntry(rules_combo_t* c, int data)
{
	int i;

	if (c->count <= 0)
		return;
	for (i = 0; i < c->count; i++) {
		if (c->entry[i].data == data) {
			ComboBox_SetCurSel(c, i);
			return;
		}
	}
	ComboBox_SetCurSel(c, 0);
}

/* Restore or clear a check box when it gets enabled or disabled (upstream's static 'checked'/'state' pairs) */
static void ToggleCheckbox(rules_check_t* c, BOOL enable, BOOL remove_checkboxes)
{
	if (remove_checkboxes) {
		if (!enable && (c->state != 1)) {
			c->saved = c->checked;
			c->checked = FALSE;
			c->state = 1;
		} else if (enable && !c->enabled && (c->state != 2)) {
			if (c->state != 0)
				c->checked = c->saved;
			c->state = 2;
		}
	}
	c->enabled = enable;
}

void rules_init(rules_state_t* m)
{
	memset(m, 0, sizeof(*m));
	m->selection_default = BT_IMAGE;
	m->selected_pt = -1;
	m->selected_fs = FS_UNKNOWN;
	m->preselected_fs = FS_UNKNOWN;
	m->default_fs = FS_UNKNOWN;
	m->drive_fs_type = FS_UNKNOWN;
	m->persistence_unit_selection = -1;
	m->image_options = IMOP_WINTOGO;
	m->op_in_progress = TRUE;
	ComboBox_ResetContent(&m->boot);
	ComboBox_ResetContent(&m->image_option);
	ComboBox_ResetContent(&m->partition);
	ComboBox_ResetContent(&m->target);
	ComboBox_ResetContent(&m->fs);
	ComboBox_ResetContent(&m->cluster);
	ComboBox_ResetContent(&m->persistence_units);
	// All the controls start enabled, as in the dialog template
	m->quick_format.enabled = TRUE;
	m->extended_label.enabled = TRUE;
	m->old_bios_fixes.enabled = TRUE;
	m->uefi_validation.enabled = TRUE;
	m->bad_blocks.enabled = TRUE;
	m->device_enabled = TRUE;
	m->boot_enabled = TRUE;
	m->hash_enabled = TRUE;
	m->save_enabled = TRUE;
	m->start_enabled = TRUE;
	m->image_option_enabled = TRUE;
	m->persistence_slider_enabled = TRUE;
	m->persistence_size_enabled = TRUE;
	m->persistence_units_enabled = TRUE;
	m->label_enabled = TRUE;
	m->partition_enabled = TRUE;
	m->target_enabled = TRUE;
	m->fs_enabled = TRUE;
	m->cluster_enabled = TRUE;
}

void SetAllowedFileSystems(rules_state_t* m, BOOL allowed_filesystem[FS_MAX])
{
	int i;

	memset(allowed_filesystem, 0, FS_MAX * sizeof(BOOL));
	// Nothing is allowed if we don't have a drive
	if (!m->has_device)
		return;
	switch (m->selection_default) {
	case BT_NON_BOOTABLE:
		for (i = 0; i < FS_MAX; i++)
			allowed_filesystem[i] = TRUE;
		break;
	case BT_MSDOS:
	case BT_FREEDOS:
		allowed_filesystem[FS_FAT16] = TRUE;
		allowed_filesystem[FS_FAT32] = TRUE;
		break;
	case BT_IMAGE:
		if (!m->has_image || !HAS_NTFSLESS_GRUB(m->img))
			allowed_filesystem[FS_NTFS] = TRUE;
		// Don't allow anything besides NTFS if the image is not compatible
		if (m->has_image && !IS_FAT32_COMPAT(m->img, m->allow_dual_uefi_bios)) {
			// Only disable FAT32 if we have NTFS enabled
			if (allowed_filesystem[FS_NTFS])
				break;
			// Else, print a warning
			uprintf("WARNING: FAT32 has been forcefully enabled, but this image may not work with FAT32.");
		}
		if (!HAS_WINDOWS(m->img) || (m->target_type != TT_BIOS) || m->allow_dual_uefi_bios) {
			if (!HAS_WINTOGO(m->img) || (rules_combo_data(&m->image_option) != IMOP_WIN_TO_GO)) {
				allowed_filesystem[FS_FAT16] = TRUE;
				allowed_filesystem[FS_FAT32] = TRUE;
			}
		}
		break;
	case BT_GRUB2:
		allowed_filesystem[FS_EXT2] = TRUE;
		allowed_filesystem[FS_EXT3] = TRUE;
		allowed_filesystem[FS_EXT4] = TRUE;
		// Fall through
	case BT_SYSLINUX_V6:
	case BT_GRUB4DOS:
		allowed_filesystem[FS_NTFS] = TRUE;
		// Fall through
	case BT_SYSLINUX_V4:
	case BT_REACTOS:
		allowed_filesystem[FS_FAT16] = TRUE;
		allowed_filesystem[FS_FAT32] = TRUE;
		break;
	case BT_UEFI_NTFS:
		allowed_filesystem[FS_NTFS] = TRUE;
		allowed_filesystem[FS_EXFAT] = TRUE;
		break;
	}
}

// Populate the Boot selection dropdown
void SetBootOptions(rules_state_t* m)
{
	ComboBox_ResetContent(&m->boot);
	ComboBox_Add(&m->boot, BT_NON_BOOTABLE, 0);
	ComboBox_Add(&m->boot, BT_IMAGE, m->has_image ? 0 : RULES_ENTRY_NO_IMAGE);
	m->image_index = 1;
	ComboBox_Add(&m->boot, BT_MSDOS, 0);
	ComboBox_Add(&m->boot, BT_FREEDOS, 0);

	if (m->advanced_mode_device) {
		ComboBox_Add(&m->boot, BT_SYSLINUX_V4, 0);
		ComboBox_Add(&m->boot, BT_SYSLINUX_V6, 0);
		ComboBox_Add(&m->boot, BT_REACTOS, 0);
		ComboBox_Add(&m->boot, BT_GRUB2, 0);
		ComboBox_Add(&m->boot, BT_GRUB4DOS, 0);
		ComboBox_Add(&m->boot, BT_UEFI_NTFS, 0);
	}
	if ((!m->advanced_mode_device) && (m->selection_default >= BT_SYSLINUX_V4))
		m->selection_default = BT_IMAGE;
	SetComboEntry(&m->boot, m->selection_default);
}

void SetPartitionSchemeAndTargetSystem(rules_state_t* m, BOOL only_target)
{
	//                                   MBR,  GPT,  SFD
	BOOL allowed_partition_scheme[3] = { TRUE, TRUE, FALSE };
	//                                   BIOS, UEFI, DUAL
	BOOL allowed_target_system[3]    = { TRUE, TRUE, FALSE };
	BOOL is_windows_to_go_selected;

	if (!only_target)
		ComboBox_ResetContent(&m->partition);
	ComboBox_ResetContent(&m->target);

	m->boot_type = rules_combo_data(&m->boot);
	is_windows_to_go_selected = (m->boot_type == BT_IMAGE) && m->has_image && HAS_WINTOGO(m->img) &&
		(rules_combo_data(&m->image_option) == IMOP_WIN_TO_GO);
	// If no device is selected, don't populate anything
	if (!m->has_device)
		return;
	switch (m->boot_type) {
	case BT_NON_BOOTABLE:
		allowed_partition_scheme[PARTITION_STYLE_SFD] = TRUE;
		allowed_target_system[0] = FALSE;
		allowed_target_system[1] = FALSE;
		allowed_target_system[2] = TRUE;
		break;
	case BT_IMAGE:
		if (!m->has_image)
			break;
		// Check if image is EFI bootable
		if (!IS_EFI_BOOTABLE(m->img)) {
			allowed_partition_scheme[PARTITION_STYLE_GPT] = FALSE;
			allowed_target_system[1] = FALSE;
			break;
		}
		// Image is EFI bootable => set dual BIOS + UEFI and so on...
		if (IS_BIOS_BOOTABLE(m->img)) {
			if (!HAS_WINDOWS(m->img) || m->allow_dual_uefi_bios || is_windows_to_go_selected) {
				allowed_target_system[0] = FALSE;
				allowed_target_system[1] = TRUE;
				allowed_target_system[2] = TRUE;
			}
			// Syslinux 4.x or earlier has no support for NTFS so if an image is using Syslinux 4.x only
			// and has a 4 GB file (which forces us to use NTFS) then disable MBR altogether as we won't
			// be able to make a working MBR install of Syslinux.
			if (HAS_SYSLINUX(m->img) && (SL_MAJOR(m->img.sl_version) < 5) && m->img.has_4GB_file &&
				!HAS_BOOTMGR(m->img) && !HAS_WINPE(m->img) && !HAS_GRUB(m->img))
				allowed_partition_scheme[PARTITION_STYLE_MBR] = FALSE;
		} else {
			allowed_target_system[0] = FALSE;
		}
		break;
	case BT_MSDOS:
	case BT_FREEDOS:
	case BT_SYSLINUX_V4:
	case BT_SYSLINUX_V6:
	case BT_REACTOS:
	case BT_GRUB4DOS:
	case BT_GRUB2:
		allowed_partition_scheme[PARTITION_STYLE_GPT] = FALSE;
		allowed_target_system[1] = FALSE;
		break;
	case BT_UEFI_NTFS:
		allowed_target_system[0] = FALSE;
		break;
	}

	if (!only_target) {
		// Override partition type selection to GPT for drives larger than 2TB
		if (m->drive.disk_size > 2 * TB)
			m->selected_pt = PARTITION_STYLE_GPT;
		// Try to reselect the current drive's partition scheme
		int preferred_pt = m->drive.partition_style;
		if (allowed_partition_scheme[PARTITION_STYLE_MBR])
			ComboBox_Add(&m->partition, PARTITION_STYLE_MBR, 0);
		if (allowed_partition_scheme[PARTITION_STYLE_GPT])
			ComboBox_Add(&m->partition, PARTITION_STYLE_GPT, 0);
		if (allowed_partition_scheme[PARTITION_STYLE_SFD])
			ComboBox_Add(&m->partition, PARTITION_STYLE_SFD, 0);
		// Override the partition scheme according to the current
		if (m->boot_type == BT_NON_BOOTABLE)
			preferred_pt = (m->selected_pt >= 0) ? m->selected_pt : PARTITION_STYLE_MBR;
		else if (m->boot_type == BT_UEFI_NTFS)
			preferred_pt = (m->selected_pt >= 0) ? m->selected_pt : PARTITION_STYLE_GPT;
		else if ((m->boot_type == BT_IMAGE) && m->has_image && (m->img.is_iso || m->img.is_windows_img)) {
			if (HAS_WINDOWS(m->img) && m->img.has_efi)
				preferred_pt = m->allow_dual_uefi_bios ? PARTITION_STYLE_MBR :
					((m->selected_pt >= 0) ? m->selected_pt : PARTITION_STYLE_GPT);
			if (IS_DD_BOOTABLE(m->img))
				preferred_pt = (m->selected_pt >= 0) ? m->selected_pt : PARTITION_STYLE_MBR;
		}
		SetComboEntry(&m->partition, preferred_pt);
		m->partition_type = rules_combo_data(&m->partition);
	}

	m->has_uefi_csm = FALSE;
	if (allowed_target_system[0] && (m->partition_type != PARTITION_STYLE_GPT)) {
		ComboBox_Add(&m->target, TT_BIOS, 0);
		m->has_uefi_csm = TRUE;
	}
	if (allowed_target_system[1] && !((m->partition_type == PARTITION_STYLE_MBR) && (m->boot_type == BT_IMAGE) &&
		IS_BIOS_BOOTABLE(m->img) && IS_EFI_BOOTABLE(m->img)))
		ComboBox_Add(&m->target, TT_UEFI, 0);
	if (allowed_target_system[2] && ((m->partition_type != PARTITION_STYLE_GPT) || (m->boot_type == BT_NON_BOOTABLE)))
		ComboBox_Add(&m->target, TT_BIOS, RULES_ENTRY_DUAL_TARGET);
	ComboBox_SetCurSel(&m->target, 0);
	m->target_type = rules_combo_data(&m->target);
}

// Populate the Allocation unit size field
BOOL SetClusterSizes(rules_state_t* m, int FSType)
{
	int i, default_index = 0;
	uint32_t j;

	ComboBox_ResetContent(&m->cluster);

	if ((FSType < 0) || (FSType >= FS_MAX))
		return FALSE;

	if ((m->ClusterSize[FSType].Allowed == 0) || (m->ClusterSize[FSType].Default == 0))
		return FALSE;

	for (i = 0, j = 0x100; j < 0x10000000; j <<= 1) {
		if (j & m->ClusterSize[FSType].Allowed) {
			if (j == m->ClusterSize[FSType].Default) {
				ComboBox_Add(&m->cluster, (int)j, RULES_ENTRY_DEFAULT);
				default_index = i;
			} else {
				ComboBox_Add(&m->cluster, (int)j, 0);
			}
			i++;
		}
	}

	ComboBox_SetCurSel(&m->cluster, default_index);
	return TRUE;
}

// The ReFS conditions of upstream depend on the Windows edition, which the front end evaluates
static BOOL IsRefsAvailable(rules_state_t* m)
{
	return m->refs_available && m->drive.is_fixed;
}

// Populate the File System and Cluster Size dropdowns
BOOL SetFileSystemAndClusterSize(rules_state_t* m, const char* fs_name)
{
	BOOL allowed_filesystem[FS_MAX];
	int fs_index;
	LONGLONG i;
	const LONGLONG DiskSize = (LONGLONG)m->drive.disk_size;
	uint8_t flags;

	ComboBox_ResetContent(&m->fs);
	ComboBox_ResetContent(&m->cluster);
	m->default_fs = FS_UNKNOWN;
	memset(&m->ClusterSize, 0, sizeof(m->ClusterSize));

/*
 * See https://support.microsoft.com/en-gb/help/140365/default-cluster-size-for-ntfs--fat--and-exfat
 * The following are MS's allowed cluster sizes for FAT16 and FAT32:
 *
 * FAT16
 * 31M  :  512 - 4096
 * 63M  : 1024 - 8192
 * 127M : 2048 - 16k
 * 255M : 4096 - 32k
 * 511M : 8192 - 64k
 * 1023M:  16k - 64k
 * 2047M:  32k - 64k
 * 4095M:  64k
 * 4GB+ : N/A
 *
 * FAT32
 * 31M  : N/A
 * 63M  : N/A			(NB unlike MS, we're allowing 512-512 here)
 * 127M :  512 - 1024
 * 255M :  512 - 2048
 * 511M :  512 - 4096
 * 1023M:  512 - 8192
 * 2047M:  512 - 16k
 * 4095M: 1024 - 32k
 * 7GB  : 2048 - 64k
 * 15GB : 4096 - 64k
 * 31GB : 8192 - 64k This is as far as Microsoft's FormatEx goes...
 * 63GB :  16k - 64k ...but we can go higher using fat32format from RidgeCrop.
 * 2TB+ : N/A
 *
 */

	// FAT 16
	if (DiskSize < 4 * GB) {
		m->ClusterSize[FS_FAT16].Allowed = 0x00001E00;
		for (i = 32; i <= 4096; i <<= 1) {			// 8 MB -> 4 GB
			if (DiskSize < i * MB) {
				m->ClusterSize[FS_FAT16].Default = 16 * (ULONG)i;
				break;
			}
			m->ClusterSize[FS_FAT16].Allowed <<= 1;
		}
		m->ClusterSize[FS_FAT16].Allowed &= 0x0001FE00;
	}

	// FAT 32
	// > 32GB FAT32 is not supported by MS and FormatEx but is achieved using fat32format
	// See: http://ridgecrop.co.uk/index.htm?fat32format.htm
	// < 32 MB FAT32 is not allowed by FormatEx, so we don't bother
	if ((DiskSize >= 32 * MB) && (DiskSize < MAX_FAT32_SIZE)) {
		m->ClusterSize[FS_FAT32].Allowed = 0x000001F8;
		for (i = 32; i <= (32 * 1024); i <<= 1) {			// 32 MB -> 32 GB
			if (DiskSize*1.0f < i * MB * FAT32_CLUSTER_THRESHOLD) {	// MS
				m->ClusterSize[FS_FAT32].Default = 8*(ULONG)i;
				break;
			}
			m->ClusterSize[FS_FAT32].Allowed <<= 1;
		}
		m->ClusterSize[FS_FAT32].Allowed &= 0x0001FE00;

		// Default cluster sizes in the 256MB to 32 GB range do not follow the rule above
		if ((DiskSize >= 256 * MB) && (DiskSize < 32 * GB)) {
			for (i = 8; i <= 32; i <<= 1) {			// 256 MB -> 32 GB
				if (DiskSize * 1.0f < i * GB * FAT32_CLUSTER_THRESHOLD) {
					m->ClusterSize[FS_FAT32].Default = ((ULONG)i / 2) * KB;
					break;
				}
			}
		}
		// More adjustments for large drives
		if (DiskSize >= 32 * GB) {
			m->ClusterSize[FS_FAT32].Allowed &= 0x0001C000;
			m->ClusterSize[FS_FAT32].Default = 0x00008000;
		}
	}

	if (DiskSize < 256 * TB) {
		// NTFS
		m->ClusterSize[FS_NTFS].Allowed = 0x0001F000;
		for (i = 16; i <= 256; i <<= 1) {			// 7 MB -> 256 TB
			if (DiskSize < i * TB) {
				m->ClusterSize[FS_NTFS].Default = ((ULONG)i / 4) * KB;
				break;
			}
		}

		// exFAT
		m->ClusterSize[FS_EXFAT].Allowed = 0x03FFFE00;
		if (DiskSize < 256 * MB)	// < 256 MB
			m->ClusterSize[FS_EXFAT].Default = 4 * KB;
		else if (DiskSize < 32 * GB)	// < 32 GB
			m->ClusterSize[FS_EXFAT].Default = 32 * KB;
		else
			m->ClusterSize[FS_EXFAT].Default = 128 * KB;

		// UDF
		m->ClusterSize[FS_UDF].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
		m->ClusterSize[FS_UDF].Default = 1;

		// ext2/ext3/ext4
		if (m->advanced_mode_format && (DiskSize >= MIN_EXT_SIZE)) {
			m->ClusterSize[FS_EXT2].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
			m->ClusterSize[FS_EXT2].Default = 1;
			m->ClusterSize[FS_EXT3].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
			m->ClusterSize[FS_EXT3].Default = 1;
		}

		// ReFS (only applicable for a select number of Windows platforms and editions)
		if ((DiskSize >= 512 * MB) && (IsRefsAvailable(m))) {
			if (DiskSize < 16 * TB) {	// < 16 TB
				m->ClusterSize[FS_REFS].Allowed = 64 * KB + 4 * KB;
				m->ClusterSize[FS_REFS].Default = 4 * KB;
			} else {
				m->ClusterSize[FS_REFS].Allowed = 64 * KB;
				m->ClusterSize[FS_REFS].Default = 64 * KB;
			}
		}
	}

	// Only add the filesystems we can service
	SetAllowedFileSystems(m, allowed_filesystem);

	for (fs_index = 0; fs_index < FS_MAX; fs_index++) {
		// Remove all cluster sizes that are below the sector size
		if (m->ClusterSize[fs_index].Allowed != SINGLE_CLUSTERSIZE_DEFAULT) {
			m->ClusterSize[fs_index].Allowed &= ~(m->drive.sector_size - 1);
			if ((m->ClusterSize[fs_index].Default & m->ClusterSize[fs_index].Allowed) == 0)
				// We lost our default => Use rightmost bit to select the new one
				m->ClusterSize[fs_index].Default =
				m->ClusterSize[fs_index].Allowed & (-(LONG)m->ClusterSize[fs_index].Allowed);
		}

		if (m->ClusterSize[fs_index].Allowed != 0) {
			flags = 0;
			// Tell the user if we're going to use Large FAT32 or regular
			if ((fs_index == FS_FAT32) && ((DiskSize > LARGE_FAT32_SIZE) || (m->force_large_fat32)))
				flags |= RULES_ENTRY_LARGE_FAT32;
			if (m->default_fs == FS_UNKNOWN) {
				flags |= RULES_ENTRY_DEFAULT;
				m->default_fs = fs_index;
			}
			if (allowed_filesystem[fs_index])
				ComboBox_Add(&m->fs, fs_index, flags);
		}
	}

	// re-select existing FS if it's one we know
	m->drive_fs_type = FS_UNKNOWN;
	if (safe_strlen(fs_name) != 0) {
		for (m->drive_fs_type = FS_MAX - 1; m->drive_fs_type >= 0; m->drive_fs_type--) {
			if (safe_strcmp(fs_name, FileSystemLabel[m->drive_fs_type]) == 0) {
				break;
			}
		}
	} else {
		// Re-select last user-selected FS
		m->drive_fs_type = m->selected_fs;
	}

	for (i = 0; i < m->fs.count; i++) {
		if (m->fs.entry[i].data == m->drive_fs_type) {
			ComboBox_SetCurSel(&m->fs, (int)i);
			break;
		}
	}

	if (i == m->fs.count) {
		// failed to reselect => pick default
		SetComboEntry(&m->fs, m->default_fs);
	}

	return SetClusterSizes(m, rules_combo_data(&m->fs));
}

static void EnableOldBiosFixes(rules_state_t* m, BOOL enable, BOOL remove_checkboxes)
{
	// The fix for old BIOSes option cannot apply if we aren't targetting BIOS, or are using an image that isn't BIOS bootable
	if ((m->partition_type != PARTITION_STYLE_MBR) || (m->target_type != TT_BIOS) || (m->boot_type == BT_NON_BOOTABLE) ||
		((m->boot_type == BT_IMAGE) && (!IS_BIOS_BOOTABLE(m->img) || IS_DD_ONLY(m->img)))) {
		enable = FALSE;
	}
	ToggleCheckbox(&m->old_bios_fixes, enable, remove_checkboxes);
}

static void EnableUefiValidation(rules_state_t* m, BOOL enable, BOOL remove_checkboxes)
{
	// The UEFI validation bootloader cannot apply if we don't write an ISO, or if the ISO is not UEFI bootable
	// or if it's a Windows To Go installation or if DD or BIOS/CSM only are enforced.
	if ((m->boot_type != BT_IMAGE) || (!IS_EFI_BOOTABLE(m->img)) || IS_DD_ONLY(m->img) ||
		((m->image_options & IMOP_WINTOGO) && (rules_combo_data(&m->image_option) == IMOP_WIN_TO_GO)) ||
		((m->target_type == TT_BIOS) && HAS_WINDOWS(m->img) && (!m->allow_dual_uefi_bios))) {
		enable = FALSE;
	}

	m->uefi_validation.checked = (!enable && remove_checkboxes) ? FALSE : m->validate_md5sum;
	m->uefi_validation.enabled = enable;
}

static void EnableExtendedLabel(rules_state_t* m, BOOL enable, BOOL remove_checkboxes)
{
	if (IS_EXT(m->fs_type) || ((m->boot_type == BT_IMAGE) && IS_DD_ONLY(m->img)))
		enable = FALSE;
	ToggleCheckbox(&m->extended_label, enable, remove_checkboxes);
}

static void EnableQuickFormat(rules_state_t* m, BOOL enable, BOOL remove_checkboxes)
{
	if ((m->boot_type == BT_IMAGE) && IS_DD_ONLY(m->img))
		enable = FALSE;

	// Disable/restore the quick format control depending on large FAT32 or ReFS
	if (((m->fs_type == FS_FAT32) && ((m->drive.disk_size > LARGE_FAT32_SIZE) || (m->force_large_fat32))) ||
		(m->fs_type == FS_REFS)) {
		enable = FALSE;
		// Quick Format is the only option for the above
		remove_checkboxes = FALSE;
		m->quick_format.checked = TRUE;
	}
	ToggleCheckbox(&m->quick_format, enable, remove_checkboxes);
}

// From ui.c, without the slider and text box updates, which the front end derives from the state
void SetPersistenceSize(rules_state_t* m)
{
	int i, proposed_unit_selection = 0;
	LONGLONG base_unit = MB;
	uint64_t max = 0, pos = 0;

	if (m->has_device) {
		max = m->drive.disk_size - PERCENTAGE(PROJECTED_SIZE_RATIO, m->img.projected_size);
		m->persistence_size = min(m->persistence_size, max);
		pos = m->persistence_size;

		// Reset the the Persistence Units dropdown
		ComboBox_ResetContent(&m->persistence_units);
		for (i = 0; i < 3; i++) {
			ComboBox_Add(&m->persistence_units, i, 0);
			// If we have more than 7 discrete positions, set this unit as our base
			if ((LONGLONG)m->drive.disk_size > 7 * base_unit)
				proposed_unit_selection = i;
			base_unit *= 1024;
			// Don't allow a base unit unless the drive is at least twice the size of that unit
			if ((LONGLONG)m->drive.disk_size < 2 * base_unit)
				break;
		}
		if (m->persistence_unit_selection < 0)
			m->persistence_unit_selection = proposed_unit_selection;

		ComboBox_SetCurSel(&m->persistence_units, m->persistence_unit_selection);

		if ((pos != 0) && (pos < MIN_EXT_SIZE))
			pos = MIN_EXT_SIZE;
		pos /= MB;
		max /= MB;
		for (i = 0; i < m->persistence_unit_selection; i++) {
			pos /= 1024;
			max /= 1024;
		}
	}

	m->persistence_max = max;
	m->persistence_pos = pos;
	// SetPersistencePos() => TogglePersistenceControls()
	m->persistence_units_visible = (m->boot_type == BT_IMAGE) && (pos != 0);
	m->persistence_size_enabled = m->persistence_units_visible;
	m->persistence_units_enabled = m->persistence_units_visible;
}

static void EnableBootOptions(rules_state_t* m, BOOL enable, BOOL remove_checkboxes)
{
	BOOL actual_enable_bb, actual_enable = enable;

	// If no device is selected, don't enable anything and also don't remove the checkboxes
	if (!m->has_device) {
		actual_enable = FALSE;
		remove_checkboxes = FALSE;
	}
	// If boot selection is set to image, but no image is currently selected, don't enable anything
	if ((m->boot_type == BT_IMAGE) && !m->has_image)
		actual_enable = FALSE;
	actual_enable_bb = actual_enable;
	// If we are dealing with a pure DD image, remove all options except Bad Blocks check
	if ((m->boot_type == BT_IMAGE) && IS_DD_BOOTABLE(m->img) && (!m->img.is_iso))
		actual_enable = FALSE;

	m->image_option_enabled = actual_enable;
	m->persistence_slider_enabled = actual_enable;
	// Make sure we set the range if we have persistence
	if (m->has_image && HAS_PERSISTENCE(m->img))
		SetPersistenceSize(m);
	m->persistence_size_enabled = (m->persistence_size != 0) && actual_enable;
	m->persistence_units_enabled = (m->persistence_size != 0) && actual_enable;
	EnableOldBiosFixes(m, actual_enable, remove_checkboxes);
	EnableUefiValidation(m, actual_enable, remove_checkboxes);

	m->label_enabled = actual_enable;
	if (m->boot_type == BT_IMAGE) {
		if (IS_DD_ONLY(m->img))
			remove_checkboxes = TRUE;
		else if (!m->has_image)
			remove_checkboxes = FALSE;
	}
	EnableQuickFormat(m, actual_enable, remove_checkboxes);
	EnableExtendedLabel(m, actual_enable, remove_checkboxes);
	m->bad_blocks.enabled = actual_enable_bb;
}

// Toggle controls according to operation
void EnableControls(rules_state_t* m, BOOL enable, BOOL remove_checkboxes)
{
	m->op_in_progress = !enable;

	// The following only get disabled on format/hash and otherwise remain enabled,
	// even if no device or image are selected
	m->device_enabled = enable;
	m->boot_enabled = enable;

	// Hash button is enabled if an image has been selected
	m->hash_enabled = enable && (m->boot_type == BT_IMAGE) && m->has_image;

	// Only enable the following controls if a device is active
	enable = !m->has_device ? FALSE : enable;
	m->image_option_enabled = enable;
	m->save_enabled = enable;

	// Enable or disable the Start button and the other boot options
	enable = ((m->boot_type == BT_IMAGE) && !m->has_image) ? FALSE : enable;
	m->start_enabled = enable;
	EnableBootOptions(m, enable, remove_checkboxes);

	// Finally, only enable the half-size dropdowns if we aren't dealing with a pure DD image
	enable = ((m->boot_type == BT_IMAGE) && m->has_image &&
		(!(m->img.is_iso || m->img.is_windows_img))) ? FALSE : enable;
	m->partition_enabled = enable;
	m->target_enabled = enable;
	m->fs_enabled = enable;
	m->cluster_enabled = enable;
}

void SetProposedLabel(rules_state_t* m, int ComboIndex)
{
	const char no_label[] = STR_NO_LABEL, empty[] = "";

	// If the user manually changed the label, try to preserve it
	if (m->user_changed_label)
		return;

	// If bootable ISO creation is selected, and we have an ISO selected with a valid name, use that
	// Also some distros (eg. Arch) require the USB to have the same label as the ISO
	if ((m->boot_type == BT_IMAGE) && m->has_image && (m->img.label[0] != 0)) {
		static_strcpy(m->label, m->img.label);
		return;
	}

	// Empty the label if no device is currently selected
	if (ComboIndex < 0) {
		m->label[0] = 0;
		return;
	}

	// Else if no existing label is available, propose one according to the size (eg: "256MB", "8GB")
	// NB: Upstream compares the "NO_LABEL" constant with the empty string here, which is always false
	if ((_stricmp(no_label, m->drive.label) == 0) || (_stricmp(no_label, empty) == 0)
		|| (safe_stricmp(m->no_label_msg, m->drive.label) == 0)) {
		static_strcpy(m->label, m->proposed_label);
	} else {
		static_strcpy(m->label, m->drive.label);
	}
}

/* The handler for IDC_FILE_SYSTEM. 'user' is FALSE for CBN_SELCHANGE_INTERNAL. */
static void OnFileSystemChange(rules_state_t* m, BOOL user)
{
	if (!m->fs_enabled)
		return;
	m->fs_type = rules_combo_data(&m->fs);
	SetClusterSizes(m, m->fs_type);
	// NB: Upstream never records FAT16 (0) as the user-selected FS here
	if (user && (m->fs_type > 0))
		m->selected_fs = m->fs_type;
	// Some FS's (such as ReFS or Large FAT32) only have QuickFormat so make sure we reflect that
	EnableQuickFormat(m, TRUE, TRUE);
	EnableOldBiosFixes(m, TRUE, TRUE);
	EnableExtendedLabel(m, TRUE, TRUE);
}

void SetFSFromISO(rules_state_t* m)
{
	int i, fs_tmp, preferred_fs = FS_UNKNOWN;
	// NB: Upstream mixes up FS indexes and FS bit masks here, so these two initial
	// values actually flag FAT32 (always) and FAT16 (when there is no 4 GB file).
	uint32_t fs_mask = FS_NTFS | (m->img.has_4GB_file ? 0 : FS_FAT32);
	BOOL windows_to_go = (m->image_options & IMOP_WINTOGO) && (m->boot_type == BT_IMAGE) &&
		HAS_WINTOGO(m->img) && (rules_combo_data(&m->image_option) == IMOP_WIN_TO_GO);

	if (!m->has_image)
		return;

	// Create a mask of all the FS's available
	for (i = 0; i < m->fs.count; i++) {
		fs_tmp = m->fs.entry[i].data;
		fs_mask |= 1 << fs_tmp;
	}

	// If the FS requested from the command line is valid use it
	if ((m->preselected_fs != FS_UNKNOWN) && (fs_mask & (1 << m->preselected_fs))) {
		preferred_fs = m->preselected_fs;
	} else {
		// Syslinux and EFI have precedence over bootmgr (unless the user selected BIOS as target type)
		if ((HAS_SYSLINUX(m->img)) || (HAS_REACTOS(m->img)) || HAS_KOLIBRIOS(m->img) ||
			(IS_EFI_BOOTABLE(m->img) && (m->target_type == TT_UEFI) && (!windows_to_go) && (!m->img.has_4GB_file))) {
			if (fs_mask & (1 << FS_FAT32))
				preferred_fs = FS_FAT32;
			else if ((fs_mask & (1 << FS_FAT16)) && !HAS_KOLIBRIOS(m->img))
				preferred_fs = FS_FAT16;
		} else if ((windows_to_go) || HAS_BOOTMGR(m->img) || HAS_WINPE(m->img)) {
			if ((fs_mask & (1 << FS_FAT32)) && (!m->img.has_4GB_file) && (m->allow_dual_uefi_bios))
				preferred_fs = FS_FAT32;
			else if (fs_mask & (1 << FS_NTFS))
				preferred_fs = FS_NTFS;
		}
	}

	// Try to select the FS
	for (i = 0; i < m->fs.count; i++) {
		fs_tmp = m->fs.entry[i].data;
		if (fs_tmp == preferred_fs) {
			ComboBox_SetCurSel(&m->fs, i);
			break;
		}
	}
	if (m->selected_fs == FS_UNKNOWN)
		m->selected_fs = preferred_fs;

	OnFileSystemChange(m, FALSE);
}

// Toggle the Image Option dropdown (Windows To Go or persistence settings)
void ToggleImageOptions(rules_state_t* m)
{
	BOOL has_wintogo, has_persistence;

	has_wintogo = ((m->boot_type == BT_IMAGE) && m->has_image && (m->img.is_iso || m->img.is_windows_img) &&
		(m->wintogo_available) && (HAS_WINTOGO(m->img)));
	has_persistence = ((m->boot_type == BT_IMAGE) && m->has_image && (m->img.is_iso) && (HAS_PERSISTENCE(m->img)));

	if (((has_wintogo) && !(m->image_options & IMOP_WINTOGO)) ||
		((!has_wintogo) && (m->image_options & IMOP_WINTOGO))) {
		m->image_options ^= IMOP_WINTOGO;
		if (m->image_options & IMOP_WINTOGO) {
			// Set the Windows To Go selection in the dropdown
			ComboBox_SetCurSel(&m->image_option, m->imop_win_sel);
		}
	}

	if (((has_persistence) && !(m->image_options & IMOP_PERSISTENCE)) ||
		((!has_persistence) && (m->image_options & IMOP_PERSISTENCE))) {
		m->image_options ^= IMOP_PERSISTENCE;
		if (m->image_options & IMOP_PERSISTENCE) {
			// TogglePersistenceControls()
			m->persistence_units_visible = (m->persistence_size != 0);
			m->persistence_size_enabled = m->persistence_units_visible;
			m->persistence_units_enabled = m->persistence_units_visible;
			SetPersistenceSize(m);
		}
	}

	// Hide or show the boot options (the other ones follow image_options directly)
	m->persistence_units_visible = (m->image_options & IMOP_PERSISTENCE) ? TRUE : FALSE;
}

// Populate the UI main dropdown properties.
// This should be called on device or boot type change.
static BOOL PopulateProperties(rules_state_t* m, const rules_drive_t* drive)
{
	// NB: 'drive' may point to m->drive
	rules_drive_t selected = { 0 };

	if (drive != NULL)
		selected = *drive;
	// memset(&SelectedDrive, 0, sizeof(SelectedDrive))
	memset(&m->drive, 0, sizeof(m->drive));
	m->drive_fs_type = 0;
	m->proposed_label[0] = 0;
	memset(&m->ClusterSize, 0, sizeof(m->ClusterSize));
	m->has_device = (drive != NULL);
	m->start_enabled = FALSE;

	if (!m->has_device)
		goto out;

	m->persistence_unit_selection = -1;
	// This fills the SelectedDrive properties
	m->drive = selected;
	SetPartitionSchemeAndTargetSystem(m, FALSE);
	// Attempt to reselect the last file system explicitly set by the user
	if (!SetFileSystemAndClusterSize(m, (m->selected_fs == FS_UNKNOWN) ? m->drive.fs_name : NULL)) {
		SetProposedLabel(m, -1);
		uprintf("No file system is selectable for this drive\n");
		return FALSE;
	}

	EnableControls(m, TRUE, FALSE);

	// Set a proposed label according to the size (eg: "256MB", "8GB")
	static_strcpy(m->proposed_label, m->drive.proposed_label);

out:
	SetProposedLabel(m, m->has_device ? 0 : -1);
	return TRUE;
}

// Insert the image name into the Boot selection dropdown and (re)populate the Image option dropdown
static void UpdateImage(rules_state_t* m, BOOL update_image_option_only)
{
	if_assert_fails(m->image_index != 0)
		return;

	if (!update_image_option_only) {
		if (((m->image_index < m->boot.count) ? m->boot.entry[m->image_index].data : -1) == BT_IMAGE)
			ComboBox_Delete(&m->boot, m->image_index);
		ComboBox_Insert(&m->boot, m->image_index, BT_IMAGE, m->has_image ? 0 : RULES_ENTRY_NO_IMAGE);
		ComboBox_SetCurSel(&m->boot, m->image_index);
		m->boot_type = rules_combo_data(&m->boot);
	}

	ComboBox_ResetContent(&m->image_option);

	if (!m->img.is_windows_img)	// Straight install.wim/install.esd only have Windows To Go option
		ComboBox_Add(&m->image_option, IMOP_WIN_STANDARD, 0);
	ComboBox_Add(&m->image_option, IMOP_WIN_TO_GO, 0);
	ComboBox_SetCurSel(&m->image_option, m->imop_win_sel);
}

void rules_on_startup(rules_state_t* m, const rules_drive_t* drive)
{
	// InitDialog()
	SetBootOptions(m);
	m->quick_format.checked = TRUE;
	m->extended_label.checked = TRUE;
	ToggleImageOptions(m);
	// GetDevices(0)
	rules_on_device(m, drive);
	EnableControls(m, TRUE, FALSE);
}

void rules_on_device(rules_state_t* m, const rules_drive_t* drive)
{
	PopulateProperties(m, drive);
	OnFileSystemChange(m, FALSE);
}

void rules_on_device_refresh(rules_state_t* m, const rules_drive_t* drive)
{
	rules_on_device(m, drive);
	EnableControls(m, TRUE, FALSE);
	if (!m->has_device) {
		SetPartitionSchemeAndTargetSystem(m, FALSE);
		SetFileSystemAndClusterSize(m, NULL);
		m->persistence_unit_selection = -1;
	}
}

void rules_on_boot_selection(rules_state_t* m, int index)
{
	const char* fs_name = "";

	ComboBox_SetCurSel(&m->boot, index);
	m->boot_type = rules_combo_data(&m->boot);
	if (m->boot_type == m->selection_default)
		return;
	m->selection_default = m->boot_type;
	SetPartitionSchemeAndTargetSystem(m, FALSE);
	// Try to reselect current FS from the drive for non-bootable
	if ((m->selected_fs == FS_UNKNOWN) && m->has_device)
		fs_name = m->drive.fs_name;
	SetFileSystemAndClusterSize(m, fs_name);
	ToggleImageOptions(m);
	SetProposedLabel(m, m->has_device ? 0 : -1);
	EnableControls(m, TRUE, TRUE);
	// NB: Upstream swaps the partition scheme and the target system here
	m->target_type = rules_combo_data(&m->partition);
	m->partition_type = rules_combo_data(&m->target);
}

void rules_on_image_option(rules_state_t* m, int index)
{
	ComboBox_SetCurSel(&m->image_option, index);
	SetFileSystemAndClusterSize(m, NULL);
	m->imop_win_sel = m->image_option.sel;
	EnableUefiValidation(m, (m->imop_win_sel == 0), TRUE);
}

void rules_on_partition_scheme(rules_state_t* m, int index)
{
	ComboBox_SetCurSel(&m->partition, index);
	m->partition_type = rules_combo_data(&m->partition);
	SetPartitionSchemeAndTargetSystem(m, TRUE);
	SetFileSystemAndClusterSize(m, NULL);
	EnableOldBiosFixes(m, TRUE, TRUE);
	EnableUefiValidation(m, TRUE, TRUE);
	m->selected_pt = m->partition_type;
}

void rules_on_target_system(rules_state_t* m, int index)
{
	ComboBox_SetCurSel(&m->target, index);
	m->target_type = rules_combo_data(&m->target);
	SetFileSystemAndClusterSize(m, NULL);
	EnableUefiValidation(m, TRUE, TRUE);
}

void rules_on_file_system(rules_state_t* m, int index)
{
	ComboBox_SetCurSel(&m->fs, index);
	OnFileSystemChange(m, TRUE);
}

void rules_on_cluster_size(rules_state_t* m, int index)
{
	ComboBox_SetCurSel(&m->cluster, index);
}

void rules_on_nb_passes(rules_state_t* m, int index)
{
	// The 5 entries are 1 pass, 2 passes, then the SLC, MLC and TLC patterns (4 passes)
	if ((index >= 0) && (index < BADLOCKS_PATTERN_TYPES))
		m->nb_passes_sel = index;
}

void rules_on_label(rules_state_t* m, const char* label)
{
	static_strcpy(m->label, label);
	m->user_changed_label = TRUE;
}

void rules_on_check(rules_state_t* m, rules_check_t* check)
{
	if (!check->enabled)
		return;
	check->checked = !check->checked;
	if (check == &m->uefi_validation)
		m->validate_md5sum = check->checked;
}

void rules_on_toggle_advanced_device(rules_state_t* m)
{
	m->advanced_mode_device = !m->advanced_mode_device;
	SetBootOptions(m);
	m->boot_type = rules_combo_data(&m->boot);
	EnableControls(m, TRUE, FALSE);
	SetFileSystemAndClusterSize(m, NULL);
	OnFileSystemChange(m, FALSE);
}

void rules_on_toggle_advanced_format(rules_state_t* m)
{
	m->advanced_mode_format = !m->advanced_mode_format;
	if (m->selected_fs == FS_UNKNOWN)
		m->selected_fs = rules_combo_data(&m->fs);
	SetFileSystemAndClusterSize(m, NULL);
	OnFileSystemChange(m, FALSE);
}

void rules_on_toggle_dual_uefi_bios(rules_state_t* m)
{
	m->allow_dual_uefi_bios = !m->allow_dual_uefi_bios;
	SetPartitionSchemeAndTargetSystem(m, FALSE);
	SetFileSystemAndClusterSize(m, NULL);
}

void rules_on_image_scan_start(rules_state_t* m)
{
	// IDC_SELECT sets image_path before starting the scan thread
	m->has_image = TRUE;
	EnableControls(m, FALSE, FALSE);
	memset(&m->img, 0, sizeof(m->img));
	ComboBox_ResetContent(&m->image_option);
	m->imop_win_sel = 0;
}

void rules_on_image_scan_end(rules_state_t* m, const RUFUS_IMG_REPORT* img, enum rules_scan_result result,
	BOOL keep_image_name)
{
	m->img = *img;
	if (result == RULES_SCAN_FAILED) {
		// Make sure to relinquish image_path before we call UpdateImage
		// otherwise the boot selection dropdown won't be properly reset.
		m->has_image = FALSE;
		UpdateImage(m, FALSE);
		PopulateProperties(m, m->has_device ? &m->drive : NULL);
		EnableControls(m, TRUE, FALSE);
		return;
	}

	if (m->img.is_windows_img || IS_DD_BOOTABLE(m->img) || m->img.is_iso)
		m->selection_default = BT_IMAGE;

	if (result == RULES_SCAN_UNSUPPORTED) {
		// No boot method that we support
		m->has_image = FALSE;
		EnableControls(m, TRUE, FALSE);
		return;
	}

	UpdateImage(m, keep_image_name);
	ToggleImageOptions(m);
	// Set Target and FS accordingly
	if (m->img.is_iso || m->img.is_windows_img) {
		ComboBox_SetCurSel(&m->boot, m->image_index);
		SetPartitionSchemeAndTargetSystem(m, FALSE);
		SetFileSystemAndClusterSize(m, NULL);
		SetFSFromISO(m);
		m->user_changed_label = FALSE;
		SetProposedLabel(m, m->has_device ? 0 : -1);
	} else {
		OnFileSystemChange(m, FALSE);
	}
	EnableControls(m, TRUE, FALSE);
}

void rules_on_start(rules_state_t* m)
{
	// Just in case
	m->boot_type = rules_combo_data(&m->boot);
	m->partition_type = rules_combo_data(&m->partition);
	m->target_type = rules_combo_data(&m->target);
	m->fs_type = rules_combo_data(&m->fs);
	// Disable all controls except Cancel
	EnableControls(m, FALSE, FALSE);
	m->selection_default = rules_combo_data(&m->boot);
}

void rules_on_operation_end(rules_state_t* m)
{
	EnableControls(m, TRUE, FALSE);
}

void rules_get_options(const rules_state_t* m, rufus_options_t* o)
{
	// IDC_START
	o->boot_type = rules_combo_data(&m->boot);
	o->partition_type = rules_combo_data(&m->partition);
	o->target_type = rules_combo_data(&m->target);
	o->fs_type = rules_combo_data(&m->fs);
	o->write_as_image = FALSE;
	o->write_as_esp = FALSE;
	o->unattend_xml_flags = 0;
	o->append_silent = FALSE;
	// The controls that the format thread reads
	o->disk_size = m->drive.disk_size;
	o->cluster_size = (uint32_t)rules_combo_data(&m->cluster);
	static_strcpy(o->label, m->label);
	o->quick_format = m->quick_format.checked;
	o->extended_label = m->extended_label.checked;
	o->old_bios_fixes = m->old_bios_fixes.checked;
	o->bad_blocks = m->bad_blocks.checked;
	o->nb_passes_sel = m->nb_passes_sel;
	o->image_option = rules_combo_data(&m->image_option);
	o->image_options = m->image_options;
	// The globals that the window modifies
	o->persistence_size = m->persistence_size;
	o->validate_md5sum = m->validate_md5sum;
	o->allow_dual_uefi_bios = m->allow_dual_uefi_bios;
	o->force_large_fat32 = m->force_large_fat32;
}

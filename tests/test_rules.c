/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the main window option rules
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
 * Two kinds of tests:
 * - Scenarios, which check the options offered for well known kinds of
 *   images against the behaviour of the Windows version.
 * - A differential test, which replays random sequences of user actions on
 *   core/rules.c and on the original Windows functions (rules_oracle/), and
 *   compares what the main window would display after every step.
 *   RUFUS_RULES_SEED and RUFUS_RULES_RUNS override the defaults.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rufus.h"
#include "resource.h"
#include "rules.h"
#include "ui_iface.h"
#include "rules_oracle/oracle.h"
#include "../res/grub/grub_version.h"
#include "../res/grub2/grub2_version.h"
#include "test.h"

extern const char* FileSystemLabel[FS_MAX];

/*
 * Scenarios
 */

static const rules_drive_t usb_16gb = {
	.disk_size = 16 * GB - 128 * MB, .sector_size = 512, .partition_style = PARTITION_STYLE_MBR,
	.fs_name = "FAT32", .label = "MYSTICK", .proposed_label = "16 GB"
};

static void scenario_setup(rules_state_t* m, const rules_drive_t* drive)
{
	rules_init(m);
	m->wintogo_available = FALSE;
	static_strcpy(m->no_label_msg, "New Volume");
	rules_on_startup(m, drive);
}

static void scenario_image(rules_state_t* m, const RUFUS_IMG_REPORT* img)
{
	rules_on_image_scan_start(m);
	rules_on_image_scan_end(m, img, RULES_SCAN_OK, FALSE);
}

static BOOL combo_has(const rules_combo_t* c, int data, uint8_t flags)
{
	int i;

	for (i = 0; i < c->count; i++) {
		if ((c->entry[i].data == data) && ((c->entry[i].flags & flags) == flags))
			return TRUE;
	}
	return FALSE;
}

static void test_scenarios(void)
{
	rules_state_t m;
	RUFUS_IMG_REPORT img;

	/* No image: non bootable, every FS, SFD allowed */
	scenario_setup(&m, &usb_16gb);
	CHECK_EQ_INT(rules_combo_data(&m.boot), BT_IMAGE);
	CHECK(!m.start_enabled);
	rules_on_boot_selection(&m, 0);
	CHECK_EQ_INT(rules_combo_data(&m.boot), BT_NON_BOOTABLE);
	CHECK_EQ_INT(m.partition.count, 3);
	CHECK(combo_has(&m.partition, PARTITION_STYLE_SFD, 0));
	CHECK_EQ_INT(m.target.count, 1);
	CHECK(combo_has(&m.target, TT_BIOS, RULES_ENTRY_DUAL_TARGET));
	CHECK_EQ_INT(rules_combo_data(&m.fs), FS_FAT32);	// Reselected from the drive
	CHECK(combo_has(&m.fs, FS_FAT32, RULES_ENTRY_DEFAULT));
	CHECK(combo_has(&m.fs, FS_UDF, 0));
	CHECK(!combo_has(&m.fs, FS_EXT2, 0));	// Advanced format options are off
	CHECK_EQ_INT(rules_combo_data(&m.cluster), 8 * KB);	// FAT32 default for 16 GB
	CHECK_EQ_STR(m.label, "MYSTICK");
	CHECK(m.start_enabled);
	CHECK(m.quick_format.checked && m.quick_format.enabled);

	/* Windows 11 ISO: GPT/UEFI and NTFS (it has a >4 GB install.wim) */
	memset(&img, 0, sizeof(img));
	img.is_iso = TRUE;
	img.has_bootmgr = img.has_bootmgr_efi = TRUE;
	img.has_efi = 1 << ARCH_X86_64;
	img.wininst_index = 1;
	img.has_4GB_file = 1;
	img.win_version.major = 11;
	img.image_size = img.projected_size = 5 * GB;
	static_strcpy(img.label, "CCCOMA_X64FRE_EN-US_DV9");
	scenario_setup(&m, &usb_16gb);
	scenario_image(&m, &img);
	CHECK_EQ_INT(rules_combo_data(&m.boot), BT_IMAGE);
	CHECK_EQ_INT(rules_combo_data(&m.partition), PARTITION_STYLE_GPT);
	CHECK_EQ_INT(rules_combo_data(&m.target), TT_UEFI);
	CHECK_EQ_INT(m.target.count, 1);
	CHECK_EQ_INT(rules_combo_data(&m.fs), FS_NTFS);
	CHECK(!combo_has(&m.fs, FS_FAT32, 0));
	CHECK_EQ_STR(m.label, "CCCOMA_X64FRE_EN-US_DV9");
	CHECK(m.image_option_enabled);
	CHECK(!(m.image_options & IMOP_WINTOGO));	// Not available on this host
	CHECK(m.uefi_validation.enabled);
	CHECK(!m.old_bios_fixes.enabled);
	// MBR is BIOS only for Windows, unless dual UEFI/BIOS is enabled (Alt-E)
	rules_on_partition_scheme(&m, 0);
	CHECK_EQ_INT(rules_combo_data(&m.partition), PARTITION_STYLE_MBR);
	CHECK_EQ_INT(rules_combo_data(&m.target), TT_BIOS);
	CHECK(!(m.target.entry[0].flags & RULES_ENTRY_DUAL_TARGET));
	CHECK(m.old_bios_fixes.enabled);
	CHECK(!m.uefi_validation.enabled);
	rules_on_toggle_dual_uefi_bios(&m);
	CHECK_EQ_INT(rules_combo_data(&m.partition), PARTITION_STYLE_MBR);
	CHECK(combo_has(&m.target, TT_BIOS, RULES_ENTRY_DUAL_TARGET));

	/* Ubuntu-like ISOHybrid (GRUB 2, BIOS + UEFI): MBR, BIOS or UEFI, FAT32, persistence */
	memset(&img, 0, sizeof(img));
	img.is_iso = TRUE;
	img.is_bootable_img = 1;
	img.has_efi = 1 << ARCH_X86_64;
	img.has_grub2 = 1;
	img.uses_casper = TRUE;
	img.image_size = img.projected_size = 5 * GB;
	static_strcpy(img.label, "Ubuntu 24.04 LTS amd64");
	scenario_setup(&m, &usb_16gb);
	scenario_image(&m, &img);
	CHECK_EQ_INT(rules_combo_data(&m.partition), PARTITION_STYLE_MBR);
	CHECK_EQ_INT(m.target.count, 1);
	CHECK(combo_has(&m.target, TT_BIOS, RULES_ENTRY_DUAL_TARGET));
	CHECK_EQ_INT(rules_combo_data(&m.fs), FS_FAT32);
	CHECK(m.image_options & IMOP_PERSISTENCE);
	CHECK(m.persistence_slider_enabled);
	CHECK_EQ_INT(m.persistence_units.count, 2);	// MB and GB
	CHECK_EQ_INT(m.persistence_unit_selection, 1);
	CHECK_EQ_INT(m.persistence_pos, 0);
	CHECK_EQ_INT(m.persistence_max, (16 * GB - 128 * MB - PERCENTAGE(PROJECTED_SIZE_RATIO, 5 * GB)) / GB);
	CHECK(!m.persistence_units_visible);	// No persistence until the slider moves
	CHECK(m.old_bios_fixes.enabled);
	// GPT: UEFI only
	rules_on_partition_scheme(&m, 1);
	CHECK_EQ_INT(rules_combo_data(&m.partition), PARTITION_STYLE_GPT);
	CHECK_EQ_INT(rules_combo_data(&m.target), TT_UEFI);
	CHECK(!m.old_bios_fixes.enabled);

	/* Raw disk image: everything but bad blocks is disabled */
	memset(&img, 0, sizeof(img));
	img.is_bootable_img = 1;
	img.image_size = img.projected_size = 1 * GB;
	scenario_setup(&m, &usb_16gb);
	scenario_image(&m, &img);
	CHECK_EQ_INT(rules_combo_data(&m.boot), BT_IMAGE);
	CHECK(!m.partition_enabled && !m.target_enabled && !m.fs_enabled && !m.cluster_enabled);
	CHECK(!m.quick_format.enabled && !m.quick_format.checked);
	CHECK(!m.label_enabled);
	CHECK(m.bad_blocks_enabled);
	CHECK(m.start_enabled);

	/* FreeDOS: FAT only, MBR/BIOS only, and the FAT16 default for a small drive */
	rules_drive_t small = usb_16gb;
	small.disk_size = 200 * MB;
	small.fs_name[0] = 0;
	static_strcpy(small.label, STR_NO_LABEL);
	static_strcpy(small.proposed_label, "200 MB");
	scenario_setup(&m, &small);
	rules_on_boot_selection(&m, 3);
	CHECK_EQ_INT(rules_combo_data(&m.boot), BT_FREEDOS);
	CHECK_EQ_INT(m.partition.count, 1);
	CHECK_EQ_INT(rules_combo_data(&m.partition), PARTITION_STYLE_MBR);
	CHECK_EQ_INT(m.target.count, 1);
	CHECK_EQ_INT(rules_combo_data(&m.target), TT_BIOS);
	CHECK_EQ_INT(m.fs.count, 2);
	CHECK(combo_has(&m.fs, FS_FAT16, RULES_ENTRY_DEFAULT));
	CHECK_EQ_INT(rules_combo_data(&m.fs), FS_FAT16);
	CHECK_EQ_STR(m.label, "200 MB");

	/* Large drives: GPT is forced above 2 TB, FAT32 is "Large FAT32" above 32 GB and gone above 2 TB */
	rules_drive_t big = usb_16gb;
	big.disk_size = 64 * GB;
	scenario_setup(&m, &big);
	rules_on_boot_selection(&m, 0);
	CHECK(combo_has(&m.fs, FS_FAT32, RULES_ENTRY_LARGE_FAT32));
	rules_on_file_system(&m, 0);
	CHECK_EQ_INT(rules_combo_data(&m.fs), FS_FAT32);
	CHECK(!m.quick_format.enabled && m.quick_format.checked);
	big.disk_size = 3 * TB;
	scenario_setup(&m, &big);
	rules_on_boot_selection(&m, 0);
	CHECK_EQ_INT(rules_combo_data(&m.partition), PARTITION_STYLE_GPT);
	CHECK(!combo_has(&m.fs, FS_FAT32, 0));
	CHECK_EQ_INT(rules_combo_data(&m.fs), FS_NTFS);

	/* No device: nothing to choose */
	scenario_setup(&m, NULL);
	CHECK_EQ_INT(m.partition.count, 0);
	CHECK_EQ_INT(m.fs.count, 0);
	CHECK(!m.start_enabled);
	CHECK_EQ_STR(m.label, "");
}

/*
 * Differential test against the Windows code
 */

static uint64_t rng_state;

static uint32_t rnd(uint32_t n)
{
	// xorshift64*
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (uint32_t)(((rng_state * 0x2545F4914F6CDD1DULL) >> 32) % n);
}

#define PICK(arr) (arr[rnd(ARRAYSIZE(arr))])

// NB: static_strcpy() evaluates its source twice
static void pick_str(char* dst, size_t size, const char* const* arr, size_t count)
{
	const char* src = arr[rnd((uint32_t)count)];
	safe_strcpy(dst, size, src);
}
#define PICK_STR(dst, arr) pick_str(dst, sizeof(dst), arr, ARRAYSIZE(arr))

static void random_drive(rules_drive_t* d)
{
	static const uint64_t sizes[] = {
		7 * MB, 8 * MB, 31 * MB, 32 * MB, 33 * MB, 63 * MB, 64 * MB, 127 * MB, 255 * MB, 256 * MB, 300 * MB,
		511 * MB, 1 * GB, 2 * GB - 1, 4 * GB - 4 * MB, 4 * GB, 7 * GB + 900 * MB, 8 * GB, 15 * GB, 16 * GB,
		32 * GB - 64 * MB, 32 * GB, 32 * GB + 300 * MB, 64 * GB, 128 * GB, 500 * GB, 1 * TB, 2 * TB,
		2 * TB + 100 * GB, 4 * TB, 16 * TB, 17 * TB, 255 * TB, 300 * TB
	};
	static const uint32_t sector_sizes[] = { 512, 512, 512, 4096 };
	static const int styles[] = { PARTITION_STYLE_MBR, PARTITION_STYLE_GPT, PARTITION_STYLE_RAW };
	static const char* fs_names[] = { "", "FAT32", "NTFS", "exFAT", "FAT", "UDF", "ext2", "ext4", "ReFS", "Unknown" };
	static const char* labels[] = { STR_NO_LABEL, "MSG_207", "MYSTICK", "" };

	memset(d, 0, sizeof(*d));
	d->disk_size = PICK(sizes);
	if (rnd(4) == 0)
		d->disk_size += rnd(1000) * MB;
	d->sector_size = PICK(sector_sizes);
	d->partition_style = PICK(styles);
	d->is_fixed = (rnd(3) == 0);
	PICK_STR(d->fs_name, fs_names);
	PICK_STR(d->label, labels);
	static_sprintf(d->proposed_label, "%uMB", (unsigned)(d->disk_size / MB));
}

static void random_image(RUFUS_IMG_REPORT* r)
{
	static const uint16_t efi[] = { 0, 0, 1, 1 << ARCH_X86_64, (1 << ARCH_X86_64) | (1 << ARCH_ARM_64), 0x80, 0x81 };
	static const uint8_t grub2[] = { 0, 0, 1, 0x80, 0x81 };
	static const uint16_t sl[] = { 0, 0, 0x0403, 0x0604 };
	static const uint8_t has_4GB[] = { 0, 0, 1, 0x11 };
	static const uint16_t winpe[] = { 0, 0, 0, WINPE_AMD64, WINPE_I386, WINPE_MININT };
	static const char* names[] = { "", "ARCH_202410", "Ubuntu 24.04", "CCCOMA_X64FRE", "Fedora-WS-Live-41" };

	memset(r, 0, sizeof(*r));
	r->is_iso = (rnd(4) != 0);
	r->is_bootable_img = (int8_t)(rnd(3) == 0 ? (rnd(4) == 0 ? 2 : 1) : (rnd(10) == 0 ? -1 : 0));
	r->is_windows_img = (rnd(10) == 0);
	r->is_vhd = (rnd(10) == 0);
	r->disable_iso = (rnd(8) == 0);
	r->has_efi = PICK(efi);
	r->has_bootmgr = (rnd(4) == 0);
	r->has_bootmgr_efi = (rnd(4) == 0);
	r->winpe = PICK(winpe);
	r->uses_minint = (rnd(10) == 0);
	r->wininst_index = (uint8_t)rnd(3);
	r->sl_version = PICK(sl);
	r->has_grub2 = PICK(grub2);
	r->has_grub2_fs = (uint8_t)rnd(8);
	r->has_grub4dos = (rnd(8) == 0);
	r->has_kolibrios = (rnd(12) == 0);
	if (rnd(10) == 0)
		static_strcpy(r->reactos_path, "/loader/setupldr.sys");
	r->has_4GB_file = PICK(has_4GB);
	r->needs_ntfs = (rnd(10) == 0);
	r->uses_casper = (rnd(4) == 0);
	PICK_STR(r->label, names);
	r->image_size = (1 + rnd(8000)) * MB;
	r->projected_size = (rnd(10) == 0) ? 0 : r->image_size;
}

static void random_env(oracle_env_t* env)
{
	static const int preselected[] = { FS_UNKNOWN, FS_UNKNOWN, FS_UNKNOWN, FS_FAT32, FS_NTFS, FS_EXFAT };

	env->advanced_mode_device = rnd(2);
	env->advanced_mode_format = rnd(2);
	env->allow_dual_uefi_bios = (rnd(4) == 0);
	env->force_large_fat32 = (rnd(6) == 0);
	env->wintogo_available = rnd(2);
	env->refs_available = (rnd(4) == 0);
	// On Windows, ReFS requires 8.1 or later, which also has Windows To Go
	if (env->refs_available)
		env->wintogo_available = TRUE;
	env->preselected_fs = PICK(preselected);
}

/* Model to snapshot, with the display strings of the mock lmprintf() */

static const char* boot_name(int bt)
{
	switch (bt) {
	case BT_NON_BOOTABLE: return "MSG_279";
	case BT_MSDOS: return "MS-DOS";
	case BT_FREEDOS: return "FreeDOS";
	case BT_SYSLINUX_V4: return "Syslinux " SNAP_SL_V4;
	case BT_SYSLINUX_V6: return "Syslinux " SNAP_SL_V6;
	case BT_REACTOS: return "ReactOS";
	case BT_GRUB2: return "Grub " GRUB2_PACKAGE_VERSION;
	case BT_GRUB4DOS: return "Grub4DOS " GRUB4DOS_VERSION;
	case BT_UEFI_NTFS: return "UEFI:NTFS";
	default: return "?";
	}
}

static void cluster_name(char* buf, size_t size, uint32_t cluster)
{
	static char ClusterSizeLabel[MAX_CLUSTER_SIZES][64];
	unsigned int i, j, msg_id;

	// Same as SetClusterSizeLabels()
	safe_sprintf(ClusterSizeLabel[0], 64, "MSG_029");
	for (i = 512, j = 1, msg_id = MSG_026; j < MAX_CLUSTER_SIZES; i <<= 1, j++) {
		if (i > 8192) {
			i /= 1024;
			msg_id++;
		}
		safe_sprintf(ClusterSizeLabel[j], 64, "%u MSG_%03u", i, msg_id - MSG_000);
	}
	for (i = 0; (i < MAX_CLUSTER_SIZES - 1) && ((0x100U << i) < cluster); i++);
	snprintf(buf, size, "%s", ClusterSizeLabel[i]);
}

static void snap_from_combo(snap_combo_t* s, const rules_combo_t* c, char kind)
{
	char tmp[128];
	int i;

	s->count = c->count;
	s->sel = c->sel;
	for (i = 0; i < c->count; i++) {
		int data = c->entry[i].data;
		uint8_t flags = c->entry[i].flags;
		s->item[i].data = data;
		switch (kind) {
		case 'b':
			if (data == BT_IMAGE)
				static_strcpy(tmp, (flags & RULES_ENTRY_NO_IMAGE) ? "MSG_281[MSG_280]" : SNAP_IMAGE_NAME);
			else
				static_strcpy(tmp, boot_name(data));
			break;
		case 'i':
			static_strcpy(tmp, (data == IMOP_WIN_STANDARD) ? "MSG_117" : "MSG_118");
			break;
		case 'p':
			static_strcpy(tmp, (data == PARTITION_STYLE_MBR) ? "MBR" : ((data == PARTITION_STYLE_GPT) ? "GPT" : "Super Floppy Disk"));
			break;
		case 't':
			static_strcpy(tmp, (data == TT_UEFI) ? "MSG_032" : ((flags & RULES_ENTRY_DUAL_TARGET) ? "MSG_033" : "MSG_031"));
			break;
		case 'f':
			static_sprintf(tmp, "%s%s", (flags & RULES_ENTRY_LARGE_FAT32) ? "Large " : "", FileSystemLabel[data]);
			break;
		case 'c':
			cluster_name(tmp, sizeof(tmp), (uint32_t)data);
			break;
		}
		if (flags & RULES_ENTRY_DEFAULT)
			static_sprintf(s->item[i].text, "MSG_030[%s]", tmp);
		else
			static_strcpy(s->item[i].text, tmp);
	}
}

static void snap_from_check(snap_check_t* s, const rules_check_t* c)
{
	s->enabled = c->enabled;
	s->checked = c->checked;
}

static void snap_from_model(snapshot_t* s, const rules_state_t* m)
{
	memset(s, 0, sizeof(*s));
	snap_from_combo(&s->boot, &m->boot, 'b');
	snap_from_combo(&s->image_option, &m->image_option, 'i');
	snap_from_combo(&s->partition, &m->partition, 'p');
	snap_from_combo(&s->target, &m->target, 't');
	snap_from_combo(&s->fs, &m->fs, 'f');
	snap_from_combo(&s->cluster, &m->cluster, 'c');
	snap_from_check(&s->quick_format, &m->quick_format);
	snap_from_check(&s->extended_label, &m->extended_label);
	snap_from_check(&s->old_bios_fixes, &m->old_bios_fixes);
	snap_from_check(&s->uefi_validation, &m->uefi_validation);
	s->device_enabled = m->device_enabled;
	s->boot_enabled = m->boot_enabled;
	s->hash_enabled = m->hash_enabled;
	s->save_enabled = m->save_enabled;
	s->start_enabled = m->start_enabled;
	s->image_option_enabled = m->image_option_enabled;
	s->persistence_slider_enabled = m->persistence_slider_enabled;
	s->persistence_size_enabled = m->persistence_size_enabled;
	s->persistence_units_enabled = m->persistence_units_enabled;
	s->persistence_units_visible = m->persistence_units_visible;
	s->label_enabled = m->label_enabled;
	s->bad_blocks_enabled = m->bad_blocks_enabled;
	s->partition_enabled = m->partition_enabled;
	s->target_enabled = m->target_enabled;
	s->fs_enabled = m->fs_enabled;
	s->cluster_enabled = m->cluster_enabled;
	static_strcpy(s->label, m->label);
	s->persistence_units = m->persistence_units.count;
	s->persistence_unit_sel = m->persistence_units.sel;
	s->persistence_max = m->persistence_max;
	s->persistence_pos = m->persistence_pos;
	s->persistence_size = m->persistence_size;
	s->g_selection_default = m->selection_default;
	s->g_boot_type = m->boot_type;
	s->g_partition_type = m->partition_type;
	s->g_target_type = m->target_type;
	s->g_fs_type = m->fs_type;
	s->g_selected_pt = m->selected_pt;
	s->g_selected_fs = m->selected_fs;
	s->g_imop_win_sel = m->imop_win_sel;
	s->g_image_options = m->image_options;
	s->g_has_uefi_csm = m->has_uefi_csm;
	s->g_op_in_progress = m->op_in_progress;
	s->g_validate_md5sum = m->validate_md5sum;
	s->g_user_changed_label = m->user_changed_label;
}

/* Comparison */

static char history[64][128];
static int history_len;

static void log_event(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void log_event(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(history[history_len++ % ARRAYSIZE(history)], sizeof(history[0]), format, args);
	va_end(args);
}

static void dump_history(void)
{
	int i, start = (history_len > (int)ARRAYSIZE(history)) ? history_len - (int)ARRAYSIZE(history) : 0;

	fprintf(stderr, "Events:\n");
	for (i = start; i < history_len; i++)
		fprintf(stderr, "  %3d: %s\n", i, history[i % ARRAYSIZE(history)]);
}

static int mismatches;

#define CMP_INT(field) do { \
	if ((long long)a->field != (long long)b->field) { \
		fprintf(stderr, "Mismatch on " #field ": rules = %lld, windows = %lld\n", \
			(long long)a->field, (long long)b->field); \
		return FALSE; \
	} } while (0)

static BOOL cmp_combo(const char* name, const snap_combo_t* a, const snap_combo_t* b)
{
	int i;

	if ((a->count != b->count) || (a->sel != b->sel)) {
		fprintf(stderr, "Mismatch on %s: rules = %d entries (sel %d), windows = %d entries (sel %d)\n",
			name, a->count, a->sel, b->count, b->sel);
		goto dump;
	}
	for (i = 0; i < a->count; i++) {
		if ((a->item[i].data != b->item[i].data) || (strcmp(a->item[i].text, b->item[i].text) != 0)) {
			fprintf(stderr, "Mismatch on %s entry %d\n", name, i);
			goto dump;
		}
	}
	return TRUE;
dump:
	for (i = 0; i < max(a->count, b->count); i++)
		fprintf(stderr, "  %2d: rules = '%s' (0x%lx), windows = '%s' (0x%lx)\n", i,
			(i < a->count) ? a->item[i].text : "", (i < a->count) ? (long)a->item[i].data : 0L,
			(i < b->count) ? b->item[i].text : "", (i < b->count) ? (long)b->item[i].data : 0L);
	return FALSE;
}

static BOOL cmp_snapshot(const snapshot_t* a, const snapshot_t* b)
{
	if (!cmp_combo("boot", &a->boot, &b->boot) || !cmp_combo("image_option", &a->image_option, &b->image_option) ||
		!cmp_combo("partition", &a->partition, &b->partition) || !cmp_combo("target", &a->target, &b->target) ||
		!cmp_combo("fs", &a->fs, &b->fs) || !cmp_combo("cluster", &a->cluster, &b->cluster))
		return FALSE;
	CMP_INT(quick_format.enabled);
	CMP_INT(quick_format.checked);
	CMP_INT(extended_label.enabled);
	CMP_INT(extended_label.checked);
	CMP_INT(old_bios_fixes.enabled);
	CMP_INT(old_bios_fixes.checked);
	CMP_INT(uefi_validation.enabled);
	CMP_INT(uefi_validation.checked);
	CMP_INT(device_enabled);
	CMP_INT(boot_enabled);
	CMP_INT(hash_enabled);
	CMP_INT(save_enabled);
	CMP_INT(start_enabled);
	CMP_INT(image_option_enabled);
	CMP_INT(persistence_slider_enabled);
	CMP_INT(persistence_size_enabled);
	CMP_INT(persistence_units_enabled);
	CMP_INT(persistence_units_visible);
	CMP_INT(label_enabled);
	CMP_INT(bad_blocks_enabled);
	CMP_INT(partition_enabled);
	CMP_INT(target_enabled);
	CMP_INT(fs_enabled);
	CMP_INT(cluster_enabled);
	if (strcmp(a->label, b->label) != 0) {
		fprintf(stderr, "Mismatch on label: rules = '%s', windows = '%s'\n", a->label, b->label);
		return FALSE;
	}
	CMP_INT(persistence_units);
	CMP_INT(persistence_unit_sel);
	CMP_INT(persistence_max);
	CMP_INT(persistence_pos);
	CMP_INT(persistence_size);
	CMP_INT(g_selection_default);
	CMP_INT(g_boot_type);
	CMP_INT(g_partition_type);
	CMP_INT(g_target_type);
	CMP_INT(g_fs_type);
	CMP_INT(g_selected_pt);
	CMP_INT(g_selected_fs);
	CMP_INT(g_imop_win_sel);
	CMP_INT(g_image_options);
	CMP_INT(g_has_uefi_csm);
	CMP_INT(g_op_in_progress);
	CMP_INT(g_validate_md5sum);
	CMP_INT(g_user_changed_label);
	return TRUE;
}

static BOOL compare(const rules_state_t* m)
{
	snapshot_t a, b;

	snap_from_model(&a, m);
	oracle_snapshot(&b);

	if (cmp_snapshot(&a, &b))
		return TRUE;
	dump_history();
	mismatches++;
	return FALSE;
}

static const oracle_drive_t* to_oracle_drive(const rules_drive_t* d, oracle_drive_t* od)
{
	if (d == NULL)
		return NULL;
	od->disk_size = d->disk_size;
	od->sector_size = d->sector_size;
	od->partition_style = d->partition_style;
	od->is_fixed = d->is_fixed;
	static_strcpy(od->fs_name, d->fs_name);
	static_strcpy(od->label, d->label);
	static_strcpy(od->proposed_label, d->proposed_label);
	return od;
}

enum { EV_DEVICE, EV_DEVICE_REFRESH, EV_BOOT, EV_IMAGE_OPTION, EV_PARTITION, EV_TARGET, EV_FS, EV_CLUSTER,
	EV_LABEL, EV_CHECK, EV_ADVANCED_DEVICE, EV_ADVANCED_FORMAT, EV_DUAL, EV_SCAN, EV_START, EV_MAX };

static BOOL run_sequence(int steps)
{
	rules_state_t m;
	oracle_env_t env;
	rules_drive_t drive, *pdrive;
	oracle_drive_t odrive;
	RUFUS_IMG_REPORT img;
	int step, ev, idx, result;
	BOOL keep;
	rules_check_t* checks[] = { &m.quick_format, &m.extended_label, &m.old_bios_fixes, &m.uefi_validation };

	history_len = 0;
	random_env(&env);
	rules_init(&m);
	m.advanced_mode_device = env.advanced_mode_device;
	m.advanced_mode_format = env.advanced_mode_format;
	m.allow_dual_uefi_bios = env.allow_dual_uefi_bios;
	m.force_large_fat32 = env.force_large_fat32;
	m.wintogo_available = env.wintogo_available;
	m.refs_available = env.refs_available;
	m.preselected_fs = env.preselected_fs;
	m.selected_fs = env.preselected_fs;
	static_strcpy(m.no_label_msg, "MSG_207");
	oracle_init(&env);
	log_event("init: adv_dev=%d adv_fmt=%d dual=%d large_fat32=%d wtg=%d refs=%d presel_fs=%d",
		env.advanced_mode_device, env.advanced_mode_format, env.allow_dual_uefi_bios, env.force_large_fat32,
		env.wintogo_available, env.refs_available, env.preselected_fs);

	random_drive(&drive);
	pdrive = (rnd(8) == 0) ? NULL : &drive;
	log_event("startup: %s", pdrive ? drive.proposed_label : "no device");
	rules_on_startup(&m, pdrive);
	oracle_startup(to_oracle_drive(pdrive, &odrive));
	if (!compare(&m))
		return FALSE;

	for (step = 0; step < steps; step++) {
		if (m.op_in_progress) {
			log_event("operation end");
			rules_on_operation_end(&m);
			oracle_operation_end();
			if (!compare(&m))
				return FALSE;
			continue;
		}
		ev = rnd(EV_MAX);
		switch (ev) {
		case EV_DEVICE:
		case EV_DEVICE_REFRESH:
			if (!m.device_enabled)
				continue;
			random_drive(&drive);
			pdrive = (rnd(6) == 0) ? NULL : &drive;
			log_event("%s: %s, sector %u, style %d, fs '%s', label '%s', fixed %d",
				(ev == EV_DEVICE) ? "device" : "device refresh", pdrive ? drive.proposed_label : "none",
				drive.sector_size, drive.partition_style, drive.fs_name, drive.label, drive.is_fixed);
			if (ev == EV_DEVICE) {
				rules_on_device(&m, pdrive);
				oracle_device(to_oracle_drive(pdrive, &odrive));
			} else {
				rules_on_device_refresh(&m, pdrive);
				oracle_device_refresh(to_oracle_drive(pdrive, &odrive));
			}
			break;
		case EV_BOOT:
			if (!m.boot_enabled || (m.boot.count == 0))
				continue;
			idx = rnd(m.boot.count);
			log_event("boot selection: %d (%d)", idx, m.boot.entry[idx].data);
			rules_on_boot_selection(&m, idx);
			oracle_boot_selection(idx);
			break;
		case EV_IMAGE_OPTION:
			if (!m.image_option_enabled || !(m.image_options & IMOP_WINTOGO) || (m.image_option.count == 0))
				continue;
			idx = rnd(m.image_option.count);
			log_event("image option: %d", idx);
			rules_on_image_option(&m, idx);
			oracle_image_option(idx);
			break;
		case EV_PARTITION:
			if (!m.partition_enabled || (m.partition.count == 0))
				continue;
			idx = rnd(m.partition.count);
			log_event("partition scheme: %d (%d)", idx, m.partition.entry[idx].data);
			rules_on_partition_scheme(&m, idx);
			oracle_partition_scheme(idx);
			break;
		case EV_TARGET:
			if (!m.target_enabled || (m.target.count == 0))
				continue;
			idx = rnd(m.target.count);
			log_event("target system: %d", idx);
			rules_on_target_system(&m, idx);
			oracle_target_system(idx);
			break;
		case EV_FS:
			if (!m.fs_enabled || (m.fs.count == 0))
				continue;
			idx = rnd(m.fs.count);
			log_event("file system: %d (%d)", idx, m.fs.entry[idx].data);
			rules_on_file_system(&m, idx);
			oracle_file_system(idx);
			break;
		case EV_CLUSTER:
			if (!m.cluster_enabled || (m.cluster.count == 0))
				continue;
			idx = rnd(m.cluster.count);
			log_event("cluster size: %d", idx);
			rules_on_cluster_size(&m, idx);
			oracle_cluster_size(idx);
			break;
		case EV_LABEL:
			if (!m.label_enabled)
				continue;
			log_event("label");
			rules_on_label(&m, "USER LABEL");
			oracle_label("USER LABEL");
			break;
		case EV_CHECK:
			idx = rnd(ARRAYSIZE(checks));
			log_event("check box %d", idx);
			rules_on_check(&m, checks[idx]);
			oracle_check((enum oracle_check_id)idx);
			break;
		case EV_ADVANCED_DEVICE:
			log_event("toggle advanced device options");
			rules_on_toggle_advanced_device(&m);
			oracle_toggle_advanced_device();
			break;
		case EV_ADVANCED_FORMAT:
			log_event("toggle advanced format options");
			rules_on_toggle_advanced_format(&m);
			oracle_toggle_advanced_format();
			break;
		case EV_DUAL:
			log_event("Alt-E");
			rules_on_toggle_dual_uefi_bios(&m);
			oracle_toggle_dual_uefi_bios();
			break;
		case EV_SCAN:
			if (!m.boot_enabled)
				continue;
			random_image(&img);
			result = (rnd(6) == 0) ? RULES_SCAN_FAILED : ((rnd(6) == 0) ? RULES_SCAN_UNSUPPORTED : RULES_SCAN_OK);
			keep = (rnd(10) == 0);
			log_event("image scan start");
			rules_on_image_scan_start(&m);
			oracle_image_scan_start();
			if (!compare(&m))
				return FALSE;
			log_event("image scan end: result %d, keep name %d, iso %d, dd %d, win %d, efi 0x%x, bootmgr %d/%d, "
				"winpe 0x%x, wininst %d, sl 0x%x, grub2 0x%x/0x%x, g4d %d, kolibri %d, reactos %d, "
				"4GB 0x%x, needs_ntfs %d, label '%s'", result, keep, img.is_iso, img.is_bootable_img,
				img.is_windows_img, img.has_efi, img.has_bootmgr, img.has_bootmgr_efi, img.winpe,
				img.wininst_index, img.sl_version, img.has_grub2, img.has_grub2_fs, img.has_grub4dos,
				img.has_kolibrios, img.reactos_path[0] != 0, img.has_4GB_file, img.needs_ntfs, img.label);
			rules_on_image_scan_end(&m, &img, result, keep);
			oracle_image_scan_end(&img, result, keep);
			break;
		case EV_START:
			if (!m.start_enabled)
				continue;
			log_event("start");
			rules_on_start(&m);
			oracle_start();
			break;
		}
		if (!compare(&m))
			return FALSE;
	}
	return TRUE;
}

static void silent_log(const char* text) { }

// TODO: Drop once stdio.c is part of the core
void uprintf(const char* format, ...) { }

int main(void)
{
	const ui_iface_t iface = { .log = silent_log };
	const char* env_seed = getenv("RUFUS_RULES_SEED");
	const char* env_runs = getenv("RUFUS_RULES_RUNS");
	uint64_t seed = (env_seed != NULL) ? strtoull(env_seed, NULL, 0) : 0x52756675735F4C78ULL;
	int i, runs = (env_runs != NULL) ? atoi(env_runs) : 3000;

	ui_set_iface(&iface);
	test_scenarios();

	for (i = 0; i < runs; i++) {
		pid_t pid;
		int status;
		// Each run gets its own seed, so that a failure can be replayed on its own
		rng_state = seed + (uint64_t)i * 0x9E3779B97F4A7C15ULL;
		if (rng_state == 0)
			rng_state = 1;
		// The upstream functions keep some state in static variables, so each run gets a fresh process
		fflush(NULL);
		pid = fork();
		if (pid == 0)
			_exit(run_sequence(60) ? 0 : 1);
		if ((pid < 0) || (waitpid(pid, &status, 0) != pid) || !WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
			mismatches++;
			fprintf(stderr, "Differential test failed on run %d (RUFUS_RULES_SEED=0x%" PRIx64 " RUFUS_RULES_RUNS=1)\n",
				i, (uint64_t)(seed + (uint64_t)i * 0x9E3779B97F4A7C15ULL));
			test_failures++;
			break;
		}
	}
	if (mismatches == 0)
		printf("%d random sequences matched the Windows rules\n", runs);
	return TEST_RESULT();
}

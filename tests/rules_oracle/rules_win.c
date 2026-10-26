/*
 * Rufus: The Reliable USB Formatting Utility
 * Reference implementation of the main window rules, from the Windows code
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
 * The code between the BEGIN UPSTREAM and END UPSTREAM markers is copied
 * verbatim from src/rufus.c and src/ui.c of Rufus 4.16 (commit 85873804),
 * the baseline of this port. Do not edit it: it is the reference that
 * core/rules.c is tested against. The rest is glue, which replays the
 * relevant parts of other upstream code paths, as noted.
 */

/* Rename what the core also defines, so that both can be linked together */
#define img_report                  win_img_report
#define image_path                  win_image_path
#define fs_type                     win_fs_type
#define boot_type                   win_boot_type
#define partition_type              win_partition_type
#define target_type                 win_target_type
#define image_options               win_image_options
#define persistence_size            win_persistence_size
#define allow_dual_uefi_bios        win_allow_dual_uefi_bios
#define right_to_left_mode          win_right_to_left_mode
#define op_in_progress              win_op_in_progress
#define usb_debug                   win_usb_debug
#define embedded_sl_version_str     win_embedded_sl_version_str
#define FileSystemLabel             win_FileSystemLabel
#define SizeToHumanReadable         win_SizeToHumanReadable
#define PrintStatusInfo             win_PrintStatusInfo
#define uprintf                     win_uprintf
#define EnableControls              win_EnableControls
#define SetComboEntry               win_SetComboEntry
#define SetPersistencePos           win_SetPersistencePos
#define SetPersistenceSize          win_SetPersistenceSize
#define ToggleImageOptions          win_ToggleImageOptions

#include "rufus.h"
#include "resource.h"
#include "win32_mock.h"
#include "../../res/grub/grub_version.h"
#include "../../res/grub2/grub2_version.h"

#define BN_CLICKED                  0
#define SETTING_ADVANCED_MODE_DEVICE "AdvancedDeviceMode"
#define SETTING_ADVANCED_MODE_FORMAT "AdvancedFormatMode"

/* The fields of RUFUS_DRIVE_INFO (drive.h) that the rules use */
typedef struct {
	LONGLONG DiskSize;
	DWORD DeviceNumber;
	DWORD SectorSize;
	MEDIA_TYPE MediaType;
	int PartitionStyle;
	int FSType;
	char proposed_label[16];
	struct {
		ULONG Allowed;
		ULONG Default;
	} ClusterSize[FS_MAX];
} RUFUS_DRIVE_INFO;

/* The Windows host, as far as ReFS and Windows To Go are concerned */
enum WindowsVersion { WINDOWS_7 = 0x61, WINDOWS_8 = 0x62, WINDOWS_8_1 = 0x63, WINDOWS_10 = 0xA0 };
static struct {
	DWORD Version;
	DWORD BuildNumber;
	DWORD Edition;
} WindowsVersion;

/* Globals of rufus.c, ui.c and drive.c, with their initial values */
RUFUS_IMG_REPORT img_report;
char* image_path;
int fs_type, boot_type, partition_type, target_type;
uint8_t image_options;
uint64_t persistence_size;
BOOL allow_dual_uefi_bios, right_to_left_mode, op_in_progress, usb_debug;
char embedded_sl_version_str[2][12];
const char* FileSystemLabel[FS_MAX] = { "FAT", "FAT32", "NTFS", "UDF", "exFAT", "ReFS", "ext2", "ext3", "ext4" };
static const char* sfd_name = "Super Floppy Disk";
static char* short_image_path;
static BOOL advanced_mode_device, advanced_mode_format, force_large_fat32, use_vds;
static BOOL has_uefi_csm, validate_md5sum, app_changed_size, enable_HDDs;
static BOOL user_changed_label, app_changed_label, dont_display_image_name;
static BOOL allowed_filesystem[FS_MAX];
static int selection_default, persistence_unit_selection, imop_win_sel, default_fs;
static int selected_pt, selected_fs, preselected_fs, image_index;
static char ClusterSizeLabel[MAX_CLUSTER_SIZES][64];
static char uppercase_close[64] = "CLOSE", uppercase_cancel[64] = "CANCEL";
static char image_option_txt[128];
static RUFUS_DRIVE_INFO SelectedDrive;
static RUFUS_DRIVE rufus_drive[MAX_DRIVES];
static oracle_drive_t current_drive;
static int rh, section_vpos[3];
static const int image_option_move_ids[] = { IDS_IMAGE_OPTION_TXT };
// From ui_data.h
static int image_option_toggle_ids[][2] = {
	{ IDS_IMAGE_OPTION_TXT, 0x03 },
	{ IDC_IMAGE_OPTION, 0x01 },
	{ IDC_PERSISTENCE_SLIDER, 0x02 },
	{ IDC_PERSISTENCE_SIZE, 0x02 },
	{ IDC_PERSISTENCE_UNITS, 0x02 }
};

/* Controls */
static HWND hMainDialog, hDeviceList, hBootType, hImageOption, hPartitionScheme, hTargetSystem;
static HWND hFileSystem, hClusterSize, hLabel, hSelectImage, hStart;
static HWND hMultiToolbar, hSaveToolbar, hHashToolbar, hAdvancedDeviceToolbar, hAdvancedFormatToolbar;
static mock_ctrl_t controls[64];
static int nb_controls;

HWND GetDlgItem(HWND hDlg, int id)
{
	int i;

	for (i = 0; i < nb_controls; i++) {
		if (controls[i].id == id)
			return &controls[i];
	}
	assert(nb_controls < (int)ARRAYSIZE(controls));
	memset(&controls[nb_controls], 0, sizeof(controls[0]));
	controls[nb_controls].id = id;
	controls[nb_controls].enabled = TRUE;
	controls[nb_controls].visible = TRUE;
	controls[nb_controls].sel = -1;
	return &controls[nb_controls++];
}

void mock_reset(void)
{
	nb_controls = 0;
	hMainDialog = GetDlgItem(NULL, IDD_DIALOG);
	hDeviceList = GetDlgItem(hMainDialog, IDC_DEVICE);
	hBootType = GetDlgItem(hMainDialog, IDC_BOOT_SELECTION);
	hImageOption = GetDlgItem(hMainDialog, IDC_IMAGE_OPTION);
	hPartitionScheme = GetDlgItem(hMainDialog, IDC_PARTITION_TYPE);
	hTargetSystem = GetDlgItem(hMainDialog, IDC_TARGET_SYSTEM);
	hFileSystem = GetDlgItem(hMainDialog, IDC_FILE_SYSTEM);
	hClusterSize = GetDlgItem(hMainDialog, IDC_CLUSTER_SIZE);
	hLabel = GetDlgItem(hMainDialog, IDC_LABEL);
	hSelectImage = GetDlgItem(hMainDialog, IDC_SELECT);
	hStart = GetDlgItem(hMainDialog, IDC_START);
	hMultiToolbar = GetDlgItem(hMainDialog, IDC_ABOUT);
	hSaveToolbar = GetDlgItem(hMainDialog, IDC_SAVE);
	hHashToolbar = GetDlgItem(hMainDialog, IDC_HASH);
	hAdvancedDeviceToolbar = GetDlgItem(hMainDialog, IDC_ADVANCED_DRIVE_PROPERTIES);
	hAdvancedFormatToolbar = GetDlgItem(hMainDialog, IDC_ADVANCED_FORMAT_OPTIONS);
}

/* Messages that take one string argument display as "MSG_xxx[arg]", the others as "MSG_xxx" */
static char* lmprintf(uint32_t msg_id, ...)
{
	static char buf[16][SNAP_TEXT];
	static int idx = 0;
	char* str = buf[idx++ % ARRAYSIZE(buf)];
	va_list args;

	if ((msg_id == MSG_030) || (msg_id == MSG_281)) {
		va_start(args, msg_id);
		snprintf(str, SNAP_TEXT, "MSG_%03d[%s]", msg_id - MSG_000, va_arg(args, const char*));
		va_end(args);
	} else {
		snprintf(str, SNAP_TEXT, "MSG_%03d", msg_id - MSG_000);
	}
	return str;
}

void uprintf(const char* format, ...) { }
void PrintStatusInfo(BOOL info, BOOL debug, unsigned int duration, int msg_id, ...) { }
static void CreateTooltip(HWND hControl, const char* message, int duration) { }
static void SetBootTypeDropdownWidth(void) { }
static void MoveCtrlY(HWND hDlg, int nID, int vertical_shift) { }
static void ResizeDialogs(int shift) { }
static void InvalidateRect(HWND h, void* rect, BOOL erase) { }
static void WriteSettingBool(const char* name, BOOL val) { }
static void ToggleAdvancedDeviceOptions(BOOL enable) { }
static void ToggleAdvancedFormatOptions(BOOL enable) { }
static void SetPassesTooltip(void) { }
static BOOL StartProcessSearch(void) { return TRUE; }
static void StopProcessSearch(void) { }
static BOOL SetProcessSearch(DWORD DeviceNum) { return TRUE; }
static uint8_t popcnt8(uint8_t val) { return (uint8_t)__builtin_popcount(val); }

// The size is only used to propose a label (and for tooltips)
char* SizeToHumanReadable(uint64_t size, BOOL copy_to_log, BOOL fake_units)
{
	return current_drive.proposed_label;
}

// Fill SelectedDrive from the current mock drive
static BOOL GetDrivePartitionData(DWORD DriveIndex, char* FileSystemName, DWORD FileSystemNameSize, BOOL bSilent)
{
	SelectedDrive.DiskSize = (LONGLONG)current_drive.disk_size;
	SelectedDrive.SectorSize = current_drive.sector_size;
	SelectedDrive.MediaType = current_drive.is_fixed ? FixedMedia : RemovableMedia;
	SelectedDrive.PartitionStyle = current_drive.partition_style;
	safe_strcpy(FileSystemName, FileSystemNameSize, current_drive.fs_name);
	return TRUE;
}

// From ui.c, without the resizing and styling of the controls
static void TogglePersistenceControls(BOOL display)
{
	HWND hSize, hUnits;
	hSize = GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE);
	hUnits = GetDlgItem(hMainDialog, IDC_PERSISTENCE_UNITS);

	EnableWindow(hSize, display ? TRUE : FALSE);
	EnableWindow(hUnits, display ? TRUE : FALSE);
	ShowWindow(hUnits, display ? SW_SHOW : SW_HIDE);
}

static INT_PTR MainCommand(HWND hDlg, WPARAM wParam, LPARAM lParam);
// ui.c
void SetComboEntry(HWND hDlg, int data);
void SetPersistencePos(uint64_t pos);
void SetPersistenceSize(void);
void ToggleImageOptions(void);

intptr_t SendMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	if (h == hMainDialog) {
		if (msg == WM_COMMAND)
			return MainCommand(hMainDialog, w, l);
		return 0;
	}
	switch (msg) {
	case TBM_SETRANGEMIN:
		h->range_min = l;
		break;
	case TBM_SETRANGEMAX:
		h->range_max = l;
		break;
	case TBM_SETPOS:
		h->pos = l;
		break;
	}
	return 0;
}

BOOL SetWindowTextU(HWND h, const char* str)
{
	static_strcpy(h->text, str);
	// Edit controls notify their parent of any change, including programmatic ones
	if ((h->id == IDC_LABEL) || (h->id == IDC_PERSISTENCE_SIZE))
		MainCommand(hMainDialog, (EN_CHANGE << 16) | h->id, 0);
	return TRUE;
}

/* ---------------------------- BEGIN UPSTREAM (rufus.c) ---------------------------- */

static void SetClusterSizeLabels(void)
{
	unsigned int i, j, msg_id;
	safe_sprintf(ClusterSizeLabel[0], 64, "%s", lmprintf(MSG_029));
	for (i=512, j=1, msg_id=MSG_026; j<MAX_CLUSTER_SIZES; i<<=1, j++) {
		if (i > 8192) {
			i /= 1024;
			msg_id++;
		}
		safe_sprintf(ClusterSizeLabel[j], 64, "%d %s", i, lmprintf(msg_id));
	}
}

static void SetAllowedFileSystems(void)
{
	int i;

	memset(allowed_filesystem, 0, sizeof(allowed_filesystem));
	// Nothing is allowed if we don't have a drive
	if (ComboBox_GetCurSel(hDeviceList) < 0)
		return;
	switch (selection_default) {
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
		if ((image_path == NULL) || !HAS_NTFSLESS_GRUB(img_report))
			allowed_filesystem[FS_NTFS] = TRUE;
		// Don't allow anything besides NTFS if the image is not compatible
		if ((image_path != NULL) && !IS_FAT32_COMPAT(img_report)) {
			// Only disable FAT32 if we have NTFS enabled
			if (allowed_filesystem[FS_NTFS])
				break;
			// Else, print a warning
			uprintf("WARNING: FAT32 has been forcefully enabled, but this image may not work with FAT32.");
		}
		if (!HAS_WINDOWS(img_report) || (target_type != TT_BIOS) || allow_dual_uefi_bios) {
			if (!HAS_WINTOGO(img_report) || (ComboBox_GetCurItemData(hImageOption) != IMOP_WIN_TO_GO)) {
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

static void SetBootOptions(void)
{
	char tmp[32];

	IGNORE_RETVAL(ComboBox_ResetContent(hBootType));
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, lmprintf(MSG_279)), BT_NON_BOOTABLE));
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType,
		(image_path == NULL) ? lmprintf(MSG_281, lmprintf(MSG_280)) : short_image_path), BT_IMAGE));
	image_index = 1;
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "MS-DOS"), BT_MSDOS));
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "FreeDOS"), BT_FREEDOS));

	if (advanced_mode_device) {
		static_sprintf(tmp, "Syslinux %s", embedded_sl_version_str[0]);
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, tmp), BT_SYSLINUX_V4));
		static_sprintf(tmp, "Syslinux %s", embedded_sl_version_str[1]);
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, tmp), BT_SYSLINUX_V6));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "ReactOS"), BT_REACTOS));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType,
			"Grub " GRUB2_PACKAGE_VERSION), BT_GRUB2));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType,
			"Grub4DOS " GRUB4DOS_VERSION), BT_GRUB4DOS));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "UEFI:NTFS"), BT_UEFI_NTFS));
	}
	if ((!advanced_mode_device) && (selection_default >= BT_SYSLINUX_V4))
		selection_default = BT_IMAGE;
	SetComboEntry(hBootType, selection_default);
}

static void SetPartitionSchemeAndTargetSystem(BOOL only_target)
{
	//                                   MBR,  GPT,  SFD
	BOOL allowed_partition_scheme[3] = { TRUE, TRUE, FALSE };
	//                                   BIOS, UEFI, DUAL
	BOOL allowed_target_system[3]    = { TRUE, TRUE, FALSE };
	BOOL is_windows_to_go_selected;

	if (!only_target)
		IGNORE_RETVAL(ComboBox_ResetContent(hPartitionScheme));
	IGNORE_RETVAL(ComboBox_ResetContent(hTargetSystem));

	boot_type = (int)ComboBox_GetCurItemData(hBootType);
	is_windows_to_go_selected = (boot_type == BT_IMAGE) && (image_path != NULL) && HAS_WINTOGO(img_report) &&
		(ComboBox_GetCurItemData(hImageOption) == IMOP_WIN_TO_GO);
	// If no device is selected, don't populate anything
	if (ComboBox_GetCurSel(hDeviceList) < 0)
		return;
	switch (boot_type) {
	case BT_NON_BOOTABLE:
		allowed_partition_scheme[PARTITION_STYLE_SFD] = TRUE;
		allowed_target_system[0] = FALSE;
		allowed_target_system[1] = FALSE;
		allowed_target_system[2] = TRUE;
		break;
	case BT_IMAGE:
		if (image_path == NULL)
			break;
		// Check if image is EFI bootable
		if (!IS_EFI_BOOTABLE(img_report)) {
			allowed_partition_scheme[PARTITION_STYLE_GPT] = FALSE;
			allowed_target_system[1] = FALSE;
			break;
		}
		// Image is EFI bootable => set dual BIOS + UEFI and so on...
		if (IS_BIOS_BOOTABLE(img_report)) {
			if (!HAS_WINDOWS(img_report) || allow_dual_uefi_bios || is_windows_to_go_selected) {
				allowed_target_system[0] = FALSE;
				allowed_target_system[1] = TRUE;
				allowed_target_system[2] = TRUE;
			}
			// Syslinux 4.x or earlier has no support for NTFS so if an image is using Syslinux 4.x only
			// and has a 4 GB file (which forces us to use NTFS) then disable MBR altogether as we won't
			// be able to make a working MBR install of Syslinux.
			if (HAS_SYSLINUX(img_report) && (SL_MAJOR(img_report.sl_version) < 5) && img_report.has_4GB_file &&
				!HAS_BOOTMGR(img_report) && !HAS_WINPE(img_report) && !HAS_GRUB(img_report))
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
		if (SelectedDrive.DiskSize > 2 * TB)
			selected_pt = PARTITION_STYLE_GPT;
		// Try to reselect the current drive's partition scheme
		int preferred_pt = SelectedDrive.PartitionStyle;
		if (allowed_partition_scheme[PARTITION_STYLE_MBR]) 
			IGNORE_RETVAL(ComboBox_SetItemData(hPartitionScheme,
				ComboBox_AddStringU(hPartitionScheme, "MBR"), PARTITION_STYLE_MBR));
		if (allowed_partition_scheme[PARTITION_STYLE_GPT])
			IGNORE_RETVAL(ComboBox_SetItemData(hPartitionScheme,
				ComboBox_AddStringU(hPartitionScheme, "GPT"), PARTITION_STYLE_GPT));
		if (allowed_partition_scheme[PARTITION_STYLE_SFD])
			IGNORE_RETVAL(ComboBox_SetItemData(hPartitionScheme,
				ComboBox_AddStringU(hPartitionScheme, sfd_name), PARTITION_STYLE_SFD));
		// Override the partition scheme according to the current 
		if (boot_type == BT_NON_BOOTABLE)
			preferred_pt = (selected_pt >= 0) ? selected_pt : PARTITION_STYLE_MBR;
		else if (boot_type == BT_UEFI_NTFS)
			preferred_pt = (selected_pt >= 0) ? selected_pt : PARTITION_STYLE_GPT;
		else if ((boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.is_iso || img_report.is_windows_img)) {
			if (HAS_WINDOWS(img_report) && img_report.has_efi)
				preferred_pt = allow_dual_uefi_bios? PARTITION_STYLE_MBR :
					((selected_pt >= 0) ? selected_pt : PARTITION_STYLE_GPT);
			if (IS_DD_BOOTABLE(img_report))
				preferred_pt = (selected_pt >= 0) ? selected_pt : PARTITION_STYLE_MBR;
		}
		SetComboEntry(hPartitionScheme, preferred_pt);
		partition_type = (int)ComboBox_GetCurItemData(hPartitionScheme);
	}

	has_uefi_csm = FALSE;
	if (allowed_target_system[0] && (partition_type != PARTITION_STYLE_GPT)) {
		IGNORE_RETVAL(ComboBox_SetItemData(hTargetSystem,
			ComboBox_AddStringU(hTargetSystem, lmprintf(MSG_031)), TT_BIOS));
		has_uefi_csm = TRUE;
	}
	if (allowed_target_system[1] && !((partition_type == PARTITION_STYLE_MBR) && (boot_type == BT_IMAGE) && IS_BIOS_BOOTABLE(img_report) && IS_EFI_BOOTABLE(img_report)) )
		IGNORE_RETVAL(ComboBox_SetItemData(hTargetSystem,
			ComboBox_AddStringU(hTargetSystem, lmprintf(MSG_032)), TT_UEFI));
	if (allowed_target_system[2] && ((partition_type != PARTITION_STYLE_GPT) || (boot_type == BT_NON_BOOTABLE)))
		IGNORE_RETVAL(ComboBox_SetItemData(hTargetSystem,
			ComboBox_AddStringU(hTargetSystem, lmprintf(MSG_033)), TT_BIOS));
	IGNORE_RETVAL(ComboBox_SetCurSel(hTargetSystem, 0));
	target_type = (int)ComboBox_GetCurItemData(hTargetSystem);
	// Can't update a tooltip from a thread, so we send a message instead
	SendMessage(hMainDialog, UM_UPDATE_CSM_TOOLTIP, 0, 0);
}

static BOOL SetClusterSizes(int FSType)
{
	char* szClustSize;
	int i, k, default_index = 0;
	ULONG j;

	IGNORE_RETVAL(ComboBox_ResetContent(hClusterSize));

	if ((FSType < 0) || (FSType >= FS_MAX)) {
		return FALSE;
	}

	if ((SelectedDrive.ClusterSize[FSType].Allowed == 0)
		|| (SelectedDrive.ClusterSize[FSType].Default == 0)) {
		return FALSE;
	}

	for (i = 0, j = 0x100, k = 0; j < 0x10000000; i++, j <<= 1) {
		if (j & SelectedDrive.ClusterSize[FSType].Allowed) {
			if (j == SelectedDrive.ClusterSize[FSType].Default) {
				szClustSize = lmprintf(MSG_030, ClusterSizeLabel[i]);
				default_index = k;
			} else {
				szClustSize = ClusterSizeLabel[i];
			}
			IGNORE_RETVAL(ComboBox_SetItemData(hClusterSize, ComboBox_AddStringU(hClusterSize, szClustSize), j));
			k++;
		}
	}

	IGNORE_RETVAL(ComboBox_SetCurSel(hClusterSize, default_index));
	return TRUE;
}

static BOOL IsRefsAvailable(MEDIA_TYPE MediaType)
{
	// The creation of ReFS drives was added in Windows 8.1... but then removed by
	// Microsoft in Windows 10 1709, except for the Enterprise and Pro Workstation
	// versions. Oh and VdsService::QueryFileSystemTypes() is *USELESS* to detect
	// if ReFS is available on the system. Oh, and it only applies to fixed media.
	// Oh and Microsoft removed the ability to format a volume to ReFS unless you
	// use VDS... Why do I even bother with this?

	if (MediaType != FixedMedia)
		return FALSE;
	if (WindowsVersion.Version < WINDOWS_8_1 || WindowsVersion.BuildNumber <= 0)
		return FALSE;
	if (!use_vds)
		return FALSE;
	// Per https://gist.github.com/0xbadfca11/da0598e47dd643d933dc
	if (WindowsVersion.BuildNumber < 16226)
		return TRUE;
	switch (WindowsVersion.Edition) {
	case 0x0000000A: // Enterprise Server
	case 0x0000001B: // Enterprise N
	case 0x00000046: // Enterprise E
	case 0x00000048: // Enterprise Eval
	case 0x00000054: // Enterprise N Eval
	case 0x0000007D: // Enterprise S
	case 0x0000007E: // Enterprise S N
	case 0x00000081: // Enterprise S Eval
	case 0x00000082: // Enterprise S N Eval
	case 0x0000008C: // Enterprise Subscription
	case 0x0000008D: // Enterprise Subscription N
	case 0x000000A1: // Pro Workstation
	case 0x000000A2: // Pro Workstation N
	case 0x000000AB: // Enterprise G
	case 0x000000AC: // Enterprise G N
		return TRUE;
	default:
		return FALSE;
	}
}

static BOOL SetFileSystemAndClusterSize(char* fs_name)
{
	int fs_index;
	LONGLONG i;
	char tmp[128] = "", *entry;

	IGNORE_RETVAL(ComboBox_ResetContent(hFileSystem));
	IGNORE_RETVAL(ComboBox_ResetContent(hClusterSize));
	default_fs = FS_UNKNOWN;
	memset(&SelectedDrive.ClusterSize, 0, sizeof(SelectedDrive.ClusterSize));

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
	if (SelectedDrive.DiskSize < 4 * GB) {
		SelectedDrive.ClusterSize[FS_FAT16].Allowed = 0x00001E00;
		for (i = 32; i <= 4096; i <<= 1) {			// 8 MB -> 4 GB
			if (SelectedDrive.DiskSize < i * MB) {
				SelectedDrive.ClusterSize[FS_FAT16].Default = 16 * (ULONG)i;
				break;
			}
			SelectedDrive.ClusterSize[FS_FAT16].Allowed <<= 1;
		}
		SelectedDrive.ClusterSize[FS_FAT16].Allowed &= 0x0001FE00;
	}

	// FAT 32
	// > 32GB FAT32 is not supported by MS and FormatEx but is achieved using fat32format
	// See: http://ridgecrop.co.uk/index.htm?fat32format.htm
	// < 32 MB FAT32 is not allowed by FormatEx, so we don't bother
	if ((SelectedDrive.DiskSize >= 32 * MB) && (SelectedDrive.DiskSize < MAX_FAT32_SIZE)) {
		SelectedDrive.ClusterSize[FS_FAT32].Allowed = 0x000001F8;
		for (i = 32; i <= (32 * 1024); i <<= 1) {			// 32 MB -> 32 GB
			if (SelectedDrive.DiskSize*1.0f < i * MB * FAT32_CLUSTER_THRESHOLD) {	// MS
				SelectedDrive.ClusterSize[FS_FAT32].Default = 8*(ULONG)i;
				break;
			}
			SelectedDrive.ClusterSize[FS_FAT32].Allowed <<= 1;
		}
		SelectedDrive.ClusterSize[FS_FAT32].Allowed &= 0x0001FE00;

		// Default cluster sizes in the 256MB to 32 GB range do not follow the rule above
		if ((SelectedDrive.DiskSize >= 256 * MB) && (SelectedDrive.DiskSize < 32 * GB)) {
			for (i = 8; i <= 32; i <<= 1) {			// 256 MB -> 32 GB
				if (SelectedDrive.DiskSize * 1.0f < i * GB * FAT32_CLUSTER_THRESHOLD) {
					SelectedDrive.ClusterSize[FS_FAT32].Default = ((ULONG)i / 2) * KB;
					break;
				}
			}
		}
		// More adjustments for large drives
		if (SelectedDrive.DiskSize >= 32 * GB) {
			SelectedDrive.ClusterSize[FS_FAT32].Allowed &= 0x0001C000;
			SelectedDrive.ClusterSize[FS_FAT32].Default = 0x00008000;
		}
	}

	if (SelectedDrive.DiskSize < 256 * TB) {
		// NTFS
		SelectedDrive.ClusterSize[FS_NTFS].Allowed = 0x0001F000;
		for (i = 16; i <= 256; i <<= 1) {			// 7 MB -> 256 TB
			if (SelectedDrive.DiskSize < i * TB) {
				SelectedDrive.ClusterSize[FS_NTFS].Default = ((ULONG)i / 4) * KB;
				break;
			}
		}

		// exFAT
		SelectedDrive.ClusterSize[FS_EXFAT].Allowed = 0x03FFFE00;
		if (SelectedDrive.DiskSize < 256 * MB)	// < 256 MB
			SelectedDrive.ClusterSize[FS_EXFAT].Default = 4 * KB;
		else if (SelectedDrive.DiskSize < 32 * GB)	// < 32 GB
			SelectedDrive.ClusterSize[FS_EXFAT].Default = 32 * KB;
		else
			SelectedDrive.ClusterSize[FS_EXFAT].Default = 128 * KB;

		// UDF
		SelectedDrive.ClusterSize[FS_UDF].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
		SelectedDrive.ClusterSize[FS_UDF].Default = 1;

		// ext2/ext3/ext4
		if (advanced_mode_format && (SelectedDrive.DiskSize >= MIN_EXT_SIZE)) {
			SelectedDrive.ClusterSize[FS_EXT2].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
			SelectedDrive.ClusterSize[FS_EXT2].Default = 1;
			SelectedDrive.ClusterSize[FS_EXT3].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
			SelectedDrive.ClusterSize[FS_EXT3].Default = 1;
		}

		// ReFS (only applicable for a select number of Windows platforms and editions)
		if ((SelectedDrive.DiskSize >= 512 * MB) && (IsRefsAvailable(SelectedDrive.MediaType))) {
			if (SelectedDrive.DiskSize < 16 * TB) {	// < 16 TB
				SelectedDrive.ClusterSize[FS_REFS].Allowed = 64 * KB + 4 * KB;
				SelectedDrive.ClusterSize[FS_REFS].Default = 4 * KB;
			} else {
				SelectedDrive.ClusterSize[FS_REFS].Allowed = 64 * KB;
				SelectedDrive.ClusterSize[FS_REFS].Default = 64 * KB;
			}
		}
	}

	// Only add the filesystems we can service
	SetAllowedFileSystems();
	SetClusterSizeLabels();

	for (fs_index = 0; fs_index < FS_MAX; fs_index++) {
		// Remove all cluster sizes that are below the sector size
		if (SelectedDrive.ClusterSize[fs_index].Allowed != SINGLE_CLUSTERSIZE_DEFAULT) {
			SelectedDrive.ClusterSize[fs_index].Allowed &= ~(SelectedDrive.SectorSize - 1);
			if ((SelectedDrive.ClusterSize[fs_index].Default & SelectedDrive.ClusterSize[fs_index].Allowed) == 0)
				// We lost our default => Use rightmost bit to select the new one
				SelectedDrive.ClusterSize[fs_index].Default =
				SelectedDrive.ClusterSize[fs_index].Allowed & (-(LONG)SelectedDrive.ClusterSize[fs_index].Allowed);
		}

		if (SelectedDrive.ClusterSize[fs_index].Allowed != 0) {
			tmp[0] = 0;
			// Tell the user if we're going to use Large FAT32 or regular
			if ((fs_index == FS_FAT32) && ((SelectedDrive.DiskSize > LARGE_FAT32_SIZE) || (force_large_fat32)))
				static_strcat(tmp, "Large ");
			static_strcat(tmp, FileSystemLabel[fs_index]);
			if (default_fs == FS_UNKNOWN) {
				entry = lmprintf(MSG_030, tmp);
				default_fs = fs_index;
			} else {
				entry = tmp;
			}
			if (allowed_filesystem[fs_index]) {
				IGNORE_RETVAL(ComboBox_SetItemData(hFileSystem,
					ComboBox_AddStringU(hFileSystem, entry), fs_index));
			}
		}
	}

	// re-select existing FS if it's one we know
	SelectedDrive.FSType = FS_UNKNOWN;
	if (safe_strlen(fs_name) != 0) {
		for (SelectedDrive.FSType = FS_MAX - 1; SelectedDrive.FSType >= 0; SelectedDrive.FSType--) {
			if (safe_strcmp(fs_name, FileSystemLabel[SelectedDrive.FSType]) == 0) {
				break;
			}
		}
	} else {
		// Re-select last user-selected FS
		SelectedDrive.FSType = selected_fs;
	}

	for (i = 0; i < ComboBox_GetCount(hFileSystem); i++) {
		if (ComboBox_GetItemData(hFileSystem, i) == SelectedDrive.FSType) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hFileSystem, i));
			break;
		}
	}

	if (i == ComboBox_GetCount(hFileSystem)) {
		// failed to reselect => pick default
		SetComboEntry(hFileSystem, default_fs);
	}

	return SetClusterSizes((int)ComboBox_GetCurItemData(hFileSystem));
}

static void SetFSFromISO(void)
{
	int i, fs_tmp, preferred_fs = FS_UNKNOWN;
	uint32_t fs_mask = FS_NTFS | (img_report.has_4GB_file ? 0 : FS_FAT32);
	BOOL windows_to_go = (image_options & IMOP_WINTOGO) && (boot_type == BT_IMAGE) &&
		HAS_WINTOGO(img_report) && (ComboBox_GetCurItemData(hImageOption) == IMOP_WIN_TO_GO);

	if (image_path == NULL)
		return;

	// Create a mask of all the FS's available
	for (i = 0; i < ComboBox_GetCount(hFileSystem); i++) {
		fs_tmp = (int)ComboBox_GetItemData(hFileSystem, i);
		fs_mask |= 1 << fs_tmp;
	}

	// If the FS requested from the command line is valid use it
	if ((preselected_fs != FS_UNKNOWN) && (fs_mask & (1 << preselected_fs))) {
		preferred_fs = preselected_fs;
	} else {
		// Syslinux and EFI have precedence over bootmgr (unless the user selected BIOS as target type)
		if ((HAS_SYSLINUX(img_report)) || (HAS_REACTOS(img_report)) || HAS_KOLIBRIOS(img_report) ||
			(IS_EFI_BOOTABLE(img_report) && (target_type == TT_UEFI) && (!windows_to_go) && (!img_report.has_4GB_file))) {
			if (fs_mask & (1 << FS_FAT32))
				preferred_fs = FS_FAT32;
			else if ((fs_mask & (1 << FS_FAT16)) && !HAS_KOLIBRIOS(img_report))
				preferred_fs = FS_FAT16;
		} else if ((windows_to_go) || HAS_BOOTMGR(img_report) || HAS_WINPE(img_report)) {
			if ((fs_mask & (1 << FS_FAT32)) && (!img_report.has_4GB_file) && (allow_dual_uefi_bios))
				preferred_fs = FS_FAT32;
			else if (fs_mask & (1 << FS_NTFS))
				preferred_fs = FS_NTFS;
		}
	}

	// Try to select the FS
	for (i = 0; i < ComboBox_GetCount(hFileSystem); i++) {
		fs_tmp = (int)ComboBox_GetItemData(hFileSystem, i);
		if (fs_tmp == preferred_fs) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hFileSystem, i));
			break;
		}
	}
	if (selected_fs == FS_UNKNOWN)
		selected_fs = preferred_fs;

	SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL << 16) | IDC_FILE_SYSTEM,
		ComboBox_GetCurSel(hFileSystem));
}

static void SetProposedLabel(int ComboIndex)
{
	const char no_label[] = STR_NO_LABEL, empty[] = "";

	// If the user manually changed the label, try to preserve it
	if (user_changed_label)
		return;

	app_changed_label = TRUE;
	// If bootable ISO creation is selected, and we have an ISO selected with a valid name, use that
	// Also some distros (eg. Arch) require the USB to have the same label as the ISO
	if ((boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.label[0] != 0)) {
		SetWindowTextU(hLabel, img_report.label);
		return;
	}

	// Empty the label if no device is currently selected
	if (ComboIndex < 0) {
		SetWindowTextU(hLabel, "");
		return;
	}

	// Else if no existing label is available, propose one according to the size (eg: "256MB", "8GB")
	if ((_stricmp(no_label, rufus_drive[ComboIndex].label) == 0) || (_stricmp(no_label, empty) == 0)
		|| (safe_stricmp(lmprintf(MSG_207), rufus_drive[ComboIndex].label) == 0)) {
		SetWindowTextU(hLabel, SelectedDrive.proposed_label);
	} else {
		SetWindowTextU(hLabel, rufus_drive[ComboIndex].label);
	}
}

static void EnableOldBiosFixes(BOOL enable, BOOL remove_checkboxes)
{
	static UINT checked, state = 0;
	HWND hCtrl = GetDlgItem(hMainDialog, IDC_OLD_BIOS_FIXES);

	// The fix for old BIOSes option cannot apply if we aren't targetting BIOS, or are using an image that isn't BIOS bootable
	if ((partition_type != PARTITION_STYLE_MBR) || (target_type != TT_BIOS) || (boot_type == BT_NON_BOOTABLE) ||
		((boot_type == BT_IMAGE) && (!IS_BIOS_BOOTABLE(img_report) || IS_DD_ONLY(img_report)))) {
		enable = FALSE;
	}

	if (remove_checkboxes) {
		if (!enable && (state != 1)) {
			checked = IsChecked(IDC_OLD_BIOS_FIXES);
			CheckDlgButton(hMainDialog, IDC_OLD_BIOS_FIXES, BST_UNCHECKED);
			state = 1;
		} else if (enable && !IsWindowEnabled(hCtrl) && (state != 2)) {
			if (state != 0)
				CheckDlgButton(hMainDialog, IDC_OLD_BIOS_FIXES, checked);
			state = 2;
		}
	}
	EnableWindow(hCtrl, enable);
}

static void EnableUefiValidation(BOOL enable, BOOL remove_checkboxes)
{
	UINT checked = validate_md5sum ? BST_CHECKED : BST_UNCHECKED;
	HWND hCtrl = GetDlgItem(hMainDialog, IDC_UEFI_MEDIA_VALIDATION);

	// The UEFI validation bootloader cannot apply if we don't write an ISO, or if the ISO is not UEFI bootable
	// or if it's a Windows To Go installation or if DD or BIOS/CSM only are enforced.
	if ((boot_type != BT_IMAGE) || (!IS_EFI_BOOTABLE(img_report)) || IS_DD_ONLY(img_report) ||
		((image_options & IMOP_WINTOGO) && (ComboBox_GetCurItemData(hImageOption) == IMOP_WIN_TO_GO)) ||
		((target_type == TT_BIOS) && HAS_WINDOWS(img_report) && (!allow_dual_uefi_bios))) {
		enable = FALSE;
	}

	if (!enable && remove_checkboxes)
		CheckDlgButton(hMainDialog, IDC_UEFI_MEDIA_VALIDATION, BST_UNCHECKED);
	else
		CheckDlgButton(hMainDialog, IDC_UEFI_MEDIA_VALIDATION, checked);
	EnableWindow(hCtrl, enable);
}

static void EnableExtendedLabel(BOOL enable, BOOL remove_checkboxes)
{
	static UINT checked, state = 0;
	HWND hCtrl = GetDlgItem(hMainDialog, IDC_EXTENDED_LABEL);

	if (IS_EXT(fs_type) || ((boot_type == BT_IMAGE) && IS_DD_ONLY(img_report)))
		enable = FALSE;

	if (remove_checkboxes) {
		if (!enable && (state != 1)) {
			checked = IsChecked(IDC_EXTENDED_LABEL);
			CheckDlgButton(hMainDialog, IDC_EXTENDED_LABEL, BST_UNCHECKED);
			state = 1;
		} else if (enable && !IsWindowEnabled(hCtrl) && (state != 2)) {
			if (state != 0)
				CheckDlgButton(hMainDialog, IDC_EXTENDED_LABEL, checked);
			state = 2;
		}
	}
	EnableWindow(hCtrl, enable);
}

static void EnableQuickFormat(BOOL enable, BOOL remove_checkboxes)
{
	static UINT checked, state = 0;
	HWND hCtrl = GetDlgItem(hMainDialog, IDC_QUICK_FORMAT);

	if ((boot_type == BT_IMAGE) && IS_DD_ONLY(img_report))
		enable = FALSE;

	// Disable/restore the quick format control depending on large FAT32 or ReFS
	if (((fs_type == FS_FAT32) && ((SelectedDrive.DiskSize > LARGE_FAT32_SIZE) || (force_large_fat32))) || (fs_type == FS_REFS)) {
		enable = FALSE;
		// Quick Format is the only option for the above
		remove_checkboxes = FALSE;
		CheckDlgButton(hMainDialog, IDC_QUICK_FORMAT, BST_CHECKED);
	}

	if (remove_checkboxes) {
		if (!enable && (state != 1)) {
			checked = IsChecked(IDC_QUICK_FORMAT);
			CheckDlgButton(hMainDialog, IDC_QUICK_FORMAT, BST_UNCHECKED);
			state = 1;
		} else if (enable && !IsWindowEnabled(hCtrl) && (state != 2)) {
			if (state != 0)
				CheckDlgButton(hMainDialog, IDC_QUICK_FORMAT, checked);
			state = 2;
		}
	}
	EnableWindow(hCtrl, enable);
}

static void EnableBootOptions(BOOL enable, BOOL remove_checkboxes)
{
	BOOL actual_enable_bb, actual_enable = enable;

	// If no device is selected, don't enable anything and also don't remove the checkboxes
	if (ComboBox_GetCurSel(hDeviceList) < 0) {
		actual_enable = FALSE;
		remove_checkboxes = FALSE;
	}
	// If boot selection is set to image, but no image is currently selected, don't enable anything
	if ((boot_type == BT_IMAGE) && (image_path == NULL))
		actual_enable = FALSE;
	actual_enable_bb = actual_enable;
	// If we are dealing with a pure DD image, remove all options except Bad Blocks check
	if ((boot_type == BT_IMAGE) && IS_DD_BOOTABLE(img_report) && (!img_report.is_iso))
		actual_enable = FALSE;

	EnableWindow(hImageOption, actual_enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER), actual_enable);
	// Make sure we set the range if we have persistence
	if ((image_path != NULL) && HAS_PERSISTENCE(img_report))
		SetPersistenceSize();
	EnableWindow(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), (persistence_size != 0) && actual_enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_PERSISTENCE_UNITS), (persistence_size != 0) && actual_enable);
	EnableOldBiosFixes(actual_enable, remove_checkboxes);
	EnableUefiValidation(actual_enable, remove_checkboxes);

	EnableWindow(GetDlgItem(hMainDialog, IDC_LABEL), actual_enable);
	if (boot_type == BT_IMAGE) {
		if (IS_DD_ONLY(img_report))
			remove_checkboxes = TRUE;
		else if (image_path == NULL)
			remove_checkboxes = FALSE;
	}
	EnableQuickFormat(actual_enable, remove_checkboxes);
	EnableExtendedLabel(actual_enable, remove_checkboxes);
	EnableWindow(GetDlgItem(hMainDialog, IDC_BAD_BLOCKS), actual_enable_bb);
	EnableWindow(GetDlgItem(hMainDialog, IDC_NB_PASSES), actual_enable_bb);
}

void EnableControls(BOOL enable, BOOL remove_checkboxes)
{
	op_in_progress = !enable;

	// The following only get disabled on format/hash and otherwise remain enabled,
	// even if no device or image are selected
	EnableWindow(hDeviceList, enable);
	EnableWindow(hBootType, enable);
	EnableWindow(hSelectImage, enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_LIST_USB_HDD), enable);
	EnableWindow(hAdvancedDeviceToolbar, enable);
	EnableWindow(hAdvancedFormatToolbar, enable);
	SendMessage(hMultiToolbar, TB_ENABLEBUTTON, (WPARAM)IDC_LANG, (LPARAM)enable);
	SendMessage(hMultiToolbar, TB_ENABLEBUTTON, (WPARAM)IDC_ABOUT, (LPARAM)enable);
	SendMessage(hMultiToolbar, TB_ENABLEBUTTON, (WPARAM)IDC_SETTINGS, (LPARAM)enable);

	// Hash button is enabled if an image has been selected
	EnableWindow(hHashToolbar, enable && (boot_type == BT_IMAGE) && (image_path != NULL));

	// Toggle CLOSE/CANCEL
	SetDlgItemTextU(hMainDialog, IDCANCEL, enable ? uppercase_close : uppercase_cancel);

	// Only enable the following controls if a device is active
	enable = (ComboBox_GetCurSel(hDeviceList) < 0) ? FALSE : enable;
	EnableWindow(hImageOption, enable);
	EnableWindow(hSaveToolbar, enable);

	// Enable or disable the Start button and the other boot options
	enable = ((boot_type == BT_IMAGE) && (image_path == NULL)) ? FALSE : enable;
	EnableWindow(hStart, enable);
	EnableBootOptions(enable, remove_checkboxes);

	// Finally, only enable the half-size dropdowns if we aren't dealing with a pure DD image
	enable = ((boot_type == BT_IMAGE) && (image_path != NULL) &&
		(!(img_report.is_iso || img_report.is_windows_img))) ? FALSE : enable;
	EnableWindow(hPartitionScheme, enable);
	EnableWindow(hTargetSystem, enable);
	EnableWindow(GetDlgItem(hMainDialog, IDS_CSM_HELP_TXT), enable);
	EnableWindow(hFileSystem, enable);
	EnableWindow(hClusterSize, enable);
}

static BOOL PopulateProperties(void)
{
	char* device_tooltip;
	int device_index = ComboBox_GetCurSel(hDeviceList);
	char fs_name[32];

	memset(&SelectedDrive, 0, sizeof(SelectedDrive));
	EnableWindow(hStart, FALSE);

	if (device_index < 0)
		goto out;

	persistence_unit_selection = -1;
	// Get data from the currently selected drive
	SelectedDrive.DeviceNumber = (DWORD)ComboBox_GetItemData(hDeviceList, device_index);
	// This fills the SelectedDrive properties
	GetDrivePartitionData(SelectedDrive.DeviceNumber, fs_name, sizeof(fs_name), FALSE);
	SetPartitionSchemeAndTargetSystem(FALSE);
	// Attempt to reselect the last file system explicitly set by the user
	if (!SetFileSystemAndClusterSize((selected_fs == FS_UNKNOWN) ? fs_name : NULL)) {
		SetProposedLabel(-1);
		uprintf("No file system is selectable for this drive\n");
		return FALSE;
	}

	EnableControls(TRUE, FALSE);

	// Set a proposed label according to the size (eg: "256MB", "8GB")
	static_sprintf(SelectedDrive.proposed_label, "%s",
		SizeToHumanReadable(SelectedDrive.DiskSize, FALSE, TRUE));

	// Add a tooltip (with the size of the device in parenthesis)
	device_tooltip = (char*) malloc(safe_strlen(rufus_drive[device_index].name) + 32);
	if (device_tooltip != NULL) {
		if (right_to_left_mode)
			safe_sprintf(device_tooltip, safe_strlen(rufus_drive[device_index].name) + 32, "(%s) %s",
				SizeToHumanReadable(SelectedDrive.DiskSize, FALSE, FALSE), rufus_drive[device_index].name);
		else
			safe_sprintf(device_tooltip, safe_strlen(rufus_drive[device_index].name) + 32, "%s (%s)",
				rufus_drive[device_index].name, SizeToHumanReadable(SelectedDrive.DiskSize, FALSE, FALSE));
		CreateTooltip(hDeviceList, device_tooltip, -1);
		free(device_tooltip);
	}

out:
	SetProposedLabel(device_index);
	return TRUE;
}

static void UpdateImage(BOOL update_image_option_only)
{
	if_assert_fails(image_index != 0)
		return;

	if (!update_image_option_only) {
		if (ComboBox_GetItemData(hBootType, image_index) == BT_IMAGE)
			ComboBox_DeleteString(hBootType, image_index);
		ComboBox_InsertStringU(hBootType, image_index,
			(image_path == NULL) ? lmprintf(MSG_281, lmprintf(MSG_280)) : short_image_path);
		ComboBox_SetItemData(hBootType, image_index, BT_IMAGE);
		IGNORE_RETVAL(ComboBox_SetCurSel(hBootType, image_index));
		boot_type = (int)ComboBox_GetCurItemData(hBootType);
		SetBootTypeDropdownWidth();
	}

	ComboBox_ResetContent(hImageOption);

	if (!img_report.is_windows_img)	// Straight install.wim/install.esd only have Windows To Go option
		IGNORE_RETVAL(ComboBox_SetItemData(hImageOption, ComboBox_AddStringU(hImageOption, lmprintf(MSG_117)), IMOP_WIN_STANDARD));
	IGNORE_RETVAL(ComboBox_SetItemData(hImageOption, ComboBox_AddStringU(hImageOption, lmprintf(MSG_118)), IMOP_WIN_TO_GO));
	IGNORE_RETVAL(ComboBox_SetCurSel(hImageOption, imop_win_sel));
}

/* ----------------------------- END UPSTREAM (rufus.c) ----------------------------- */

/* ----------------------------- BEGIN UPSTREAM (ui.c) ------------------------------ */

void SetComboEntry(HWND hDlg, int data)
{
	int i, nb_entries = ComboBox_GetCount(hDlg);

	if (nb_entries <= 0)
		return;
	for (i = 0; i < nb_entries; i++) {
		if (ComboBox_GetItemData(hDlg, i) == data) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hDlg, i));
			return;
		}
	}
	if (i == nb_entries)
		IGNORE_RETVAL(ComboBox_SetCurSel(hDlg, 0));
}

void SetPersistencePos(uint64_t pos)
{
	char tmp[64];

	if ((boot_type == BT_IMAGE) && (pos != 0)) {
		TogglePersistenceControls(TRUE);
		static_sprintf(tmp, "%ld", (LONG)pos);
	} else {
		TogglePersistenceControls(FALSE);
		static_sprintf(tmp, "0 (%s)", lmprintf(MSG_124));
	}
	app_changed_size = TRUE;
	SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
}

void SetPersistenceSize(void)
{
	int i, proposed_unit_selection = 0;
	LONGLONG base_unit = MB;
	HWND hCtrl;
	uint64_t max = 0, pos = 0;

	if (ComboBox_GetCurSel(hDeviceList) >= 0) {
		max = SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size);
		persistence_size = min(persistence_size, max);
		pos = persistence_size;

		// Reset the the Persistence Units dropdown
		hCtrl = GetDlgItem(hMainDialog, IDC_PERSISTENCE_UNITS);
		IGNORE_RETVAL(ComboBox_ResetContent(hCtrl));
		for (i = 0; i < 3; i++) {
			IGNORE_RETVAL(ComboBox_SetItemData(hCtrl, ComboBox_AddStringU(hCtrl, lmprintf(MSG_022 + i)), i));
			// If we have more than 7 discrete positions, set this unit as our base
			if (SelectedDrive.DiskSize > 7 * base_unit)
				proposed_unit_selection = i;
			base_unit *= 1024;
			// Don't allow a base unit unless the drive is at least twice the size of that unit
			if (SelectedDrive.DiskSize < 2 * base_unit)
				break;
		}
		if (persistence_unit_selection < 0)
			persistence_unit_selection = proposed_unit_selection;

		IGNORE_RETVAL(ComboBox_SetCurSel(hCtrl, persistence_unit_selection));
		if ((pos != 0) && (pos < MIN_EXT_SIZE))
			pos = MIN_EXT_SIZE;
		pos /= MB;
		max /= MB;
		for (i = 0; i < persistence_unit_selection; i++) {
			pos /= 1024;
			max /= 1024;
		}

	}

	hCtrl = GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER);
	// Wow! Unless you set *all* these redraw WPARAMs to true, the one from
	// TBM_SETPOS gets completely ignored if the value is zero!
	SendMessage(hCtrl, TBM_SETRANGEMIN, (WPARAM)TRUE, (LPARAM)0);
	SendMessage(hCtrl, TBM_SETRANGEMAX, (WPARAM)TRUE, (LPARAM)max);
	SendMessage(hCtrl, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)pos);

	SetPersistencePos(pos);
}

void ToggleImageOptions(void)
{
	BOOL has_wintogo, has_persistence;
	uint8_t entry_image_options = image_options;
	int i, shift = rh;

	has_wintogo = ((boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.is_iso || img_report.is_windows_img) &&
		(WindowsVersion.Version >= WINDOWS_8) && (HAS_WINTOGO(img_report)));
	has_persistence = ((boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.is_iso) && (HAS_PERSISTENCE(img_report)));

	assert(popcnt8(image_options) <= 1);

	// Keep a copy of the "Image Option" text (so that we don't have to duplicate its translation in the .loc)
	if (image_option_txt[0] == 0)
		GetWindowTextU(GetDlgItem(hMainDialog, IDS_IMAGE_OPTION_TXT), image_option_txt, sizeof(image_option_txt));

	if (((has_wintogo) && !(image_options & IMOP_WINTOGO)) ||
		((!has_wintogo) && (image_options & IMOP_WINTOGO))) {
		image_options ^= IMOP_WINTOGO;
		if (image_options & IMOP_WINTOGO) {
			SetWindowTextU(GetDlgItem(hMainDialog, IDS_IMAGE_OPTION_TXT), image_option_txt);
			// Set the Windows To Go selection in the dropdown
			IGNORE_RETVAL(ComboBox_SetCurSel(hImageOption, imop_win_sel));
		}
	}

	if (((has_persistence) && !(image_options & IMOP_PERSISTENCE)) ||
		((!has_persistence) && (image_options & IMOP_PERSISTENCE))) {
		image_options ^= IMOP_PERSISTENCE;
		if (image_options & IMOP_PERSISTENCE) {
			SetWindowTextU(GetDlgItem(hMainDialog, IDS_IMAGE_OPTION_TXT), lmprintf(MSG_123));
			TogglePersistenceControls(persistence_size != 0);
			SetPersistenceSize();
		}
	}

	if ( ((entry_image_options != 0) && (has_wintogo || has_persistence)) ||
		 ((entry_image_options == 0) && !(has_wintogo || has_persistence)) )
		shift = 0;

	if (shift != 0) {
		if (entry_image_options != 0)
			shift = -shift;
		section_vpos[1] += shift;
		section_vpos[2] += shift;

		for (i = 0; i < ARRAYSIZE(image_option_move_ids); i++)
			MoveCtrlY(hMainDialog, image_option_move_ids[i], shift);

		// Resize the main dialog and log window
		ResizeDialogs(shift);
	}

	// Hide or show the boot options
	for (i = 0; i < ARRAYSIZE(image_option_toggle_ids); i++) {
		ShowWindow(GetDlgItem(hMainDialog, image_option_toggle_ids[i][0]),
			(image_options & image_option_toggle_ids[i][1]) ? SW_SHOW : SW_HIDE);
	}
	// If you don't force a redraw here, all kind of bad UI artifacts happen...
	InvalidateRect(hMainDialog, NULL, TRUE);
}

/* ------------------------------ END UPSTREAM (ui.c) ------------------------------- */

/* MainCallback(), WM_COMMAND part */
static INT_PTR MainCommand(HWND hDlg, WPARAM wParam, LPARAM lParam)
{
	int i, nb_devices, nDeviceIndex;
	BOOL set_selected_fs;
	DWORD DeviceNum;
	LONG lPos;
	char tmp[128];

	switch (LOWORD(wParam)) {
/* ---------------------------- BEGIN UPSTREAM (rufus.c) ---------------------------- */
		case IDC_ADVANCED_DRIVE_PROPERTIES:
			advanced_mode_device = !advanced_mode_device;
			WriteSettingBool(SETTING_ADVANCED_MODE_DEVICE, advanced_mode_device);
			ToggleAdvancedDeviceOptions(advanced_mode_device);
			SetBootOptions();
			boot_type = (int)ComboBox_GetCurItemData(hBootType);
			EnableControls(TRUE, FALSE);
			SetFileSystemAndClusterSize(NULL);
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL<<16) | IDC_FILE_SYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			break;
		case IDC_ADVANCED_FORMAT_OPTIONS:
			advanced_mode_format = !advanced_mode_format;
			WriteSettingBool(SETTING_ADVANCED_MODE_FORMAT, advanced_mode_format);
			ToggleAdvancedFormatOptions(advanced_mode_format);
			if (selected_fs == FS_UNKNOWN)
				selected_fs = (int)ComboBox_GetCurItemData(hFileSystem);
			SetFileSystemAndClusterSize(NULL);
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL << 16) | IDC_FILE_SYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			break;
		case IDC_LABEL:
			if (HIWORD(wParam) == EN_CHANGE) {
				// We will get EN_CHANGE when we change the label automatically, so we need to detect that
				if (!app_changed_label)
					user_changed_label = TRUE;
				app_changed_label = FALSE;
			}
			break;
		case IDC_DEVICE:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			nb_devices = ComboBox_GetCount(hDeviceList);
			PrintStatusDebug(0, (nb_devices == 1) ? MSG_208 : MSG_209, nb_devices);
			PopulateProperties();
			nDeviceIndex = ComboBox_GetCurSel(hDeviceList);
			DeviceNum = (nDeviceIndex == CB_ERR) ? 0 : (DWORD)ComboBox_GetItemData(hDeviceList, nDeviceIndex);
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL << 16) | IDC_FILE_SYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			if (nb_devices == 0) {
				// No need to run the process search if no device is selected
				StopProcessSearch();
			} else if (!StartProcessSearch() || !SetProcessSearch(DeviceNum)) {
				uprintf("Failed to start conflicting process search");
				StopProcessSearch();
			}
			break;
		case IDC_IMAGE_OPTION:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			SetFileSystemAndClusterSize(NULL);
			imop_win_sel = ComboBox_GetCurSel(hImageOption);
			EnableUefiValidation((imop_win_sel == 0), TRUE);
			break;
		case IDC_PERSISTENCE_SIZE:
			if (HIWORD(wParam) == EN_CHANGE) {
				uint64_t pos;
				// We get EN_CHANGE when we change the size automatically, so we need to detect that
				if (app_changed_size) {
					app_changed_size = FALSE;
					break;
				}
				GetWindowTextA(GetDlgItem(hDlg, IDC_PERSISTENCE_SIZE), tmp, sizeof(tmp));
				lPos = strtol(tmp, NULL, 10);
				persistence_unit_selection = ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_PERSISTENCE_UNITS));
				persistence_size = lPos * MB;
				for (i = 0; i < persistence_unit_selection; i++)
					persistence_size *= 1024;
				if (persistence_size > SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size))
					persistence_size = SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size);
				pos = persistence_size / MB;
				for (i = 0; i < persistence_unit_selection; i++)
					pos /= 1024;
				lPos = (LONG)pos;
				SendMessage(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER), TBM_SETPOS, TRUE, lPos);
				if (persistence_size >= (SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size))) {
					static_sprintf(tmp, "%ld", lPos);
					app_changed_size = TRUE;
					SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
				}
			} else if (HIWORD(wParam) == EN_KILLFOCUS) {
				if (persistence_size == 0) {
					TogglePersistenceControls(FALSE);
					static_sprintf(tmp, "0 (%s)", lmprintf(MSG_124));
					app_changed_size = TRUE;
					SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
				} else if (persistence_size < MIN_EXT_SIZE) {
					persistence_size = MIN_EXT_SIZE;
					uint64_t pos = persistence_size / MB;
					for (i = 0; i < persistence_unit_selection; i++)
						pos /= 1024;
					lPos = (LONG)pos;
					SendMessage(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER), TBM_SETPOS, TRUE, lPos);
					static_sprintf(tmp, "%ld", lPos);
					app_changed_size = TRUE;
					SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
				}
			}
			break;
		case IDC_PERSISTENCE_UNITS:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			if (ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_PERSISTENCE_UNITS)) == persistence_unit_selection)
				break;
			GetWindowTextA(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp, sizeof(tmp));
			persistence_size = strtol(tmp, NULL, 10) * MB;
			for (i = 0; i < persistence_unit_selection; i++)
				persistence_size *= 1024;
			persistence_unit_selection = ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_PERSISTENCE_UNITS));
			SetPersistenceSize();
			break;
		case IDC_NB_PASSES:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			SetPassesTooltip();
			break;
		case IDC_TARGET_SYSTEM:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			target_type = (int)ComboBox_GetCurItemData(hTargetSystem);
			SendMessage(hMainDialog, UM_UPDATE_CSM_TOOLTIP, 0, 0);
			SetFileSystemAndClusterSize(NULL);
			EnableUefiValidation(TRUE, TRUE);
			break;
		case IDC_PARTITION_TYPE:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			partition_type = (int)ComboBox_GetCurItemData(hPartitionScheme);
			SetPartitionSchemeAndTargetSystem(TRUE);
			SetFileSystemAndClusterSize(NULL);
			EnableOldBiosFixes(TRUE, TRUE);
			EnableUefiValidation(TRUE, TRUE);
			selected_pt = partition_type;
			break;
		case IDC_FILE_SYSTEM:
			if ((HIWORD(wParam) != CBN_SELCHANGE) && (HIWORD(wParam) != CBN_SELCHANGE_INTERNAL))
				break;
			if (!IsWindowEnabled(hFileSystem))
				break;
			set_selected_fs = (HIWORD(wParam) == CBN_SELCHANGE);
			fs_type = (int)ComboBox_GetCurItemData(hFileSystem);
			SetClusterSizes(fs_type);
			if (set_selected_fs && (fs_type > 0))
				selected_fs = fs_type;
			// Some FS's (such as ReFS or Large FAT32) only have QuickFormat so make sure we reflect that
			EnableQuickFormat(TRUE, TRUE);
			EnableOldBiosFixes(TRUE, TRUE);
			EnableExtendedLabel(TRUE, TRUE);
			break;
		case IDC_BOOT_SELECTION:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			boot_type = (int)ComboBox_GetCurItemData(hBootType);
			if ((HIWORD(wParam) != CBN_SELCHANGE) || (boot_type == selection_default))
				break;
			selection_default = boot_type;
			SetPartitionSchemeAndTargetSystem(FALSE);
			// Try to reselect current FS from the drive for non-bootable
			tmp[0] = 0;
			if ((selected_fs == FS_UNKNOWN) && (SelectedDrive.DeviceNumber != 0))
				GetDrivePartitionData(SelectedDrive.DeviceNumber, tmp, sizeof(tmp), !usb_debug);
			SetFileSystemAndClusterSize(tmp);
			ToggleImageOptions();
			SetProposedLabel(ComboBox_GetCurSel(hDeviceList));
			EnableControls(TRUE, TRUE);
			target_type = (int)ComboBox_GetCurItemData(hPartitionScheme);
			partition_type = (int)ComboBox_GetCurItemData(hTargetSystem);
			return (INT_PTR)TRUE;
		case IDC_UEFI_MEDIA_VALIDATION:
			if ((HIWORD(wParam)) == BN_CLICKED) {
				validate_md5sum = IsChecked(IDC_UEFI_MEDIA_VALIDATION);
//				WriteSettingBool(SETTING_ENABLE_RUNTIME_VALIDATION, validate_md5sum);
			}
			break;
/* ----------------------------- END UPSTREAM (rufus.c) ----------------------------- */
	}
	return (INT_PTR)TRUE;
}

/* The tail of GetDevices() (dev.c), for a device list of zero or one drive */
static void GetDevices(const oracle_drive_t* drive)
{
	ComboBox_ResetContent(hDeviceList);
	memset(rufus_drive, 0, sizeof(rufus_drive));
	if (drive != NULL) {
		current_drive = *drive;
		rufus_drive[0].index = DRIVE_INDEX_MIN;
		rufus_drive[0].name = "Mock USB Device";
		rufus_drive[0].label = current_drive.label;
		ComboBox_SetItemData(hDeviceList, ComboBox_AddStringU(hDeviceList, rufus_drive[0].name), rufus_drive[0].index);
	}
	IGNORE_RETVAL(ComboBox_SetCurSel(hDeviceList, 0));
	SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_DEVICE, 0);
}

void oracle_init(const oracle_env_t* env)
{
	mock_reset();
	memset(&img_report, 0, sizeof(img_report));
	memset(&SelectedDrive, 0, sizeof(SelectedDrive));
	memset(&current_drive, 0, sizeof(current_drive));
	safe_free(image_path);
	short_image_path = SNAP_IMAGE_NAME;
	static_strcpy(embedded_sl_version_str[0], SNAP_SL_V4);
	static_strcpy(embedded_sl_version_str[1], SNAP_SL_V6);
	fs_type = boot_type = partition_type = target_type = 0;
	image_options = IMOP_WINTOGO;
	persistence_size = 0;
	op_in_progress = TRUE;
	has_uefi_csm = validate_md5sum = app_changed_size = enable_HDDs = FALSE;
	user_changed_label = app_changed_label = dont_display_image_name = FALSE;
	selection_default = BT_IMAGE;
	persistence_unit_selection = -1;
	imop_win_sel = 0;
	default_fs = 0;
	selected_pt = -1;
	image_index = 0;
	image_option_txt[0] = 0;
	right_to_left_mode = FALSE;
	rh = 0;

	advanced_mode_device = env->advanced_mode_device;
	advanced_mode_format = env->advanced_mode_format;
	allow_dual_uefi_bios = env->allow_dual_uefi_bios;
	force_large_fat32 = env->force_large_fat32;
	// Set up a Windows host that matches the requested capabilities
	WindowsVersion.Version = env->wintogo_available ? WINDOWS_10 : WINDOWS_7;
	WindowsVersion.BuildNumber = 19045;
	WindowsVersion.Edition = 0x0000000A;	// Enterprise Server
	use_vds = env->refs_available;
	if (env->refs_available)
		WindowsVersion.Version = env->wintogo_available ? WINDOWS_10 : WINDOWS_8_1;
	// The --filesystem option sets both
	preselected_fs = env->preselected_fs;
	selected_fs = preselected_fs;
}

void oracle_startup(const oracle_drive_t* drive)
{
	// InitDialog()
	SetBootOptions();
	CheckDlgButton(hMainDialog, IDC_QUICK_FORMAT, BST_CHECKED);
	CheckDlgButton(hMainDialog, IDC_EXTENDED_LABEL, BST_CHECKED);
	ToggleImageOptions();
	// WM_INITDIALOG
	GetDevices(drive);
	EnableControls(TRUE, FALSE);
}

void oracle_device(const oracle_drive_t* drive)
{
	GetDevices(drive);
}

// WM_DEVICECHANGE
void oracle_device_refresh(const oracle_drive_t* drive)
{
	GetDevices(drive);
	EnableControls(TRUE, FALSE);
	if (ComboBox_GetCurSel(hDeviceList) < 0) {
		SetPartitionSchemeAndTargetSystem(FALSE);
		SetFileSystemAndClusterSize(NULL);
		ShowWindow(GetDlgItem(hMainDialog, IDS_CSM_HELP_TXT), SW_HIDE);
		persistence_unit_selection = -1;
	}
}

static void SelectAndNotify(HWND h, int index)
{
	ComboBox_SetCurSel(h, index);
	SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE << 16) | h->id, 0);
}

void oracle_boot_selection(int index) { SelectAndNotify(hBootType, index); }
void oracle_image_option(int index) { SelectAndNotify(hImageOption, index); }
void oracle_partition_scheme(int index) { SelectAndNotify(hPartitionScheme, index); }
void oracle_target_system(int index) { SelectAndNotify(hTargetSystem, index); }
void oracle_file_system(int index) { SelectAndNotify(hFileSystem, index); }
void oracle_cluster_size(int index) { SelectAndNotify(hClusterSize, index); }
void oracle_label(const char* label) { SetWindowTextU(hLabel, label); }

void oracle_check(enum oracle_check_id id)
{
	static const int ids[] = { IDC_QUICK_FORMAT, IDC_EXTENDED_LABEL, IDC_OLD_BIOS_FIXES, IDC_UEFI_MEDIA_VALIDATION };
	HWND h = GetDlgItem(hMainDialog, ids[id]);

	if (!h->enabled)
		return;
	h->checked = (h->checked == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED;
	SendMessage(hMainDialog, WM_COMMAND, (BN_CLICKED << 16) | ids[id], 0);
}

void oracle_toggle_advanced_device(void)
{
	SendMessage(hMainDialog, WM_COMMAND, (BN_CLICKED << 16) | IDC_ADVANCED_DRIVE_PROPERTIES, 0);
}

void oracle_toggle_advanced_format(void)
{
	SendMessage(hMainDialog, WM_COMMAND, (BN_CLICKED << 16) | IDC_ADVANCED_FORMAT_OPTIONS, 0);
}

// Alt-E (from the main message loop)
void oracle_toggle_dual_uefi_bios(void)
{
	allow_dual_uefi_bios = !allow_dual_uefi_bios;
	SetPartitionSchemeAndTargetSystem(FALSE);
	SetFileSystemAndClusterSize(NULL);
}

// IDC_SELECT, then the start of ImageScanThread()
void oracle_image_scan_start(void)
{
	safe_free(image_path);
	image_path = strdup(SNAP_IMAGE_NAME);
	EnableControls(FALSE, FALSE);
	memset(&img_report, 0, sizeof(img_report));
	ComboBox_ResetContent(hImageOption);
	imop_win_sel = 0;
}

// The end of ImageScanThread(), with the image analysis replaced by the provided report
void oracle_image_scan_end(const RUFUS_IMG_REPORT* report, int result, BOOL keep_image_name)
{
	img_report = *report;
	dont_display_image_name = keep_image_name;
	if (result == 1) {
		safe_free(image_path);
		UpdateImage(FALSE);
		PopulateProperties();
		EnableControls(TRUE, FALSE);
		goto out;
	}
	if (img_report.is_windows_img)
		selection_default = BT_IMAGE;
	else if (IS_DD_BOOTABLE(img_report))
		selection_default = BT_IMAGE;
	if (img_report.is_iso)
		selection_default = BT_IMAGE;
	if (result == 2) {
		safe_free(image_path);
		EnableControls(TRUE, FALSE);
	} else {
		UpdateImage(dont_display_image_name);
		ToggleImageOptions();
		// Set Target and FS accordingly
		if (img_report.is_iso || img_report.is_windows_img) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hBootType, image_index));
			SetPartitionSchemeAndTargetSystem(FALSE);
			SetFileSystemAndClusterSize(NULL);
			SetFSFromISO();
			user_changed_label = FALSE;
			SetProposedLabel(ComboBox_GetCurSel(hDeviceList));
		} else {
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL << 16) | IDC_FILE_SYSTEM,
				ComboBox_GetCurSel(hFileSystem));
		}
		EnableControls(TRUE, FALSE);
	}
out:
	dont_display_image_name = FALSE;
}

// IDC_START, up to the creation of the boot check thread
void oracle_start(void)
{
	boot_type = (int)ComboBox_GetCurItemData(hBootType);
	partition_type = (int)ComboBox_GetCurItemData(hPartitionScheme);
	target_type = (int)ComboBox_GetCurItemData(hTargetSystem);
	fs_type = (int)ComboBox_GetCurItemData(hFileSystem);
	EnableControls(FALSE, FALSE);
	selection_default = (int)ComboBox_GetCurItemData(hBootType);
}

// UM_FORMAT_COMPLETED
void oracle_operation_end(void)
{
	EnableControls(TRUE, FALSE);
}

static void snap_combo(snap_combo_t* s, HWND h)
{
	int i;

	s->count = h->count;
	s->sel = h->sel;
	for (i = 0; i < h->count; i++) {
		static_strcpy(s->item[i].text, h->item[i].text);
		s->item[i].data = h->item[i].data;
	}
}

static void snap_check(snap_check_t* s, int id)
{
	HWND h = GetDlgItem(hMainDialog, id);

	s->enabled = h->enabled;
	s->checked = (h->checked == BST_CHECKED);
}

void oracle_snapshot(snapshot_t* s)
{
	HWND hUnits = GetDlgItem(hMainDialog, IDC_PERSISTENCE_UNITS);
	HWND hSlider = GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER);

	memset(s, 0, sizeof(*s));
	snap_combo(&s->boot, hBootType);
	snap_combo(&s->image_option, hImageOption);
	snap_combo(&s->partition, hPartitionScheme);
	snap_combo(&s->target, hTargetSystem);
	snap_combo(&s->fs, hFileSystem);
	snap_combo(&s->cluster, hClusterSize);
	snap_check(&s->quick_format, IDC_QUICK_FORMAT);
	snap_check(&s->extended_label, IDC_EXTENDED_LABEL);
	snap_check(&s->old_bios_fixes, IDC_OLD_BIOS_FIXES);
	snap_check(&s->uefi_validation, IDC_UEFI_MEDIA_VALIDATION);
	s->device_enabled = hDeviceList->enabled;
	s->boot_enabled = hBootType->enabled;
	s->hash_enabled = hHashToolbar->enabled;
	s->save_enabled = hSaveToolbar->enabled;
	s->start_enabled = hStart->enabled;
	s->image_option_enabled = hImageOption->enabled;
	s->persistence_slider_enabled = hSlider->enabled;
	s->persistence_size_enabled = GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE)->enabled;
	s->persistence_units_enabled = hUnits->enabled;
	s->persistence_units_visible = hUnits->visible;
	s->label_enabled = GetDlgItem(hMainDialog, IDC_LABEL)->enabled;
	s->bad_blocks_enabled = GetDlgItem(hMainDialog, IDC_BAD_BLOCKS)->enabled;
	s->partition_enabled = hPartitionScheme->enabled;
	s->target_enabled = hTargetSystem->enabled;
	s->fs_enabled = hFileSystem->enabled;
	s->cluster_enabled = hClusterSize->enabled;
	static_strcpy(s->label, hLabel->text);
	s->persistence_units = hUnits->count;
	s->persistence_unit_sel = hUnits->sel;
	s->persistence_max = (uint64_t)hSlider->range_max;
	s->persistence_pos = (uint64_t)hSlider->pos;
	s->persistence_size = persistence_size;
	s->g_selection_default = selection_default;
	s->g_boot_type = boot_type;
	s->g_partition_type = partition_type;
	s->g_target_type = target_type;
	s->g_fs_type = fs_type;
	s->g_selected_pt = selected_pt;
	s->g_selected_fs = selected_fs;
	s->g_imop_win_sel = imop_win_sel;
	s->g_image_options = image_options;
	s->g_has_uefi_csm = has_uefi_csm;
	s->g_op_in_progress = op_in_progress;
	s->g_validate_md5sum = validate_md5sum;
	s->g_user_changed_label = user_changed_label;
}

/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual Disk Handling functions
 * Copyright © 2013-2026 Pete Batard <pete@akeo.ie>
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
 * NB: These functions come from vhd.c, which is now part of the core. They
 * mount VHD/VHDX images through VirtDisk, and save drives to VHD, VHDX or FFU,
 * which are stage 4 features of the Linux port (FFU is not feasible), and are
 * not ported yet. The VirtDisk definitions that upstream has in vhd.h are kept
 * here for that reason.
 */

#if 0	// From vhd.h
#define VIRTUAL_STORAGE_TYPE_DEVICE_FFU                    99
#define CREATE_VIRTUAL_DISK_VERSION_2                       2
#define CREATE_VIRTUAL_DISK_FLAG_CREATE_BACKING_STORAGE     8

#ifndef CREATE_VIRTUAL_DISK_PARAMETERS_DEFAULT_BLOCK_SIZE
#define CREATE_VIRTUAL_DISK_PARAMETERS_DEFAULT_BLOCK_SIZE   0
#endif

#ifndef CREATE_VIRTUAL_DISK_PARAMETERS_DEFAULT_SECTOR_SIZE
#define CREATE_VIRTUAL_DISK_PARAMETERS_DEFAULT_SECTOR_SIZE  0
#endif

typedef struct
{
	CREATE_VIRTUAL_DISK_VERSION Version;
	union
	{
		struct
		{
			GUID                  UniqueId;
			ULONGLONG             MaximumSize;
			ULONG                 BlockSizeInBytes;
			ULONG                 SectorSizeInBytes;
			PCWSTR                ParentPath;
			PCWSTR                SourcePath;
		} Version1;
		struct
		{
			GUID                   UniqueId;
			ULONGLONG              MaximumSize;
			ULONG                  BlockSizeInBytes;
			ULONG                  SectorSizeInBytes;
			ULONG                  PhysicalSectorSizeInBytes;
			PCWSTR                 ParentPath;
			PCWSTR                 SourcePath;
			OPEN_VIRTUAL_DISK_FLAG OpenFlags;
			VIRTUAL_STORAGE_TYPE   ParentVirtualStorageType;
			VIRTUAL_STORAGE_TYPE   SourceVirtualStorageType;
			GUID                   ResiliencyGuid;
		} Version2;
	};
} STOPGAP_CREATE_VIRTUAL_DISK_PARAMETERS;

// From https://docs.microsoft.com/en-us/previous-versions/msdn10/dd834960(v=msdn.10)
// as well as https://msfn.org/board/topic/150700-wimgapi-wimmountimage-progressbar/
enum WIMMessage {
	WIM_MSG = WM_APP + 0x1476,
	WIM_MSG_TEXT,
	WIM_MSG_PROGRESS,	// Indicates an update in the progress of an image application.
	WIM_MSG_PROCESS,	// Enables the caller to prevent a file or a directory from being captured or applied.
	WIM_MSG_SCANNING,	// Indicates that volume information is being gathered during an image capture.
	WIM_MSG_SETRANGE,	// Indicates the number of files that will be captured or applied.
	WIM_MSG_SETPOS,		// Indicates the number of files that have been captured or applied.
	WIM_MSG_STEPIT,		// Indicates that a file has been either captured or applied.
	WIM_MSG_COMPRESS,	// Enables the caller to prevent a file resource from being compressed during a capture.
	WIM_MSG_ERROR,		// Alerts the caller that an error has occurred while capturing or applying an image.
	WIM_MSG_ALIGNMENT,	// Enables the caller to align a file resource on a particular alignment boundary.
	WIM_MSG_RETRY,		// Sent when the file is being reapplied because of a network timeout.
	WIM_MSG_SPLIT,		// Enables the caller to align a file resource on a particular alignment boundary.
	WIM_MSG_FILEINFO,	// Used in conjunction with WimApplyImages()'s WIM_FLAG_FILEINFO flag to provide detailed file info.
	WIM_MSG_INFO,		// Sent when an info message is available.
	WIM_MSG_WARNING,	// Sent when a warning message is available.
	WIM_MSG_CHK_PROCESS,
	WIM_MSG_SUCCESS = 0,
	WIM_MSG_ABORT_IMAGE = -1
};

extern uint32_t GetWimVersion(const char* image);
extern BOOL WimExtractFile(const char* wim_image, int index, const char* src, const char* dst);
extern BOOL WimApplyImage(const char* image, int index, const char* dst);
extern BOOL WimSplitFile(const char* src, const char* dst);
extern int8_t IsBootableImage(const char* path);
extern char* VhdMountImageAndGetSize(const char* path, uint64_t* disksize);
#define VhdMountImage(path) VhdMountImageAndGetSize(path, NULL)
extern void VhdUnmountImage(void);
extern BOOL SaveImage(void);
extern void OpticalDiscSaveImage(void);
extern DWORD WINAPI IsoSaveImageThread(void* param);
#endif

// Mount an ISO or a VHD/VHDX image and provide its size
// Returns the physical path of the mounted image or NULL on error.
char* VhdMountImageAndGetSize(const char* path, uint64_t* disk_size)
{
	VIRTUAL_STORAGE_TYPE vtype = { VIRTUAL_STORAGE_TYPE_DEVICE_ISO, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT };
	ATTACH_VIRTUAL_DISK_PARAMETERS vparams = { 0 };
	GET_VIRTUAL_DISK_INFO disk_info = { 0 };
	DWORD r;
	wchar_t wtmp[128];
	ULONG size = ARRAYSIZE(wtmp);
	wconvert(path);
	char *ret = NULL, *ext = NULL;

	if (wpath == NULL)
		return NULL;

	if ((mounted_handle != NULL) && (mounted_handle != INVALID_HANDLE_VALUE))
		VhdUnmountImage();

	if (safe_strlen(path) > 4)
		for (ext = (char*)&path[safe_strlen(path) - 1]; (*ext != '.') && (ext != path); ext--);
	if (safe_stricmp(ext, ".vhdx") == 0)
		vtype.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
	else if (safe_stricmp(ext, ".vhd") == 0)
		vtype.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;

	r = OpenVirtualDisk(&vtype, wpath, VIRTUAL_DISK_ACCESS_READ | VIRTUAL_DISK_ACCESS_GET_INFO,
		OPEN_VIRTUAL_DISK_FLAG_NONE, NULL, &mounted_handle);
	if (r != ERROR_SUCCESS) {
		SetLastError(r);
		uprintf("Could not open image '%s': %s", path, WindowsErrorString());
		goto out;
	}

	vparams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
	r = AttachVirtualDisk(mounted_handle, NULL, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
		ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &vparams, NULL);
	if (r != ERROR_SUCCESS) {
		SetLastError(r);
		uprintf("Could not mount image '%s': %s", path, WindowsErrorString());
		goto out;
	}

	r = GetVirtualDiskPhysicalPath(mounted_handle, &size, wtmp);
	if (r != ERROR_SUCCESS) {
		SetLastError(r);
		uprintf("Could not obtain physical path for mounted image '%s': %s", path, WindowsErrorString());
		goto out;
	}
	wchar_to_utf8_no_alloc(wtmp, physical_path, sizeof(physical_path));

	if (disk_size != NULL) {
		*disk_size = 0;
		disk_info.Version = GET_VIRTUAL_DISK_INFO_SIZE;
		size = sizeof(disk_info);
		r = GetVirtualDiskInformation(mounted_handle, &size, &disk_info, NULL);
		if (r != ERROR_SUCCESS) {
			SetLastError(r);
			uprintf("Could not obtain virtual size of mounted image '%s': %s", path, WindowsErrorString());
			goto out;
		}
		*disk_size = disk_info.Size.VirtualSize;
	}

	ret = physical_path;

out:
	if (ret == NULL)
		VhdUnmountImage();
	wfree(path);
	return ret;
}

void VhdUnmountImage(void)
{
	if ((mounted_handle == NULL) || (mounted_handle == INVALID_HANDLE_VALUE))
		goto out;

	DetachVirtualDisk(mounted_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
	safe_closehandle(mounted_handle);
out:
	physical_path[0] = 0;
}

// Since we no longer have to deal with Windows 7, we can call on CreateVirtualDisk()
// to backup a physical disk to VHD/VHDX. Now if this could also be used to create an
// ISO from optical media that would be swell, but no matter what I tried, it didn't
// seem possible...
static DWORD WINAPI VhdSaveImageThread(void* param)
{
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	HANDLE handle = INVALID_HANDLE_VALUE;
	WCHAR* wSrc = utf8_to_wchar(img_save->DevicePath);
	WCHAR* wDst = utf8_to_wchar(img_save->ImagePath);
	VIRTUAL_STORAGE_TYPE vtype = { img_save->Type, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT };
	STOPGAP_CREATE_VIRTUAL_DISK_PARAMETERS vparams = { 0 };
	VIRTUAL_DISK_PROGRESS vprogress = { 0 };
	OVERLAPPED overlapped = { 0 };
	DWORD r = ERROR_NOT_FOUND, flags, bytes_read = 0;

	if_assert_fails(img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_VHD ||
		img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_VHDX)
		return ERROR_INVALID_PARAMETER;

	UpdateProgressWithInfoInit(NULL, FALSE);

	vparams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
	vparams.Version2.UniqueId = GUID_NULL;
	vparams.Version2.BlockSizeInBytes = CREATE_VIRTUAL_DISK_PARAMETERS_DEFAULT_BLOCK_SIZE;
	vparams.Version2.SectorSizeInBytes = CREATE_VIRTUAL_DISK_PARAMETERS_DEFAULT_SECTOR_SIZE;
	vparams.Version2.PhysicalSectorSizeInBytes = SelectedDrive.SectorSize;
	vparams.Version2.SourcePath = wSrc;

	// When CREATE_VIRTUAL_DISK_FLAG_CREATE_BACKING_STORAGE is specified with
	// a source path, CreateVirtualDisk() automatically clones the source to
	// the virtual disk.
	flags = CREATE_VIRTUAL_DISK_FLAG_CREATE_BACKING_STORAGE;
	// The following ensures that VHD images are stored uncompressed and can
	// be used as DD images.
	if (img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_VHD)
		flags |= CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION;

	overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);

	// CreateVirtualDisk() does not have an overwrite flag...
	DeleteFileW(wDst);

	r = CreateVirtualDisk(&vtype, wDst, VIRTUAL_DISK_ACCESS_NONE, NULL,
		flags, 0, (PCREATE_VIRTUAL_DISK_PARAMETERS)&vparams, &overlapped, &handle);
	if (r != ERROR_SUCCESS && r != ERROR_IO_PENDING) {
		SetLastError(r);
		uprintf("Could not create virtual disk: %s", WindowsErrorString());
		goto out;
	}

	if (r == ERROR_IO_PENDING) {
		while ((r = WaitForSingleObject(overlapped.hEvent, 100)) == WAIT_TIMEOUT) {
			if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) {
				CancelIoEx(handle, &overlapped);
				goto out;
			}
			if (GetVirtualDiskOperationProgress(handle, &overlapped, &vprogress) == ERROR_SUCCESS) {
				if (vprogress.OperationStatus == ERROR_IO_PENDING)
					UpdateProgressWithInfo(OP_FORMAT, MSG_261, vprogress.CurrentValue, vprogress.CompletionValue);
			}
		}
		if (r != WAIT_OBJECT_0) {
			uprintf("Could not save virtual disk: %s", WindowsErrorString());
			goto out;
		}
	}

	if (!GetOverlappedResult(handle, &overlapped, &bytes_read, FALSE)) {
		r = GetLastError();
		uprintf("Could not save virtual disk: %s", WindowsErrorString());
		goto out;
	}

	r = 0;
	UpdateProgressWithInfo(OP_FORMAT, MSG_261, SelectedDrive.DiskSize, SelectedDrive.DiskSize);
	uprintf("Saved '%s'", img_save->ImagePath);

out:
	safe_closehandle(overlapped.hEvent);
	safe_closehandle(handle);
	safe_free(wSrc);
	safe_free(wDst);
	safe_free(img_save->DevicePath);
	safe_free(img_save->ImagePath);
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	if (r != 0 && !IS_ERROR(ErrorStatus))
		ErrorStatus = RUFUS_ERROR(SCODE_CODE(r));
	ExitThread(r);
}

// FfuProvider.dll has some nice FfuApplyImage()/FfuCaptureImage() calls... which
// Microsoft decided not make public!
// Considering that trying to both figure out how to use these internal function
// calls, as well as how to properly hook into the DLL for every arch/every release
// of Windows, would be a massive timesink, we just take a shortcut by calling dism
// directly, as imperfect as such a solution might be...
static DWORD WINAPI FfuSaveImageThread(void* param)
{
	DWORD r;
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	char cmd[2 * KB], letters[27], *label;

	GetDriveLabel(SelectedDrive.DeviceNumber, letters, &label, TRUE);
	static_sprintf(cmd, "%s\\dism.exe /Capture-Ffu /CaptureDrive:%s /ImageFile:\"%s\" "
		"/Name:\"%s\" /Description:\"Created by %s (%s)\"", sysnative_dir,
		img_save->DevicePath, img_save->ImagePath, label, APPLICATION_NAME, RUFUS_URL);
	uprintf("Running command: '%s'", cmd);
	// For detecting typical dism.exe commandline progress report of type:
	// "\r[====                       8.0%                           ]"
	r = RunCommandWithProgress(cmd, sysnative_dir, TRUE, MSG_261, ".*\r\\[[= ]+([0-9\\.]+)%[= ]+\\].*");
	if (r != 0 && !IS_ERROR(ErrorStatus)) {
		SetLastError(r);
		uprintf("Failed to capture FFU image: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(SCODE_CODE(r));
	}
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	if (!IS_ERROR(ErrorStatus))
		uprintf("Saved '%s'", img_save->ImagePath);
	safe_free(img_save->DevicePath);
	safe_free(img_save->ImagePath);
	ExitThread(r);
}

BOOL SaveImage(void)
{
	UINT i;
	static IMG_SAVE img_save;
	char filename[128], letters[27], path[MAX_PATH];
	int DriveIndex = ComboBox_GetCurSel(hDeviceList);
	enum { image_type_none = 0, image_type_vhd, image_type_vhdx, image_type_ffu, image_type_iso };
	static EXT_DECL(img_ext, filename, __VA_GROUP__("*.vhd", "*.vhdx", "*.ffu", "*.iso"),
		__VA_GROUP__(lmprintf(MSG_343), lmprintf(MSG_342), lmprintf(MSG_344), lmprintf(MSG_036)));
	int i_to_type[5] = { image_type_none, image_type_vhd, image_type_vhdx, image_type_ffu, image_type_iso };
	ULARGE_INTEGER free_space;

	memset(&img_save, 0, sizeof(IMG_SAVE));
	// Should have been validated prior to calling SaveImage()
	if_assert_fails((DriveIndex >= 0) && (format_thread == NULL))
		return FALSE;

	static_sprintf(filename, "%s", rufus_drive[DriveIndex].label);
	img_save.DeviceNum = (DWORD)ComboBox_GetItemData(hDeviceList, DriveIndex);
	img_save.DevicePath = GetPhysicalName(img_save.DeviceNum);
	img_ext.count = 2;
	// FFU support requires GPT
	if (has_ffu_support && SelectedDrive.PartitionStyle == PARTITION_STYLE_GPT) {
		img_ext.count += 1;
	} else {
		// Move the ISO extension one place down
		img_ext.extension[image_type_ffu - 1] = img_ext.extension[image_type_iso - 1];
		img_ext.description[image_type_ffu - 1] = img_ext.description[image_type_iso - 1];
		i_to_type[image_type_ffu] = image_type_iso;
		i_to_type[image_type_iso] = image_type_none;
	}
	// ISO support requires a mounted file system
	if (GetDriveLetters(SelectedDrive.DeviceNumber, letters) && letters[0] != '\0')
		img_ext.count += 1;
	for (i = 1; i <= (UINT)img_ext.count && (safe_strcmp(save_image_type, &img_ext.extension[i - 1][2]) != '\0'); i++);
	if (i > (UINT)img_ext.count)
		i = image_type_vhdx;
	img_save.ImagePath = FileDialog(TRUE, NULL, &img_ext, &i);
	if (img_save.ImagePath == NULL) {
		ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
		goto out;
	}
	// Start from the end of our extension array, since '.vhd' would match for '.vhdx' otherwise
	for (i = (UINT)img_ext.count; (i > 0) && (strstr(img_save.ImagePath, &img_ext.extension[i - 1][1]) == NULL); i--);
	if (i == 0) {
		uprintf("WARNING: Can not determine image type from extension - Saving to uncompressed VHD.");
		i = image_type_vhd;
	} else {
		save_image_type = (char*)&img_ext.extension[i - 1][2];
		WriteSettingStr(SETTING_PREFERRED_SAVE_IMAGE_TYPE, save_image_type);
	}
	assert(i < ARRAYSIZE(i_to_type));
	switch (i_to_type[i]) {
	case image_type_vhd:
		img_save.Type = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
		break;
	case image_type_ffu:
		img_save.Type = VIRTUAL_STORAGE_TYPE_DEVICE_FFU;
		break;
	case image_type_iso:
		// ISO requires oscdimg.exe. If not already present, attempt to download it.
		static_sprintf(path, "%s\\%s\\oscdimg_%s.exe", app_data_dir, FILES_DIR, APPLICATION_ARCH);
		if (!PathFileExistsU(path)) {
			if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_115), lmprintf(MSG_337, "oscdimg.exe")) != IDYES) {
				ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
				goto out;
			}
			IGNORE_RETVAL(_chdirU(app_data_dir));
			IGNORE_RETVAL(_mkdir(FILES_DIR));
			if (DownloadToFileOrBufferEx(OSCDIMG_URL, path, SYMBOL_SERVER_USER_AGENT, NULL, hMainDialog, FALSE) < 64 * KB) {
				ErrorStatus = GetLastError();
				goto out;
			}
		}
		img_save.Type = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
		break;
	default:
		img_save.Type = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
		break;
	}
	img_save.BufSize = DD_BUFFER_SIZE;
	img_save.DeviceSize = SelectedDrive.DiskSize;
	if (img_save.DevicePath != NULL && img_save.ImagePath != NULL) {
		// Reset all progress bars
		SendMessage(hMainDialog, UM_PROGRESS_INIT, 0, 0);
		ErrorStatus = 0;
		if (img_save.Type == VIRTUAL_STORAGE_TYPE_DEVICE_VHD) {
			free_space.QuadPart = 0;
			if ((GetVolumePathNameA(img_save.ImagePath, path, sizeof(path)))
				&& (GetDiskFreeSpaceExA(path, &free_space, NULL, NULL))
				&& ((LONGLONG)free_space.QuadPart < (SelectedDrive.DiskSize + 512))) {
				uprintf("The VHD size is too large for the target drive");
				ErrorStatus = RUFUS_ERROR(ERROR_FILE_TOO_LARGE);
				goto out;
			}
		}
		// Disable all controls except Cancel
		EnableControls(FALSE, FALSE);
		ErrorStatus = 0;
		InitProgress(TRUE);
		format_thread = CreateThread(NULL, 0, img_save.Type == VIRTUAL_STORAGE_TYPE_DEVICE_FFU ?
			FfuSaveImageThread : (img_save.Type == VIRTUAL_STORAGE_TYPE_DEVICE_ISO ? IsoSaveImageThread : VhdSaveImageThread),
			&img_save, 0, NULL);
		if (format_thread != NULL) {
			uprintf("\r\nSave to image operation started");
			PrintInfo(0, -1);
			SendMessage(hMainDialog, UM_TIMER_START, 0, 0);
		}
	}
out:
	if (format_thread == NULL) {
		safe_free(img_save.DevicePath);
		safe_free(img_save.ImagePath);
	}
	return (format_thread != NULL);
}

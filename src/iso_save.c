/*
 * Rufus: The Reliable USB Formatting Utility
 * ISO file extraction - optical disc and drive image saving
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Based on libcdio's iso & udf samples:
 * Copyright © 2003-2014 Rocky Bernstein <rocky@gnu.org>
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
 * NB: These functions come from iso.c, which is now part of the core. They
 * save an optical disc (Alt-O) or a drive (through oscdimg.exe) to an ISO,
 * which are stage 4 features of the Linux port, and are not ported yet.
 */

static DWORD WINAPI OpticalDiscSaveImageThread(void* param)
{
	BOOL s;
	DWORD rSize, wSize;
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	HANDLE hPhysicalDrive = INVALID_HANDLE_VALUE;
	HANDLE hDestImage = INVALID_HANDLE_VALUE;
	LARGE_INTEGER li;
	uint8_t* buffer = NULL;
	uint64_t wb;
	int i;

	assert(img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_ISO);

	PrintInfoDebug(0, MSG_225);
	hPhysicalDrive = CreateFileA(img_save->DevicePath, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (hPhysicalDrive == INVALID_HANDLE_VALUE) {
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}

	// In case someone poked the disc before us
	li.QuadPart = 0;
	if (!SetFilePointerEx(hPhysicalDrive, li, NULL, FILE_BEGIN))
		uprintf("WARNING: Unable to rewind device position - wrong data might be copied!");
	hDestImage = CreateFileU(img_save->ImagePath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hDestImage == INVALID_HANDLE_VALUE) {
		uprintf("Could not open image '%s': %s", img_save->ImagePath, WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}

	buffer = (uint8_t*)_mm_malloc(img_save->BufSize, 16);
	if (buffer == NULL) {
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
		uprintf("Could not allocate buffer");
		goto out;
	}

	uprintf("Will use a buffer size of %s", SizeToHumanReadable(img_save->BufSize, FALSE, FALSE));
	uprintf("Saving to image '%s'...", img_save->ImagePath);

	// Don't bother trying for something clever, using double buffering overlapped and whatnot:
	// With Windows' default optimizations, sync read + sync write for sequential operations
	// will be as fast, if not faster, than whatever async scheme you can come up with.
	UpdateProgressWithInfoInit(NULL, FALSE);
	for (wb = 0; ; wb += wSize) {
		// Optical drives do not appear to increment the sectors to read automatically
		li.QuadPart = wb;
		if (!SetFilePointerEx(hPhysicalDrive, li, NULL, FILE_BEGIN))
			uprintf("WARNING: Unable to set device position - wrong data might be copied!");
		s = ReadFile(hPhysicalDrive, buffer,
			(DWORD)MIN(img_save->BufSize, img_save->DeviceSize - wb), &rSize, NULL);
		if (!s) {
			ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
			uprintf("Read error: %s", WindowsErrorString());
			goto out;
		}
		if (rSize == 0)
			break;
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, wb, img_save->DeviceSize);
		for (i = 1; i <= WRITE_RETRIES; i++) {
			CHECK_FOR_USER_CANCEL;
			s = WriteFile(hDestImage, buffer, rSize, &wSize, NULL);
			if ((s) && (wSize == rSize))
				break;
			if (s)
				uprintf("Write error: Wrote %d bytes, expected %d bytes", wSize, rSize);
			else
				uprintf("Write error: %s", WindowsErrorString());
			if (i < WRITE_RETRIES) {
				li.QuadPart = wb;
				uprintf("Retrying in %d seconds...", WRITE_TIMEOUT / 1000);
				Sleep(WRITE_TIMEOUT);
				if (!SetFilePointerEx(hDestImage, li, NULL, FILE_BEGIN)) {
					uprintf("Write error: Could not reset position - %s", WindowsErrorString());
					goto out;
				}
			} else {
				ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
				goto out;
			}
			Sleep(200);
		}
		if (i > WRITE_RETRIES)
			goto out;
	}
	if (wb != img_save->DeviceSize) {
		uprintf("Error: wrote %s, expected %s", SizeToHumanReadable(wb, FALSE, FALSE),
			SizeToHumanReadable(img_save->DeviceSize, FALSE, FALSE));
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	uprintf("Operation complete (Wrote %s).", SizeToHumanReadable(wb, FALSE, FALSE));

out:
	safe_free(img_save->ImagePath);
	safe_mm_free(buffer);
	safe_closehandle(hDestImage);
	safe_unlockclose(hPhysicalDrive);
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	ExitThread(0);
}

void OpticalDiscSaveImage(void)
{
	static IMG_SAVE img_save = { 0 };
	char filename[33] = "disc_image.iso";
	EXT_DECL(img_ext, filename, __VA_GROUP__("*.iso"), __VA_GROUP__(lmprintf(MSG_036)));

	if (op_in_progress || (format_thread != NULL))
		return;

	img_save.Type = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
	if (!GetOpticalMedia(&img_save)) {
		uprintf("No dumpable optical media found.");
		return;
	}
	// Adjust the buffer size according to the disc size so that we get a decent speed.
	for (img_save.BufSize = 32 * MB;
		(img_save.BufSize > 8 * MB) && (img_save.DeviceSize <= img_save.BufSize * 64);
		img_save.BufSize /= 2);
	if ((img_save.Label != NULL) && (img_save.Label[0] != 0))
		static_sprintf(filename, "%s.iso", img_save.Label);

	img_save.ImagePath = FileDialog(TRUE, NULL, &img_ext, 0);
	if (img_save.ImagePath == NULL)
		return;

	uprintf("ISO media size %s", SizeToHumanReadable(img_save.DeviceSize, FALSE, FALSE));
	SendMessage(hMainDialog, UM_PROGRESS_INIT, 0, 0);
	ErrorStatus = 0;
	// Disable all controls except cancel
	EnableControls(FALSE, FALSE);
	InitProgress(TRUE);
	format_thread = CreateThread(NULL, 0, OpticalDiscSaveImageThread, &img_save, 0, NULL);
	if (format_thread != NULL) {
		uprintf("\r\nSave to ISO operation started");
		PrintInfo(0, -1);
		SendMessage(hMainDialog, UM_TIMER_START, 0, 0);
	} else {
		uprintf("Unable to start ISO save thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		safe_free(img_save.ImagePath);
		PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)FALSE, 0);
	}
}

// Create an ISO image from the currently selected drive, using oscdimg.exe
DWORD WINAPI IsoSaveImageThread(void* param)
{
	DWORD r = ERROR_NOT_FOUND;
	HANDLE exe = INVALID_HANDLE_VALUE;
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	char cmd[2 * KB], letters[27], *label;

	if (!GetDriveLabel(SelectedDrive.DeviceNumber, letters, &label, TRUE) || letters[0] == '\0')
		goto out;

	// Get a lock and validate that the oscdimg.exe has not been tampered with
	static_sprintf(cmd, "%s\\%s\\oscdimg_%s.exe", app_data_dir, FILES_DIR, APPLICATION_ARCH);
	exe = CreateFileU(cmd, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (exe == INVALID_HANDLE_VALUE) {
		uprintf("ERROR: Could not lock 'oscdimg.exe'");
		r = GetLastError();
		goto out;
	}
	if (!FileMatchesHash(cmd, OSCDIMG_HASH)) {
		uprintf("ERROR: Existing 'oscdimg.exe' hash does not match expected value!");
		r = ERROR_INVALID_IMAGE_HASH;
		goto out;
	}

	// UDF labels cannot be more than 32 characters (and if we have more than 32 chars we are
	// using the static modifiable char buffer from GetDriveLabel(), so we can alter it).
	if (strlen(label) > 32)
		label[32] = '\0';

	// Save to UDF only, as Microsoft's implementation of ISO-9660 doesn't support multiextent
	// and produces BROKEN images if you try to add files larger than 4 GB.
	// Plus ISO-9660/Joliet limits labels to 16 characters and has issues with long paths.
	static_sprintf(cmd, "\"%s\\%s\\oscdimg_%s.exe\" -g -h -k -l\"%s\" -m -u2 -udfver102 %c:\\ \"%s\"",
		app_data_dir, FILES_DIR, APPLICATION_ARCH, label, letters[0], img_save->ImagePath);
	uprintf("Running command: '%s'", cmd);
	// For detecting typical oscdimg commandline progress report of type: "\r15.5% complete"
	r = RunCommandWithProgress(cmd, sysnative_dir, FALSE, MSG_261, ".*\r([0-9\\.]+)% complete.*");

out:
	safe_closehandle(exe);
	if (r != 0 && !IS_ERROR(ErrorStatus)) {
		SetLastError(r);
		uprintf("Failed to write ISO image: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(SCODE_CODE(r));
	}
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	if (!IS_ERROR(ErrorStatus))
		uprintf("Saved '%s'", img_save->ImagePath);
	safe_free(img_save->DevicePath);
	safe_free(img_save->ImagePath);
	ExitThread(r);
}

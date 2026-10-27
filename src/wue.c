/*
 * Rufus: The Reliable USB Formatting Utility
 * Windows User Experience
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
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
 * The image scanning functions and the unattend.xml creation, which the boot
 * check uses. What gets applied to the target drive is in wue_apply.c.
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "vhd.h"
#include "wue.h"
#include "xml.h"
#include "wimlib.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"
#include "regional.h"

const char* bypass_name[] = { "BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck" };

int wintogo_index = -1, wininst_index = 0;
int unattend_xml_mask = UNATTEND_DEFAULT_SELECTION_MASK;
int unattend_edition_index = 1;
char unattend_username[MAX_USERNAME_LENGTH];	// unattend_xml_path is in core/globals.c
uint32_t removable_section[2] = { 0, 0 };

// Get the UI language provided by the ISO's boot.wim
char* GetUILanguage(void)
{
	static char ui_language[8];
	int r;
	char wim_path[4 * MAX_PATH] = "";
	// NB: The XML data of a WIM is UTF-16LE, which ezxml converts
	void* xml = NULL;
	ezxml_t pxml = NULL;
	size_t xml_len;
	WIMStruct* wim = NULL;

	assert(safe_strlen(image_path) + 32 < ARRAYSIZE(wim_path));
	static_strcpy(ui_language, "en-US");
	static_strcpy(wim_path, image_path);
	static_strcat(wim_path, "|sources/boot.wim");

	r = wimlib_open_wim(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: Error %d", r);
		goto out;
	}

	r = wimlib_get_xml_data(wim, &xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: Error %d", r);
		goto out;
	}

	pxml = ezxml_parse_str((char*)xml, xml_len);
	if (pxml == NULL)
		goto out;

	char* lang = ezxml_get_val(pxml, "IMAGE", 1, "WINDOWS", 0, "LANGUAGES", 0, "LANGUAGE", -1);
	if (lang != NULL)
		static_strcpy(ui_language, lang);

out:
	ezxml_free(pxml);
	free(xml);
	wimlib_free(wim);
	return ui_language;
}

/// <summary>
/// Create an installation answer file containing the sections specified by the flags.
/// </summary>
/// <param name="arch">The processor architecture of the Windows image being used.</param>
/// <param name="flags">A bitmask representing the sections to enable.
/// See "Windows User Experience flags and masks" from rufus.h</param>
/// <returns>The path of a newly created answer file on success or NULL on error.</returns>
char* CreateUnattendXml(int arch, int flags)
{
	static const char* xml_arch_names[5] = { "x86", "amd64", "arm", "arm64" };
	static const char* unallowed_account_names[] = {
		// From https://learn.microsoft.com/en-us/archive/technet-wiki/13813.localized-names-for-administrator-account-in-windows
		"Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda", "Administrador", "Администратор", "Administratör",
		"Guest", "DefaultAccount", "WDAGUtilityAccount", "HelpAssistant", "KRBTGT", "Local", "NONE", "SYSTEM"
	};
	static char path[MAX_PATH], tmp[MAX_PATH];
	char iana_tz[64], locale_name[64];
	const char* tzstr;
	FILE* fd;
	int i, order, fdesc;
	opts.unattend_xml_flags = flags;
	StrArray commands = { 0 };
	if (arch < ARCH_X86_32 || arch > ARCH_ARM_64 || flags == 0) {
		uprintf("Note: No Windows User Experience options selected");
		return NULL;
	}
	arch--;
	static_sprintf(path, "%s/%s-unattend-XXXXXX", temp_dir, APPLICATION_NAME);
	fdesc = mkstemp(path);
	if (fdesc < 0) {
		uprintf("Could not create '%s': %s", path, strerror(errno));
		return NULL;
	}
	fd = fdopen(fdesc, "w");
	if (fd == NULL) {
		close(fdesc);
		unlink(path);
		return NULL;
	}

	uprintf("Selected Windows User Experience options:");
	fprintf(fd, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	fprintf(fd, "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");

	StrArrayCreate(&commands, 16);
	if (flags & UNATTEND_WINPE_SETUP_MASK) {
		fprintf(fd, "  <settings pass=\"windowsPE\">\n");
		fprintf(fd, "    <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"%s\" language=\"neutral\" "
			"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
			"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
		// WinPE will complain if we don't provide a product key. *Any* product key. This is soooo idiotic...
		fprintf(fd, "      <UserData>\n");
		fprintf(fd, "        <AcceptEula>true</AcceptEula>\n");
		fprintf(fd, "        <ProductKey>\n");
		fprintf(fd, "          <Key />\n");
		fprintf(fd, "        </ProductKey>\n");
		fprintf(fd, "      </UserData>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			uprintf("• ⚠Silent Install⚠");
			// Automatically partition the disk and set the installation target
			fprintf(fd, "      <DiskConfiguration>\n");
			fprintf(fd, "        <WillShowUI>OnError</WillShowUI>\n");
			if (flags & UNATTEND_DISABLE_BITLOCKER)
				fprintf(fd, "        <DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>\n");
			// The following ensures that we display the disk selection screen if only the boot media is
			// available (i.e. if the system does not see any target for installation) or (in most cases)
			// if the user happens to have more than one disk connected besides the instal USB. In that
			// case the label assignment below fails and the UI is displayed allowing the user to provide
			// drivers or pick their target disk explicitly.
			// This also prevents the install media from being scratched.
			// With a special mention to Claude AI, that explicitly said this could not be accomplished...
			// Note that this may result in the disk partition screen being produced on systems that have
			// card readers, but we'd rather inconvenience a few people, to prevent potential data loss,
			// than the opposite. Also note that you do *NOT* want to try to use drive letter mapping for
			// the detection here, as if your drive isn't of type FIXED, the file copy steps bails out at
			// 75%. See https://github.com/pbatard/rufus/issues/2960 for more details.
			fprintf(fd, "        <Disk wcm:action=\"modify\">\n");
			fprintf(fd, "          <DiskID>1</DiskID> \n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"modify\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>2</PartitionID>\n");
			fprintf(fd, "              <Label>RUFUS_BOOT</Label>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "        <Disk wcm:action=\"add\">\n");
			fprintf(fd, "          <DiskID>0</DiskID> \n");
			fprintf(fd, "          <WillWipeDisk>true</WillWipeDisk> \n");
			fprintf(fd, "          <CreatePartitions>\n");
			// NB: Don't bother creating a recovery partition. Windows setup will do it for us.
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <Type>EFI</Type>\n");
			fprintf(fd, "              <Size>260</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <Type>MSR</Type>\n");
			fprintf(fd, "              <Size>16</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>3</Order>\n");
			fprintf(fd, "              <Type>Primary</Type>\n");
			fprintf(fd, "              <Extend>true</Extend>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "          </CreatePartitions>\n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>1</PartitionID>\n");
			fprintf(fd, "              <Label>EFI</Label>\n");
			fprintf(fd, "              <Format>FAT32</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <PartitionID>3</PartitionID>\n");
			fprintf(fd, "              <Label>Windows</Label>\n");
			fprintf(fd, "              <Letter>C</Letter>\n");
			fprintf(fd, "              <Format>NTFS</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "      </DiskConfiguration>\n");
			fprintf(fd, "      <ImageInstall>\n");
			fprintf(fd, "        <OSImage>\n");
			fprintf(fd, "          <WillShowUI>OnError</WillShowUI>\n");
			fprintf(fd, "          <InstallFrom>\n");
			fprintf(fd, "            <MetaData wcm:action=\"add\">\n");
			fprintf(fd, "              <Key>/IMAGE/INDEX</Key>\n");
			fprintf(fd, "              <Value>%d</Value>\n", unattend_edition_index);
			fprintf(fd, "            </MetaData>\n");
			fprintf(fd, "          </InstallFrom>\n");
			fprintf(fd, "          <InstallTo>\n");
			fprintf(fd, "            <DiskID>0</DiskID>\n");
			fprintf(fd, "            <PartitionID>3</PartitionID>\n");
			fprintf(fd, "          </InstallTo>\n");
			fprintf(fd, "        </OSImage>\n");
			fprintf(fd, "      </ImageInstall>\n");
		}
		if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
			// This part produces the unbecoming display of a command prompt window during initial setup as well
			// as alters the layout and options of the initial Windows installer screens, which may scare users.
			// So, in format.c, we'll try to insert the registry keys directly and drop this section. However,
			// because Microsoft prevents Store apps from editing an offline registry, we do need this fallback.
			// NB: We could probably avoid this by running powershell with -WindowStyle "Hidden" but hey...
			uprintf("• Bypass SB/TPM/RAM");
			order = 1;
			removable_section[0] = (uint32_t)ftell(fd);
			fprintf(fd, "      <RunSynchronous>\n");
			for (i = 0; i < ARRAYSIZE(bypass_name); i++) {
				fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order++);
				fprintf(fd, "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n", bypass_name[i]);
				fprintf(fd, "        </RunSynchronousCommand>\n");
			}
			fprintf(fd, "      </RunSynchronous>\n");
			removable_section[1] = (uint32_t)ftell(fd);
		}
		fprintf(fd, "    </component>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			fprintf(fd, "    <component name=\"Microsoft-Windows-International-Core-WinPE\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// Set the language from the <LANGUAGE> tag of the boot.wim index
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", GetUILanguage());
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_SPECIALIZE_DEPLOYMENT_MASK) {
		fprintf(fd, "  <settings pass=\"specialize\">\n");
		fprintf(fd, "    <component name=\"Microsoft-Windows-Deployment\" processorArchitecture=\"%s\" language=\"neutral\" "
			"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
			"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
		// This part was picked from https://github.com/AveYo/MediaCreationTool.bat/blob/main/bypass11/AutoUnattend.xml
		// NB: This is INCOMPATIBLE with S-Mode below
		if (flags & UNATTEND_NO_ONLINE_ACCOUNT) {
			StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f", TRUE);
			uprintf("• Bypass online account requirement");
		}
		if (flags & UNATTEND_QOL_ENHANCEMENTS) {
			uprintf("• QoL: Disable OneDrive and Outlook by default");
			// Remove OneDrive
			StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\OneDrive\" /v DisableFileSyncNGSC /t REG_DWORD /d 1 /f", TRUE);
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Remove-Item -Path $env:SystemRoot\\System32\\OneDriveSetup.exe -Force -Confirm:$false; "
				"Remove-Item -Path $env:SystemRoot\\SysWOW64\\OneDriveSetup.exe -Force -Confirm:$false;\"", TRUE);
			// Remove Outlook. How the frig is forcing Outlook on users legal when MS got pinned for bundling IE with Windows?
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Outlook*'} |"
				" Remove-AppxProvisionedPackage -Online\"", TRUE);
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Outlook* | Remove-AppxPackage -AllUsers\"", TRUE);
			// Same for Teams. Also: "HEY, MICROSOFT, SCREW YOU!!!!"
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Teams*'} |"
				" Remove-AppxProvisionedPackage -Online\"", TRUE);
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Teams* | Remove-AppxPackage -AllUsers\"", TRUE);
		}
		// Now that we have all the commands to run, create the RunSynchronous section.
		for (order = 1; order <= (int)commands.Index; order++) {
			if (order == 1)
				fprintf(fd, "      <RunSynchronous>\n");
			fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
			fprintf(fd, "          <Order>%d</Order>\n", order);
			fprintf(fd, "          <Path>%s</Path>\n", commands.String[order - 1]);
			fprintf(fd, "        </RunSynchronousCommand>\n");
			if (order == commands.Index)
				fprintf(fd, "      </RunSynchronous>\n");
		}
		fprintf(fd, "    </component>\n");
		fprintf(fd, "  </settings>\n");
		StrArrayClear(&commands);
	}

	if (flags & UNATTEND_OOBE_MASK) {
		fprintf(fd, "  <settings pass=\"oobeSystem\">\n");
		if (flags & UNATTEND_OOBE_SHELL_SETUP_MASK) {
			fprintf(fd, "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// We should always have UNATTEND_NO_DATA_COLLECTION if UNATTEND_SILENT_INSTALL is set but whatever...
			if (flags & (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SILENT_INSTALL)) {
				uprintf("• Disable data collection");
				fprintf(fd, "      <OOBE>\n");
				fprintf(fd, "        <HideEULAPage>true</HideEULAPage>\n");
				// https://docs.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-oobe-protectyourpc
				// It is really super disingenuous of Microsoft to call this option "ProtectYourPC", when it's really only about
				// data collection. But of course, if it was called "AllowDataCollection", everyone would turn it off...
				fprintf(fd, "        <ProtectYourPC>3</ProtectYourPC>\n");
				// Without these, the "Let's connect you to a network" can appear in silent installs when Ethernet is disconnected.
				if (flags & UNATTEND_SILENT_INSTALL) {
					fprintf(fd, "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n");
					fprintf(fd, "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n");
				}
				fprintf(fd, "      </OOBE>\n");
			}
			if (flags & UNATTEND_DUPLICATE_LOCALE) {
				// NB: Upstream uses the TimeZoneKeyName of the system. Here, it's the Windows name of its IANA zone.
				if (!GetSystemTimeZone(iana_tz, sizeof(iana_tz))) {
					uprintf("WARNING: Could not retrieve current timezone: %s", "No time zone is set");
				} else if ((tzstr = IanaToWindowsTimeZone(iana_tz)) == NULL) {
					uprintf("WARNING: Could not retrieve current timezone: '%s' has no Windows equivalent", iana_tz);
				} else {
					fprintf(fd, "      <TimeZone>%s</TimeZone>\n", tzstr);
				}
			}
			if (flags & UNATTEND_SET_USER) {
				// Remove leading and trailing whitespaces (https://github.com/pbatard/rufus/issues/2950)
				trim(unattend_username);
				for (i = 0; (i < ARRAYSIZE(unallowed_account_names)) && (stricmp(unattend_username, unallowed_account_names[i]) != 0); i++);
				if (i < ARRAYSIZE(unallowed_account_names)) {
					uprintf("WARNING: '%s' is not allowed as local account name - Option ignored", unattend_username);
				} else if (unattend_username[0] != 0) {
					char* org_username = safe_strdup(unattend_username);
					filter_chars(unattend_username, USERNAME_INVALID_CHARS, '_');
					uprintf("• Use '%s' for local account name", unattend_username);
					if (strcmp(org_username, unattend_username) != 0)
						uprintf("WARNING: Local account name '%s' contained unallowed characters and has been sanitized", org_username);
					free(org_username);
					// If we create a local account in unattend.xml, then we can get Windows 11
					// 22H2 to skip MSA even if the network is connected during installation.
					fprintf(fd, "      <UserAccounts>\n");
					fprintf(fd, "        <LocalAccounts>\n");
					fprintf(fd, "          <LocalAccount wcm:action=\"add\">\n");
					fprintf(fd, "            <Name>%s</Name>\n", unattend_username);
					fprintf(fd, "            <DisplayName>%s</DisplayName>\n", unattend_username);
					fprintf(fd, "            <Group>Administrators;Power Users</Group>\n");
					// Sets an empty password for the account (which, in Microsoft's convoluted ways,
					// needs to be initialized to the Base64 encoded UTF-16 string "Password").
					// The use of an empty password has both the advantage of not having to ask users
					// to type in a password in Rufus (which they might be weary of) as well as allowing
					// automated logon during setup.
					fprintf(fd, "            <Password>\n");
					fprintf(fd, "              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n");
					fprintf(fd, "              <PlainText>false</PlainText>\n");
					fprintf(fd, "            </Password>\n");
					fprintf(fd, "          </LocalAccount>\n");
					fprintf(fd, "        </LocalAccounts>\n");
					fprintf(fd, "      </UserAccounts>\n");
					// Since we set a blank password, we'll ask the user to change it at next logon.
					// NB: In case you wanna try, please be aware that Microsoft doesn't let you have multiple
					// <FirstLogonCommands> sections in unattend.xml. Don't ask me how I know... :(
					static_sprintf(tmp, "net user \"%s\" /logonpasswordchg:yes", unattend_username);
					StrArrayAdd(&commands, tmp, TRUE);
					// The `net user` command above might reset the password expiration to 90 days...
					// To alleviate that, blanket set passwords on the target machine to never expire.
					StrArrayAdd(&commands, "net accounts /maxpwage:unlimited", TRUE);
				}
			}
			// Apply SkuSiPolicy.p7b, if the user requested it and we're not creating a Windows To Go drive.
			// See https://support.microsoft.com/kb/5042562. We do it post install, on first logon, because
			// we'd have to patch tons of files otherwise. And we *ALWAYS* use the installed system's
			// SkuSiPolicy.p7b instead of the host system's, even if the latter might be more recent, on
			// account that the bootloaders from the installed system might be trailing behind, and wills
			// produce the 0xc0000428 signature validation error on (re)boot if the system hasn't gone
			// through a full Windows Update cycle.
			if (flags & UNATTEND_APPLY_SKUSIPOLICY) {
				uprintf("• Apply SkuSiPolicy.p7b");
				StrArrayAdd(&commands, "cmd /c mountvol S: /S &amp;&amp; "
					"copy %WINDIR%\\system32\\SecureBootUpdates\\SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot &amp;&amp; "
					"mountvol S: /D", TRUE);
			}
			if (flags & UNATTEND_QOL_ENHANCEMENTS) {
				uprintf("• QoL: Disable Fast Startup, Copilot, Recommendations, News and Teams by default");
				// Disable Fast Startup 
				StrArrayAdd(&commands, "reg add \"HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Power\" "
					"/v HiberbootEnabled /t REG_DWORD /d 0 /f", TRUE);
				// Disable Copilot and set search as an icon rather than the default real-estate gobbler
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v ShowCopilotButton /t REG_DWORD /d 0 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" "
					"/v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarMode /t REG_DWORD /d 1 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarModeCache /t REG_DWORD /d 1 /f", TRUE);
				// Disable Windows Phone and similarly pushed unwanted crap
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f", TRUE);
				// Disable ads in menu
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" "
					"/v SystemPaneSuggestionsEnabled /t REG_DWORD /d 0 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v BingSearchEnabled /t REG_DWORD /d 0 /f", TRUE);
				// Disable auto installation of manufacturer's crappy apps when plugging new hardware
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Device Metadata\" "
					"/v PreventDeviceMetadataFromNetwork /t REG_DWORD /d 1 /f", TRUE);
				// Disable News
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Dsh\" "
					"/v AllowNewsAndInterests /t REG_DWORD /d 0 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Windows Feeds\" "
					"/v EnableFeeds /t REG_DWORD /d 0 /f", TRUE);
				// Disable Teams
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Communications\" "
					"/v ConfigureChatAutoInstall /t REG_DWORD /d 0 /f", TRUE);
				// Prevent frigging Outlook from being forced into the user's taskbar
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableCloudOptimizedContent /t REG_DWORD /d 1 /f", TRUE);
				// Skip Edge's first run dialog
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Edge\" "
					"/v HideFirstRunExperience /t REG_DWORD /d 1 /f", TRUE);
				uprintf("• QoL: More pins for the Start Menu and enable useful shortcuts");
				// More pins by default on the Start Menu
				// https://learn.microsoft.com/en-us/windows/apps/develop/settings/settings-windows-11
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v Start_Layout /t REG_DWORD /d 1 /f", TRUE);
				// Add Documents, Downloads, Network, Personal Folder, File Explorer and Settings as start menu shortcuts
				// I wish there was a more transparent way of doing it, but Microsoft made it obscure. Oh, and for some
				// reason, feeding the full hex string through 'reg add' doesn't create the key when ran through unattend
				// (but doesn't produce an error and works post logon). PowerShell it is then...
				StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
					"\"Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Start' "
					"-Name 'VisiblePlaces' -Value $([convert]::FromBase64String('ztU0LVr6Q0WC8iLm6vd3PC+zZ+PeiVVDv85h83sYqTe8JIo"
					"UDNaJQqCAbtm7okiCRIF1/g0IrkKL2jTtl7ZjlEqwvXRK+WhPi9ZDmAcdqLyGCHNSqlFDQp97J3ZYRlnU')) -Type 'Binary'\"", TRUE);
				// Restore the classic right click context menu
				uprintf("• QoL: Restore classic context menu");
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32\" "
					"/ve /t REG_SZ /d \"\" /f", TRUE);
			}
			// Now that we have all the commands to run, create the FirstLogonCommands section.
			for (order = 1; order <= (int)commands.Index; order++) {
				if (order == 1)
					fprintf(fd, "      <FirstLogonCommands>\n");
				fprintf(fd, "        <SynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order);
				fprintf(fd, "          <CommandLine>%s</CommandLine>\n", commands.String[order - 1]);
				fprintf(fd, "        </SynchronousCommand>\n");
				if (order == commands.Index)
					fprintf(fd, "      </FirstLogonCommands>\n");
			}
			fprintf(fd, "    </component>\n");
			StrArrayClear(&commands);
		}
		if (flags & UNATTEND_OOBE_INTERNATIONAL_MASK) {
			uprintf("• Use the same regional options as this user's");
			fprintf(fd, "    <component name=\"Microsoft-Windows-International-Core\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// NB: Upstream gets these from the registry and the locale APIs. Here, the keyboard is the
			// first XKB layout of the user, and the locales are the POSIX ones, as BCP-47 names.
			if (GetInputLocale(locale_name, sizeof(locale_name)))
				fprintf(fd, "      <InputLocale>%s</InputLocale>\n", locale_name);
			if (LocaleToBcp47(GetLocaleCategory("LC_CTYPE"), locale_name, sizeof(locale_name)))
				fprintf(fd, "      <SystemLocale>%s</SystemLocale>\n", locale_name);
			if (LocaleToBcp47(GetLocaleCategory("LC_TIME"), locale_name, sizeof(locale_name)))
				fprintf(fd, "      <UserLocale>%s</UserLocale>\n", locale_name);
			if (LocaleToBcp47(GetLocaleCategory("LC_MESSAGES"), locale_name, sizeof(locale_name)))
				fprintf(fd, "      <UILanguage>%s</UILanguage>\n", locale_name);
			// The language that Windows falls back to (InstallLanguageFallback upstream)
			fprintf(fd, "      <UILanguageFallback>en-US</UILanguageFallback>\n");
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_DISABLE_BITLOCKER) {
			uprintf("• Disable BitLocker");
			fprintf(fd, "    <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n");
			fprintf(fd, "    </component>\n");
			fprintf(fd, "    <component name=\"Microsoft-Windows-EnhancedStorage-Adm\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_OFFLINE_SERVICING_MASK) {
		fprintf(fd, "  <settings pass=\"offlineServicing\">\n");
		if (flags & UNATTEND_OFFLINE_INTERNAL_DRIVES) {
			uprintf("• Set internal drives offline");
			fprintf(fd, "    <component name=\"Microsoft-Windows-PartitionManager\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <SanPolicy>4</SanPolicy>\n");
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_FORCE_S_MODE) {
			uprintf("• Enforce S Mode");
			fprintf(fd, "    <component name=\"Microsoft-Windows-CodeIntegrity\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <SkuPolicyRequired>1</SkuPolicyRequired>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_USE_MS2023_BOOTLOADERS)
		uprintf("• Use 'Windows CA 2023' signed bootloaders");

	StrArrayDestroy(&commands);
	fprintf(fd, "</unattend>\n");
	fclose(fd);
	return path;
}

/// <summary>
/// Populate the img_report Window version from an install[.wim|.esd] XML index
/// </summary>
/// <param name="xml">The index XML data.</param>
/// <param name="xml_len">The length of the index XML data.</param>
/// <param name="index">The index of the occurrence to look for.</param>
static void PopulateWindowsVersionFromXml(void* xml, size_t xml_len, int index)
{
	char* val;
	ezxml_t pxml = ezxml_parse_str((char*)xml, xml_len);
	if (pxml == NULL)
		return;

	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "MAJOR", -1);
	img_report.win_version.major = (uint16_t)safe_atoi(val);
	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "MINOR", -1);
	img_report.win_version.minor = (uint16_t)safe_atoi(val);
	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "BUILD", -1);
	img_report.win_version.build = (uint16_t)safe_atoi(val);
	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "SPBUILD", -1);
	img_report.win_version.revision = (uint16_t)safe_atoi(val);
	// Adjust versions so that we produce a more accurate report in the log
	// (and yeah, I know we won't properly report Server, but I don't care)
	if (img_report.win_version.major <= 5) {
		// Don't want to support XP or earlier
		img_report.win_version.major = 0;
		img_report.win_version.minor = 0;
	} else if (img_report.win_version.major == 6) {
		// Don't want to support Vista
		if (img_report.win_version.minor == 0) {
			img_report.win_version.major = 0;
		} else if (img_report.win_version.minor == 1) {
			img_report.win_version.major = 7;
			img_report.win_version.minor = 0;
		} else if (img_report.win_version.minor == 2) {
			img_report.win_version.major = 8;
			img_report.win_version.minor = 0;
		} else if (img_report.win_version.minor == 3) {
			img_report.win_version.major = 8;
			img_report.win_version.minor = 1;
		} else if (img_report.win_version.minor == 4) {
			img_report.win_version.major = 10;
			img_report.win_version.minor = 0;
		}
	} else if (img_report.win_version.major == 10) {
		if (img_report.win_version.build > 20000)
			img_report.win_version.major = 11;
	}
	ezxml_free(pxml);
}

/// <summary>
/// Populate the img_report Window version from an install[.wim|.esd].
/// </summary>
/// <param name="">(none)</param>
/// <returns>TRUE on success, FALSE if we couldn't populate the version.</returns>
BOOL PopulateWindowsVersion(void)
{
	int r;
	char wim_path[4 * MAX_PATH] = "";
	void* xml = NULL;
	size_t xml_len;
	WIMStruct* wim = NULL;

	memset(&img_report.win_version, 0, sizeof(img_report.win_version));

	assert(safe_strlen(image_path) + 1 < ARRAYSIZE(wim_path));
	static_strcpy(wim_path, image_path);
	if (!img_report.is_windows_img) {
		// NB: Upstream skips the "?:\" prefix of the path
		assert(safe_strlen(image_path) + safe_strlen(img_report.wininst_path[0]) + 1 < ARRAYSIZE(wim_path));
		static_strcat(wim_path, "|");
		static_strcat(wim_path, img_report.wininst_path[0]);
	}

	r = wimlib_open_wim(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: Error %d", r);
		goto out;
	}

	r = wimlib_get_xml_data(wim, &xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: Error %d", r);
		goto out;
	}

	PopulateWindowsVersionFromXml(xml, xml_len, 0);

out:
	free(xml);
	wimlib_free(wim);

	return ((img_report.win_version.major != 0) && (img_report.win_version.build != 0));
}

int GetEditions(StrArray* version_name, StrArray* version_index)
{
	int i, r, n = -1;
	WIMStruct* wim = NULL;
	selection_dialog_options_t selection = { 0 };
	char wim_path[4 * MAX_PATH] = "";
	void* xml = NULL;
	size_t xml_len;
	BOOL bNonStandard = FALSE;
	ezxml_t index = NULL, image = NULL;

	// If we have multiple windows install images, ask the user the one to use
	if (img_report.wininst_index > 1) {
		StrArrayCreate(&selection.choices, 16);
		for (i = 0; i < img_report.wininst_index; i++)
			StrArrayAdd(&selection.choices, img_report.wininst_path[i], TRUE);
		wininst_index = _log2(SelectionDialog(lmprintf(MSG_130), lmprintf(MSG_131), &selection));
		StrArrayDestroy(&selection.choices);
		if (wininst_index < 0)
			return -2;
		if (wininst_index >= MAX_WININST)
			wininst_index = 0;
	}

	assert(safe_strlen(image_path) + safe_strlen(img_report.wininst_path[wininst_index]) + 2 < ARRAYSIZE(wim_path));
	static_strcpy(wim_path, image_path);
	if (!img_report.is_windows_img) {
		static_strcat(wim_path, "|");
		static_strcat(wim_path, img_report.wininst_path[wininst_index]);
	}

	r = wimlib_open_wim(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: %d", r);
		goto out;
	}
	r = wimlib_get_xml_data(wim, &xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: %d", r);
		goto out;
	}

	index = ezxml_parse_str((char*)xml, xml_len);
	if (index == NULL) {
		uprintf("Could not parse WIM XML");
		goto out;
	}

	// NB: Upstream preselects the edition that matches the one of the Windows host
	unattend_edition_index = 1;
	for (n = 0, image = ezxml_child(index, "IMAGE");
		StrArrayAdd(version_index, ezxml_attr(image, "INDEX"), TRUE) >= 0;
		image = image->next, n++) {
		// Some people are apparently creating *unofficial* Windows ISOs that don't have DISPLAYNAME elements.
		// If we are parsing such an ISO, try to fall back to using DESCRIPTION.
		if (StrArrayAdd(version_name, ezxml_child_val(image, "DISPLAYNAME"), TRUE) < 0) {
			if (StrArrayAdd(version_name, ezxml_child_val(image, "DESCRIPTION"), TRUE) < 0) {
				uprintf("WARNING: Could not find a description for image index %d", n + 1);
				StrArrayAdd(version_name, "Unknown Windows Version", TRUE);
			}
			bNonStandard = TRUE;
		}
	}
	if (bNonStandard)
		uprintf("WARNING: Nonstandard Windows image (missing <DISPLAYNAME> entries)");

out:
	free(xml);
	ezxml_free(index);
	wimlib_free(wim);
	return n;
}

/// <summary>
/// Checks which versions of Windows are available in an install image
/// to set our extraction index. Asks the user to select one if needed.
/// </summary>
/// <param name="is_fixed">Whether the target is a fixed drive (SelectedDrive.MediaType == FixedMedia upstream).</param>
/// <returns>-2 on user cancel, -1 on other error, >=0 on success.</returns>
int SetWinToGoIndex(BOOL is_fixed)
{
	int i, r;
	WIMStruct* wim = NULL;
	char wim_path[4 * MAX_PATH] = "";
	void* xml = NULL;
	size_t xml_len;
	StrArray edition_index = { 0 };
	BOOL bNonStandard = FALSE;
	ezxml_t index = NULL, image = NULL;
	selection_dialog_options_t selection = { 0 };

	// Sanity checks
	wintogo_index = -1;
	wininst_index = 0;
	if (opts.fs_type != FS_NTFS)
		return -1;

	// If we have multiple windows install images, ask the user the one to use
	if (img_report.wininst_index > 1) {
		StrArrayCreate(&selection.choices, 16);
		for (i = 0; i < img_report.wininst_index; i++)
			StrArrayAdd(&selection.choices, img_report.wininst_path[i], TRUE);
		wininst_index = _log2(SelectionDialog(lmprintf(MSG_130), lmprintf(MSG_131), &selection));
		StrArrayDestroy(&selection.choices);
		if (wininst_index < 0)
			return -2;
		if (wininst_index >= MAX_WININST)
			wininst_index = 0;
	}

	assert(safe_strlen(image_path) + safe_strlen(img_report.wininst_path[wininst_index]) + 2 < ARRAYSIZE(wim_path));
	static_strcpy(wim_path, image_path);
	if (!img_report.is_windows_img) {
		static_strcat(wim_path, "|");
		static_strcat(wim_path, img_report.wininst_path[wininst_index]);
	}

	r = wimlib_open_wim(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: %d", r);
		goto out;
	}
	r = wimlib_get_xml_data(wim, &xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: %d", r);
		goto out;
	}

	StrArrayCreate(&selection.choices, 16);
	StrArrayCreate(&edition_index, 16);
	index = ezxml_parse_str((char*)xml, xml_len);
	if (index == NULL) {
		uprintf("Could not parse WIM XML");
		goto out;
	}

	// NB: Upstream preselects the edition that matches the one of the Windows host
	unattend_edition_index = 1;
	for (i = 0, image = ezxml_child(index, "IMAGE");
		StrArrayAdd(&edition_index, ezxml_attr(image, "INDEX"), TRUE) >= 0;
		image = image->next, i++) {
		// Some people are apparently creating *unofficial* Windows ISOs that don't have DISPLAYNAME elements.
		// If we are parsing such an ISO, try to fall back to using DESCRIPTION.
		if (StrArrayAdd(&selection.choices, ezxml_child_val(image, "DISPLAYNAME"), TRUE) < 0) {
			if (StrArrayAdd(&selection.choices, ezxml_child_val(image, "DESCRIPTION"), TRUE) < 0) {
				uprintf("WARNING: Could not find a description for image index %d", i + 1);
				StrArrayAdd(&selection.choices, "Unknown Windows Version", TRUE);
			}
			bNonStandard = TRUE;
		}
	}
	if (bNonStandard)
		uprintf("WARNING: Nonstandard Windows image (missing <DISPLAYNAME> entries)");

	selection.mask = 1 << unattend_edition_index;
	if (i > 1)
		// NB: _log2 returns -2 if SelectionDialog() returns negative (user cancelled)
		i = _log2(SelectionDialog(lmprintf(MSG_291), lmprintf(MSG_292), &selection)) + 1;
	if (i < 0)
		wintogo_index = -2;	// Cancelled by the user
	else if (i == 0)
		wintogo_index = 1;
	else
		wintogo_index = atoi(edition_index.String[i - 1]);
	if (i > 0) {
		// re-populate the version data from the selected XML index
		PopulateWindowsVersionFromXml(xml, xml_len, i - 1);
		// If we couldn't obtain the major and build, we have a problem
		if (img_report.win_version.major == 0 || img_report.win_version.build == 0)
			uprintf("WARNING: Could not obtain version information from XML index (Nonstandard Windows image?)");
		uprintf("Will use '%s' (Build: %d, Index %s) for Windows To Go",
			selection.choices.String[i - 1], img_report.win_version.build, edition_index.String[i - 1]);
		// Need Windows 10 Creator Update or later for boot on REMOVABLE to work
		if ((img_report.win_version.build < 15000) && !is_fixed) {
			if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_190), lmprintf(MSG_098)) != IDYES)
				wintogo_index = -2;
		}
		// Display a notice about WppRecorder.sys for 1809 ISOs
		if (img_report.win_version.build == 17763) {
			notification_info more_info;
			more_info.id = MORE_INFO_URL;
			more_info.url = WPPRECORDER_MORE_INFO_URL;
			NotificationEx(MB_ICONINFORMATION | MB_CLOSE, NULL, &more_info, lmprintf(MSG_128, "Windows To Go"), lmprintf(MSG_133));
		}
	}

out:
	StrArrayDestroy(&selection.choices);
	StrArrayDestroy(&edition_index);
	free(xml);
	ezxml_free(index);
	wimlib_free(wim);
	return wintogo_index;
}


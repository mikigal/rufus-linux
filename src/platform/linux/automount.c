/*
 * Rufus: The Reliable USB Formatting Utility
 * Automount suppression and private mounts
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
 * On Windows, Rufus disables the automounting of new volumes (MountMgr) and
 * closes the "You need to format the disk" prompts. Here, a udev rule in
 * /run/udev/rules.d marks the drive and its partitions with UDISKS_IGNORE and
 * UDISKS_AUTO=0, which keeps udisks (and therefore the desktop) from mounting
 * or displaying them. /run is a tmpfs, so a rule that a crash left behind is
 * gone after a reboot, and RemoveAutomountRules() removes it at the next start.
 *
 * The file systems that Rufus mounts itself go to a private mount namespace,
 * where nothing else sees them.
 */

#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rufus.h"
#include "disk.h"

#define UDEV_RULES_DIR              "/run/udev/rules.d"
#define RULE_PREFIX                 "90-rufus-"
#define RUFUS_RUN_DIR               "/run/rufus"

static BOOL ReloadUdevRules(void)
{
	const char* argv[] = { "udevadm", "control", "--reload", NULL };

	return (RunCommand(argv, NULL, usb_debug) == 0);
}

void GetAutomountRule(const char* syspath, char* rule, size_t size)
{
	// DEVPATH is the sysfs path without "/sys", and the partitions are below the disk
	const char* devpath = (strncmp(syspath, "/sys/", 5) == 0) ? &syspath[4] : syspath;

	safe_sprintf(rule, size, "# Added by " APPLICATION_NAME " while it writes to this drive\n"
		"SUBSYSTEM==\"block\", DEVPATH==\"%s*\", ENV{UDISKS_IGNORE}=\"1\", ENV{UDISKS_AUTO}=\"0\"\n", devpath);
}

BOOL SuppressAutomount(const char* syspath)
{
	char rule[1024], path[PATH_MAX], tmp[PATH_MAX];
	const char* name = strrchr(syspath, '/');
	FILE* fd;

	if ((name == NULL) || (name[1] == 0))
		return FALSE;
	GetAutomountRule(syspath, rule, sizeof(rule));
	static_sprintf(path, "%s/%s%s.rules", UDEV_RULES_DIR, RULE_PREFIX, &name[1]);
	static_sprintf(tmp, "%s.tmp", path);
	if (mkdir_p(UDEV_RULES_DIR, 0755) != 0) {
		uprintf("Could not create '%s': %s", UDEV_RULES_DIR, WindowsErrorString());
		return FALSE;
	}
	// The rule must never be read half written
	fd = fopen(tmp, "we");
	if ((fd == NULL) || (fputs(rule, fd) < 0) || (fclose(fd) != 0) || (rename(tmp, path) != 0)) {
		uprintf("Could not write '%s': %s", path, WindowsErrorString());
		unlink(tmp);
		return FALSE;
	}
	if (!ReloadUdevRules()) {
		uprintf("Could not reload the udev rules");
		return FALSE;
	}
	uprintf("Disabled the automounting of %s", &name[1]);
	return TRUE;
}

void RemoveAutomountRules(void)
{
	char path[PATH_MAX];
	struct dirent* entry;
	DIR* dir = opendir(UDEV_RULES_DIR);
	int removed = 0;

	if (dir == NULL)
		return;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, RULE_PREFIX, strlen(RULE_PREFIX)) != 0)
			continue;
		static_sprintf(path, "%s/%s", UDEV_RULES_DIR, entry->d_name);
		if (unlink(path) == 0)
			removed++;
	}
	closedir(dir);
	if (removed != 0)
		ReloadUdevRules();
}

BOOL EnterPrivateMountNamespace(void)
{
	if (unshare(CLONE_NEWNS) != 0) {
		uprintf("Could not create a mount namespace: %s", WindowsErrorString());
		return FALSE;
	}
	// Otherwise, the mounts would still propagate to the namespace of the system
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
		uprintf("Could not make the mounts private: %s", WindowsErrorString());
		return FALSE;
	}
	return TRUE;
}

char* MountVolume(const char* devnode, const char* fs_type, BOOL read_only)
{
	char dir[] = RUFUS_RUN_DIR "/mnt-XXXXXX";

	if ((mkdir_p(RUFUS_RUN_DIR, 0700) != 0) || (mkdtemp(dir) == NULL)) {
		uprintf("Could not create a mount point in '%s': %s", RUFUS_RUN_DIR, WindowsErrorString());
		return NULL;
	}
	if (mount(devnode, dir, fs_type, MS_NOSUID | MS_NODEV | MS_NOEXEC | (read_only ? MS_RDONLY : 0), NULL) != 0) {
		uprintf("Could not mount %s (%s) on '%s': %s", devnode, fs_type, dir, WindowsErrorString());
		rmdir(dir);
		return NULL;
	}
	uuprintf("Mounted %s on '%s'", devnode, dir);
	return strdup(dir);
}

BOOL UnmountVolume(const char* mount_point)
{
	int i;

	if (mount_point == NULL)
		return FALSE;
	// The data must be on the drive when this returns
	sync();
	for (i = 0; umount2(mount_point, UMOUNT_NOFOLLOW) != 0; i++) {
		if ((errno != EBUSY) || (i >= DRIVE_ACCESS_RETRIES / 10)) {
			uprintf("Could not unmount '%s': %s", mount_point, WindowsErrorString());
			return FALSE;
		}
		usleep(DRIVE_ACCESS_TIMEOUT * 1000 / DRIVE_ACCESS_RETRIES);
	}
	rmdir(mount_point);
	return TRUE;
}

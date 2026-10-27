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
#include <fcntl.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rufus.h"
#include "disk.h"

#define UDEV_RULES_DIR              "/run/udev/rules.d"
#define RULE_PREFIX                 "90-rufus-"
#define RUFUS_RUN_DIR               "/run/rufus"

extern char** environ;

// Run udevadm, even once the operation is cancelled (unlike RunCommand()), as cleaning up depends on it
static BOOL Udevadm(const char* const* argv)
{
	posix_spawn_file_actions_t actions;
	int r, status = 0;
	pid_t pid;

	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	if (!usb_debug)
		posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	r = posix_spawnp(&pid, argv[0], &actions, NULL, (char* const*)argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (r != 0)
		return FALSE;
	while ((waitpid(pid, &status, 0) < 0) && (errno == EINTR));
	return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

static BOOL ReloadUdevRules(void)
{
	const char* argv[] = { "udevadm", "control", "--reload", NULL };

	return Udevadm(argv);
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
	char path[PATH_MAX], devices[MAX_DRIVES][64], parent[96];
	const char* trigger[] = { "udevadm", "trigger", "--settle", "--action=change", parent, NULL };
	struct dirent* entry;
	DIR* dir = opendir(UDEV_RULES_DIR);
	size_t len;
	int i, removed = 0;

	if (dir == NULL)
		return;
	while ((entry = readdir(dir)) != NULL) {
		len = strlen(entry->d_name);
		if ((strncmp(entry->d_name, RULE_PREFIX, strlen(RULE_PREFIX)) != 0) || (len < strlen(RULE_PREFIX) + 7) ||
			(strcmp(&entry->d_name[len - 6], ".rules") != 0))
			continue;
		static_sprintf(path, "%s/%s", UDEV_RULES_DIR, entry->d_name);
		if ((unlink(path) == 0) && (removed < MAX_DRIVES)) {
			// The name of the drive, from "90-rufus-<name>.rules"
			len -= strlen(RULE_PREFIX) + 6;
			if (len < sizeof(devices[0])) {
				memcpy(devices[removed], &entry->d_name[strlen(RULE_PREFIX)], len);
				devices[removed++][len] = 0;
			}
		}
	}
	closedir(dir);
	if (removed == 0)
		return;
	ReloadUdevRules();
	// Without a new event, udisks would still ignore the drive and its partitions
	for (i = 0; i < removed; i++) {
		static_sprintf(parent, "--parent-match=/sys/class/block/%s", devices[i]);
		if (!Udevadm(trigger))
			uprintf("Could not refresh the udev properties of %s", devices[i]);
	}
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
	// The names of the files of the image are UTF-8, whatever the defaults of the kernel are
	const char* data = (strcmp(fs_type, "vfat") == 0) ? "utf8,shortname=mixed" :
		((strcmp(fs_type, "exfat") == 0) || (strcmp(fs_type, "ntfs3") == 0)) ? "iocharset=utf8" : NULL;
	int i;

	if ((mkdir_p(RUFUS_RUN_DIR, 0700) != 0) || (mkdtemp(dir) == NULL)) {
		uprintf("Could not create a mount point in '%s': %s", RUFUS_RUN_DIR, WindowsErrorString());
		return NULL;
	}
	/*
	 * Closing a drive that was open for writing makes udev read its partition table again
	 * (the "watch" of 60-block.rules), which removes its partitions and adds them back:
	 * the partition may be missing for a moment, after the udev queue was found empty.
	 */
	for (i = 0; mount(devnode, dir, fs_type, MS_NOSUID | MS_NODEV | MS_NOEXEC | (read_only ? MS_RDONLY : 0), data) != 0;
		i++) {
		if (((errno != ENOENT) && (errno != ENXIO)) || (i >= DRIVE_ACCESS_RETRIES / 10)) {
			uprintf("Could not mount %s (%s) on '%s': %s", devnode, fs_type, dir, WindowsErrorString());
			rmdir(dir);
			return NULL;
		}
		usleep(DRIVE_ACCESS_TIMEOUT * 1000 / DRIVE_ACCESS_RETRIES);
		WaitForUdev(DRIVE_ACCESS_TIMEOUT);
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

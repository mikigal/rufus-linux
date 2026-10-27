/*
 * Rufus: The Reliable USB Formatting Utility
 * The disks that the system uses
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
 * On Windows, GetDevices() removes the drives that hold the system directory
 * or Rufus itself, from their drive letters. Here, the mounts and swap areas
 * are traced back to the disks they are on, through the device mapper and
 * RAID layers, so that a disk that holds the system is never listed.
 */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "platform.h"

/* How deep the device mapper and RAID layers may be stacked */
#define MAX_STACK_DEPTH             16

BOOL DiskSetContains(const disk_set_t* set, dev_t dev)
{
	size_t i;

	for (i = 0; i < set->count; i++) {
		if (set->dev[i] == dev)
			return TRUE;
	}
	return FALSE;
}

static void DiskSetAdd(disk_set_t* set, dev_t dev)
{
	dev_t* tmp;

	if (DiskSetContains(set, dev))
		return;
	tmp = realloc(set->dev, (set->count + 1) * sizeof(dev_t));
	if (tmp == NULL)
		return;
	set->dev = tmp;
	set->dev[set->count++] = dev;
}

void DiskSetFree(disk_set_t* set)
{
	safe_free(set->dev);
	set->count = 0;
}

// Read a "major:minor" sysfs attribute
static BOOL ReadDevAttr(const char* path, dev_t* dev)
{
	unsigned int maj, min;
	FILE* fd = fopen(path, "r");
	BOOL r;

	if (fd == NULL)
		return FALSE;
	r = (fscanf(fd, "%u:%u", &maj, &min) == 2);
	fclose(fd);
	if (r)
		*dev = makedev(maj, min);
	return r;
}

static void AddDisksOfDeviceDepth(disk_set_t* set, dev_t dev, int depth)
{
	char path[PATH_MAX], real[PATH_MAX], *p;
	DIR* dir;
	struct dirent* entry;
	dev_t slave;
	int nb_slaves = 0;

	if (depth > MAX_STACK_DEPTH)
		return;
	static_sprintf(path, "/sys/dev/block/%u:%u", major(dev), minor(dev));
	if (realpath(path, real) == NULL)
		return;

	// A partition is a subdirectory of its disk in sysfs
	static_sprintf(path, "%s/partition", real);
	if (access(path, F_OK) == 0) {
		p = strrchr(real, '/');
		if (p == NULL)
			return;
		*p = 0;
		static_sprintf(path, "%s/dev", real);
		if (ReadDevAttr(path, &dev))
			AddDisksOfDeviceDepth(set, dev, depth + 1);
		return;
	}

	// A device mapper or RAID device lists the devices it is built on as its slaves
	static_sprintf(path, "%s/slaves", real);
	dir = opendir(path);
	if (dir != NULL) {
		while ((entry = readdir(dir)) != NULL) {
			if (entry->d_name[0] == '.')
				continue;
			static_sprintf(path, "%s/slaves/%s/dev", real, entry->d_name);
			if (ReadDevAttr(path, &slave)) {
				AddDisksOfDeviceDepth(set, slave, depth + 1);
				nb_slaves++;
			}
		}
		closedir(dir);
	}
	if (nb_slaves == 0)
		DiskSetAdd(set, dev);
}

void AddDisksOfDevice(disk_set_t* set, dev_t dev)
{
	AddDisksOfDeviceDepth(set, dev, 0);
}

// Undo the octal escapes (\040 for a space...) of /proc/self/mountinfo
static void Unescape(char* str)
{
	char *src = str, *dst = str;

	while (*src != 0) {
		if ((src[0] == '\\') && (src[1] >= '0') && (src[1] <= '3') && (src[2] >= '0') && (src[2] <= '7') &&
			(src[3] >= '0') && (src[3] <= '7')) {
			*dst++ = (char)(((src[1] - '0') << 6) | ((src[2] - '0') << 3) | (src[3] - '0'));
			src += 4;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = 0;
}

typedef struct {
	dev_t dev;
	char mount_point[PATH_MAX];
	char source[PATH_MAX];
} mount_entry_t;

/*
 * Read the next entry of /proc/self/mountinfo. For the file systems that report
 * an anonymous device (btrfs...), the device is the one of the mount source.
 */
static BOOL NextMount(FILE* fd, mount_entry_t* m)
{
	char line[3 * PATH_MAX], *sep;
	unsigned int maj, min;
	struct stat st;

	while (fgets(line, sizeof(line), fd) != NULL) {
		if (sscanf(line, "%*u %*u %u:%u %*s %4095s", &maj, &min, m->mount_point) != 3)
			continue;
		Unescape(m->mount_point);
		m->source[0] = 0;
		sep = strstr(line, " - ");
		if ((sep == NULL) || (sscanf(sep, " - %*s %4095s", m->source) != 1))
			m->source[0] = 0;
		Unescape(m->source);
		m->dev = makedev(maj, min);
		if ((maj == 0) && (strncmp(m->source, "/dev/", 5) == 0) && (stat(m->source, &st) == 0) &&
			S_ISBLK(st.st_mode))
			m->dev = st.st_rdev;
		return TRUE;
	}
	return FALSE;
}

void AddDisksOfPath(disk_set_t* set, const char* path)
{
	char real[PATH_MAX];
	mount_entry_t m, best = { 0 };
	size_t len, best_len = 0;
	FILE* fd;

	if ((path == NULL) || (realpath(path, real) == NULL))
		return;
	fd = fopen("/proc/self/mountinfo", "r");
	if (fd == NULL)
		return;
	// The mount that holds a path is the last one with the longest matching mount point
	while (NextMount(fd, &m)) {
		len = strlen(m.mount_point);
		if ((strncmp(real, m.mount_point, len) != 0) || ((real[len] != '/') && (real[len] != 0) && (len != 1)))
			continue;
		if (len >= best_len) {
			best = m;
			best_len = len;
		}
	}
	fclose(fd);
	if (best_len != 0)
		AddDisksOfDevice(set, best.dev);
}

void GetSystemDisks(disk_set_t* set)
{
	static const char* system_mounts[] = { "/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home" };
	char line[PATH_MAX + 128], path[PATH_MAX];
	struct stat st;
	mount_entry_t m;
	size_t i;
	FILE* fd;

	fd = fopen("/proc/self/mountinfo", "r");
	if (fd != NULL) {
		while (NextMount(fd, &m)) {
			for (i = 0; i < ARRAYSIZE(system_mounts); i++) {
				if (strcmp(m.mount_point, system_mounts[i]) == 0)
					AddDisksOfDevice(set, m.dev);
			}
		}
		fclose(fd);
	}

	// Swap partitions, and the disks of the file systems that hold swap files
	fd = fopen("/proc/swaps", "r");
	if (fd != NULL) {
		while (fgets(line, sizeof(line), fd) != NULL) {
			if ((sscanf(line, "%4095s", path) != 1) || (path[0] != '/'))
				continue;
			Unescape(path);
			if (stat(path, &st) != 0)
				continue;
			if (S_ISBLK(st.st_mode))
				AddDisksOfDevice(set, st.st_rdev);
			else
				AddDisksOfPath(set, path);
		}
		fclose(fd);
	}
}

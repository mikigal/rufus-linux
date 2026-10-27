/*
 * Rufus: The Reliable USB Formatting Utility
 * Search for the processes that use a drive
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
 * GetProcessSearch() of process.c looks for the handles that processes have
 * on the drive, with NtQuerySystemInformation. Here, /proc tells which
 * processes have one of the devices of the drive open (with the access mode
 * from fdinfo), or a file, a current or root directory, an executable or a
 * mapping on one of its file systems. Without root, only the processes of the
 * user can be seen.
 */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "rufus.h"
#include "disk.h"

#define ACCESS_READ                 0x01
#define ACCESS_WRITE                0x02
#define ACCESS_FS                   0x04	// Uses a file system of the drive

// The access mode of an open file, from /proc/<pid>/fdinfo/<fd>
static int FdAccess(const char* pid, const char* fd_name)
{
	char path[64], line[128];
	unsigned int flags = 0;
	FILE* fd;

	static_sprintf(path, "/proc/%s/fdinfo/%s", pid, fd_name);
	fd = fopen(path, "r");
	if (fd == NULL)
		return ACCESS_READ;
	while (fgets(line, sizeof(line), fd) != NULL) {
		if (sscanf(line, "flags: %o", &flags) == 1)
			break;
	}
	fclose(fd);
	return ((flags & O_ACCMODE) == O_RDONLY) ? ACCESS_READ : (ACCESS_READ | ACCESS_WRITE);
}

// What a process does with the devices of a drive
static int ProcessAccess(const char* pid, const disk_set_t* devs)
{
	static const char* links[] = { "cwd", "root", "exe" };
	char path[PATH_MAX], line[PATH_MAX + 128];
	unsigned int maj, min;
	struct dirent* entry;
	struct stat st;
	size_t i;
	DIR* dir;
	FILE* fd;
	int access = 0;

	static_sprintf(path, "/proc/%s/fd", pid);
	dir = opendir(path);
	if (dir != NULL) {
		while ((entry = readdir(dir)) != NULL) {
			if (entry->d_name[0] == '.')
				continue;
			static_sprintf(path, "/proc/%s/fd/%s", pid, entry->d_name);
			if (stat(path, &st) != 0)
				continue;
			if (S_ISBLK(st.st_mode) && DiskSetContains(devs, st.st_rdev))
				access |= FdAccess(pid, entry->d_name);
			else if (DiskSetContains(devs, st.st_dev))
				access |= ACCESS_FS | FdAccess(pid, entry->d_name);
		}
		closedir(dir);
	}
	for (i = 0; i < ARRAYSIZE(links); i++) {
		static_sprintf(path, "/proc/%s/%s", pid, links[i]);
		if ((stat(path, &st) == 0) && DiskSetContains(devs, st.st_dev))
			access |= ACCESS_FS;
	}
	// Mapped files (libraries, executables...)
	static_sprintf(path, "/proc/%s/maps", pid);
	fd = fopen(path, "r");
	if (fd != NULL) {
		while (fgets(line, sizeof(line), fd) != NULL) {
			if ((sscanf(line, "%*s %*s %*s %x:%x", &maj, &min) == 2) && ((maj != 0) || (min != 0)) &&
				DiskSetContains(devs, makedev(maj, min)))
				access |= ACCESS_FS | ACCESS_READ;
		}
		fclose(fd);
	}
	return access;
}

int SearchProcesses(dev_t disk, StrArray* processes)
{
	disk_set_t devs = { 0 };
	char path[64], comm[64], entry_str[160];
	struct dirent* entry;
	pid_t self = getpid();
	DIR* dir;
	FILE* fd;
	int access, n = 0;

	GetDiskDevices(disk, &devs);
	dir = opendir("/proc");
	if (dir == NULL) {
		DiskSetFree(&devs);
		return 0;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (!isdigit((unsigned char)entry->d_name[0]) || (atoi(entry->d_name) == self))
			continue;
		access = ProcessAccess(entry->d_name, &devs);
		if (access == 0)
			continue;
		comm[0] = 0;
		static_sprintf(path, "/proc/%s/comm", entry->d_name);
		fd = fopen(path, "r");
		if (fd != NULL) {
			if (fgets(comm, sizeof(comm), fd) == NULL)
				comm[0] = 0;
			fclose(fd);
			trim(comm);
		}
		static_sprintf(entry_str, "[%s] %s (%s%s%s)", entry->d_name, (comm[0] != 0) ? comm : "?",
			(access & ACCESS_READ) ? "r" : "", (access & ACCESS_WRITE) ? "w" : "",
			(access & ACCESS_FS) ? ", files" : "");
		StrArrayAdd(processes, entry_str, TRUE);
		n++;
	}
	closedir(dir);
	DiskSetFree(&devs);
	return n;
}

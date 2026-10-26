/*
 * Rufus: The Reliable USB Formatting Utility
 * Marker init for the end-to-end boot tests
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
 * Statically linked /init of the test media's initramfs. Once the boot loader
 * that Rufus installed has started the kernel, this reports to the serial
 * console that the boot worked, along with the kernel command line (to check
 * the boot options that Rufus patched in, e.g. for persistence) and the block
 * devices the kernel sees, then powers the VM off.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>

static int out = -1;

static void say(const char *format, ...)
{
	char buf[1024];
	va_list args;
	int n;

	va_start(args, format);
	n = vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	if (n > 0 && out >= 0)
		(void)!write(out, buf, (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1);
}

static void read_line(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n = (fd >= 0) ? read(fd, buf, size - 1) : -1;

	buf[(n > 0) ? n : 0] = 0;
	buf[strcspn(buf, "\n")] = 0;
	if (fd >= 0)
		close(fd);
}

int main(void)
{
	char cmdline[4096], size[32], path[512];
	struct dirent *de;
	DIR *dir;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);

	out = open("/dev/ttyS0", O_WRONLY | O_NOCTTY);
	if (out < 0)
		out = open("/dev/console", O_WRONLY | O_NOCTTY);

	say("\nRUFUS-E2E-BOOT-OK\n");
	read_line("/proc/cmdline", cmdline, sizeof(cmdline));
	say("RUFUS-E2E-CMDLINE: %s\n", cmdline);
	dir = opendir("/sys/class/block");
	while (dir != NULL && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/block/%s/size", de->d_name);
		read_line(path, size, sizeof(size));
		say("RUFUS-E2E-BLOCK: %s %s\n", de->d_name, size);
	}
	if (dir != NULL)
		closedir(dir);
	say("RUFUS-E2E-DONE\n");

	sync();
	reboot(RB_POWER_OFF);
	return 0;
}

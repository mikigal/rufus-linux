/*
 * Rufus: The Reliable USB Formatting Utility
 * Standard Windows function calls
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "rufus.h"
#include "cregex.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"

extern char** environ;

/*
 * Hash table functions - modified From glibc 2.3.2:
 * [Aho,Sethi,Ullman] Compilers: Principles, Techniques and Tools, 1986
 * [Knuth]            The Art of Computer Programming, part 3 (6.4)
 */

/*
 * For the used double hash method the table size has to be a prime. To
 * correct the user given table size we need a prime test.  This trivial
 * algorithm is adequate because the code is called only during init and
 * the number is likely to be small
 */
static uint32_t isprime(uint32_t number)
{
	// no even number will be passed
	uint32_t divider = 3;

	while((divider * divider < number) && (number % divider != 0))
		divider += 2;

	return (number % divider != 0);
}

/*
 * Before using the hash table we must allocate memory for it.
 * We allocate one element more as the found prime number says.
 * This is done for more effective indexing as explained in the
 * comment for the hash function.
 */
BOOL htab_create(uint32_t nel, htab_table* htab)
{
	if (htab == NULL) {
		return FALSE;
	}
	if_assert_fails(htab->table == NULL) {
		uprintf("WARNING: htab_create() was called with a non empty table");
		return FALSE;
	}

	// Change nel to the first prime number not smaller as nel.
	nel |= 1;
	while(!isprime(nel))
		nel += 2;

	htab->size = nel;
	htab->filled = 0;

	// allocate memory and zero out.
	htab->table = (htab_entry*)calloc(htab->size + 1, sizeof(htab_entry));
	if (htab->table == NULL) {
		uprintf("Could not allocate space for hash table");
		return FALSE;
	}

	return TRUE;
}

/* After using the hash table it has to be destroyed.  */
void htab_destroy(htab_table* htab)
{
	size_t i;

	if ((htab == NULL) || (htab->table == NULL)) {
		return;
	}

	for (i=0; i<htab->size+1; i++) {
		if (htab->table[i].used) {
			safe_free(htab->table[i].str);
		}
	}
	htab->filled = 0; htab->size = 0;
	safe_free(htab->table);
	htab->table = NULL;
}

/*
 * This is the search function. It uses double hashing with open addressing.
 * We use a trick to speed up the lookup. The table is created with one
 * more element available. This enables us to use the index zero special.
 * This index will never be used because we store the first hash index in
 * the field used where zero means not used. Every other value means used.
 * The used field can be used as a first fast comparison for equality of
 * the stored and the parameter value. This helps to prevent unnecessary
 * expensive calls of strcmp.
 */
uint32_t htab_hash(char* str, htab_table* htab)
{
	uint32_t hval, hval2;
	uint32_t idx;
	uint32_t r = 0;
	int c;
	char* sz = str;

	if ((htab == NULL) || (htab->table == NULL) || (str == NULL)) {
		return 0;
	}

	// Compute main hash value using sdbm's algorithm (empirically
	// shown to produce half the collisions as djb2's).
	// See http://www.cse.yorku.ca/~oz/hash.html
	while ((c = *sz++) != 0)
		r = c + (r << 6) + (r << 16) - r;
	if (r == 0)
		++r;

	// compute table hash: simply take the modulus
	hval = r % htab->size;
	if (hval == 0)
		++hval;

	// Try the first index
	idx = hval;

	if (htab->table[idx].used) {
		if ( (htab->table[idx].used == hval)
		  && (safe_strcmp(str, htab->table[idx].str) == 0) ) {
			// existing hash
			return idx;
		}
		// uprintf("Hash collision ('%s' vs '%s')", str, htab->table[idx].str);

		// Second hash function, as suggested in [Knuth]
		hval2 = 1 + hval % (htab->size - 2);

		do {
			// Because size is prime this guarantees to step through all available indexes
			if (idx <= hval2) {
				idx = ((uint32_t)htab->size) + idx - hval2;
			} else {
				idx -= hval2;
			}

			// If we visited all entries leave the loop unsuccessfully
			if (idx == hval) {
				break;
			}

			// If entry is found use it.
			if ( (htab->table[idx].used == hval)
			  && (safe_strcmp(str, htab->table[idx].str) == 0) ) {
				return idx;
			}
		}
		while (htab->table[idx].used);
	}

	// Not found => New entry

	// If the table is full return an error
	if_assert_fails(htab->filled < htab->size) {
		uprintf("Hash table is full (%d entries)", htab->size);
		return 0;
	}

	safe_free(htab->table[idx].str);
	htab->table[idx].used = hval;
	htab->table[idx].str = (char*) malloc(safe_strlen(str) + 1);
	if (htab->table[idx].str == NULL) {
		uprintf("Could not duplicate string for hash table");
		return 0;
	}
	memcpy(htab->table[idx].str, str, safe_strlen(str) + 1);
	++htab->filled;

	return idx;
}

const char* GetEditionName(DWORD ProductType)
{
	static _Thread_local char unknown_edition_str[64] = "";

	// From: https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getproductinfo
	// These values can be found in the winnt.h header.
	switch (ProductType) {
	case 0x00000000: return "";	//  Undefined
	case 0x00000001: return " Ultimate";
	case 0x00000002: return " Home Basic";
	case 0x00000003: return " Home Premium";
	case 0x00000004: return " Enterprise";
	case 0x00000005: return " Home Basic N";
	case 0x00000006: return " Business";
	case 0x00000007: return " Server Standard";
	case 0x00000008: return " Server Datacenter";
	case 0x00000009: return " Smallbusiness Server";
	case 0x0000000A: return " Server Enterprise";
	case 0x0000000B: return " Starter";
	case 0x0000000C: return " Server Datacenter (Core)";
	case 0x0000000D: return " Server Standard (Core)";
	case 0x0000000E: return " Server Enterprise (Core)";
	case 0x00000010: return " Business N";
	case 0x00000011: return " Web Server";
	case 0x00000012: return " HPC Edition";
	case 0x00000013: return " Storage Server (Essentials)";
	case 0x0000001A: return " Home Premium N";
	case 0x0000001B: return " Enterprise N";
	case 0x0000001C: return " Ultimate N";
	case 0x00000022: return " Home Server";
	case 0x00000024: return " Server Standard without Hyper-V";
	case 0x00000025: return " Server Datacenter without Hyper-V";
	case 0x00000026: return " Server Enterprise without Hyper-V";
	case 0x00000027: return " Server Datacenter without Hyper-V (Core)";
	case 0x00000028: return " Server Standard without Hyper-V (Core)";
	case 0x00000029: return " Server Enterprise without Hyper-V (Core)";
	case 0x0000002A: return " Hyper-V Server";
	case 0x0000002F: return " Starter N";
	case 0x00000030: return " Pro";
	case 0x00000031: return " Pro N";
	case 0x00000034: return " Server Solutions Premium";
	case 0x00000035: return " Server Solutions Premium (Core)";
	case 0x00000040: return " Server Hyper Core V";
	case 0x00000042: return " Starter E";
	case 0x00000043: return " Home Basic E";
	case 0x00000044: return " Premium E";
	case 0x00000045: return " Pro E";
	case 0x00000046: return " Enterprise E";
	case 0x00000047: return " Ultimate E";
	case 0x00000048: return " Enterprise (Eval)";
	case 0x0000004F: return " Server Standard (Eval)";
	case 0x00000050: return " Server Datacenter (Eval)";
	case 0x00000054: return " Enterprise N (Eval)";
	case 0x00000057: return " Thin PC";
	case 0x00000058: case 0x00000059: case 0x0000005A: case 0x0000005B: case 0x0000005C: return "Embedded";
	case 0x00000062: return " Home N";
	case 0x00000063: return " Home China";
	case 0x00000064: return " Home Single Language";
	case 0x00000065: return " Home";
	case 0x00000067: return " Pro with Media Center";
	case 0x00000069: case 0x0000006A: case 0x0000006B: case 0x0000006C: return "Embedded";
	case 0x0000006F: return " Home Connected";
	case 0x00000070: return " Pro Student";
	case 0x00000071: return " Home Connected N";
	case 0x00000072: return " Pro Student N";
	case 0x00000073: return " Home Connected Single Language";
	case 0x00000074: return " Home Connected China";
	case 0x00000079: return " Education";
	case 0x0000007A: return " Education N";
	case 0x0000007D: return " Enterprise LTSB";
	case 0x0000007E: return " Enterprise LTSB N";
	case 0x0000007F: return " Pro S";
	case 0x00000080: return " Pro S N";
	case 0x00000081: return " Enterprise LTSB (Eval)";
	case 0x00000082: return " Enterprise LTSB N (Eval)";
	case 0x0000008A: return " Pro Single Language";
	case 0x0000008B: return " Pro China";
	case 0x0000008C: return " Enterprise Subscription";
	case 0x0000008D: return " Enterprise Subscription N";
	case 0x00000091: return " Server Datacenter SA (Core)";
	case 0x00000092: return " Server Standard SA (Core)";
	case 0x00000095: return " Utility VM";
	case 0x000000A1: return " Pro for Workstations";
	case 0x000000A2: return " Pro for Workstations N";
	case 0x000000A4: return " Pro for Education";
	case 0x000000A5: return " Pro for Education N";
	case 0x000000AB: return " Enterprise G";	// I swear Microsoft are just making up editions...
	case 0x000000AC: return " Enterprise G N";
	case 0x000000B2: return " Cloud";
	case 0x000000B3: return " Cloud N";
	case 0x000000B6: return " Home OS";
	case 0x000000B7: case 0x000000CB: return "Cloud E";
	case 0x000000B9: return " IoT OS";
	case 0x000000BA: case 0x000000CA: return "Cloud E N";
	case 0x000000BB: return " IoT Edge OS";
	case 0x000000BC: return " IoT Enterprise";
	case 0x000000BD: return " Lite";
	case 0x000000BF: return " IoT Enterprise S";
	case 0x000000C0: case 0x000000C2: case 0x000000C3: case 0x000000C4: case 0x000000C5: case 0x000000C6: return "XBox";
	case 0x000000C7: case 0x000000C8: case 0x00000196: case 0x00000197: case 0x00000198: return "Azure Server";
	case 0xABCDABCD: return " (Unlicensed)";
	default:
		static_sprintf(unknown_edition_str, " (Unknown Edition 0x%02X)", (uint32_t)ProductType);
		return unknown_edition_str;
	}
}

/*
 * String array manipulation
 */
void StrArrayCreate(StrArray* arr, uint32_t initial_size)
{
	if (arr == NULL) return;
	arr->Max = initial_size; arr->Index = 0;
	arr->String = (char**)calloc(arr->Max, sizeof(char*));
	if (arr->String == NULL)
		uprintf("Could not allocate string array");
}

int32_t StrArrayAdd(StrArray* arr, const char* str, BOOL duplicate)
{
	char** old_table;
	if ((arr == NULL) || (arr->String == NULL) || (str == NULL))
		return -1;
	if (arr->Index == arr->Max) {
		arr->Max *= 2;
		old_table = arr->String;
		arr->String = (char**)realloc(arr->String, arr->Max*sizeof(char*));
		if (arr->String == NULL) {
			free(old_table);
			uprintf("Could not reallocate string array");
			return -1;
		}
	}
	arr->String[arr->Index] = (duplicate)?safe_strdup(str):(char*)str;
	if (arr->String[arr->Index] == NULL) {
		uprintf("Could not store string in array");
		return -1;
	}
	return arr->Index++;
}

int32_t StrArrayFind(StrArray* arr, const char* str)
{
	uint32_t i;
	if ((str == NULL) || (arr == NULL) || (arr->String == NULL))
		return -1;
	for (i = 0; i < arr->Index; i++) {
		if (strcmp(arr->String[i], str) == 0)
			return (int32_t)i;
	}
	return -1;
}

int32_t StrArrayAddUnique(StrArray* arr, const char* str, BOOL duplicate)
{
	int32_t i = StrArrayFind(arr, str);
	return (i < 0) ? StrArrayAdd(arr, str, duplicate) : i;
}

void StrArrayClear(StrArray* arr)
{
	uint32_t i;
	if ((arr == NULL) || (arr->String == NULL))
		return;
	for (i = 0; i < arr->Index; i++) {
		safe_free(arr->String[i]);
	}
	arr->Index = 0;
}

void StrArrayDestroy(StrArray* arr)
{
	StrArrayClear(arr);
	if (arr != NULL)
		safe_free(arr->String);
}

BOOL FileIO(enum file_io_type io_type, char* path, char** buffer, DWORD* size)
{
	struct stat st;
	ssize_t r = -1;
	int fd = -1, flags = 0;
	BOOL ret = FALSE;

	switch (io_type) {
	case FILE_IO_READ:
		*buffer = NULL;
		flags = O_RDONLY;
		break;
	case FILE_IO_WRITE:
		flags = O_WRONLY | O_CREAT | O_TRUNC;
		break;
	case FILE_IO_APPEND:
		flags = O_WRONLY | O_CREAT | O_APPEND;
		break;
	default:
		assert(FALSE);
		break;
	}
	fd = open(path, flags | O_CLOEXEC, 0644);

	if (fd < 0) {
		uprintf("Could not open '%s': %s", path, WindowsErrorString());
		goto out;
	}

	switch (io_type) {
	case FILE_IO_READ:
		if (fstat(fd, &st) != 0)
			break;
		*size = (DWORD)st.st_size;
		*buffer = (char*)malloc(*size);
		if (*buffer == NULL) {
			uprintf("Could not allocate buffer for reading file");
			goto out;
		}
		r = read(fd, *buffer, *size);
		break;
	case FILE_IO_APPEND:
	case FILE_IO_WRITE:
		r = write(fd, *buffer, *size);
		break;
	}

	if (r < 0) {
		uprintf("I/O Error: %s", WindowsErrorString());
		goto out;
	}
	*size = (DWORD)r;

	PrintInfoDebug(0, (io_type == FILE_IO_READ) ? MSG_215 : MSG_216, path);
	ret = TRUE;

out:
	safe_close(fd);
	if (!ret && (io_type == FILE_IO_READ)) {
		// Only leave the buffer allocated if we were able to read data
		safe_free(*buffer);
		*size = 0;
	}
	return ret;
}

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

// Wait for a process to terminate, for up to timeout ms. Returns TRUE if it did, with its exit code.
static BOOL WaitForProcess(pid_t pid, uint64_t timeout, DWORD* exit_code)
{
	uint64_t start = monotonic_ms();
	int status;
	pid_t r;

	while (1) {
		r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			*exit_code = WIFEXITED(status) ? (DWORD)WEXITSTATUS(status) : ERROR_OPERATION_ABORTED;
			return TRUE;
		}
		if ((r < 0) && (errno != EINTR)) {
			*exit_code = WindowsErrorFromErrno(errno);
			return TRUE;
		}
		if (monotonic_ms() - start >= timeout)
			return FALSE;
		usleep(10000);
	}
}

// Run a command, with optional redirection of stdout and stderr to our log as well as
// optional progress reporting if msg is not 0. 'argv' is NULL terminated and its first
// element is looked up in PATH. Returns the exit code of the command, or a Win32 error
// code if the command could not be run.
// NB: Upstream takes a single command line string, which Windows splits. Taking the
// arguments separately means that paths never need any quoting.
DWORD RunCommandWithProgress(const char* const* argv, const char* dir, BOOL log, int msg, const char* pattern)
{
	DWORD ret = ERROR_SUCCESS;
	posix_spawn_file_actions_t actions;
	pid_t pid;
	int r, pipefd[2] = { -1, -1 };
	struct pollfd pfd;
	char output[4096], *p;
	ssize_t len;
	cregex_node_t* node = NULL;
	cregex_program_t* program = NULL;
	char* matches[REGEX_VM_MAX_MATCHES];

	if ((argv == NULL) || (argv[0] == NULL))
		return ERROR_INVALID_PARAMETER;

	posix_spawn_file_actions_init(&actions);
	if (msg != 0 || log) {
		if (pipe2(pipefd, O_CLOEXEC) != 0) {
			ret = WindowsErrorFromErrno(errno);
			uprintf("Could not set commandline pipe: %s", WindowsErrorString());
			goto out;
		}
		posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
		posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
	} else {
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	}
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (dir != NULL)
		posix_spawn_file_actions_addchdir_np(&actions, dir);

	r = posix_spawnp(&pid, argv[0], &actions, NULL, (char* const*)argv, environ);
	if (r != 0) {
		errno = r;
		ret = WindowsErrorFromErrno(r);
		uprintf("Unable to launch command '%s': %s", argv[0], WindowsErrorString());
		goto out;
	}
	safe_close(pipefd[1]);

	if (msg != 0) {
		node = cregex_parse(pattern);
		if (node != NULL) {
			program = cregex_compile_node(node);
			cregex_parse_free(node);
		}
		if (node == NULL || program == NULL)
			uprintf("Internal error: Failed to parse '%s'", pattern);
	}

	if (program != NULL)
		UpdateProgressWithInfoInit(NULL, FALSE);
	while (1) {
		// Check for user cancel
		if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) {
			kill(pid, SIGTERM);
			if (!WaitForProcess(pid, 5000, &ret)) {
				uprintf("Command did not terminate within timeout duration");
				kill(pid, SIGKILL);
				WaitForProcess(pid, 5000, &ret);
			} else {
				uprintf("Command was terminated by user");
			}
			ret = ERROR_CANCELLED;
			goto out;
		}
		if (pipefd[0] >= 0) {
			pfd.fd = pipefd[0];
			pfd.events = POLLIN;
			if ((poll(&pfd, 1, 100) > 0) && (pfd.revents & (POLLIN | POLLHUP))) {
				len = read(pipefd[0], output, sizeof(output) - 1);
				if (len > 0) {
					output[len] = 0;
					// Process a commandline progress into a percentage
					if (program != NULL && cregex_program_run(program, output, (const char**)matches, ARRAYSIZE(matches)) > 0 &&
						matches[2] != NULL && matches[3] != NULL) {
						// matches[2] is for the first group
						// matches[3] is for the end of the first group
						matches[3][0] = '\0';
						float f = 0.0f;
						IGNORE_RETVAL(sscanf(matches[2], "%f", &f));
						UpdateProgressWithInfo(OP_FORMAT, msg, (uint64_t)(f * 100.0f), 100 * 100ULL);
					} else if (log) {
						// output may contain a '%' so don't feed it as a naked format string
						uprintfs(output);
					} else if ((p = strstr(output, "ERROR:")) != NULL) {
						uprintf("%s", p);
					}
				} else {
					// EOF: the command closed its output
					safe_close(pipefd[0]);
				}
			}
		} else {
			usleep(100000);
		}
		if (WaitForProcess(pid, 0, &ret))
			break;
	}

out:
	cregex_compile_free(program);
	posix_spawn_file_actions_destroy(&actions);
	safe_close(pipefd[0]);
	safe_close(pipefd[1]);
	return ret;
}

BOOL CompareGUID(const GUID *guid1, const GUID *guid2) {
	if ((guid1 != NULL) && (guid2 != NULL)) {
		return (memcmp(guid1, guid2, sizeof(GUID)) == 0);
	}
	return FALSE;
}

#ifndef _WIMLIB_FILE_IO_H
#define _WIMLIB_FILE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#ifdef WITH_LIBCDIO
#  include <cdio/iso9660.h>
#  include <cdio/udf.h>
#endif

/* Wrapper around a file descriptor that keeps track of offset (including in
 * pipes, which don't support lseek()) and a cached flag that tells whether the
 * file descriptor is a pipe or not.
 *
 * Rufus: with WITH_LIBCDIO, it can also refer to a file stored inside an
 * ISO-9660 or UDF image (see open_iso_wim_file()), in which case 'fd' is -1
 * and reads go through libcdio.  */
struct filedes {
	int fd;
	unsigned int is_pipe : 1;
#ifdef WITH_LIBCDIO
	unsigned int is_iso : 1;
	unsigned int is_udf : 1;
	iso9660_t *p_iso;
	iso9660_stat_t *p_iso_file;
	udf_t *p_udf;
	udf_dirent_t *p_udf_file;
#endif
	off_t offset;
};

int
full_read(struct filedes *fd, void *buf, size_t n);

int
full_pread(struct filedes *fd, void *buf, size_t nbyte, off_t offset);

int
full_write(struct filedes *fd, const void *buf, size_t n);

int
full_pwrite(struct filedes *fd, const void *buf, size_t count, off_t offset);

#ifndef _WIN32
#  define O_BINARY 0
#endif

off_t
filedes_seek(struct filedes *fd, off_t offset);

bool
filedes_is_seekable(struct filedes *fd);

static inline void filedes_init(struct filedes *fd, int raw_fd)
{
	memset(fd, 0, sizeof(*fd));
	fd->fd = raw_fd;
}

static inline void filedes_invalidate(struct filedes *fd)
{
	fd->fd = -1;
}

#ifdef WITH_LIBCDIO
int
filedes_close(struct filedes *fd);

static inline bool
filedes_valid(const struct filedes *fd)
{
	return fd->fd != -1 || fd->is_iso || fd->is_udf;
}
#else
#define filedes_close(f) close((f)->fd)

static inline bool
filedes_valid(const struct filedes *fd)
{
	return fd->fd != -1;
}
#endif

#endif /* _WIMLIB_FILE_IO_H */

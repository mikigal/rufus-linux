/******************************************************************
    Copyright (C) 2009  Henrik Carlqvist
    Modified for Rufus/Windows (C) 2011-2019  Pete Batard
    Ported to Linux (C) 2026  Rufus for Linux contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
******************************************************************/
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "file.h"

/* Provided by the application */
extern void uprintf(const char *format, ...);
/* Defined in br.c */
extern unsigned long ulBytesPerSector;

/* Returns the number of bytes written or -1 on error (errno is preserved) */
int64_t write_sectors(int fd, uint64_t SectorSize,
                      uint64_t StartSector, uint64_t nSectors,
                      const void *pBuf)
{
   const uint8_t *p = pBuf;
   uint64_t Size = nSectors*SectorSize, Done = 0;
   off_t Offset = (off_t)(StartSector*SectorSize);
   int err;

   if(Size > 0xFFFFFFFFUL)
   {
      uprintf("write_sectors: nSectors x SectorSize is too big\n");
      errno = EINVAL;
      return -1;
   }

   while(Done < Size)
   {
      ssize_t r = pwrite(fd, &p[Done], (size_t)(Size - Done), Offset + (off_t)Done);
      if(r < 0 && errno == EINTR)
         continue;
      if(r <= 0)
      {
         err = (r < 0) ? errno : EIO;
         uprintf("write_sectors: Write error %s\n", strerror(err));
         uprintf("  Wrote: %" PRIu64 ", Expected: %" PRIu64 "\n", Done, Size);
         uprintf("  StartSector: 0x%08" PRIx64 ", nSectors: 0x%" PRIx64 ", SectorSize: 0x%" PRIx64 "\n", StartSector, nSectors, SectorSize);
         errno = err;
         return -1;
      }
      Done += (uint64_t)r;
   }

   return (int64_t)Size;
}

/* Returns the number of bytes read or -1 on error (errno is preserved) */
int64_t read_sectors(int fd, uint64_t SectorSize,
                     uint64_t StartSector, uint64_t nSectors,
                     void *pBuf)
{
   uint8_t *p = pBuf;
   uint64_t Size = nSectors*SectorSize, Done = 0;
   off_t Offset = (off_t)(StartSector*SectorSize);
   int err;

   if(Size > 0xFFFFFFFFUL)
   {
      uprintf("read_sectors: nSectors x SectorSize is too big\n");
      errno = EINVAL;
      return -1;
   }

   while(Done < Size)
   {
      ssize_t r = pread(fd, &p[Done], (size_t)(Size - Done), Offset + (off_t)Done);
      if(r < 0 && errno == EINTR)
         continue;
      if(r < 0)
      {
         err = errno;
         uprintf("read_sectors: Read error %s\n", strerror(err));
         uprintf("  Read: %" PRIu64 ", Expected: %" PRIu64 "\n", Done, Size);
         uprintf("  StartSector: 0x%08" PRIx64 ", nSectors: 0x%" PRIx64 ", SectorSize: 0x%" PRIx64 "\n", StartSector, nSectors, SectorSize);
         errno = err;
         return -1;
      }
      if(r == 0)   /* EOF: short read, as with ReadFile() */
         break;
      Done += (uint64_t)r;
   }

   return (int64_t)Done;
}

/*
* The following calls use a hijacked fp that contains:
* fp->_fd: a file descriptor
* fp->_offset: a file offset
*/

static unsigned char *aligned_buffer(size_t alignment)
{
   void *p = NULL;
   return (posix_memalign(&p, alignment, MAX_DATA_LEN) == 0) ? p : NULL;
}

int contains_data(FILE *fp, uint64_t Position,
	const void *pData, uint64_t Len)
{
   int r = 0;
   unsigned char *aucBuf = aligned_buffer(16);

   if(aucBuf == NULL)
      return 0;

   if(!read_data(fp, Position, aucBuf, Len))
      goto out;

   if(memcmp(pData, aucBuf, (size_t)Len))
      goto out;

   r = 1;

out:
   free(aucBuf);
   return r;
} /* contains_data */

int read_data(FILE *fp, uint64_t Position,
              void *pData, uint64_t Len)
{
   int r = 0;
   unsigned char *aucBuf = aligned_buffer(16);
   FAKE_FD* fd = (FAKE_FD*)fp;
   uint64_t StartSector, EndSector, NumSectors;

   if (aucBuf == NULL)
      return 0;

   Position += fd->_offset;

   StartSector = Position/ulBytesPerSector;
   EndSector   = (Position+Len+ulBytesPerSector -1)/ulBytesPerSector;
   NumSectors  = (size_t)(EndSector - StartSector);

   if((NumSectors*ulBytesPerSector) > MAX_DATA_LEN)
   {
      uprintf("read_data: Please increase MAX_DATA_LEN in file.h\n");
      goto out;
   }

   if(Len > 0xFFFFFFFFUL)
   {
      uprintf("read_data: Len is too big\n");
      goto out;
   }

   if(read_sectors(fd->_fd, ulBytesPerSector, StartSector,
                     NumSectors, aucBuf) != (int64_t)(NumSectors*ulBytesPerSector))
      goto out;

   memcpy(pData, &aucBuf[Position - StartSector*ulBytesPerSector], (size_t)Len);

   r = 1;

out:
   free(aucBuf);
   return r;
}  /* read_data */

/* May read/write the same sector many times, but compatible with existing ms-sys */
int write_data(FILE *fp, uint64_t Position,
               const void *pData, uint64_t Len)
{
   int r = 0;
   /* Direct I/O requires a buffer that is aligned to the sector size */
   unsigned char *aucBuf = aligned_buffer(4096);
   FAKE_FD* fd = (FAKE_FD*)fp;
   uint64_t StartSector, EndSector, NumSectors;

   if (aucBuf == NULL)
      return 0;

   Position += fd->_offset;

   StartSector = Position/ulBytesPerSector;
   EndSector   = (Position+Len+ulBytesPerSector-1)/ulBytesPerSector;
   NumSectors  = EndSector - StartSector;

   if((NumSectors*ulBytesPerSector) > MAX_DATA_LEN)
   {
      uprintf("write_data: Please increase MAX_DATA_LEN in file.h\n");
      goto out;
   }

   if(Len > 0xFFFFFFFFUL)
   {
      uprintf("write_data: Len is too big\n");
      goto out;
   }

   /* Data to write may not be aligned on a sector boundary => read into a sector buffer first */
   if(read_sectors(fd->_fd, ulBytesPerSector, StartSector,
                     NumSectors, aucBuf) != (int64_t)(NumSectors*ulBytesPerSector))
      goto out;

   if(!memcpy(&aucBuf[Position - StartSector*ulBytesPerSector], pData, (size_t)Len))
      goto out;

   if(write_sectors(fd->_fd, ulBytesPerSector, StartSector,
                     NumSectors, aucBuf) <= 0)
      goto out;

   r = 1;

out:
   free(aucBuf);
   return r;
} /* write_data */

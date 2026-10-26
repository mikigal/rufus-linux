/* config.h for libcdio, for Linux with glibc */

/* what to put between the brackets for empty arrays */
#define EMPTY_ARRAY_SIZE

/* DO_NOT_WANT_COMPATIBILITY is set by the build system for all consumers */

/* Define 1 if you want ISO-9660 Joliet extension support. */
#define HAVE_JOLIET 1

/* Define 1 if you want ISO-9660 Rock-Ridge extension support. */
#define HAVE_ROCK 1

/* Define if you have the iconv() function and it works (glibc provides it).
   Required to convert Joliet and UDF names to UTF-8. */
#define HAVE_ICONV 1

/* Define as const if the declaration of iconv() needs const. */
#define ICONV_CONST

/* Headers */
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* Functions */
#define HAVE_CHDIR 1
#define HAVE_DRAND48 1
#define HAVE_FSEEKO 1
#define HAVE_FTRUNCATE 1
#define HAVE_GMTIME_R 1
#define HAVE_LANGINFO_CODESET 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LSTAT 1
#define HAVE_MEMCPY 1
#define HAVE_MEMSET 1
#define HAVE_RAND 1
#define HAVE_READLINK 1
#define HAVE_REALPATH 1
#define HAVE_SETEGID 1
#define HAVE_SETENV 1
#define HAVE_SETEUID 1
#define HAVE_SLEEP 1
#define HAVE_SNPRINTF 1
#define HAVE_STRDUP 1
#define HAVE_STRNDUP 1
#define HAVE_STRTOK_R 1
#define HAVE_TIMEGM 1
#define HAVE_TZSET 1
#define HAVE_UNSETENV 1
#define HAVE_USLEEP 1
#define HAVE_VSNPRINTF 1

/* Types and variables */
#define HAVE_DAYLIGHT 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_STRUCT_TM_TM_ZONE 1
#define HAVE_S_ISLNK 1
#define HAVE_S_ISSOCK 1
#define HAVE_TIMEZONE_VAR 1
#define HAVE_TM_GMTOFF 1
#define HAVE_TZNAME 1

/* Supports ISO _Pragma() macro */
#define HAVE_ISOC99_PRAGMA 1

/* Is set when libcdio's config.h has been included. Applications wishing to
   use their own config.h values (such as set by the application's configure
   script can define this before including any of libcdio's headers. */
#define LIBCDIO_CONFIG_H 1

/* Define WORDS_BIGENDIAN to 1 if your processor stores words with the most
   significant byte first (like Motorola and SPARC, unlike Intel). */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WORDS_BIGENDIAN 1
#endif

# wimlib for Rufus (Linux port)

This directory contains [wimlib](https://wimlib.net) **v1.14.5** (upstream commit
`cd5e231c348c255ae5088873b5a66ee0eb96fa07`), with the upstream layout (`src/`,
`include/`) so that it can be refreshed by copying a newer release over it.

Files that are not needed on Linux were left out: the Windows backends
(`src/win32_*.c`, `src/wimboot.c`) and `src/test_support.c`. `config.h` is
hand-written for Linux/glibc, as upstream generates it with autoconf.

## Rufus patches

All Rufus specific changes are guarded by `WITH_LIBCDIO` or `_RUFUS`:

* **Reading WIMs from inside ISO images** (`WITH_LIBCDIO`, from Pete Batard's
  Windows version): `wimlib_open_wim()` accepts `"<image>|<path in image>"`,
  e.g. `"Win11.iso|sources/install.wim"`, and reads the WIM through libcdio
  (UDF first, then ISO-9660), without mounting or extracting it.
  `include/wimlib/file_io.h`, `src/file_io.c`, `src/wim.c`.
  Compared to the Windows version, this one also honours the current offset in
  `full_read()`, fails reads past the end of the file instead of returning a
  short buffer, supports seeking, and splits on the *last* `|` because `|` is
  a valid character in Linux file names (a name that exists as a regular file
  is never treated as an image path).
* **Logging** (`_RUFUS`): errors and warnings always go to the Rufus log through
  `uprintf()`. `include/wimlib/error.h`.
* **Progress granularity** (`_RUFUS`): progress messages are sent every 1/1000
  of the total (or 256 MiB) instead of every 1/100 (or 1 GiB).
  `include/wimlib/progress.h`.
* **No in-place compaction warning** (`_RUFUS`), as Rufus compacts the
  `boot.wim` on its own media. `src/write.c`.

Rufus: The Reliable USB Formatting Utility (Linux port)
=======================================================

> **This is an unofficial port of [Rufus](https://github.com/pbatard/rufus) to Linux, with
> a Qt 6 user interface that runs natively under Wayland. It is not affiliated with or
> supported by the upstream Rufus project: please report its problems here, not
> upstream. See [LINUX_PORT.md](LINUX_PORT.md) for how it was ported.**

[![Linux Build Status](https://img.shields.io/github/actions/workflow/status/mikigal/rufus-linux/linux.yml?style=flat-square&label=Linux%20Build)](https://github.com/mikigal/rufus-linux/actions/workflows/linux.yml)
[![Latest Release](https://img.shields.io/github/release-pre/mikigal/rufus-linux.svg?style=flat-square&label=Latest%20Release)](https://github.com/mikigal/rufus-linux/releases)
[![Licence](https://img.shields.io/badge/license-GPLv3-blue.svg?style=flat-square&label=License)](https://www.gnu.org/licenses/gpl-3.0.en.html)
[![Download Stats](https://img.shields.io/github/downloads/mikigal/rufus-linux/total.svg?label=Downloads&style=flat-square)](https://github.com/mikigal/rufus-linux/releases)

![Rufus logo](res/icons/rufus-128.png)

Rufus is a utility that helps format and create bootable USB flash drives.

Features
--------

* Format USB, flash card and virtual drives to FAT/FAT32/NTFS/UDF/exFAT/ext2/ext3
* Create DOS bootable USB drives using [FreeDOS](https://www.freedos.org) or MS-DOS
* Create BIOS or UEFI bootable drives, including [UEFI bootable NTFS](https://github.com/pbatard/uefi-ntfs)
* Create bootable drives from bootable ISOs (Windows, Linux, etc.)
* Create bootable drives from bootable disk images, including compressed ones
* Create Windows 11 installation drives for PCs that don't have TPM or Secure Boot
* Create VHD, VHDX and ISO images of an existing drive, and ISO images of optical discs
* Create persistent Linux partitions
* Compute MD5, SHA-1, SHA-256 and SHA-512 checksums of the selected image
* Perform runtime validation of UEFI bootable media
* Improve Windows installation experience by automatically setting up OOBE parameters (local account, privacy options, etc.)
* Perform bad blocks checks, including detection of "fake" flash drives
* Download official Microsoft Windows 8, Windows 10 or Windows 11 retail ISOs
* Download [UEFI Shell](https://github.com/pbatard/UEFI-Shell) ISOs
* Modern and familiar UI, with [38 languages natively supported](https://github.com/pbatard/rufus/wiki/FAQ#What_languages_are_natively_supported_by_Rufus)
* A command line interface, `rufus-cli`, with the same rules as the window
* Secure Boot compatible
* 100% [Free Software](https://www.gnu.org/philosophy/free-sw) ([GPL v3](https://www.gnu.org/licenses/gpl-3.0))

Installation
------------

The [releases](https://github.com/mikigal/rufus-linux/releases) have packages for:

* Arch Linux, which are also in the AUR: `rufus` (built from source) and `rufus-bin`
* Debian 13 and Ubuntu 26.04 (`.deb`)
* Fedora and openSUSE Tumbleweed (`.rpm`)
* any other distribution: an AppImage, and the same files as a `.tar.gz`, with their
  own Qt (so without the style of the desktop). `rufus-cli` is the AppImage started
  with `--cli`, or through a symbolic link named `rufus-cli`.

The window runs as the user. Writing to a drive goes through `rufus-helper`, which
`pkexec` starts as root, with a single password prompt for the session. Rufus needs
`mkfs.fat` (dosfstools) and `mkntfs` (ntfs-3g or ntfsprogs), and uses `mkfs.exfat`
(exfatprogs), `mkudffs` (udftools) and `xorriso` when they are there.

Compilation
-----------

This port only targets Linux. You need CMake (3.25 or later), a C17 and C++20 compiler
and Qt 6 (6.5 or later, Widgets module):

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Pass `-DRUFUS_BUILD_GUI=OFF` to skip the Qt user interface, or `-DRUFUS_WERROR=ON` to
make warnings in Rufus' own code fatal. The libraries it needs are OpenSSL, libcurl,
libext2fs, libudev and hivex (optional). `packaging/` has what builds the packages, and
`tests/e2e` and `tests/gui` describe the end-to-end and GUI tests.

Additional information
----------------------

Rufus provides extensive information about what it is doing through its log (the
log button, or `rufus -v` on the terminal).

* [Website of upstream Rufus](https://rufus.ie)
* [FAQ of upstream Rufus](https://github.com/pbatard/rufus/wiki/FAQ)

Enhancements/Bugs
-----------------

Please use the [issue tracker of this port](https://github.com/mikigal/rufus-linux/issues)
for reporting problems or suggesting new features.

# End-to-end tests

These tests check Rufus the way a user would: by writing to a USB stick and
booting from it. Everything is emulated, so they need no root access on the
host, no physical drive and no human watching a screen. See section 8 of
[LINUX_PORT.md](../../LINUX_PORT.md) for the overall design.

## How it works

* **Root in a VM** (`harness/runner.py`): commands that need root run in a
  KVM guest that boots the host kernel with the host file system, through
  [virtme-ng](https://github.com/arighi/virtme-ng). The target is an image file
  on the host, attached to the guest as a real USB mass storage device (BOT
  or UAS) on an emulated xHCI controller.
* **Boot verification** (`harness/qemu.py`): the written image is booted in QEMU
  as a USB stick, under SeaBIOS or OVMF. Success is observed through:
  * the serial console;
  * the VGA text buffer (BIOS text mode);
  * screenshots read by tesseract OCR.
* **Test media** (`harness/media.py`): small media built locally. They boot the
  host kernel with a static marker `init` (`initramfs/init.c`) that prints
  `RUFUS-E2E-BOOT-OK`, the kernel command line and the block devices to the
  serial console, then powers off.

## Requirements

On Arch Linux / CachyOS:

```
sudo pacman -S --needed qemu-system-x86 qemu-img edk2-ovmf libisoburn mtools syslinux \
    virtme-ng tesseract tesseract-data-eng ntfsprogs exfatprogs udftools wimlib hivex
```

Tests whose tools are missing are skipped rather than failed. KVM (read/write
access to `/dev/kvm`) is strongly recommended: without it, QEMU falls back to
much slower emulation.

## Running

```
cmake -S . -B build -DRUFUS_E2E_TESTS=ON
ctest --test-dir build -L e2e --output-on-failure
```

or directly, for more control:

```
cd tests/e2e && python3 -m unittest -v test_harness
```

Environment variables:

| Variable | Effect |
|---|---|
| `RUFUS_E2E_KEEP=1` | keep the VM work directories (serial logs, screenshots, images) |
| `RUFUS_E2E_WORK=<dir>` | build and cache the test media in `<dir>` |
| `RUFUS_E2E_KERNEL=<path>` | kernel of the root VM and of the test media (default: the running one) |
| `RUFUS_E2E_OVMF_CODE=<path>`, `RUFUS_E2E_OVMF_VARS=<path>` | UEFI firmware (default: the Arch, Debian/Ubuntu or Fedora location) |

Firmware support for USB boot mirrors real hardware: OVMF has no UAS driver,
and SeaBIOS only boots UAS devices on USB 2 ports, so UAS boot tests use
SeaBIOS with an EHCI controller. Writes through the Linux `uas` driver are
covered by the root VM tests.

## Real ISOs (T5)

`test_real_isos.py` writes real ISOs with `rufus-cli`, as the other tests do,
then boots them under BIOS, UEFI and UEFI with Secure Boot (with a db of the
certificates that signed their boot loader), and reads their boot menu, the
shell of the live system or the first screen of Windows Setup with OCR:

| ISO | Written | Booted |
|---|---|---|
| Ubuntu 26.04.1 desktop | ISO mode with persistence, DD mode | BIOS, UEFI + Secure Boot |
| Fedora Workstation 44 | ISO mode (the labels in the GRUB configuration) | BIOS (with the `core.img` of `/boot/grub2`), UEFI + Secure Boot |
| Debian 13.7 netinst | ISO mode (isolinux, Syslinux 6) | BIOS, UEFI + Secure Boot |
| Arch Linux 2026.10.01 | ISO mode | BIOS and UEFI, to the shell of the live system |
| Windows 11 | GPT/NTFS with UEFI:NTFS and the Windows User Experience, FAT32 (split WIM), MBR for BIOS | UEFI + Secure Boot, UEFI, BIOS |

The writes get the network of the VM, for the files that Rufus downloads. The
Linux ISOs are pinned by SHA-256 and only get downloaded (about 11 GB, into
`~/.cache/rufus-tests`) with `RUFUS_TEST_DOWNLOAD=1`. Windows is
`RUFUS_TEST_WINDOWS_ISO=<path>`, or what `rufus-cli download` gets with
`RUFUS_TEST_DOWNLOAD=1`. The sticks are sparse files in the same directory, and
the log of each write and the last screen of a failed boot are kept there.

```
RUFUS_TEST_DOWNLOAD=1 ctest --test-dir build -L slow --output-on-failure
```

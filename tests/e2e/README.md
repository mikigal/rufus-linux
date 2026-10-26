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
| `RUFUS_E2E_KERNEL=<path>` | kernel used by the test media (default: the running one) |

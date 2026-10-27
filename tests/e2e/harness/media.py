# Rufus: The Reliable USB Formatting Utility
# Synthetic test media for the end-to-end tests
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Builds small, deterministic boot media from tools that are installed on the
host, so that the end-to-end tests never need to download anything. Every
medium boots the host kernel with the marker initramfs (see initramfs/init.c),
which reports RUFUS-E2E-BOOT-OK on the serial console."""

import os
import shutil
import stat
import subprocess

from . import qemu

HERE = os.path.dirname(os.path.abspath(__file__))
INIT_SOURCE = os.path.join(HERE, "..", "initramfs", "init.c")

BOOT_MARKER = r"RUFUS-E2E-BOOT-OK"
DONE_MARKER = r"RUFUS-E2E-DONE"
# Appended to the kernel command line of every medium, so that tests can check
# which boot entry (and which boot loader configuration) was used
CMDLINE = "console=ttyS0 panic=-1 rufus.e2e=1"


def missing_tools(*tools: str) -> list[str]:
    return [t for t in tools if shutil.which(t) is None]


def _cpio_entry(name: str, mode: int, data: bytes = b"", rdevmajor: int = 0, rdevminor: int = 0) -> bytes:
    """One "newc" cpio entry, as understood by the kernel's initramfs unpacker.
    Device nodes can be described without having to create them on the host."""
    name_bytes = name.encode() + b"\0"
    fields = [0, mode, 0, 0, 1, 0, len(data), 0, 0, rdevmajor, rdevminor, len(name_bytes), 0]
    header = b"070701" + b"".join(b"%08X" % f for f in fields)
    entry = header + name_bytes
    entry += b"\0" * (-len(entry) % 4)
    entry += data
    entry += b"\0" * (-len(entry) % 4)
    return entry


def build_initramfs(out_dir: str) -> str:
    """Compile the static marker init and pack it into an initramfs."""
    path = os.path.join(out_dir, "initrd.cpio")
    if os.path.exists(path):
        return path
    init = os.path.join(out_dir, "init")
    subprocess.run([os.environ.get("CC", "cc"), "-static", "-Os", "-s", "-o", init, INIT_SOURCE], check=True)
    with open(init, "rb") as f:
        init_data = f.read()
    archive = b"".join([
        _cpio_entry("dev", stat.S_IFDIR | 0o755),
        _cpio_entry("dev/console", stat.S_IFCHR | 0o600, rdevmajor=5, rdevminor=1),
        _cpio_entry("dev/ttyS0", stat.S_IFCHR | 0o600, rdevmajor=4, rdevminor=64),
        _cpio_entry("proc", stat.S_IFDIR | 0o755),
        _cpio_entry("sys", stat.S_IFDIR | 0o755),
        _cpio_entry("init", stat.S_IFREG | 0o755, init_data),
        _cpio_entry("TRAILER!!!", 0),
    ])
    archive += b"\0" * (-len(archive) % 512)
    with open(path, "wb") as f:
        f.write(archive)
    return path


def _stage_kernel(tree: str, out_dir: str) -> None:
    kernel = qemu.host_kernel()
    if kernel is None:
        raise RuntimeError("cannot find a readable host kernel (set RUFUS_E2E_KERNEL)")
    os.makedirs(os.path.join(tree, "boot"), exist_ok=True)
    shutil.copyfile(kernel, os.path.join(tree, "boot", "vmlinuz"))
    shutil.copyfile(build_initramfs(out_dir), os.path.join(tree, "boot", "initrd"))


def build_grub_iso(out_dir: str, extra_cmdline: str = "") -> str:
    """A BIOS + UEFI hybrid ISO made by grub-mkrescue, like most Linux media."""
    path = os.path.join(out_dir, "linux-grub.iso")
    if os.path.exists(path):
        return path
    tree = os.path.join(out_dir, "grub-tree")
    _stage_kernel(tree, out_dir)
    os.makedirs(os.path.join(tree, "boot", "grub"), exist_ok=True)
    with open(os.path.join(tree, "boot", "grub", "grub.cfg"), "w") as f:
        f.write("set timeout=1\n"
                "set default=0\n"
                "terminal_output console\n"
                "menuentry 'Rufus E2E Linux' {\n"
                f"  linux /boot/vmlinuz {CMDLINE} boot=grub {extra_cmdline}\n"
                "  initrd /boot/initrd\n"
                "}\n")
    subprocess.run(["grub-mkrescue", "-o", path, tree, "--", "-volid", "RUFUS_E2E"],
                   check=True, capture_output=True)
    return path


def build_casper_iso(out_dir: str) -> str:
    """An Ubuntu-like ISO made by grub-mkrescue: the kernel in /casper, and
    "boot=casper" on its command line, which Rufus offers persistence for."""
    path = os.path.join(out_dir, "linux-casper.iso")
    if os.path.exists(path):
        return path
    tree = os.path.join(out_dir, "casper-tree")
    os.makedirs(os.path.join(tree, "casper"), exist_ok=True)
    shutil.copyfile(qemu.host_kernel(), os.path.join(tree, "casper", "vmlinuz"))
    shutil.copyfile(build_initramfs(out_dir), os.path.join(tree, "casper", "initrd"))
    os.makedirs(os.path.join(tree, "boot", "grub"), exist_ok=True)
    with open(os.path.join(tree, "boot", "grub", "grub.cfg"), "w") as f:
        f.write("set timeout=1\n"
                "set default=0\n"
                "terminal_output console\n"
                "menuentry 'Rufus E2E casper' {\n"
                f"  linux /casper/vmlinuz {CMDLINE} boot=casper\n"
                "  initrd /casper/initrd\n"
                "}\n")
    subprocess.run(["grub-mkrescue", "-o", path, tree, "--", "-volid", "RUFUS_CASPER"],
                   check=True, capture_output=True)
    return path


def build_syslinux_iso(out_dir: str) -> str:
    """An ISO that Rufus takes for an isolinux one (Syslinux 4). In ISO mode,
    Rufus installs its own ldlinux.sys, so isolinux.bin only needs its version
    string, and the ISO can't boot as such."""
    path = os.path.join(out_dir, "linux-syslinux.iso")
    if os.path.exists(path):
        return path
    tree = os.path.join(out_dir, "syslinux-tree")
    _stage_kernel(tree, out_dir)
    os.makedirs(os.path.join(tree, "isolinux"), exist_ok=True)
    version = b"ISOLINUX 4.07 2013-07-25 Copyright (C) 1994-2013 H. Peter Anvin et al\0"
    data = bytearray(2048)
    data[256:256 + len(version)] = version
    with open(os.path.join(tree, "isolinux", "isolinux.bin"), "wb") as f:
        f.write(data)
    with open(os.path.join(tree, "isolinux", "isolinux.cfg"), "w") as f:
        f.write("DEFAULT linux\n"
                "PROMPT 0\n"
                "TIMEOUT 10\n"
                "LABEL linux\n"
                "  KERNEL /boot/vmlinuz\n"
                f"  APPEND initrd=/boot/initrd {CMDLINE} boot=syslinux\n")
    subprocess.run(["xorriso", "-as", "mkisofs", "-o", path, "-V", "RUFUS_SL", "-J", "-R", tree],
                   check=True, capture_output=True)
    return path


def build_windows_like_iso(out_dir: str) -> str:
    """An ISO that Rufus takes for Windows media (bootmgr, bootmgr.efi), with a
    UEFI boot loader that is a standalone GRUB, which boots the marker kernel
    from the drive it was loaded from (FAT or NTFS)."""
    path = os.path.join(out_dir, "windows-like.iso")
    if os.path.exists(path):
        return path
    tree = os.path.join(out_dir, "windows-tree")
    _stage_kernel(tree, out_dir)
    for name in ("bootmgr", "bootmgr.efi"):
        with open(os.path.join(tree, name), "wb") as f:
            f.write(b"Not the Windows Boot Manager\0" + b"\0" * 4067)
    os.makedirs(os.path.join(tree, "efi", "boot"), exist_ok=True)
    cfg = os.path.join(out_dir, "windows-grub.cfg")
    with open(cfg, "w") as f:
        f.write("insmod part_msdos\n"
                "insmod part_gpt\n"
                "insmod fat\n"
                "insmod ntfs\n"
                "search --no-floppy --file /boot/vmlinuz --set=root\n"
                f"linux ($root)/boot/vmlinuz {CMDLINE} boot=windows-like\n"
                "initrd ($root)/boot/initrd\n"
                "boot\n")
    subprocess.run(["grub-mkstandalone", "-O", "x86_64-efi", "-o", os.path.join(tree, "efi", "boot", "bootx64.efi"),
                    f"boot/grub/grub.cfg={cfg}"], check=True, capture_output=True)
    subprocess.run(["xorriso", "-as", "mkisofs", "-o", path, "-V", "WINDOWS_LIKE", "-J", "-R", tree],
                   check=True, capture_output=True)
    return path


def build_windows11_iso(out_dir: str) -> str:
    """The Windows-like ISO, with the boot.wim and install.wim of a Windows 11
    24H2 media (from rufus-test-mkwim, $RUFUS_TEST_MKWIM), for the Windows User
    Experience."""
    path = os.path.join(out_dir, "windows11-like.iso")
    if os.path.exists(path):
        return path
    build_windows_like_iso(out_dir)
    tree = os.path.join(out_dir, "windows-tree")
    subprocess.run([os.environ["RUFUS_TEST_MKWIM"], tree], check=True, capture_output=True)
    subprocess.run(["xorriso", "-as", "mkisofs", "-o", path, "-V", "CCCOMA_X64FRE_EN-US_DV9", "-J", "-R", tree],
                   check=True, capture_output=True)
    return path


def blank_stick(path: str, size: int = 256 * 1024 * 1024) -> str:
    with open(path, "wb") as f:
        f.truncate(size)
    return path


def dd_to_stick(image: str, stick: str) -> None:
    """What Rufus' DD mode does, minus the safety checks."""
    with open(image, "rb") as src, open(stick, "r+b") as dst:
        shutil.copyfileobj(src, dst, 4 * 1024 * 1024)

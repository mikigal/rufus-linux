# Rufus: The Reliable USB Formatting Utility
# Runs commands as root in a throwaway VM with an emulated USB stick
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Root-only operations (partitioning, mkfs, mounting...) are tested inside a
KVM guest that boots the host kernel with the host root file system, through
virtme-ng. Inside it the test is root, the binaries from the build directory
run unchanged, and the target is a real USB mass storage device backed by an
image file on the host, so the udev/sysfs/block layer code paths are the same
as with a physical stick. No root access to the host is needed."""

import os
import shutil
import subprocess


def vng_binary() -> str | None:
    return shutil.which("vng")


def usb_stick_qemu_opts(image: str, uas: bool = False, name: str = "stick") -> str:
    opts = f"-device qemu-xhci,id=xhci -drive if=none,id={name},format=raw,file={image} "
    if uas:
        opts += (f"-device usb-uas,id={name}-uas,bus=xhci.0 "
                 f"-device scsi-hd,bus={name}-uas.0,scsi-id=0,lun=0,drive={name}")
    else:
        opts += f"-device usb-storage,bus=xhci.0,drive={name},removable=on"
    return opts


def run_in_vm(command: list[str], stick: str | None = None, uas: bool = False,
              rw_dirs: tuple[str, ...] = (), memory: str = "2G", timeout: float = 600) -> subprocess.CompletedProcess:
    """Run 'command' as root in a VM booted from the host kernel. 'rw_dirs' are
    host directories the command may write to (the rest of the host file
    system is read-only in the guest)."""
    if vng_binary() is None:
        raise RuntimeError("virtme-ng (vng) is not installed")
    args = ["vng", "--run", "--user", "root", "--memory", memory, "--cpus", "2"]
    for d in rw_dirs:
        args += ["--rwdir", d]
    if stick is not None:
        args += ["--qemu-opts", usb_stick_qemu_opts(os.path.abspath(stick), uas)]
    args += ["--", *command]
    return subprocess.run(args, capture_output=True, timeout=timeout, stdin=subprocess.DEVNULL)

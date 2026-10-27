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
import signal
import subprocess
import tempfile


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


def virtio_disk_qemu_opts(image: str, name: str = "vdisk") -> str:
    """A non-USB disk, which Rufus must never list."""
    return f"-drive if=none,id={name},format=raw,file={image} -device virtio-blk-pci,drive={name} "


def run_in_vm(script: str, stick: str | None = None, uas: bool = False,
              rw_dirs: tuple[str, ...] = (), memory: str = "2G", timeout: float = 600,
              extra_qemu_opts: str = "", network: bool = False) -> subprocess.CompletedProcess:
    """Run the shell 'script' as root in a VM booted from the host kernel, and
    return its exit status and output. 'rw_dirs' are host directories the
    script may write to (the rest of the host file system is read-only in the
    guest), 'extra_qemu_opts' other devices for the VM, and 'network' gives it
    the user mode network of QEMU (for the downloads of Rufus)."""
    if vng_binary() is None:
        raise RuntimeError("virtme-ng (vng) is not installed")
    # The default "microvm" machine has no PCI bus, hence no USB controller. The kernel is the running
    # one, or $RUFUS_E2E_KERNEL (with its modules in /lib/modules)
    args = ["vng", "--run"] + ([os.environ["RUFUS_E2E_KERNEL"]] if os.environ.get("RUFUS_E2E_KERNEL") else [])
    args += ["--disable-microvm", "--user", "root", "--memory", memory, "--cpus", "2"]
    for d in rw_dirs:
        args.append(f"--rwdir={d}")
    if network:
        args += ["--network", "user"]
    qemu_opts = extra_qemu_opts
    if stick is not None:
        qemu_opts = usb_stick_qemu_opts(os.path.abspath(stick), uas) + " " + qemu_opts
    if qemu_opts.strip():
        # The "=" form is needed as the value starts with a dash
        args.append("--qemu-opts=" + qemu_opts.strip())
    # The command goes to the guest through the kernel command line, which is
    # limited to 2 KB, so the script is given as a file of a read-only share
    with tempfile.TemporaryDirectory(prefix="rufus-e2e-script-") as script_dir:
        path = os.path.join(script_dir, "script.sh")
        with open(path, "w") as f:
            f.write(script)
        args += [f"--rodir={script_dir}", "--exec", f"sh {path}"]
        # vng leaves QEMU running if it gets killed, so kill the whole process group on timeout
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            raise
    return subprocess.CompletedProcess(args, proc.returncode, stdout, stderr)

# Rufus: The Reliable USB Formatting Utility
# QEMU virtual machine driver for boot verification
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Boots disks and ISOs under SeaBIOS or OVMF and observes them through three
channels, so that no human has to look at a screen:

* the serial console (a file),
* the VGA text buffer at 0xB8000 (BIOS text mode: boot menus, DOS prompts,
  firmware errors),
* screenshots run through tesseract OCR (graphical UEFI screens).
"""

import os
import re
import shutil
import subprocess
import tempfile
import time

from .qmp import QMP, QMPError

# (code, vars) firmware pairs, as packaged by Arch, Debian/Ubuntu and Fedora
OVMF_CANDIDATES = (
    ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
    ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
    ("/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),
)

VGA_TEXT_ADDR = 0xB8000
VGA_COLS, VGA_ROWS = 80, 25


def kvm_available() -> bool:
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


def qemu_binary() -> str | None:
    return shutil.which("qemu-system-x86_64")


def ovmf() -> tuple[str, str] | None:
    """Locate the OVMF (UEFI) firmware, overridable with RUFUS_E2E_OVMF_CODE
    and RUFUS_E2E_OVMF_VARS."""
    code, nvram = os.environ.get("RUFUS_E2E_OVMF_CODE"), os.environ.get("RUFUS_E2E_OVMF_VARS")
    if code and nvram:
        return code, nvram
    for code, nvram in OVMF_CANDIDATES:
        if os.path.exists(code) and os.path.exists(nvram):
            return code, nvram
    return None


def host_kernel() -> str | None:
    """The running kernel, which the test media boot with a marker initramfs."""
    env = os.environ.get("RUFUS_E2E_KERNEL")
    if env:
        return env
    release = os.uname().release
    for path in (f"/lib/modules/{release}/vmlinuz", f"/boot/vmlinuz-{release}"):
        if os.access(path, os.R_OK):
            return path
    return None


class VM:
    """A QEMU instance. Use as a context manager, or call start()/stop()."""

    def __init__(self, firmware: str = "bios", memory: int = 1024, workdir: str | None = None):
        if firmware not in ("bios", "uefi"):
            raise ValueError(f"unsupported firmware: {firmware}")
        self.firmware = firmware
        self.memory = memory
        self.own_workdir = workdir is None
        self.workdir = workdir or tempfile.mkdtemp(prefix="rufus-e2e-vm-")
        self.serial_log = os.path.join(self.workdir, "serial.log")
        self.qemu_log = os.path.join(self.workdir, "qemu.log")
        self.qmp_path = os.path.join(self.workdir, "qmp.sock")
        self.args = []
        self.boot_index = 0
        self.proc = None
        self.qmp = None
        self.usb_buses = set()

    # --- Configuration --------------------------------------------------

    def _next_bootindex(self) -> int:
        index = self.boot_index
        self.boot_index += 1
        return index

    def _usb_bus(self, usb2: bool) -> str:
        controller = "ehci" if usb2 else "xhci"
        if controller not in self.usb_buses:
            self.args += ["-device", "usb-ehci,id=ehci" if usb2 else "qemu-xhci,id=xhci"]
            self.usb_buses.add(controller)
        return f"{controller}.0"

    def add_usb_stick(self, image: str, uas: bool = False, usb2: bool = False,
                      readonly: bool = False, name: str = "stick"):
        """Attach a raw image as a USB mass storage device, using either the
        Bulk-Only Transport or UAS, on an xHCI (USB 3) or EHCI (USB 2) port.

        Firmware support differs like on real hardware: OVMF has no UAS driver
        at all, and SeaBIOS only boots UAS devices on USB 2 ports."""
        bus = self._usb_bus(usb2)
        ro = ",readonly=on" if readonly else ""
        self.args += ["-drive", f"if=none,id={name},format=raw,file={image}{ro}"]
        if uas:
            self.args += ["-device", f"usb-uas,id={name}-uas,bus={bus}",
                          "-device", f"scsi-hd,bus={name}-uas.0,scsi-id=0,lun=0,drive={name},"
                                     f"bootindex={self._next_bootindex()}"]
        else:
            self.args += ["-device", f"usb-storage,bus={bus},drive={name},removable=on,"
                                     f"bootindex={self._next_bootindex()}"]
        return self

    def add_cdrom(self, iso: str, name: str = "cd"):
        self.args += ["-drive", f"if=none,id={name},media=cdrom,format=raw,readonly=on,file={iso}",
                      "-device", f"ide-cd,drive={name},bootindex={self._next_bootindex()}"]
        return self

    def direct_kernel(self, kernel: str, initrd: str, cmdline: str):
        self.args += ["-kernel", kernel, "-initrd", initrd, "-append", cmdline]
        return self

    # --- Lifecycle ------------------------------------------------------

    def command_line(self) -> list[str]:
        cmd = [qemu_binary(), "-machine", "q35", "-m", str(self.memory),
               "-nodefaults", "-vga", "std", "-display", "none", "-nic", "none",
               "-no-reboot", "-serial", f"file:{self.serial_log}",
               "-qmp", f"unix:{self.qmp_path},server=on,wait=off"]
        if kvm_available():
            cmd += ["-accel", "kvm", "-cpu", "host"]
        else:
            cmd += ["-accel", "tcg"]
        if self.firmware == "uefi":
            firmware = ovmf()
            if firmware is None:
                raise FileNotFoundError("OVMF firmware not found (set RUFUS_E2E_OVMF_CODE/VARS)")
            vars_copy = os.path.join(self.workdir, "OVMF_VARS.fd")
            shutil.copyfile(firmware[1], vars_copy)
            cmd += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={firmware[0]}",
                    "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}"]
        return cmd + self.args

    def start(self):
        open(self.serial_log, "wb").close()
        with open(self.qemu_log, "wb") as log:
            self.proc = subprocess.Popen(self.command_line(), stdin=subprocess.DEVNULL,
                                         stdout=log, stderr=subprocess.STDOUT)
        try:
            self.qmp = QMP(self.qmp_path)
        except QMPError:
            self.stop()
            with open(self.qemu_log, "rb") as log:
                raise QMPError("QEMU failed to start:\n" + log.read().decode(errors="replace"))
        return self

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.qmp is not None:
            try:
                if self.running():
                    self.qmp.command("quit")
            except (QMPError, OSError):
                pass
            self.qmp.close()
            self.qmp = None
        if self.proc is not None:
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        if self.own_workdir and not os.environ.get("RUFUS_E2E_KEEP"):
            shutil.rmtree(self.workdir, ignore_errors=True)

    def wait_exit(self, timeout: float) -> bool:
        try:
            self.proc.wait(timeout=timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    # --- Observation channels -------------------------------------------

    def serial(self) -> str:
        with open(self.serial_log, "rb") as f:
            return f.read().decode(errors="replace")

    def vga_text(self) -> str:
        """Decode the VGA text mode buffer (CP437 characters, attributes skipped)."""
        dump = os.path.join(self.workdir, "vga.bin")
        self.qmp.command("pmemsave", val=VGA_TEXT_ADDR, size=VGA_COLS * VGA_ROWS * 2, filename=dump)
        with open(dump, "rb") as f:
            raw = f.read()
        chars = bytes(raw[0::2]).decode("cp437")
        rows = [chars[i:i + VGA_COLS].rstrip() for i in range(0, len(chars), VGA_COLS)]
        return "\n".join(rows).rstrip()

    def screenshot(self, name: str = "screen.png") -> str:
        path = os.path.join(self.workdir, name)
        self.qmp.command("screendump", filename=path, format="png")
        return path

    def ocr(self) -> str:
        """Screenshot the display and return the text tesseract finds on it."""
        png = self.screenshot()
        out = subprocess.run(["tesseract", png, "-", "--psm", "6"], capture_output=True)
        return out.stdout.decode(errors="replace")

    def wait_for(self, channel: str, pattern: str, timeout: float, interval: float = 0.5) -> str | None:
        """Poll a channel ("serial", "vga" or "ocr") until 'pattern' (a regex)
        matches. Returns the matching text, or None on timeout or VM exit."""
        reader = {"serial": self.serial, "vga": self.vga_text, "ocr": self.ocr}[channel]
        regex = re.compile(pattern, re.MULTILINE)
        deadline = time.monotonic() + timeout
        while True:
            alive = self.running()
            try:
                text = reader()
            except (QMPError, OSError):
                text = self.serial() if channel == "serial" else ""
            if regex.search(text):
                return text
            if not alive or time.monotonic() > deadline:
                return None
            time.sleep(interval)

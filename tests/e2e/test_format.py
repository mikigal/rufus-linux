# Rufus: The Reliable USB Formatting Utility
# The format pipeline, from the drive list to a stick that boots
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Runs tests/test_format_vm.c as root in a VM against an emulated USB stick
(T2), then boots the sticks that are meant to boot in QEMU (T3). Run with:

    RUFUS_TEST_FORMAT_VM=build/tests/test_format_vm python3 -m unittest -v test_format
"""

import os
import shutil
import tempfile
import unittest

from harness import media, qemu, runner

TEST = os.environ.get("RUFUS_TEST_FORMAT_VM")

# usb-storage scans a new device after a delay, so wait for the stick first
SCRIPT = r"""
for i in $(seq 100); do
	udevadm settle
	for d in /sys/block/sd*; do [ -e "$d" ] && break 2; done
	sleep 0.2
done
udevadm settle
RUFUS_TEST_VM=1 {test} {args}
"""

WORK = None
ISO = None


def setUpModule():
    global WORK
    WORK = tempfile.mkdtemp(prefix="rufus-e2e-format-")


def tearDownModule():
    if WORK and not os.environ.get("RUFUS_E2E_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)


class FormatPipeline(unittest.TestCase):

    def setUp(self):
        if TEST is None or not os.access(TEST, os.X_OK):
            self.skipTest("RUFUS_TEST_FORMAT_VM does not point to test_format_vm")
        if runner.vng_binary() is None:
            self.skipTest("virtme-ng (vng) is not installed")
        if not qemu.kvm_available():
            self.skipTest("KVM is not available")

    def _iso(self) -> str:
        global ISO
        missing = media.missing_tools("grub-mkrescue", "xorriso", "mformat")
        if missing:
            self.skipTest("missing tools: " + ", ".join(missing))
        if qemu.host_kernel() is None:
            self.skipTest("no readable host kernel")
        if ISO is None:
            ISO = media.build_grub_iso(WORK)
        return ISO

    def _format(self, name: str, *args: str) -> str:
        stick = media.blank_stick(os.path.join(WORK, name + ".img"), 256 * 1024 * 1024)
        script = SCRIPT.format(test=os.path.abspath(TEST), args=" ".join(args))
        result = runner.run_in_vm(script, stick=stick, rw_dirs=(WORK,), timeout=600)
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertEqual(result.returncode, 0, output[-6000:])
        self.assertIn("FORMAT TEST PASSED", output)
        return stick

    def _boot(self, stick: str, firmware: str):
        if firmware == "uefi" and qemu.ovmf() is None:
            self.skipTest("OVMF is not installed")
        vm = qemu.VM(firmware)
        vm.add_usb_stick(stick)
        with vm:
            text = vm.wait_for("serial", media.DONE_MARKER, timeout=120)
            self.assertIsNotNone(text, f"serial:\n{vm.serial()[-2000:]}")
            self.assertRegex(text, r"RUFUS-E2E-CMDLINE: .*boot=grub")

    def test_fat32(self):
        self._format("fat32", "fat32")

    def test_ext4(self):
        self._format("ext4", "ext4")

    def test_iso_mode_uefi(self):
        stick = self._format("iso-uefi", "iso-uefi", self._iso())
        self._boot(stick, "uefi")

    def test_iso_mode_bios(self):
        # The GRUB of the host must be the one of the embedded core.img (GRUB2_PACKAGE_VERSION)
        stick = self._format("iso-bios", "iso-bios", self._iso())
        self._boot(stick, "bios")

    def test_syslinux4(self):
        stick = self._format("syslinux4", "syslinux4")
        vm = qemu.VM("bios")
        vm.add_usb_stick(stick)
        with vm:
            # Without a syslinux.cfg, Syslinux stops at its prompt
            text = vm.wait_for("vga", r"(?s)SYSLINUX 4\.\d+ .*\nboot:", timeout=60)
            self.assertIsNotNone(text, f"vga:\n{vm.vga_text()}")

    def test_freedos(self):
        stick = self._format("freedos", "freedos")
        vm = qemu.VM("bios")
        vm.add_usb_stick(stick)
        with vm:
            # The language menu of CONFIG.SYS picks the Polish keyboard after 5 seconds
            text = vm.wait_for("vga", r"(?s)FreeDOS KEYB .*Keyboard layout *: .*:PL \[852\].*C:\\>", timeout=90)
            self.assertIsNotNone(text, f"vga:\n{vm.vga_text()}")

    def test_bad_blocks(self):
        self._format("badblocks", "badblocks")

    def test_dd_mode(self):
        iso = self._iso()
        stick = self._format("dd", "dd", iso)
        with open(iso, "rb") as a, open(stick, "rb") as b:
            data = a.read()
            self.assertEqual(b.read(len(data)), data)
        self._boot(stick, "bios")
        self._boot(stick, "uefi")


if __name__ == "__main__":
    unittest.main()

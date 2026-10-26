# Rufus: The Reliable USB Formatting Utility
# Self-tests of the end-to-end test harness
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Checks that every observation channel of the harness can tell a successful
boot from a failed one, and that the synthetic media boot both from a CD and
as a dd-written USB stick, under BIOS and UEFI. Run with:

    python3 -m unittest discover -s tests/e2e -v
"""

import os
import shutil
import tempfile
import unittest

from harness import media, qemu, runner

WORK = None


def setUpModule():
    global WORK
    if qemu.qemu_binary() is None:
        raise unittest.SkipTest("qemu-system-x86_64 is not installed")
    WORK = os.environ.get("RUFUS_E2E_WORK") or tempfile.mkdtemp(prefix="rufus-e2e-")
    os.makedirs(WORK, exist_ok=True)


def tearDownModule():
    if WORK and not os.environ.get("RUFUS_E2E_WORK") and not os.environ.get("RUFUS_E2E_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)


class ObservationChannels(unittest.TestCase):

    def test_serial_marker_from_direct_kernel_boot(self):
        kernel = qemu.host_kernel()
        if kernel is None:
            self.skipTest("no readable host kernel")
        initrd = media.build_initramfs(WORK)
        with qemu.VM("bios").direct_kernel(kernel, initrd, media.CMDLINE + " rufus.e2e.case=direct") as vm:
            text = vm.wait_for("serial", media.DONE_MARKER, timeout=90)
            self.assertIsNotNone(text, vm.serial()[-2000:])
            self.assertIn(media.BOOT_MARKER, text)
            self.assertRegex(text, r"RUFUS-E2E-CMDLINE: .*rufus\.e2e\.case=direct")
            self.assertTrue(vm.wait_exit(30), "the marker init did not power the VM off")

    def test_vga_text_shows_seabios_error_without_bootable_media(self):
        stick = media.blank_stick(os.path.join(WORK, "blank-bios.img"), 16 * 1024 * 1024)
        with qemu.VM("bios").add_usb_stick(stick) as vm:
            text = vm.wait_for("vga", r"No bootable device", timeout=60)
            self.assertIsNotNone(text, vm.vga_text())
            self.assertIsNone(vm.wait_for("serial", media.BOOT_MARKER, timeout=1))

    def test_ocr_reads_ovmf_screen_without_bootable_media(self):
        if media.missing_tools("tesseract"):
            self.skipTest("tesseract is not installed")
        stick = media.blank_stick(os.path.join(WORK, "blank-uefi.img"), 16 * 1024 * 1024)
        with qemu.VM("uefi").add_usb_stick(stick) as vm:
            text = vm.wait_for("ocr", r"(?i)(shell>|no bootable|boot manager)", timeout=90, interval=3)
            self.assertIsNotNone(text, vm.ocr())

    def test_wait_for_times_out(self):
        stick = media.blank_stick(os.path.join(WORK, "blank-timeout.img"), 16 * 1024 * 1024)
        with qemu.VM("bios").add_usb_stick(stick) as vm:
            self.assertIsNone(vm.wait_for("serial", r"this never appears", timeout=2))


class SyntheticMedia(unittest.TestCase):

    def setUp(self):
        missing = media.missing_tools("grub-mkrescue", "xorriso", "mformat")
        if missing:
            self.skipTest("missing tools: " + ", ".join(missing))
        if qemu.host_kernel() is None:
            self.skipTest("no readable host kernel")
        self.iso = media.build_grub_iso(WORK)

    def _boot(self, firmware: str, as_usb: bool, uas: bool = False):
        vm = qemu.VM(firmware)
        if as_usb:
            stick = media.blank_stick(os.path.join(vm.workdir, "stick.img"))
            media.dd_to_stick(self.iso, stick)
            vm.add_usb_stick(stick, uas=uas)
        else:
            vm.add_cdrom(self.iso)
        with vm:
            text = vm.wait_for("serial", media.DONE_MARKER, timeout=120)
            self.assertIsNotNone(text, f"serial:\n{vm.serial()[-2000:]}")
            self.assertRegex(text, r"RUFUS-E2E-CMDLINE: .*boot=grub")

    def test_grub_iso_bios_cdrom(self):
        self._boot("bios", as_usb=False)

    def test_grub_iso_uefi_cdrom(self):
        self._boot("uefi", as_usb=False)

    def test_grub_iso_bios_usb(self):
        self._boot("bios", as_usb=True)

    def test_grub_iso_uefi_usb(self):
        self._boot("uefi", as_usb=True)

    def test_grub_iso_uefi_usb_uas(self):
        self._boot("uefi", as_usb=True, uas=True)


class RootVM(unittest.TestCase):
    """The privileged tier: commands run as root in a VM, against a USB stick
    that is an image file on the host."""

    def setUp(self):
        if runner.vng_binary() is None:
            self.skipTest("virtme-ng (vng) is not installed")
        if not qemu.kvm_available():
            self.skipTest("KVM is not available")

    def _check_stick(self, uas: bool):
        stick = media.blank_stick(os.path.join(WORK, f"root-{int(uas)}.img"), 64 * 1024 * 1024)
        driver = "uas" if uas else "usb-storage"
        # Find the stick through udev like Rufus will, check its bus and
        # driver, then write a marker to it as root
        script = (
            "set -e; udevadm settle; "
            "for d in /sys/block/sd*; do dev=/dev/${d##*/}; "
            "  if udevadm info -q property -n $dev | grep -qx ID_BUS=usb; then break; fi; done; "
            "udevadm info -q property -n $dev | grep -x ID_BUS=usb; "
            f"udevadm info -q property -n $dev | grep -x ID_USB_DRIVER={driver}; "
            "printf RUFUS-E2E-ROOT-WRITE | dd of=$dev bs=512 seek=1 conv=fsync status=none; "
            "id -u"
        )
        result = runner.run_in_vm(["sh", "-c", script], stick=stick, uas=uas)
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertEqual(result.returncode, 0, output[-3000:])
        self.assertIn(f"ID_USB_DRIVER={driver}", output)
        self.assertRegex(output, r"(?m)^0$")
        with open(stick, "rb") as f:
            f.seek(512)
            self.assertEqual(f.read(20), b"RUFUS-E2E-ROOT-WRITE")

    def test_usb_storage_stick_written_as_root(self):
        self._check_stick(uas=False)

    def test_uas_stick_written_as_root(self):
        self._check_stick(uas=True)


if __name__ == "__main__":
    unittest.main()

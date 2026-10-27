# Rufus: The Reliable USB Formatting Utility
# The disk access, with emulated USB sticks
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Runs tests/test_disk_vm.c as root in a VM, against an emulated USB stick
(BOT and UAS): identity checks, busy drives, unmounting, exclusive opens,
partition refresh, automount suppression and private mounts. Run with:

    RUFUS_TEST_DISK_VM=build/tests/test_disk_vm python3 -m unittest -v test_disk
"""

import os
import shutil
import tempfile
import unittest

from harness import media, qemu, runner

TEST = os.environ.get("RUFUS_TEST_DISK_VM")

# usb-storage scans a new device after a delay, so wait for the stick first
SCRIPT = r"""
for i in $(seq 100); do
	udevadm settle
	for d in /sys/block/sd*; do [ -e "$d" ] && break 2; done
	sleep 0.2
done
udevadm settle
RUFUS_TEST_VM=1 {test}
"""


class DiskAccess(unittest.TestCase):

    def setUp(self):
        if TEST is None or not os.access(TEST, os.X_OK):
            self.skipTest("RUFUS_TEST_DISK_VM does not point to test_disk_vm")
        if runner.vng_binary() is None:
            self.skipTest("virtme-ng (vng) is not installed")
        if not qemu.kvm_available():
            self.skipTest("KVM is not available")
        self.work = tempfile.mkdtemp(prefix="rufus-e2e-disk-")

    def tearDown(self):
        if not os.environ.get("RUFUS_E2E_KEEP"):
            shutil.rmtree(self.work, ignore_errors=True)

    def _check(self, uas: bool):
        stick = media.blank_stick(os.path.join(self.work, "stick.img"), 128 * 1024 * 1024)
        result = runner.run_in_vm(SCRIPT.format(test=os.path.abspath(TEST)), stick=stick, uas=uas, timeout=300)
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertEqual(result.returncode, 0, output[-6000:])
        self.assertIn("ALL DISK TESTS PASSED", output)

    def test_usb_storage_stick(self):
        self._check(uas=False)

    def test_uas_stick(self):
        self._check(uas=True)


if __name__ == "__main__":
    unittest.main()

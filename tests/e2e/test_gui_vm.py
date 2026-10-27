# Rufus: The Reliable USB Formatting Utility
# The GUI with the real helper, as root in a VM against an emulated USB stick
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Runs tests/gui/vm_write.py (the GUI under headless KWin, through AT-SPI) as root
in a VM with an emulated USB stick (T2), then checks the drive it wrote. Run with:

    RUFUS_GUI=build/src/ui/qt/rufus RUFUS_HELPER=build/src/helper/rufus-helper \\
        python3 -m unittest -v test_gui_vm
"""

import os
import shutil
import sys
import tempfile
import unittest

from harness import media, qemu, runner

GUI = os.environ.get("RUFUS_GUI")
HELPER = os.environ.get("RUFUS_HELPER")
GUI_TESTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "gui"))

SCRIPT = r"""
for i in $(seq 100); do
	udevadm settle
	for d in /sys/block/sd*; do [ -e "$d" ] && break 2; done
	sleep 0.2
done
udevadm settle
dev=/dev/$(basename $(ls -d /sys/block/sd* | head -1))
echo "STICK: $dev"
cd {gui_tests} && python3 vm_write.py {gui} {helper} "$dev" || exit 1
udevadm settle
fsck.fat -n "${{dev}}1" || exit 1
blkid -p -o value -s LABEL "${{dev}}1"
blkid -p -o value -s LABEL "${{dev}}1" | grep -qx "GUI TEST" && echo "VM TEST PASSED"
"""


class GuiWithHelper(unittest.TestCase):

    def setUp(self):
        for name, value in (("RUFUS_GUI", GUI), ("RUFUS_HELPER", HELPER)):
            if value is None or not os.access(value, os.X_OK):
                self.skipTest(f"{name} does not point to an executable")
        if runner.vng_binary() is None:
            self.skipTest("virtme-ng (vng) is not installed")
        if not qemu.kvm_available():
            self.skipTest("KVM is not available")
        sys.path.insert(0, GUI_TESTS)
        import gui
        missing = gui.tools_missing("kwin")
        if missing:
            self.skipTest("missing: " + ", ".join(missing))

    def test_write(self):
        work = tempfile.mkdtemp(prefix="rufus-e2e-gui-")
        self.addCleanup(lambda: None if os.environ.get("RUFUS_E2E_KEEP") else shutil.rmtree(work, True))
        stick = media.blank_stick(os.path.join(work, "stick.img"), 256 * 1024 * 1024)
        script = SCRIPT.format(gui_tests=GUI_TESTS, gui=os.path.abspath(GUI), helper=os.path.abspath(HELPER))
        # NB: Not the /tmp of the host, where a lock file of an earlier VM (whose PID may be in use) would stay
        result = runner.run_in_vm(script, stick=stick, rw_dirs=(work,), timeout=900)
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertIn("GUI TEST PASSED", output, output[-8000:])
        self.assertIn("VM TEST PASSED", output, output[-8000:])
        self.assertIn("Started " + os.path.abspath(HELPER), output)


if __name__ == "__main__":
    unittest.main()

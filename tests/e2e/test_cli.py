# Rufus: The Reliable USB Formatting Utility
# rufus-cli and rufus-helper, from the drive list to a stick that boots
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Runs tests/test_helper_vm.c, then rufus-cli writes through rufus-helper, as
root in a VM against an emulated USB stick (T2), and the sticks that are meant
to boot get booted in QEMU (T3). Run with:

    RUFUS_CLI=build/src/cli/rufus-cli RUFUS_HELPER=build/src/helper/rufus-helper \\
        RUFUS_TEST_HELPER_VM=build/tests/test_helper_vm python3 -m unittest -v test_cli
"""

import os
import shutil
import tempfile
import unittest

from harness import media, qemu, runner

CLI = os.environ.get("RUFUS_CLI")
HELPER = os.environ.get("RUFUS_HELPER")
TEST = os.environ.get("RUFUS_TEST_HELPER_VM")

# usb-storage scans a new device after a delay, so wait for the stick first.
# As root, rufus-cli starts the helper directly rather than through pkexec.
SCRIPT = r"""
for i in $(seq 100); do
	udevadm settle
	for d in /sys/block/sd*; do [ -e "$d" ] && break 2; done
	sleep 0.2
done
udevadm settle
export RUFUS_HELPER={helper}
dev=$({cli} list --hdd --json | python3 -c 'import json, sys; print([d["device"] for d in json.load(sys.stdin) if d["vid_pid"].startswith("46f4:")][0])')
echo "STICK: $dev"
{commands}
"""

WORK = None
ISO = None


def setUpModule():
    global WORK
    WORK = tempfile.mkdtemp(prefix="rufus-e2e-cli-")


def tearDownModule():
    if WORK and not os.environ.get("RUFUS_E2E_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)


class CommandLine(unittest.TestCase):

    def setUp(self):
        for name, value in (("RUFUS_CLI", CLI), ("RUFUS_HELPER", HELPER)):
            if value is None or not os.access(value, os.X_OK):
                self.skipTest(f"{name} does not point to an executable")
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

    def _run(self, name: str, commands: str) -> tuple[str, str]:
        stick = media.blank_stick(os.path.join(WORK, name + ".img"), 256 * 1024 * 1024)
        script = SCRIPT.format(cli=os.path.abspath(CLI), helper=os.path.abspath(HELPER), commands=commands)
        result = runner.run_in_vm(script, stick=stick, rw_dirs=(WORK,), timeout=600)
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertEqual(result.returncode, 0, output[-6000:])
        return stick, output

    def _write(self, name: str, *args: str) -> str:
        cli = os.path.abspath(CLI)
        stick, output = self._run(name, f'{cli} -v write "$dev" --hdd --yes {" ".join(args)} && echo "CLI TEST PASSED"')
        self.assertIn("CLI TEST PASSED", output, output[-6000:])
        self.assertIn("Done", output)
        return stick

    def _boot(self, stick: str, firmware: str):
        if firmware == "uefi" and qemu.ovmf() is None:
            self.skipTest("OVMF is not installed")
        vm = qemu.VM(firmware)
        vm.add_usb_stick(stick)
        with vm:
            text = vm.wait_for("serial", media.DONE_MARKER, timeout=120)
            self.assertIsNotNone(text, f"serial:\n{vm.serial()[-2000:]}")

    def test_helper(self):
        if TEST is None or not os.access(TEST, os.X_OK):
            self.skipTest("RUFUS_TEST_HELPER_VM does not point to test_helper_vm")
        _, output = self._run("helper", f"RUFUS_TEST_VM=1 {os.path.abspath(TEST)}")
        self.assertIn("HELPER TESTS PASSED", output)

    def test_non_bootable(self):
        # The defaults of the main window: FAT32 on MBR, with the size as the label
        _, output = self._run("non-bootable", f'{os.path.abspath(CLI)} write "$dev" --hdd --yes --label "Dane USB" && '
                              'udevadm settle && fsck.fat -n "${dev}1" && '
                              'blkid -p -o value -s LABEL "${dev}1" | grep -qx "DANE USB" && echo "CLI TEST PASSED"')
        self.assertIn("CLI TEST PASSED", output, output[-6000:])
        # The confirmation of upstream, answered by --yes
        self.assertIn("WARNING: ALL DATA ON DEVICE", output)

    def test_iso_mode_uefi(self):
        stick = self._write("iso-uefi", "--image", self._iso(), "--iso", "--partition", "gpt", "--target", "uefi")
        self._boot(stick, "uefi")

    def test_dd_mode(self):
        iso = self._iso()
        stick = self._write("dd", "--image", iso, "--dd")
        with open(iso, "rb") as a, open(stick, "rb") as b:
            data = a.read()
            self.assertEqual(b.read(len(data)), data)
        self._boot(stick, "bios")

    def test_cycle_port(self):
        # Alt-C: the stick goes away, and comes back as a new disk
        cli = os.path.abspath(CLI)
        _, output = self._run("cycle", f'''
seq=$(cat /sys/block/${{dev#/dev/}}/diskseq)
{cli} -v cycle "$dev" --hdd || exit 1
for i in $(seq 100); do
	udevadm settle
	new=$(cat /sys/block/sd*/diskseq 2>/dev/null | head -1)
	[ -n "$new" ] && [ "$new" != "$seq" ] && break
	sleep 0.2
done
echo "DISKSEQ: $seq -> $new"
[ -n "$new" ] && [ "$new" != "$seq" ] && echo "CLI TEST PASSED"
''')
        self.assertIn("Cycling port", output)
        self.assertIn("CLI TEST PASSED", output, output[-6000:])

    def test_virtual_disk(self):
        # The loop device of a file is a VHD: listed, and written to like a drive
        cli = os.path.abspath(CLI)
        _, output = self._run("vhd", f'''
modprobe loop 2>/dev/null
truncate -s 64M /tmp/vhd.img
loop=$(losetup -fP --show /tmp/vhd.img)
udevadm settle
{cli} list | grep -q "Loop Device (vhd.img)" || exit 1
{cli} -v write "$loop" --yes --fs FAT32 && udevadm settle && fsck.fat -n "${{loop}}p1" && echo "CLI TEST PASSED"
losetup -d "$loop"
''')
        self.assertIn("Found VHD device 'Loop Device (vhd.img)'", output)
        self.assertIn("CLI TEST PASSED", output, output[-6000:])

    def test_interrupt(self):
        # SIGINT cancels the helper, which cleans up after itself
        cli = os.path.abspath(CLI)
        _, output = self._run("interrupt", f'''
{cli} -v write "$dev" --hdd --yes --bad-blocks tlc 2> /tmp/cli.log &
pid=$!
for i in $(seq 400); do
	grep -q "Bad Blocks: Writing test pattern" /tmp/cli.log && break
	sleep 0.05
done
kill -INT $pid
wait $pid
echo "EXIT: $?"
cat /tmp/cli.log
ls /run/udev/rules.d/ | grep -i rufus && exit 1
grep /run/rufus /proc/self/mountinfo && exit 1
pgrep -x rufus-helper && exit 1
echo "CLI TEST PASSED"
''')
        self.assertIn("EXIT: 1", output, output[-6000:])
        self.assertIn("Cancelling the operation of the helper", output)
        self.assertIn("Failed: ", output)
        self.assertIn("CLI TEST PASSED", output, output[-6000:])


if __name__ == "__main__":
    unittest.main()

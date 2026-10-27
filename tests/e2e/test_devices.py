# Rufus: The Reliable USB Formatting Utility
# The drive list, with emulated USB sticks
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Runs 'rufus-cli list' in a VM that has an emulated USB stick (BOT or UAS)
and a virtio disk. The stick must be listed with its USB properties, the
virtio disk never, and the stick no longer either once the system uses it
(mounted on /var, as swap, or below the device mapper). Run with:

    RUFUS_CLI=build/src/cli/rufus-cli python3 -m unittest -v test_devices
"""

import json
import os
import shutil
import tempfile
import unittest

from harness import media, qemu, runner

CLI = os.environ.get("RUFUS_CLI")

# Inside the VM: wait for the stick (usb-storage scans a new device after a
# delay), then list the drives in each situation. The outputs go to $out.
SCRIPT = r"""
set -e
cli={cli}; out={out}
list() {{ name=$1; shift; "$cli" -vv list --json "$@" > "$out/$name.json" 2> "$out/$name.log"; }}
count() {{ python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))))" "$1"; }}
for i in $(seq 100); do
	udevadm settle
	list default
	[ "$(count "$out/default.json")" -ge 1 ] && break
	sleep 0.2
done
list all --hdd --non-usb
dev=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[0]['device'])" "$out/default.json")

# Mounted on a system directory (not /home, where rufus-cli may be)
mkfs.ext4 -q -F -L SYSTEM "$dev"
mount -t ext4 "$dev" /var
udevadm settle
list var --hdd --non-usb
umount /var

# Used as swap
mkswap -q "$dev" > /dev/null
swapon "$dev"
list swap --hdd --non-usb
swapoff "$dev"
wipefs -q -a "$dev"

# Below the device mapper, as LVM or LUKS would put it
if command -v dmsetup > /dev/null; then
	echo "0 $(blockdev --getsz "$dev") linear $dev 0" | dmsetup create rufus-e2e
	mkfs.ext4 -q -F /dev/mapper/rufus-e2e
	mount -t ext4 /dev/mapper/rufus-e2e /boot
	list dm --hdd --non-usb
	umount /boot
	dmsetup remove rufus-e2e
fi
echo DONE
"""


class DeviceList(unittest.TestCase):

    def setUp(self):
        if CLI is None or not os.access(CLI, os.X_OK):
            self.skipTest("RUFUS_CLI does not point to rufus-cli")
        if runner.vng_binary() is None:
            self.skipTest("virtme-ng (vng) is not installed")
        if not qemu.kvm_available():
            self.skipTest("KVM is not available")
        self.work = tempfile.mkdtemp(prefix="rufus-e2e-devices-")

    def tearDown(self):
        if not os.environ.get("RUFUS_E2E_KEEP"):
            shutil.rmtree(self.work, ignore_errors=True)

    def _load(self, name: str):
        with open(os.path.join(self.work, name + ".json")) as f:
            return json.load(f)

    def _log(self, name: str) -> str:
        with open(os.path.join(self.work, name + ".log"), errors="replace") as f:
            return f.read()

    def _check(self, uas: bool):
        stick = media.blank_stick(os.path.join(self.work, "stick.img"), 256 * 1024 * 1024)
        disk = media.blank_stick(os.path.join(self.work, "disk.img"), 512 * 1024 * 1024)
        script = SCRIPT.format(cli=os.path.abspath(CLI), out=self.work)
        result = runner.run_in_vm(script, stick=stick, uas=uas, rw_dirs=(self.work,),
                                  extra_qemu_opts=runner.virtio_disk_qemu_opts(disk))
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertEqual(result.returncode, 0, output[-3000:])
        self.assertIn("DONE", output)

        # The stick, with its USB properties
        devices = self._load("default")
        self.assertEqual(len(devices), 1, self._log("default"))
        d = devices[0]
        self.assertTrue(d["usb"])
        self.assertEqual(d["uas"], uas)
        # QEMU's mass storage (BOT) and UAS devices
        self.assertEqual(d["vid_pid"], "46f4:0003" if uas else "46f4:0001")
        self.assertEqual(d["name"], "QEMU QEMU HARDDISK " + ("UAS" if uas else "USB") + " Device")
        self.assertEqual(d["size"], 256 * 1024 * 1024)
        self.assertNotEqual(d["diskseq"], 0)
        self.assertRegex(d["display_name"], r"^NO_LABEL \(sd[a-z]\) \[256 MB\]$")

        # Never the virtio disk, even with all the options
        self.assertEqual([x["device"] for x in self._load("all")], [d["device"]])
        self.assertIn("Found non-USB non-removable device", self._log("all"))

        # Not once the system uses it
        for name in ("var", "swap") + (("dm",) if os.path.exists(os.path.join(self.work, "dm.json")) else ()):
            with self.subTest(name):
                self.assertEqual(self._load(name), [], self._log(name))
                self.assertIn(f"Removing {d['device']} from the list: This is the system disk!", self._log(name))
        self.assertTrue(os.path.exists(os.path.join(self.work, "dm.json")) or shutil.which("dmsetup") is None)

    def test_usb_storage_stick(self):
        self._check(uas=False)

    def test_uas_stick(self):
        self._check(uas=True)


if __name__ == "__main__":
    unittest.main()

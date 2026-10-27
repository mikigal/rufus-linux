# Rufus: The Reliable USB Formatting Utility
# The full stack of the GUI: run as root in the VM of tests/e2e/test_gui_vm.py
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Writes a non-bootable FAT32 drive to the emulated USB stick, with the GUI, the
real rufus-helper (started directly, as Rufus runs as root here) and the real
drive list, under headless KWin. Prints "GUI TEST PASSED" on success.

Usage: python3 vm_write.py <rufus> <rufus-helper> <devnode>
"""

import os
import sys

import gui


def main():
    rufus, helper, devnode = sys.argv[1:4]
    if os.geteuid() != 0:
        sys.exit("vm_write.py must run as root, in the VM of tests/e2e/test_gui_vm.py")
    os.environ["RUFUS_GUI"] = rufus
    os.environ["RUFUS_FAKE_HELPER"] = ""
    try:
        with gui.Compositor("kwin") as compositor:
            # -x: the QEMU stick counts as a USB hard drive
            r = gui.Rufus(compositor, args=["-x"], env={"RUFUS_HELPER": helper})
            try:
                name = os.path.basename(devnode)
                r.wait(lambda: any(f"({name})" in i for i in r.items("IDC_DEVICE")), what="the stick")
                r.select("IDC_DEVICE", next(i for i in r.items("IDC_DEVICE") if f"({name})" in i))
                r.select("IDC_BOOT_SELECTION", "Non bootable")
                r.select("IDC_FILE_SYSTEM", "FAT32")
                r.set_text("IDC_LABEL", "GUI TEST")
                r.click("IDC_START")
                dlg = r.dialog("IDD_NOTIFICATION", timeout=60)
                r.click("IDOK", dlg)
                r.wait(lambda: r.enabled("IDC_START") and r.get("IDC_PROGRESS").get_description() == "READY",
                       timeout=300, what="the end of the write")
                print("STATUS:", r.status())
            finally:
                print(r.log()[-4000:])
                r.close()
    finally:
        gui.close_desktop()
    print("GUI TEST PASSED")


if __name__ == "__main__":
    main()

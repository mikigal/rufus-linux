# Rufus: The Reliable USB Formatting Utility
# The media of "Done when" of stage 2, written by rufus-cli through rufus-helper
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Writes each kind of media of stage 2 with rufus-cli, as root in a VM against
an emulated USB stick (T2), and boots the ones that are meant to boot in QEMU
(T3): non-bootable drives in every file system, DD images (raw, xz, zstd), the
GRUB and Syslinux ISOs, a Windows-like ISO (GPT/UEFI, and MBR/BIOS+UEFI with
UEFI:NTFS), FreeDOS and persistence. Run with:

    RUFUS_CLI=build/src/cli/rufus-cli RUFUS_HELPER=build/src/helper/rufus-helper \\
        python3 -m unittest -v test_media
"""

import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from harness import media, qemu, runner
from test_cli import SCRIPT

CLI = os.environ.get("RUFUS_CLI")
HELPER = os.environ.get("RUFUS_HELPER")

WORK = None
BUILT = {}


def setUpModule():
    global WORK
    WORK = tempfile.mkdtemp(prefix="rufus-e2e-media-")


def tearDownModule():
    if WORK and not os.environ.get("RUFUS_E2E_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)


class StageTwoMedia(unittest.TestCase):

    def setUp(self):
        for name, value in (("RUFUS_CLI", CLI), ("RUFUS_HELPER", HELPER)):
            if value is None or not os.access(value, os.X_OK):
                self.skipTest(f"{name} does not point to an executable")
        if runner.vng_binary() is None:
            self.skipTest("virtme-ng (vng) is not installed")
        if not qemu.kvm_available():
            self.skipTest("KVM is not available")

    def _need(self, *tools: str):
        missing = media.missing_tools(*tools)
        if missing:
            self.skipTest("missing tools: " + ", ".join(missing))

    def _medium(self, name: str, *tools: str) -> str:
        """Build a medium once (see harness/media.py)."""
        self._need(*tools)
        if qemu.host_kernel() is None:
            self.skipTest("no readable host kernel")
        if name not in BUILT:
            BUILT[name] = getattr(media, "build_" + name)(WORK)
        return BUILT[name]

    def _run_script(self, name: str, commands: str, size: int = 256 * 1024 * 1024) -> tuple[str, str]:
        stick = media.blank_stick(os.path.join(WORK, name + ".img"), size)
        script = SCRIPT.format(cli=os.path.abspath(CLI), helper=os.path.abspath(HELPER), commands=commands)
        result = runner.run_in_vm(script, stick=stick, rw_dirs=(WORK,), timeout=900)
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertEqual(result.returncode, 0, output[-6000:])
        return stick, output

    def _write(self, name: str, args: str, checks: str = "", size: int = 256 * 1024 * 1024) -> tuple[str, str]:
        """rufus-cli write with these arguments, then the shell checks, in the VM."""
        cli = os.path.abspath(CLI)
        stick, output = self._run_script(name, f'{cli} -v write "$dev" --hdd --yes {args} || exit 1\n'
                                         f'udevadm settle\n{checks}\necho "MEDIA TEST PASSED"\n', size)
        self.assertIn("MEDIA TEST PASSED", output, output[-6000:])
        return stick, output

    def _boot(self, stick: str, firmware: str, channel: str = "serial", pattern: str = media.DONE_MARKER,
              timeout: float = 120) -> str:
        if firmware == "uefi" and qemu.ovmf() is None:
            self.skipTest("OVMF is not installed")
        vm = qemu.VM(firmware)
        vm.add_usb_stick(stick)
        with vm:
            text = vm.wait_for(channel, pattern, timeout=timeout)
            self.assertIsNotNone(text, f"{channel}:\n{vm.serial()[-2000:] if channel == 'serial' else vm.vga_text()}")
        return text

    # Non-bootable drives, in every file system (and the tools that check them)
    def _file_system(self, fs: str, blkid_type: str, check: str, *tools: str):
        self._need(*tools)
        self._write("fs-" + fs.lower(), f"--fs {fs} --label RUFUS",
                    f'blkid -p -o value -s TYPE "${{dev}}1" | grep -qx {blkid_type} || exit 1\n'
                    f'{check} "${{dev}}1" || exit 1\n')

    def test_fat16(self):
        self._file_system("FAT", "vfat", "fsck.fat -n", "fsck.fat")

    def test_fat32(self):
        self._file_system("FAT32", "vfat", "fsck.fat -n", "fsck.fat")

    def test_ntfs(self):
        self._file_system("NTFS", "ntfs", "ntfsfix -n", "mkntfs", "ntfsfix")

    def test_udf(self):
        self._file_system("UDF", "udf", "udfinfo", "mkudffs", "udfinfo")

    def test_exfat(self):
        self._file_system("exFAT", "exfat", "fsck.exfat -n", "mkfs.exfat", "fsck.exfat")

    def test_ext2(self):
        self._file_system("ext2", "ext2", "e2fsck -fn")

    def test_ext3(self):
        self._file_system("ext3", "ext3", "e2fsck -fn")

    def test_ext4_is_not_offered(self):
        # As on Windows, the rules offer ext2 and ext3 (ext4 is for persistence)
        cli = os.path.abspath(CLI)
        _, output = self._run_script("fs-ext4", f'{cli} write "$dev" --hdd --yes --fs ext4; echo "EXIT: $?"\n')
        self.assertIn("This file system is not available for this boot selection", output)
        self.assertIn("EXIT: 1", output)

    def test_archive(self):
        # An archive to extract on top of what gets written
        archive = os.path.join(WORK, "extra.zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("extra/hello.txt", "Hello from the archive\n")
        self._write("archive", f'--archive "{archive}"',
                    'mtype -i "${dev}1" ::/extra/hello.txt | grep -qx "Hello from the archive" || exit 1\n')

    # DD images
    def _dd(self, name: str, compressor: list[str]):
        iso = self._medium("grub_iso", "grub-mkrescue", "xorriso", "mformat", compressor[0])
        image = os.path.join(WORK, os.path.basename(iso) + "." + name)
        if not os.path.exists(image):
            with open(image, "wb") as f:
                subprocess.run(compressor + [iso], stdout=f, check=True)
        stick, output = self._write("dd-" + name, f'--image "{image}"')
        with open(iso, "rb") as a, open(stick, "rb") as b:
            data = a.read()
            self.assertEqual(b.read(len(data)), data)
        self._boot(stick, "bios")

    def test_dd_xz(self):
        self._dd("xz", ["xz", "-c", "-T0"])

    def test_dd_zstd(self):
        self._dd("zst", ["zstd", "-c", "-q"])

    # Syslinux, from an isolinux ISO (ISO mode, with the embedded Syslinux 4)
    def test_syslinux_iso(self):
        iso = self._medium("syslinux_iso", "xorriso")
        stick, _ = self._write("syslinux-iso", f'--image "{iso}" --iso --partition mbr --target bios',
                               'mtype -i "${dev}1" ::/syslinux.cfg | grep -qi isolinux.cfg || exit 1\n'
                               'mattrib -i "${dev}1" ::/ldlinux.sys | grep -q SHR || exit 1\n')
        text = self._boot(stick, "bios")
        self.assertRegex(text, r"RUFUS-E2E-CMDLINE: .*boot=syslinux")

    # A Windows-like ISO: bootmgr, and a UEFI boot loader (a standalone GRUB)
    def test_windows_like_gpt_uefi(self):
        iso = self._medium("windows_like_iso", "grub-mkstandalone", "xorriso")
        stick, _ = self._write("windows-gpt", f'--image "{iso}" --partition gpt --target uefi',
                               'blkid -p -o value -s PTTYPE "$dev" | grep -qx gpt || exit 1\n'
                               'mdir -i "${dev}1" ::/efi/boot/bootx64.efi ::/bootmgr || exit 1\n')
        text = self._boot(stick, "uefi")
        self.assertRegex(text, r"RUFUS-E2E-CMDLINE: .*boot=windows-like")

    def test_windows_like_mbr_uefi_ntfs(self):
        iso = self._medium("windows_like_iso", "grub-mkstandalone", "xorriso", "mkntfs")
        stick, _ = self._write("windows-ntfs", f'--image "{iso}" --partition mbr --fs NTFS',
                               'blkid -p -o value -s TYPE "${dev}1" | grep -qx ntfs || exit 1\n'
                               'blkid -p -o value -s LABEL "${dev}2" | grep -qx RUFUS_BOOT || exit 1\n')
        # UEFI:NTFS loads the NTFS driver, then the boot loader of the NTFS partition
        text = self._boot(stick, "uefi")
        self.assertRegex(text, r"RUFUS-E2E-CMDLINE: .*boot=windows-like")

    def test_freedos(self):
        # NB: mdir does not list hidden files
        stick, _ = self._write("freedos", "--boot freedos", 'mattrib -i "${dev}1" ::/KERNEL.SYS | grep -q "SH " || exit 1\n')
        self._boot(stick, "bios", "vga", r"[A-Z]:\\>", 90)

    # Persistence, for an Ubuntu-like (casper) ISO
    def test_persistence(self):
        iso = self._medium("casper_iso", "grub-mkrescue", "xorriso", "mformat")
        stick, _ = self._write("persistence", f'--image "{iso}" --iso --partition mbr --target bios --persistence 300',
                               'blkid -p -o value -s LABEL "${dev}2" | grep -qx casper-rw || exit 1\n'
                               'e2fsck -fn "${dev}2" || exit 1\n', size=1024 * 1024 * 1024)
        text = self._boot(stick, "bios")
        self.assertRegex(text, r"RUFUS-E2E-CMDLINE: .*boot=casper persistent")


if __name__ == "__main__":
    unittest.main()

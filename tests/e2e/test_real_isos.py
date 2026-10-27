# Rufus: The Reliable USB Formatting Utility
# T5: real ISOs, written by rufus-cli through rufus-helper, then booted
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Writes real ISOs (Ubuntu, Fedora, Debian, Arch Linux and Windows 11) with
rufus-cli, as root in a VM against an emulated USB stick (T2), then boots them in
QEMU (T3), under BIOS, UEFI and UEFI with Secure Boot, and reads their boot menu,
the shell of the live system or the first screen of Windows Setup with OCR.

The Linux ISOs are kept in $RUFUS_TEST_CACHE (~/.cache/rufus-tests by default),
pinned by SHA-256. They only get downloaded with RUFUS_TEST_DOWNLOAD=1 (about
11 GB); without it, the tests of the ISOs that are not there are skipped.
Windows 11 is $RUFUS_TEST_WINDOWS_ISO, or with RUFUS_TEST_DOWNLOAD=1 the ISO that
rufus-cli downloads as Fido does (English, x64), whose hash is not pinned, as
Microsoft refreshes it. The sticks are sparse files in the cache directory,
since /tmp is often a tmpfs. Run with:

    RUFUS_CLI=build/src/cli/rufus-cli RUFUS_HELPER=build/src/helper/rufus-helper \\
        RUFUS_TEST_DOWNLOAD=1 python3 -m unittest -v test_real_isos
"""

import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from typing import NamedTuple

from harness import qemu, runner, secureboot
from test_cli import SCRIPT

CLI = os.environ.get("RUFUS_CLI")
HELPER = os.environ.get("RUFUS_HELPER")
CACHE = os.environ.get("RUFUS_TEST_CACHE") or os.path.join(
    os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "rufus-tests")
DOWNLOAD = os.environ.get("RUFUS_TEST_DOWNLOAD") == "1"
UEFI_NTFS_IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "res", "uefi", "uefi-ntfs.img")
GIB = 1024 ** 3


class Iso(NamedTuple):
    name: str
    sha256: str
    urls: tuple[str, ...]


# The mirrors move the releases that are not the latest to their archives
UBUNTU = Iso("ubuntu-26.04.1-desktop-amd64.iso",
             "601e30fbf5d97759367c632e2c33630665039b7e2158fd068403da3ccf1bda1f",
             ("https://releases.ubuntu.com/26.04.1/ubuntu-26.04.1-desktop-amd64.iso",
              "https://old-releases.ubuntu.com/releases/26.04.1/ubuntu-26.04.1-desktop-amd64.iso"))
FEDORA = Iso("Fedora-Workstation-Live-44-1.7.x86_64.iso",
             "1620295f6a00c27c3208f0c00b8ece4eab1ec69b9002152d97488bf26a426ddf",
             ("https://dl.fedoraproject.org/pub/fedora/linux/releases/44/Workstation/x86_64/iso/"
              "Fedora-Workstation-Live-44-1.7.x86_64.iso",
              "https://archives.fedoraproject.org/pub/archive/fedora/linux/releases/44/Workstation/x86_64/iso/"
              "Fedora-Workstation-Live-44-1.7.x86_64.iso"))
DEBIAN = Iso("debian-13.7.0-amd64-netinst.iso",
             "a7ef94ac2fb9a7fec454552abd629b7cc9d5155c886165a45649f5ce6167e355",
             ("https://cdimage.debian.org/debian-cd/13.7.0/amd64/iso-cd/debian-13.7.0-amd64-netinst.iso",
              "https://cdimage.debian.org/cdimage/archive/13.7.0/amd64/iso-cd/debian-13.7.0-amd64-netinst.iso"))
ARCH = Iso("archlinux-2026.10.01-x86_64.iso",
           "684ded26c63240ff4a41e8c25ee84ea6da233f557364821f13d12c2b0a9059a5",
           ("https://archive.archlinux.org/iso/2026.10.01/archlinux-2026.10.01-x86_64.iso",))
WINDOWS_ISO = "Win11_English_x64.iso"

# The first screen of Windows Setup, in English or in the language of the ISO (OCR in English, without
# the diacritics)
WINDOWS_SETUP = r"(?i)(windows setup|install now|language|keyboard|instal|j.zyk|ustawie|klawiat)"

WORK = None


def setUpModule():
    global WORK
    os.makedirs(CACHE, exist_ok=True)
    WORK = tempfile.mkdtemp(prefix="work-", dir=CACHE)


def tearDownModule():
    if WORK and not os.environ.get("RUFUS_E2E_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(16 * 1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def fetch(iso: Iso) -> str | None:
    """The path of a pinned ISO in the cache, downloaded (and checked) if allowed. A stamp next to it
    records that its hash was checked, so that it does not get hashed on every run."""
    path = os.path.join(CACHE, iso.name)
    stamp = path + ".sha256"
    if os.path.exists(path):
        if os.path.exists(stamp) and os.path.getmtime(stamp) >= os.path.getmtime(path):
            with open(stamp) as f:
                if f.read().strip() == iso.sha256:
                    return path
        if sha256(path) == iso.sha256:
            with open(stamp, "w") as f:
                f.write(iso.sha256 + "\n")
            return path
        raise AssertionError(f"{path} does not have the SHA-256 of {iso.name}")
    if not DOWNLOAD:
        return None
    part = path + ".part"
    for url in iso.urls:
        if subprocess.run(["curl", "-fL", "--retry", "3", "-C", "-", "-o", part, url]).returncode != 0:
            continue
        if sha256(part) != iso.sha256:
            os.unlink(part)
            continue
        os.rename(part, path)
        with open(stamp, "w") as f:
            f.write(iso.sha256 + "\n")
        return path
    raise AssertionError(f"could not download {iso.name}")


def windows_iso() -> str | None:
    env = os.environ.get("RUFUS_TEST_WINDOWS_ISO")
    if env:
        return env if os.path.exists(env) else None
    path = os.path.join(CACHE, WINDOWS_ISO)
    if os.path.exists(path) or not DOWNLOAD:
        return path if os.path.exists(path) else None
    subprocess.run([os.path.abspath(CLI), "download", "Windows 11", "1", "1", "English (United States)", "x64",
                    "-o", path + ".part"], check=True)
    os.rename(path + ".part", path)
    return path


class RealIsos(unittest.TestCase):

    def setUp(self):
        for name, value in (("RUFUS_CLI", CLI), ("RUFUS_HELPER", HELPER)):
            if value is None or not os.access(value, os.X_OK):
                self.skipTest(f"{name} does not point to an executable")
        if runner.vng_binary() is None:
            self.skipTest("virtme-ng (vng) is not installed")
        if not qemu.kvm_available():
            self.skipTest("KVM is not available")
        missing = [t for t in ("tesseract", "mcopy", "curl") if shutil.which(t) is None]
        if missing:
            self.skipTest("missing tools: " + ", ".join(missing))
        if qemu.ovmf() is None or secureboot.ovmf_secboot() is None:
            self.skipTest("OVMF (with Secure Boot) is not installed")

    def _iso(self, iso: Iso) -> str:
        path = fetch(iso)
        if path is None:
            self.skipTest(f"{iso.name} is not in {CACHE} (RUFUS_TEST_DOWNLOAD=1 downloads it)")
        return path

    def _write(self, name: str, iso: str, args: str, checks: tuple[str, ...] = (),
               size: int = 8 * GIB) -> tuple[str, str]:
        """rufus-cli write of an ISO with these arguments, then the shell commands of the checks (where
        $mnt is the first partition, mounted read-only), in a VM with a network for the downloads of
        Rufus."""
        stick = os.path.join(WORK, name + ".img")
        with open(stick, "wb") as f:
            f.truncate(size)
        cli = os.path.abspath(CLI)
        data = os.path.join(WORK, "data")
        mnt = os.path.join(WORK, "mnt-" + name)
        commands = (f'export XDG_DATA_HOME="{data}" XDG_CACHE_HOME="{data}"\n'
                    f'{cli} -v write "$dev" --hdd --yes --image "{iso}" {args} || exit 1\n'
                    'udevadm settle\n'
                    f'mnt="{mnt}"\nmkdir -p "$mnt"\nmount -o ro "${{dev}}1" "$mnt" || exit 1\n'
                    # The UEFI boot loader on the drive, for the db of Secure Boot
                    f'find "$mnt" -maxdepth 3 -ipath "$mnt/efi/boot/bootx64.efi" -exec cp {{}} "{WORK}/{name}.efi" \\;\n'
                    + "".join(f'{c} || {{ echo "CHECK FAILED: {i}"; exit 1; }}\n' for i, c in enumerate(checks))
                    + 'umount "$mnt"\necho "REAL ISO TEST PASSED"\n')
        script = SCRIPT.format(cli=cli, helper=os.path.abspath(HELPER), commands=commands)
        result = runner.run_in_vm(script, stick=stick, rw_dirs=(WORK,), memory="4G", timeout=3600, network=True)
        output = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        # The log of the last write of each test, for when the tail is not enough
        with open(os.path.join(CACHE, f"write-{name}.log"), "w") as f:
            f.write(output)
        failed = re.search(r"^CHECK FAILED: (\d+)$", output, re.MULTILINE)
        if failed:
            self.fail(f"the check '{checks[int(failed.group(1))]}' failed:\n{output[-4000:]}")
        self.assertEqual(result.returncode, 0, output[-8000:])
        self.assertIn("REAL ISO TEST PASSED", output, output[-8000:])
        return stick, output

    def _secure_boot_db(self, name: str) -> list[bytes]:
        """The certificates of the signature of the UEFI boot loader that the write of this name put on the
        drive, and the Microsoft UEFI CA 2011 that signed UEFI:NTFS"""
        loader = os.path.join(WORK, name + ".efi")
        uefi_ntfs = os.path.join(WORK, "uefi-ntfs-bootx64.efi")
        subprocess.run(["mcopy", "-o", "-i", UEFI_NTFS_IMG, "::/efi/boot/bootx64.efi", uefi_ntfs], check=True)
        return secureboot.pe_certificates(loader) + [
            c for c in secureboot.pe_certificates(uefi_ntfs)
            if b"Microsoft Corporation UEFI CA 2011" in c and b"Third Party Marketplace Root" in c]

    def _boot(self, name: str, stick: str, firmware: str, pattern: str, db: list[bytes] | None = None,
              memory: int = 2048, timeout: float = 240) -> str:
        """Boot the stick until OCR finds the pattern. The screen where it does, or the last one, is
        kept in the cache directory."""
        vm = qemu.VM(firmware, memory=memory, secure_boot_db=db)
        vm.add_usb_stick(stick)
        with vm:
            text = vm.wait_for("ocr", pattern, timeout=timeout, interval=2)
            if text is not None:
                shutil.copyfile(os.path.join(vm.workdir, "screen.png"), os.path.join(CACHE, f"boot-{name}-{firmware}.png"))
            else:
                screen = os.path.join(CACHE, f"failed-{name}-{firmware}.png")
                shutil.copyfile(os.path.join(vm.workdir, "screen.png"), screen)
                self.fail(f"'{pattern}' is not on the screen of {name} ({firmware}), see {screen}:\n{vm.ocr()}")
        return text

    def _same_start(self, stick: str, iso: str):
        with open(iso, "rb") as a, open(stick, "rb") as b:
            while chunk := a.read(64 * 1024 * 1024):
                if b.read(len(chunk)) != chunk:
                    self.fail("the stick does not start with the ISO")

    # Ubuntu: GRUB 2 and shim, with persistence (casper: a "casper-rw" partition, as upstream makes, and
    # "persistent" on the kernel command line)
    def test_ubuntu(self):
        iso = self._iso(UBUNTU)
        stick, _ = self._write("ubuntu", iso, "--iso --persistence 1024",
                               ('blkid -p -o value -s LABEL "${dev}2" | grep -qx casper-rw',
                                'grep -q persistent "$mnt/boot/grub/grub.cfg"'), size=10 * GIB)
        self._boot("ubuntu", stick, "uefi-sb", r"Try or Install Ubuntu", db=self._secure_boot_db("ubuntu"))
        self._boot("ubuntu", stick, "bios", r"Try or Install Ubuntu")

    def test_ubuntu_dd(self):
        iso = self._iso(UBUNTU)
        stick, _ = self._write("ubuntu-dd", iso, "--dd")
        self._same_start(stick, iso)
        self._boot("ubuntu-dd", stick, "uefi-sb", r"Try or Install Ubuntu", db=self._secure_boot_db("ubuntu-dd"))

    # Fedora: GRUB 2 and shim, which find the live system by the label of the ISO, which Rufus changes in
    # the configuration files to the label of the drive
    def test_fedora(self):
        iso = self._iso(FEDORA)
        stick, _ = self._write("fedora", iso, "--iso",
                               ('grep -rl --include=grub.cfg "CDLABEL=$(blkid -p -o value -s LABEL "${dev}1")" "$mnt"',))
        # The menu of GRUB, or the entry that it boots after its short timeout
        self._boot("fedora", stick, "uefi-sb", r"Fedora-Workstation-Live", db=self._secure_boot_db("fedora"))
        self._boot("fedora", stick, "bios", r"Fedora-Workstation-Live")

    # Debian: isolinux (Syslinux 6) for BIOS, and GRUB 2 with shim for UEFI
    def test_debian(self):
        iso = self._iso(DEBIAN)
        stick, _ = self._write("debian", iso, "--iso",
                               ('ls "$mnt" | grep -qix syslinux.cfg',
                                'mattrib -i "${dev}1" ::/ldlinux.sys | grep -q SHR'))
        # The titles of the menus (OCR does not read the highlighted entry of isolinux well)
        self._boot("debian", stick, "bios", r"installer menu \(BIOS mode\)")
        self._boot("debian", stick, "uefi-sb", r"UEFI Installer menu", db=self._secure_boot_db("debian"))

    # Arch Linux: Syslinux for BIOS and systemd-boot for UEFI, which find the live system by its label:
    # it boots to the shell of the live system
    def test_arch(self):
        iso = self._iso(ARCH)
        stick, _ = self._write("arch", iso, "--iso", ('blkid -p -o value -s LABEL "${dev}1" | grep -qx ARCH_202610',))
        self._boot("arch", stick, "bios", r"root@archiso")
        self._boot("arch", stick, "uefi", r"root@archiso")

    # Windows 11, with the default options of Rufus: GPT, NTFS (its install.wim is over 4 GB) with
    # UEFI:NTFS, and the default options of the Windows User Experience, booted with Secure Boot
    def _windows(self) -> str:
        iso = windows_iso()
        if iso is None:
            self.skipTest("no Windows 11 ISO ($RUFUS_TEST_WINDOWS_ISO, or RUFUS_TEST_DOWNLOAD=1)")
        return iso

    def test_windows_ntfs(self):
        iso = self._windows()
        stick, output = self._write("windows-ntfs", iso, "",
                                    ('blkid -p -o value -s TYPE "${dev}1" | grep -qx ntfs',
                                     'blkid -p -o value -s LABEL "${dev}2" | grep -qx RUFUS_BOOT',
                                     'ls "$mnt/sources" | grep -qix install.wim'), size=16 * GIB)
        self.assertIn("Applying Windows customization:", output)
        self._boot("windows-ntfs", stick, "uefi-sb", WINDOWS_SETUP, db=self._secure_boot_db("windows-ntfs"), memory=4096,
                   timeout=420)

    # FAT32, on which install.wim gets split: as upstream, only in the dual BIOS+UEFI mode (Alt-E)
    def test_windows_fat32(self):
        iso = self._windows()
        stick, _ = self._write("windows-fat32", iso, "--dual --fs FAT32",
                               ('blkid -p -o value -s TYPE "${dev}1" | grep -qx vfat',
                                'ls "$mnt/sources" | grep -qix install.swm',
                                '! ls "$mnt/sources" | grep -qix install.wim'), size=16 * GIB)
        self._boot("windows-fat32", stick, "uefi", WINDOWS_SETUP, memory=4096, timeout=420)

    # MBR for BIOS (bootmgr and the NTFS boot records of ms-sys)
    def test_windows_bios(self):
        iso = self._windows()
        stick, _ = self._write("windows-bios", iso, "--partition mbr --target bios",
                               ('blkid -p -o value -s PTTYPE "$dev" | grep -qx dos',), size=16 * GIB)
        self._boot("windows-bios", stick, "bios", WINDOWS_SETUP, memory=4096, timeout=420)


if __name__ == "__main__":
    unittest.main()

# Rufus: The Reliable USB Formatting Utility
# GUI tests (T4): the main window, its dialogs, languages and cheat codes
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Runs the Qt GUI under headless KWin and sway (each one skipped if it is not
installed), with fake drives (RUFUS_TEST_DEVICES) and the fake helper
(RUFUS_FAKE_HELPER), so that nothing needs root or a USB stick. Run with:

    RUFUS_GUI=build/src/ui/qt/rufus RUFUS_FAKE_HELPER=build/tests/rufus-fake-helper \\
        python3 -m unittest -v test_gui

Captures of the windows go to $RUFUS_GUI_ARTIFACTS, if set.
"""

import hashlib
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

import gui

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "e2e"))
from harness import media, qemu  # noqa: E402

GUI = os.environ.get("RUFUS_GUI")
HELPER = os.environ.get("RUFUS_FAKE_HELPER")
ARTIFACTS = os.environ.get("RUFUS_GUI_ARTIFACTS")
SOURCE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOC = os.path.join(SOURCE, "res", "loc", "rufus.loc")
CASPER_ISO = os.path.join(SOURCE, "res", "loc", "test", "casper_test.iso")

# FS_* and BT_* of rufus.h
FS_FAT32, FS_NTFS = 1, 2
BT_NON_BOOTABLE = 0
DOWNLOADS = os.path.join(SOURCE, "tests", "data", "download")


class DownloadServer(http.server.ThreadingHTTPServer):
    """What $RUFUS_TEST_URL_PREFIX sends the downloads to: the files of tests/data/download (the signed
    Fido.ps1 of GitHub), the replies of the Microsoft download site, and an ISO for its links."""

    def __init__(self):
        super().__init__(("127.0.0.1", 0), DownloadHandler)
        self.requests = []
        threading.Thread(target=self.serve_forever, daemon=True).start()

    @property
    def prefix(self) -> str:
        return f"http://127.0.0.1:{self.server_address[1]}"


class DownloadHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, body: bytes, content_type: str = "application/json", status: int = 200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self.server.requests.append(self.path)
        path, _, query = self.path.partition("?")
        params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        if path.endswith("/software-download/windows11") or path.startswith("/vlscppe.") or \
                path == "/ov-df.microsoft.com/":
            self.reply(b"", "text/html")
        elif path == "/ov-df.microsoft.com/mdt.js":
            self.reply(b'var u="?w=0A1B2C3D&s=1";var t={rticks="+1761580000123"};', "application/javascript")
        elif path.endswith("/getskuinformationbyproductedition"):
            edition = params.get("productEditionId", "")
            self.reply(json.dumps({"Skus": [{"Id": edition + "1", "Language": "English",
                                             "LocalizedLanguage": "English (United States)"},
                                            {"Id": edition + "2", "Language": "Polish", "LocalizedLanguage": "Polski"}]
                                   }).encode())
        elif path.endswith("/GetProductDownloadLinksBySku"):
            sku = params.get("SKU", "")
            arch = 2 if sku.startswith("3816") else 1
            self.reply(json.dumps({"ProductDownloadOptions": [
                {"Uri": f"https://software.download.prss.microsoft.com/Win11_{sku}.iso?t=1", "DownloadType": arch}]}
            ).encode())
        elif path.startswith("/software.download.prss.microsoft.com/") and path.endswith(".iso"):
            with open(CASPER_ISO, "rb") as f:
                self.reply(f.read(), "application/octet-stream")
        else:
            file = os.path.join(DOWNLOADS, path.lstrip("/"))
            if ".." in path or not os.path.isfile(file):
                self.reply(b"Not Found", "text/plain", 404)
            else:
                with open(file, "rb") as f:
                    self.reply(f.read(), "application/octet-stream")


def unescape(text: str) -> str:
    return text.replace("\\n", "\n").replace('\\"', '"')


def load_loc() -> dict:
    """The texts of the .loc: {locale: {"IDD_DIALOG/IDC_START": text, "MSG_210": text}}, with the base of each"""
    locales, base, current, group, key = {}, {}, None, None, None
    with open(LOC, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'^l "([^"]+)"', line)
            if m:
                current, group = m.group(1), None
                locales[current] = {}
                continue
            if current is None:
                continue
            m = re.match(r'^b "([^"]+)"', line)
            if m:
                base[current] = m.group(1)
                continue
            m = re.match(r'^g (\w+)', line)
            if m:
                group = m.group(1)
                continue
            m = re.match(r'^t (\w+) "(.*)"\s*$', line)
            if m:
                key = m.group(1) if m.group(1).startswith("MSG_") else f"{group}/{m.group(1)}"
                locales[current][key] = unescape(m.group(2))
                continue
            # The continuation of a string, on the next lines
            m = re.match(r'^\s+"(.*)"\s*$', line)
            if m and key is not None:
                locales[current][key] += unescape(m.group(1))
                continue
            key = None
    for loc, b in base.items():
        locales[loc] = {**locales.get(b, {}), **locales[loc]}
    return locales


LOCALES = load_loc()


def msg(number: int, *args, locale: str = "en-US") -> str:
    """A message, formatted as printf() would (some translations leave the arguments out)"""
    text = LOCALES[locale][f"MSG_{number:03d}"]
    if not args:
        return text
    conversions = re.findall(r"%[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h)?[a-zA-Z%]", text)
    args = list(args[:len([c for c in conversions if c != "%%"])])
    return re.sub(r"%(l|ll|h)?([du])", r"%\2", text) % tuple(args)


def setUpModule():
    if ARTIFACTS:
        os.makedirs(ARTIFACTS, exist_ok=True)


def tearDownModule():
    gui.close_desktop()


class Base:
    """The tests, for one compositor (see the classes at the end)"""

    COMPOSITOR = None

    def setUp(self):
        for name, value in (("RUFUS_GUI", GUI), ("RUFUS_FAKE_HELPER", HELPER)):
            if value is None or not os.access(value, os.X_OK):
                self.skipTest(f"{name} does not point to an executable")
        missing = gui.tools_missing(self.COMPOSITOR)
        if missing:
            self.skipTest("missing: " + ", ".join(missing))

    def run_rufus(self, scale: float = 1.0, **kwargs):
        compositor = gui.Compositor(self.COMPOSITOR, scale)
        self.addCleanup(compositor.close)
        rufus = gui.Rufus(compositor, **kwargs)
        self.addCleanup(rufus.close)
        return rufus

    def stop(self, r: gui.Rufus):
        r.close()
        r.compositor.close()

    def artifact(self, r: gui.Rufus, name: str):
        """A capture of the window, kept in $RUFUS_GUI_ARTIFACTS"""
        path = os.path.join(r.dir, f"{self.COMPOSITOR}-{name}.png")
        result = r.capture(path)
        if ARTIFACTS:
            shutil.copy(path, ARTIFACTS)
        return result

    def assertLayout(self, r: gui.Rufus):
        """The visible controls of the main window fit in it, without overlapping"""
        names = ["IDC_DEVICE", "IDC_SAVE", "IDC_BOOT_SELECTION", "IDC_HASH", "IDC_SELECT", "IDC_PARTITION_TYPE",
                 "IDC_TARGET_SYSTEM", "IDC_ADVANCED_DRIVE_PROPERTIES", "IDC_LIST_USB_HDD", "IDC_OLD_BIOS_FIXES",
                 "IDC_UEFI_MEDIA_VALIDATION", "IDC_LABEL", "IDC_FILE_SYSTEM", "IDC_CLUSTER_SIZE",
                 "IDC_ADVANCED_FORMAT_OPTIONS", "IDC_QUICK_FORMAT", "IDC_EXTENDED_LABEL", "IDC_BAD_BLOCKS",
                 "IDC_NB_PASSES", "IDC_PROGRESS", "IDC_LANG", "IDC_ABOUT", "IDC_LOG", "IDC_START",
                 "IDCANCEL", "IDS_DEVICE_TXT", "IDS_BOOT_SELECTION_TXT", "IDS_LABEL_TXT"]
        frame = r.main.get_extents(r.Atspi.CoordType.WINDOW)
        boxes = {}
        for name in names:
            acc = r.find(name)
            if acc is None or not r.showing(acc):
                continue
            e = r.extents(acc)
            self.assertGreater(e.width, 0, name)
            self.assertGreater(e.height, 0, name)
            self.assertTrue(e.x >= 0 and e.y >= 0 and e.x + e.width <= frame.width and e.y + e.height <= frame.height,
                            f"{name} ({e.x},{e.y} {e.width}x{e.height}) is outside of the window "
                            f"({frame.width}x{frame.height})")
            boxes[name] = e
        items = list(boxes.items())
        for i, (a, ea) in enumerate(items):
            for b, eb in items[i + 1:]:
                overlap = (ea.x < eb.x + eb.width and eb.x < ea.x + ea.width and
                           ea.y < eb.y + eb.height and eb.y < ea.y + ea.height)
                self.assertFalse(overlap, f"{a} overlaps {b}")
        return boxes

    def format(self, r: gui.Rufus, confirm: bool = True):
        """START, and the confirmation of upstream (MSG_003)"""
        r.click("IDC_START")
        dlg = r.dialog("IDD_NOTIFICATION")
        self.assertIn("WILL BE DESTROYED", r.text("IDC_NOTIFICATION_TEXT", dlg))
        r.click("IDOK" if confirm else "IDCANCEL", dlg)
        r.dialog_closed("IDD_NOTIFICATION")

    def wait_helper(self, r: gui.Rufus, event: str, timeout: float = 30) -> dict:
        def find():
            return next((e for e in r.helper_events() if e["event"] == event), None)
        return r.wait(find, timeout=timeout, what=f"the '{event}' event of the fake helper")

    def wait_ready(self, r: gui.Rufus):
        r.wait(lambda: r.enabled("IDC_START") and r.get("IDC_PROGRESS").get_description() == msg(210),
               timeout=30, what="the end of the operation")

    # The tests

    def test_main_window(self):
        for scale in (1.0, 1.25, 1.5):
            with self.subTest(scale=scale):
                r = self.run_rufus(scale, devices=[gui.USB_STICK])
                self.addCleanup(self.stop, r)
                self.assertEqual(r.main.get_name(), "Rufus 4.16.0")
                self.assertEqual(r.name("IDC_DEVICE"), "MY_USB (sdz) [17 GB]")
                self.assertEqual(r.status(), msg(208, 1))
                self.assertEqual(r.get("IDC_PROGRESS").get_description(), msg(210))
                self.assertEqual(r.name("IDC_START"), "START")
                self.assertEqual(r.name("IDCANCEL"), "CLOSE")
                self.assertEqual(r.name("IDC_SELECT"), "SELECT")
                self.assertEqual(r.name("IDC_FILE_SYSTEM"), "FAT32 (Default)")
                self.assertEqual(r.text("IDC_LABEL"), "MY_USB")
                # The advanced options are hidden by default
                self.assertFalse(r.showing(r.get("IDC_LIST_USB_HDD")))
                r.click("IDC_ADVANCED_DRIVE_PROPERTIES")
                r.click("IDC_ADVANCED_FORMAT_OPTIONS")
                r.wait(lambda: r.showing(r.get("IDC_LIST_USB_HDD")) and r.showing(r.get("IDC_BAD_BLOCKS")),
                       what="the advanced options")
                self.assertLayout(r)
                dpr, width, height = self.artifact(r, f"main-{scale}")
                self.assertAlmostEqual(dpr, scale, places=2)
                frame = r.main.get_extents(r.Atspi.CoordType.WINDOW)
                self.assertAlmostEqual(width, frame.width * scale, delta=2 * scale + 1)
                self.stop(r)

    def test_languages(self):
        for locale in ("pl-PL", "de-DE", "fr-FR", "ja-JP", "ru-RU", "ar-SA", "he-IL", "fa-IR"):
            with self.subTest(locale=locale):
                texts = LOCALES[locale]
                rtl = locale in ("ar-SA", "he-IL", "fa-IR")
                r = self.run_rufus(1.25, args=["-l", locale], devices=[gui.USB_STICK])
                # Only one instance can run
                self.addCleanup(self.stop, r)
                self.assertEqual(r.name("IDS_DEVICE_TXT"), texts["IDD_DIALOG/IDS_DEVICE_TXT"])
                self.assertEqual(r.name("IDC_START"), texts["IDD_DIALOG/IDC_START"].upper())
                self.assertEqual(r.status(), msg(208, 1, locale=locale))
                boxes = self.assertLayout(r)
                # Mirrored for the RTL languages
                start, close = boxes["IDC_START"], boxes["IDCANCEL"]
                boot, select = boxes["IDC_BOOT_SELECTION"], boxes["IDC_SELECT"]
                if rtl:
                    self.assertGreater(start.x, close.x)
                    self.assertLess(select.x, boot.x)
                else:
                    self.assertLess(start.x, close.x)
                    self.assertGreater(select.x, boot.x)
                self.artifact(r, f"lang-{locale}")
                self.stop(r)

    def test_language_menu(self):
        r = self.run_rufus(devices=[gui.USB_STICK])
        r.select("IDC_BOOT_SELECTION", "Non bootable")
        r.select("IDC_FILE_SYSTEM", "NTFS")
        # The entries of the menu (which the headless compositors can't pop up without an input event)
        entries = r.command("language").split(" ", 1)[1].split("|")
        self.assertEqual(len(entries), len(LOCALES))
        self.assertIn("English (English) [x]", entries)
        r.command("language Polish")

        # The window gets created again, in Polish, with the same file system
        def polish_window():
            w = r.window("IDD_DIALOG")
            label = (w is not None) and r.find("IDS_DEVICE_TXT", w)
            return w if label and label.get_name() == LOCALES["pl-PL"]["IDD_DIALOG/IDS_DEVICE_TXT"] else None
        r.main = r.wait(polish_window, what="the window in Polish")
        self.assertEqual(r.name("IDC_FILE_SYSTEM"), "NTFS")
        with open(os.path.join(r.dir, "config", "rufus", "rufus.ini")) as f:
            self.assertIn("Locale = pl-PL", f.read())

    def test_write(self):
        r = self.run_rufus(devices=[gui.USB_STICK])
        r.select("IDC_BOOT_SELECTION", "Non bootable")
        r.select("IDC_FILE_SYSTEM", "NTFS")
        r.set_text("IDC_LABEL", "TESTLABEL")
        # The progress on the taskbar (com.canonical.Unity.LauncherEntry)
        monitor = subprocess.Popen(["dbus-monitor", "--address", gui.desktop().env["DBUS_SESSION_BUS_ADDRESS"],
                                    "type='signal',interface='com.canonical.Unity.LauncherEntry'"],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.format(r)
        self.wait_helper(r, "done")
        self.wait_ready(r)
        monitor.terminate()
        signals = monitor.communicate()[0].decode(errors="replace")
        self.assertIn("application://rufus.desktop", signals)
        self.assertIn('string "progress"', signals)
        job = self.wait_helper(r, "format")
        self.assertEqual((job["devnode"], job["diskseq"]), ("/dev/sdz", 42))
        self.assertEqual((job["boot_type"], job["fs_type"], job["label"]), (BT_NON_BOOTABLE, FS_NTFS, "TESTLABEL"))
        self.assertEqual((job["quick_format"], job["zero_drive"], job["image_fd"]), (1, 0, 0))
        self.assertEqual(r.value("IDC_PROGRESS"), 65535)
        self.assertNotEqual(r.name("IDC_STATUS_TIMER"), "")
        # Declining the confirmation writes nothing
        self.format(r, confirm=False)
        r.wait(lambda: r.status() == msg(41), what="the cancellation message")
        self.assertEqual([e["event"] for e in r.helper_events()].count("format"), 1)

    def test_question_and_cancel(self):
        r = self.run_rufus(devices=[gui.USB_STICK], env={"RUFUS_FAKE_HELPER_ASK": "1"})
        r.select("IDC_BOOT_SELECTION", "Non bootable")
        self.format(r)
        # The helper asks a question, which the GUI forwards
        dlg = r.dialog("IDD_NOTIFICATION")
        self.assertEqual(r.text("IDC_NOTIFICATION_TEXT", dlg), "Do you want to go on?")
        r.click("IDYES", dlg)
        self.assertEqual(self.wait_helper(r, "answer")["answer"], 6)
        self.assertEqual(self.wait_helper(r, "done")["result"], "ok")
        self.wait_ready(r)
        self.stop(r)

        # CLOSE cancels an operation, after a confirmation
        r = self.run_rufus(devices=[gui.USB_STICK], env={"RUFUS_FAKE_HELPER_STEPS": "200"})
        r.select("IDC_BOOT_SELECTION", "Non bootable")
        self.format(r)
        self.wait_helper(r, "format")
        r.click("IDCANCEL")
        dlg = r.dialog("IDD_NOTIFICATION")
        self.assertEqual(r.text("IDC_NOTIFICATION_TEXT", dlg), msg(105))
        r.click("IDYES", dlg)
        self.assertEqual(self.wait_helper(r, "done")["result"], "cancelled")
        dlg = r.dialog("IDD_NOTIFICATION")
        self.assertEqual(r.text("IDC_NOTIFICATION_TEXT", dlg), msg(41))
        r.click("IDCLOSE", dlg)
        r.wait(lambda: r.get("IDC_PROGRESS").get_description() == msg(211), what="the cancelled state")
        self.assertTrue(r.enabled("IDC_START"))

    def test_failure(self):
        r = self.run_rufus(devices=[gui.USB_STICK], env={"RUFUS_FAKE_HELPER_FAIL": "1"})
        r.select("IDC_BOOT_SELECTION", "Non bootable")
        self.format(r)
        dlg = r.dialog("IDD_NOTIFICATION", timeout=30)
        self.assertEqual(dlg.get_name(), msg(42))
        r.click("IDCLOSE", dlg)
        r.wait(lambda: r.get("IDC_PROGRESS").get_description() == msg(212), what="the failed state")

    def test_image_and_hash(self):
        r = self.run_rufus(devices=[gui.USB_STICK])
        r.command(f"drop {CASPER_ISO}")
        r.wait(lambda: r.name("IDC_BOOT_SELECTION") == "casper_test.iso", what="the image")
        # A Linux image with persistence
        r.wait(lambda: r.showing(r.get("IDC_PERSISTENCE_SLIDER")), what="the persistence options")
        self.assertEqual(r.name("IDS_IMAGE_OPTION_TXT"), msg(123))
        self.assertEqual(r.text("IDC_LABEL"), "CASPER")
        r.click("IDC_HASH")
        dlg = r.dialog("IDD_HASH", timeout=30)
        with open(CASPER_ISO, "rb") as f:
            data = f.read()
        self.assertEqual(r.text("IDC_MD5", dlg), hashlib.md5(data).hexdigest())
        self.assertEqual(r.text("IDC_SHA256", dlg), hashlib.sha256(data).hexdigest())
        r.click("IDOK", dlg)
        r.dialog_closed("IDD_HASH")

        # A disk image (which needs no download), which goes to the helper
        image = os.path.join(r.dir, "disk.img")
        with open(image, "wb") as f:
            f.write(b"\0" * 510 + b"\x55\xaa" + b"\0" * (1024 * 1024 - 512))
        r.command(f"drop {image}")
        r.wait(lambda: r.name("IDC_BOOT_SELECTION") == "disk.img", what="the disk image")
        self.format(r)
        job = self.wait_helper(r, "format")
        self.assertEqual((job["image_fd"], job["image"]), (1, image))
        self.wait_ready(r)

    def test_save(self):
        # The save button (IDC_SAVE) is with the advanced drive properties: a VHDX, then a VHD (which needs
        # the space of the drive)
        r = self.run_rufus(devices=[dict(gui.USB_STICK, size=256 * 1024**2)])
        r.click("IDC_ADVANCED_DRIVE_PROPERTIES")
        r.wait(lambda: r.showing(r.get("IDC_SAVE")), what="the save button")
        for name, save_type in (("saved.vhdx", 2), ("saved.vhd", 1)):
            image = os.path.join(r.dir, name)
            r.command(f"savefile {image}")
            r.click("IDC_SAVE")
            r.wait(lambda: any(e["event"] == "save" and e["image"] == image for e in r.helper_events()),
                   timeout=30, what="the save request")
            job = next(e for e in r.helper_events() if e["event"] == "save" and e["image"] == image)
            self.assertEqual((job["devnode"], job["diskseq"], job["type"], job["image_fd"]), ("/dev/sdz", 42, save_type, 1))
            # START needs an image, which this test does not select
            r.wait(lambda: r.enabled("IDC_SAVE") and r.get("IDC_PROGRESS").get_description() == msg(210),
                   timeout=30, what="the end of the save")
            with open(image, "rb") as f:
                self.assertEqual(f.read(), b"Fake image")
        # The type of the last image is the one that the next save proposes
        with open(os.path.join(r.dir, "config", "rufus", "rufus.ini")) as f:
            self.assertIn("PreferredSaveImageType = vhd\n", f.read())
        self.stop(r)

        # A VHD needs the space of the drive
        large = dict(gui.USB_STICK, size=64 * 1024**3)
        r = self.run_rufus(devices=[large])
        if shutil.disk_usage(r.dir).free >= large["size"]:
            self.skipTest("too much free space to fail")
        r.click("IDC_ADVANCED_DRIVE_PROPERTIES")
        r.wait(lambda: r.showing(r.get("IDC_SAVE")), what="the save button")
        r.command(f"savefile {os.path.join(r.dir, 'large.vhd')}")
        r.click("IDC_SAVE")
        dlg = r.dialog("IDD_NOTIFICATION")
        self.assertEqual(dlg.get_name(), msg(42))
        r.click("IDCLOSE", dlg)
        r.dialog_closed("IDD_NOTIFICATION")
        self.assertFalse(any(e["event"] == "save" for e in r.helper_events()))

    def test_download(self):
        # Fido: SELECT gets a DOWNLOAD entry once Fido.ps1 is found, which opens the dialog of its steps
        server = DownloadServer()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        r = self.run_rufus(devices=[gui.USB_STICK], env={"RUFUS_TEST_URL_PREFIX": server.prefix})
        r.wait(lambda: r.command("menu IDC_SELECT").startswith("OK "), timeout=30, what="the DOWNLOAD entry")
        self.assertEqual(r.command("menu IDC_SELECT"), "OK SELECT [x]|DOWNLOAD")
        r.command("menu IDC_SELECT DOWNLOAD")
        r.wait(lambda: r.name("IDC_SELECT") == "DOWNLOAD", what="the DOWNLOAD button")
        r.click("IDC_SELECT")
        dlg = r.dialog("IDD_FIDO", timeout=30)
        self.assertEqual(r.name("IDC_FIDO_VERSION", dlg), "Windows 11")
        # The release, the edition, the language (of the user) and the architecture (of this machine)
        for combo, value in (("IDC_FIDO_RELEASE", None), ("IDC_FIDO_EDITION", "Windows 11 Home/Pro/Edu"),
                             ("IDC_FIDO_LANGUAGE", "English (United States)"), ("IDC_FIDO_ARCH", None)):
            r.click("IDC_FIDO_CONTINUE", dlg)
            r.wait(lambda: r.find(combo, dlg) is not None, timeout=30, what=combo)
            if value is not None:
                self.assertEqual(r.name(combo, dlg), value)
        self.assertEqual(r.name("IDC_FIDO_CONTINUE", dlg), msg(40))
        image = os.path.join(r.dir, "Win11.iso")
        r.command(f"savefile {image}")
        r.click("IDC_FIDO_CONTINUE", dlg)
        r.dialog_closed("IDD_FIDO")
        # The ISO gets downloaded, then selected
        r.wait(lambda: r.name("IDC_BOOT_SELECTION") == "Win11.iso", timeout=30, what="the downloaded ISO")
        with open(image, "rb") as a, open(CASPER_ISO, "rb") as b:
            self.assertTrue(a.read() == b.read(), "the downloaded ISO differs")
        self.assertTrue(any(p.startswith("/vlscppe.microsoft.com/tags?org_id=y6jn8c31&") for p in server.requests))
        self.assertIn("/software.download.prss.microsoft.com/Win11_38131.iso?t=1", server.requests)

    def test_selection(self):
        # An ISOHybrid: the boot check asks how to write it (IDD_SELECTION)
        missing = media.missing_tools("grub-mkrescue", "xorriso", "mformat")
        if missing or qemu.host_kernel() is None:
            self.skipTest("missing: " + ", ".join(missing or ["a readable host kernel"]))
        work = tempfile.mkdtemp(prefix="rufus-gui-iso-")
        self.addCleanup(shutil.rmtree, work, True)
        iso = media.build_grub_iso(work)
        r = self.run_rufus(devices=[gui.USB_STICK])
        r.command(f"drop {iso}")
        r.wait(lambda: r.name("IDC_BOOT_SELECTION") == "linux-grub.iso", timeout=30, what="the image")
        r.click("IDC_START")
        dlg = r.dialog("IDD_SELECTION", timeout=30)
        self.assertIn("ISOHybrid", dlg.get_name())
        self.assertTrue(r.checked("IDC_SELECTION_CHOICE1", dlg))
        # DD mode, which needs no download
        r.click("IDC_SELECTION_CHOICE2", dlg)
        r.wait(lambda: r.checked("IDC_SELECTION_CHOICE2", dlg), what="the DD mode")
        r.click("IDOK", dlg)
        r.dialog_closed("IDD_SELECTION")
        dlg = r.dialog("IDD_NOTIFICATION", timeout=30)
        r.click("IDOK", dlg)
        job = self.wait_helper(r, "format")
        # NB: Upstream stores the bit of the choice (2) as the BOOL
        self.assertTrue(job["write_as_image"])
        self.assertEqual(job["image_fd"], 1)
        self.wait_ready(r)

    def test_dialogs(self):
        r = self.run_rufus(devices=[gui.USB_STICK])
        r.click("IDC_ABOUT")
        about = r.dialog("IDD_ABOUTBOX")
        self.assertIn("Rufus", r.text("IDC_ABOUT_BLURB", about))
        r.click("IDC_ABOUT_LICENSE", about)
        lic = r.dialog("IDD_LICENSE")
        self.assertIn("GNU GENERAL PUBLIC LICENSE", r.text("IDC_LICENSE_TEXT", lic))
        r.click("IDCANCEL", lic)
        r.dialog_closed("IDD_LICENSE")
        r.click("IDOK", about)
        r.dialog_closed("IDD_ABOUTBOX")

        # No update check, so no settings (IDD_UPDATE_POLICY)
        self.assertIsNone(r.find("IDC_SETTINGS"))

        # The log, from its button and with Ctrl-L
        r.click("IDC_LOG")
        log = r.dialog("IDD_LOG")
        self.assertIn("Rufus v4.16.0", r.text("IDC_LOG_EDIT", log))
        r.click("IDCANCEL", log)
        r.dialog_closed("IDD_LOG")
        r.key("ctrl+l")
        r.dialog("IDD_LOG")
        r.key("ctrl+l", "IDC_LOG_EDIT")
        r.dialog_closed("IDD_LOG")

    def test_cheat_codes(self):
        r = self.run_rufus(devices=[gui.USB_STICK, gui.USB_HDD, gui.LOOP])
        r.select("IDC_BOOT_SELECTION", "Non bootable")
        # The virtual disk is listed by default, unlike the USB hard drive
        self.assertTrue(any("loop7" in i for i in r.items("IDC_DEVICE")))
        self.assertFalse(any("sdy" in i for i in r.items("IDC_DEVICE")))
        listed = len(r.items("IDC_DEVICE"))

        def toggle(keys: str, number: int, enabled: bool):
            r.key(keys)
            expected = msg(250 if enabled else 251, msg(number))
            r.wait(lambda: r.status() == expected, what=f"'{expected}' after {keys}")

        # The toggles, which say what they did on the status bar
        for keys, number, first in (("alt+period", 270, True), ("alt+comma", 282, False), ("alt+a", 349, False),
                                    ("alt+b", 256, False), ("alt+h", 312, True), ("alt+i", 262, False),
                                    ("alt+j", 257, False), ("alt+k", 258, False), ("alt+l", 254, True),
                                    ("alt+m", 319, True), ("alt+n", 260, True), ("alt+q", 290, True),
                                    ("alt+s", 252, False), ("alt+t", 269, True), ("alt+u", 263, True),
                                    ("alt+w", 265, True), ("ctrl+p", 336, True), ("ctrl+alt+e", 347, True)):
            with self.subTest(keys=keys):
                toggle(keys, number, first)
        # Alt-E: dual UEFI/BIOS
        toggle("alt+e", 266, True)
        # Alt-F lists the USB hard drive, and Alt-G hides the virtual disk
        toggle("alt+f", 253, True)
        r.wait(lambda: len(r.items("IDC_DEVICE")) == listed + 1, what="the USB hard drive")
        toggle("alt+g", 308, False)
        r.wait(lambda: len(r.items("IDC_DEVICE")) == listed, what="the virtual disk to go")
        self.assertFalse(any("loop7" in i for i in r.items("IDC_DEVICE")))
        toggle("alt+f", 253, False)
        # Ctrl-Alt-F: non-USB removable drives (and USB hard drives)
        toggle("ctrl+alt+f", 287, True)
        r.wait(lambda: any("sdy" in i for i in r.items("IDC_DEVICE")), what="the USB hard drive")
        toggle("ctrl+alt+f", 287, False)
        # Alt-+/-: the priority of the format
        r.key("alt+plus")
        r.wait(lambda: r.status() == msg(318, 2), what="the priority")
        r.key("alt+minus")
        r.wait(lambda: r.status() == msg(318, 1), what="the priority")
        # Alt-D: the downloaded files
        r.key("alt+d")
        r.wait(lambda: r.status().startswith(msg(264, "")[:10]), what="the deletion message")
        # Alt-R: the settings
        r.key("alt+r")
        r.wait(lambda: r.status() == msg(248), what="the deletion of the settings")
        self.assertEqual(os.path.getsize(os.path.join(r.dir, "config", "rufus", "rufus.ini")), 0)
        # Alt-O: an optical disc to an ISO, which needs a disc
        r.key("alt+o")
        r.wait(lambda: "No dumpable optical media found." in r.log(), what="the optical media message")
        # The ones that Linux does not need
        for keys in ("alt+p", "alt+v", "alt+x"):
            with self.subTest(keys=keys):
                r.key(keys)
                r.wait(lambda: ("Linux" in r.status()) or ("VDS" in r.status()) or ("not available" in r.status()),
                       what=f"the message of {keys}")
        # Ctrl-Alt-D: dark mode (no relaunch needed)
        r.key("ctrl+alt+d")
        ini = os.path.join(r.dir, "config", "rufus", "rufus.ini")
        r.wait(lambda: "DarkMode = " in gui.read(ini), what="the dark mode setting")
        self.artifact(r, "dark")
        # Alt-C: the port of the selected drive gets cycled by the helper
        r.select("IDC_DEVICE", "MY_USB")
        r.key("alt+c")
        self.assertEqual(self.wait_helper(r, "cycle_port")["devnode"], "/dev/sdz")
        # Alt-Z and Ctrl-Alt-Z: zero the drive
        r.wait(lambda: r.enabled("IDC_START"), what="the end of the port cycle")
        r.key("alt+z")
        dlg = r.dialog("IDD_NOTIFICATION")
        r.click("IDOK", dlg)
        r.wait(lambda: any(e["event"] == "format" and e["zero_drive"] == 1 and e["fast_zeroing"] == 0
                           for e in r.helper_events()), timeout=30, what="the zeroing")
        self.wait_ready(r)
        r.key("ctrl+alt+z")
        dlg = r.dialog("IDD_NOTIFICATION")
        r.click("IDOK", dlg)
        r.wait(lambda: any(e["event"] == "format" and e["fast_zeroing"] == 1 for e in r.helper_events()),
               timeout=30, what="the fast zeroing")
        self.wait_ready(r)
        self.stop(r)

        # A bare Alt cycles the progress text, also in the helper during a format
        r = self.run_rufus(devices=[gui.USB_STICK], env={"RUFUS_FAKE_HELPER_STEPS": "100"})
        r.select("IDC_BOOT_SELECTION", "Non bootable")
        self.format(r)
        self.wait_helper(r, "format")
        r.key("alt")
        r.wait(lambda: any(e["event"] == "progress_type" and e["type"] == 1 for e in r.helper_events()),
               what="the progress type in the helper")
        self.wait_helper(r, "done")


@unittest.skipUnless(shutil.which("kwin_wayland"), "KWin is not installed")
class KWin(Base, unittest.TestCase):
    COMPOSITOR = "kwin"


@unittest.skipUnless(shutil.which("sway"), "sway is not installed")
class Sway(Base, unittest.TestCase):
    COMPOSITOR = "sway"


if __name__ == "__main__":
    unittest.main()

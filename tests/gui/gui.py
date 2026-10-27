# Rufus: The Reliable USB Formatting Utility
# GUI tests (T4): a headless Wayland desktop, and Rufus driven through AT-SPI
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""The desktop of the GUI tests, without root and without a screen:

- a session bus and an accessibility bus of their own (dbus-daemon), with the
  AT-SPI registry, so that nothing depends on systemd or on the desktop of
  the user;
- a headless compositor: KWin (kwin_wayland --virtual) or sway (the wlroots
  headless backend), at a given scale;
- Rufus, with its accessibility always on, its settings in a temporary
  directory and the test hooks of src/ui/qt/test_hooks.h, driven through
  AT-SPI, where the widgets have their IDC_* name as their accessible id.
"""

import json
import os
import select
import shutil
import signal
import subprocess
import tempfile
import time

DESKTOP = None


def read(path: str) -> str:
    with open(path, errors="replace") as f:
        return f.read()


def tools_missing(compositor: str) -> list[str]:
    tools = ["dbus-daemon", "/usr/lib/at-spi2-registryd"]
    tools.append("kwin_wayland" if compositor == "kwin" else "sway")
    missing = [t for t in tools if shutil.which(t) is None and not os.access(t, os.X_OK)]
    try:
        import gi
        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi  # noqa: F401
    except (ImportError, ValueError):
        missing.append("python-gobject with Atspi")
    if not os.path.exists("/usr/share/defaults/at-spi2/accessibility.conf"):
        missing.append("at-spi2-core")
    return missing


class Desktop:
    """The buses, which last for the whole test module (AT-SPI can't change buses)"""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="rufus-gui-")
        self.runtime = os.path.join(self.dir, "runtime")
        os.mkdir(self.runtime, 0o700)
        self.procs = []
        self.env = {
            "XDG_RUNTIME_DIR": self.runtime,
            "XDG_CONFIG_HOME": os.path.join(self.dir, "config"),
            "XDG_DATA_HOME": os.path.join(self.dir, "data"),
            "XDG_STATE_HOME": os.path.join(self.dir, "state"),
            "XDG_CACHE_HOME": os.path.join(self.dir, "cache"),
            "HOME": os.path.join(self.dir, "home"),
        }
        os.mkdir(self.env["HOME"])
        session = self._bus(["--session"])
        a11y = self._bus(["--config-file=/usr/share/defaults/at-spi2/accessibility.conf"])
        self.env["DBUS_SESSION_BUS_ADDRESS"] = session
        self.env["AT_SPI_BUS_ADDRESS"] = a11y
        # Before AT-SPI gets initialized in this process
        os.environ.update(self.env)
        self.procs.append(subprocess.Popen(["/usr/lib/at-spi2-registryd"], env=self.full_env(),
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        import gi
        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi
        self.atspi = Atspi
        Atspi.init()
        # Wait for the registry
        for _ in range(100):
            try:
                Atspi.get_desktop(0).get_child_count()
                break
            except Exception:
                time.sleep(0.05)

    def _bus(self, args: list[str]) -> str:
        r, w = os.pipe()
        proc = subprocess.Popen(["dbus-daemon", "--nofork", "--print-address=%d" % w] + args,
                                pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(w)
        address = b""
        while not address.endswith(b"\n"):
            chunk = os.read(r, 256)
            if not chunk:
                raise RuntimeError("dbus-daemon did not start")
            address += chunk
        os.close(r)
        self.procs.append(proc)
        return address.decode().strip()

    def full_env(self, extra: dict | None = None) -> dict:
        env = {k: v for k, v in os.environ.items() if not k.startswith(("WAYLAND_", "DISPLAY", "QT_", "KDE_"))}
        env.update(self.env)
        env.pop("DISPLAY", None)
        # No MangoHud and such in the tests
        env.pop("LD_PRELOAD", None)
        env["MANGOHUD"] = "0"
        if extra:
            env.update(extra)
        return env

    def close(self):
        for proc in reversed(self.procs):
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        if not os.environ.get("RUFUS_GUI_KEEP"):
            shutil.rmtree(self.dir, ignore_errors=True)


def desktop() -> Desktop:
    global DESKTOP
    if DESKTOP is None:
        DESKTOP = Desktop()
    return DESKTOP


def close_desktop():
    global DESKTOP
    if DESKTOP is not None:
        DESKTOP.close()
        DESKTOP = None


class Compositor:
    """A headless Wayland compositor, at a scale"""

    def __init__(self, kind: str, scale: float = 1.0, width: int = 1280, height: int = 1024):
        self.kind = kind
        self.scale = scale
        d = desktop()
        self.socket = f"rufus-test-{kind}-{os.getpid()}-{int(time.time() * 1000) % 100000}"
        self.log = open(os.path.join(d.dir, f"{self.socket}.log"), "wb")
        # A configuration of its own, as KWin stores the scale of its outputs there
        config_home = os.path.join(d.dir, f"{self.socket}.config")
        os.mkdir(config_home)
        if kind == "kwin":
            # The virtual backend ignores --scale once the output has a configuration
            with open(os.path.join(config_home, "kwinoutputconfig.json"), "w") as f:
                json.dump([
                    {"name": "outputs", "data": [{
                        "connectorName": "Virtual-0", "scale": scale, "transform": "Normal",
                        "mode": {"basic": {"width": width, "height": height, "refreshRate": 60000}, "flags": 1}}]},
                    {"name": "setups", "data": [{"lidClosed": False, "outputs": [{
                        "enabled": True, "outputIndex": 0, "position": {"x": 0, "y": 0}, "priority": 0,
                        "replicationSource": ""}]}]},
                ], f)
            cmd = ["kwin_wayland", "--virtual", "--no-lockscreen", "--socket", self.socket,
                   "--width", str(width), "--height", str(height), "--scale", str(scale)]
            env = d.full_env({"KWIN_COMPOSE": "Q", "XDG_CONFIG_HOME": config_home})
        else:
            config = os.path.join(d.dir, f"{self.socket}.sway")
            with open(config, "w") as f:
                f.write(f"output HEADLESS-1 resolution {width}x{height} scale {scale}\n")
            cmd = ["sway", "--unsupported-gpu", "-c", config]
            env = d.full_env({"WLR_BACKENDS": "headless", "WLR_LIBINPUT_NO_DEVICES": "1",
                              "WLR_RENDERER": "pixman", "WAYLAND_DISPLAY": self.socket,
                              "XDG_CONFIG_HOME": config_home})
        self.proc = subprocess.Popen(cmd, env=env, stdout=self.log, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        path = os.path.join(d.runtime, self.socket)
        for _ in range(200):
            if os.path.exists(path) or self.proc.poll() is not None:
                break
            time.sleep(0.05)
        if not os.path.exists(path):
            self.close()
            raise RuntimeError(f"{kind} did not start (see {self.log.name})")

    def close(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def write_devices(path: str, devices: list[dict]):
    """The drives of RUFUS_TEST_DEVICES"""
    with open(path, "w") as f:
        for d in devices:
            f.write("|".join(str(d.get(k, "")) for k in
                             ("devnode", "diskseq", "size", "name", "label", "fs", "vid_pid", "flags")) + "\n")


USB_STICK = {"devnode": "/dev/sdz", "diskseq": 42, "size": 16 * 1024**3, "name": "SanDisk Cruzer Blade USB Device",
             "label": "MY_USB", "fs": "FAT32", "vid_pid": "0781:5567", "flags": "usb,removable"}
USB_HDD = {"devnode": "/dev/sdy", "diskseq": 43, "size": 2000 * 1024**3, "name": "WD Elements 25A2 USB Device",
           "label": "BACKUP", "fs": "NTFS", "vid_pid": "1058:25a2", "flags": "usb"}
LOOP = {"devnode": "/dev/loop7", "diskseq": 44, "size": 4 * 1024**3, "name": "Loop Device (disk.img)",
        "label": "", "fs": "", "vid_pid": "", "flags": "vhd"}


class Rufus:
    """Rufus in a compositor, driven through AT-SPI and its test hooks"""

    def __init__(self, compositor: Compositor, args: list[str] = (), devices: list[dict] | None = None,
                 env: dict | None = None, settings: str | None = None):
        d = desktop()
        self.Atspi = d.atspi
        self.dir = tempfile.mkdtemp(prefix="rufus-run-", dir=d.dir)
        binary = os.environ.get("RUFUS_GUI", "rufus")
        self.helper_log = os.path.join(self.dir, "helper.jsonl")
        extra = {
            "WAYLAND_DISPLAY": compositor.socket,
            "QT_QPA_PLATFORM": "wayland",
            "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1",
            "QT_NO_XDG_DESKTOP_PORTAL": "1",
            "QT_QPA_PLATFORMTHEME": "",
            "XDG_CONFIG_HOME": os.path.join(self.dir, "config"),
            "XDG_DATA_HOME": os.path.join(self.dir, "data"),
            "XDG_STATE_HOME": os.path.join(self.dir, "state"),
            "RUFUS_TEST_CONTROL": "1",
            "RUFUS_FAKE_HELPER": os.environ.get("RUFUS_FAKE_HELPER", ""),
            "RUFUS_FAKE_HELPER_LOG": self.helper_log,
            # Nothing listens there: no download reaches the network (see download.c)
            "RUFUS_TEST_URL_PREFIX": "http://127.0.0.1:9",
            "LANG": "en_US.UTF-8",
            "LC_ALL": "",
            "LC_MESSAGES": "",
        }
        if settings is not None:
            os.makedirs(os.path.join(self.dir, "config", "rufus"))
            with open(os.path.join(self.dir, "config", "rufus", "rufus.ini"), "w") as f:
                f.write(settings)
        if devices is not None:
            extra["RUFUS_TEST_DEVICES"] = os.path.join(self.dir, "devices")
            write_devices(extra["RUFUS_TEST_DEVICES"], devices)
        if env:
            extra.update(env)
        self.stderr = open(os.path.join(self.dir, "rufus.log"), "wb")
        self.proc = subprocess.Popen([binary, "-v"] + list(args), env=d.full_env(extra), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.stderr)
        self.out = b""
        self.answer("READY", timeout=30)
        self.app = self.wait(self._find_app, timeout=30, what="the application in AT-SPI")
        self.main = self.wait(lambda: self.window("IDD_DIALOG"), timeout=30, what="the main window")

    # The test hooks

    def answer(self, prefix: str, timeout: float = 15) -> str:
        end = time.time() + timeout
        while True:
            while b"\n" in self.out:
                line, self.out = self.out.split(b"\n", 1)
                line = line.decode(errors="replace")
                if line.startswith(prefix) or line.startswith("ERROR"):
                    if line.startswith("ERROR"):
                        raise RuntimeError(f"test hook: {line}")
                    return line
            left = end - time.time()
            if left <= 0 or self.proc.poll() is not None:
                raise TimeoutError(f"no '{prefix}' from Rufus (log: {self.log()[-3000:]})")
            r, _, _ = select.select([self.proc.stdout], [], [], left)
            if r:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    continue
                self.out += chunk

    def command(self, line: str, timeout: float = 15) -> str:
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()
        return self.answer("OK", timeout)

    def key(self, keys: str, name: str = ""):
        self.command(f"key {keys} {name}".strip())

    def capture(self, path: str) -> tuple[float, int, int]:
        _, dpr, size = self.command(f"capture {path}").split()
        w, h = size.split("x")
        return float(dpr), int(w), int(h)

    def log(self) -> str:
        self.stderr.flush()
        with open(self.stderr.name, errors="replace") as f:
            return f.read()

    def helper_events(self) -> list[dict]:
        if not os.path.exists(self.helper_log):
            return []
        with open(self.helper_log) as f:
            return [json.loads(line) for line in f if line.strip()]

    def close(self):
        if self.proc.poll() is None:
            try:
                self.command("quit", timeout=5)
            except (RuntimeError, TimeoutError, BrokenPipeError):
                pass
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.stderr.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # AT-SPI

    def wait(self, fn, timeout: float = 10, what: str = "a condition"):
        end = time.time() + timeout
        while True:
            try:
                r = fn()
            except Exception:
                r = None
            if r:
                return r
            if time.time() > end:
                raise TimeoutError(f"timed out waiting for {what}")
            time.sleep(0.05)

    def _find_app(self):
        desktop_acc = self.Atspi.get_desktop(0)
        for i in range(desktop_acc.get_child_count()):
            app = desktop_acc.get_child_at_index(i)
            if app is not None and app.get_process_id() == self.proc.pid:
                return app
        return None

    def windows(self) -> list:
        return [self.app.get_child_at_index(i) for i in range(self.app.get_child_count())]

    def window(self, name: str):
        """A top level window, by its object name (IDD_*)"""
        for w in self.windows():
            if w is not None and w.get_accessible_id().split(".")[-1] == name and self.showing(w):
                return w
        return None

    def find(self, name: str, root=None):
        """A widget by its object name (IDC_*, IDS_*...), in the main window unless root is given"""
        stack = [root if root is not None else self.main]
        while stack:
            acc = stack.pop()
            if acc is None:
                continue
            if acc.get_accessible_id().split(".")[-1] == name:
                return acc
            stack.extend(acc.get_child_at_index(i) for i in range(acc.get_child_count()))
        return None

    def get(self, name: str, root=None):
        acc = self.wait(lambda: self.find(name, root), what=name)
        return acc

    def state(self, acc, state: str) -> bool:
        return acc.get_state_set().contains(getattr(self.Atspi.StateType, state.upper()))

    def showing(self, acc) -> bool:
        return self.state(acc, "showing")

    def enabled(self, name: str, root=None) -> bool:
        return self.state(self.get(name, root), "enabled")

    def checked(self, name: str, root=None) -> bool:
        return self.state(self.get(name, root), "checked")

    def name(self, name: str, root=None) -> str:
        return self.get(name, root).get_name()

    def text(self, name: str, root=None) -> str:
        acc = self.get(name, root)
        if "Text" not in acc.get_interfaces():
            return acc.get_name()
        return self.Atspi.Text.get_text(acc, 0, self.Atspi.Text.get_character_count(acc))

    def extents(self, name_or_acc, root=None):
        acc = self.get(name_or_acc, root) if isinstance(name_or_acc, str) else name_or_acc
        return acc.get_extents(self.Atspi.CoordType.WINDOW)

    def click(self, name: str, root=None):
        acc = self.get(name, root)
        self.wait(lambda: self.state(acc, "enabled"), what=f"{name} to be enabled")
        action = acc.get_action_iface()
        for i in range(action.get_n_actions()):
            if action.get_action_name(i).lower() in ("press", "click", "toggle"):
                action.do_action(i)
                return
        raise RuntimeError(f"{name} can't be clicked")

    def items(self, name: str, root=None) -> list[str]:
        combo = self.get(name, root)
        lst = combo.get_child_at_index(0)
        return [lst.get_child_at_index(i).get_name() for i in range(lst.get_child_count())]

    def select(self, name: str, item: str, root=None):
        """Select the entry of a dropdown that has this text (or starts with it), with the keyboard"""
        names = self.items(name, root)
        matches = [i for i, n in enumerate(names) if n == item] or [i for i, n in enumerate(names)
                                                                     if n.startswith(item)]
        if not matches:
            raise RuntimeError(f"'{item}' is not in {name}: {names}")
        self.key("home", name)
        for _ in range(matches[0]):
            self.key("down", name)
        self.wait(lambda: self.get(name, root).get_name() == names[matches[0]], what=f"{name} = {item}")

    def set_text(self, name: str, text: str, root=None):
        self.Atspi.EditableText.set_text_contents(self.get(name, root), text)

    def value(self, name: str, root=None) -> float:
        return self.Atspi.Value.get_current_value(self.get(name, root))

    def status(self) -> str:
        return self.name("IDC_STATUS")

    def dialog(self, name: str, timeout: float = 10):
        return self.wait(lambda: self.window(name), timeout=timeout, what=f"the {name} dialog")

    def dialog_closed(self, name: str, timeout: float = 10):
        self.wait(lambda: self.window(name) is None, timeout=timeout, what=f"the {name} dialog to close")

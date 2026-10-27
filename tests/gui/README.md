# GUI tests (T4)

These tests drive the Qt user interface the way a user would, through the
accessibility tree (AT-SPI), under a headless Wayland compositor: no screen,
no root and no USB drive are needed. See section 8.5 of
[LINUX_PORT.md](../../LINUX_PORT.md) for the overall design.

## How it works

* **A desktop of their own** (`gui.py`): a session bus and an accessibility
  bus (`dbus-daemon`, without systemd), the AT-SPI registry, and a headless
  compositor for each test: KWin (`kwin_wayland --virtual`) and sway (the
  wlroots headless backend), at a given scale.
* **AT-SPI**: the widgets have the `IDC_*`/`IDS_*`/`IDD_*` names of their Windows
  counterparts as their accessible id, so the tests find, read and press them
  by name.
* **Test hooks** (`src/ui/qt/test_hooks.h`), for what AT-SPI can't do under
  Wayland: keys (the cheat codes, and the dropdowns, which take the keyboard),
  drag and drop, captures of the windows, and the language menu, which a
  headless compositor does not pop up without an input event. They are only
  active with their environment variables.
* **Fake drives** (`RUFUS_TEST_DEVICES`), which go through the real filters of
  the drive list, and a **fake helper** (`tests/fake_helper.c`,
  `RUFUS_FAKE_HELPER`), which speaks the protocol of `rufus-helper` as the
  user, records what it gets, and replays progress, a question, a failure or
  a cancellation.
* **The full stack** (`vm_write.py`, run by `tests/e2e/test_gui_vm.py`): the GUI
  writes to an emulated USB stick through the real `rufus-helper`, as root in
  the VM of the end-to-end tests, and the drive gets checked after. It is part
  of `ctest -L e2e` (and `-L gui`), with `-DRUFUS_E2E_TESTS=ON`.

## Requirements

On Arch Linux / CachyOS:

```
sudo pacman -S --needed kwin sway at-spi2-core python-gobject
```

A compositor that is not installed is skipped.

## Running

```
cmake -S . -B build -DRUFUS_GUI_TESTS=ON
ctest --test-dir build -L gui --output-on-failure
```

or directly:

```
cd tests/gui && RUFUS_GUI=../../build/src/ui/qt/rufus RUFUS_FAKE_HELPER=../../build/tests/rufus-fake-helper \
    python3 -m unittest -v test_gui
```

| Variable | Effect |
|---|---|
| `RUFUS_GUI_ARTIFACTS=<dir>` | save captures of the windows (scales, languages, dark mode) in `<dir>` |
| `RUFUS_GUI_KEEP=1` | keep the work directory (logs of Rufus and of the compositors) |

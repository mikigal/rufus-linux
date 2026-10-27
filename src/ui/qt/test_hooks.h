/*
 * Rufus: The Reliable USB Formatting Utility
 * Hooks for the GUI tests (T4)
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The tests drive the GUI through AT-SPI, as a user would, except for what
 * AT-SPI can't do under Wayland, which these hooks provide when their
 * environment variable is set:
 * - RUFUS_TEST_CONTROL: commands on the standard input, one per line, each
 *   answered with a line starting with "OK" (or "ERROR") on the standard
 *   output:
 *     key <keys> [name] Press and release keys, e.g. "alt+f", "ctrl+alt+z" or
 *                       "alt" (a bare Alt), in the widget that has the focus
 *                       or the one of this object name, through the event
 *                       filter of the cheat codes
 *     capture <file>    Save a capture of the active window (or the main one),
 *                       and answer with its device pixel ratio and size
 *     drop <file>       Drop a file on the main window
 *     savefile <file>   The file that the next file save dialog chooses, without
 *                       being displayed (TakeTestSaveFile())
 *     menu <name> [text] The entries of the menu of a button (as IDC_SELECT has),
 *                       or trigger the one that starts with text
 *     language [text]   The entries of the language menu (IDC_LANG), or choose
 *                       the one that starts with text: a headless compositor
 *                       does not show a popup menu without an input event
 *     popup             The object name and entries of the open popup menu
 *     choose <text>     Choose the entry of the open popup menu that starts
 *                       with this text (with the keyboard)
 *     quit              Close the main window
 * - RUFUS_TEST_DEVICES: a file of drives that replaces the ones of the
 *   machine, one per line, as "devnode|diskseq|size|name|label|fs|vid:pid|flags",
 *   with "usb", "uas", "removable", "card" and "vhd" as the flags. They go
 *   through the filters of GetDevices() (ShouldListDevice()).
 */

#pragma once

#include "core.h"

class MainWindow;

void InstallTestControl(MainWindow* window);
// Returns -1 if RUFUS_TEST_DEVICES is not set
int GetTestDevices(rufus_device_t* devs, int max_devs, const device_filter_t* filter);
// The file of the "savefile" command, once, or an empty string
QString TakeTestSaveFile();

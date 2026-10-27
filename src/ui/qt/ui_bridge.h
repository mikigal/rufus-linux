/*
 * Rufus: The Reliable USB Formatting Utility
 * The ui_iface_t of the Qt user interface
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

#pragma once

class LogWindow;
class MainWindow;

/*
 * The callbacks of the core run on any thread. They are marshalled to the
 * thread of the user interface: queued for the log, the status, the progress
 * and the events, and blocking queued for the questions, which gives them the
 * semantics that SendMessage() had on Windows. Until there is a main window,
 * the log is collected and the questions get the headless answers.
 */
void InstallUiBridge(LogWindow* log, bool log_to_stderr);
void SetBridgeWindow(MainWindow* window);

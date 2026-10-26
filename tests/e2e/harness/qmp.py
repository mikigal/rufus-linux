# Rufus: The Reliable USB Formatting Utility
# Minimal QEMU Machine Protocol (QMP) client
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""A small synchronous QMP client, so that the harness needs nothing beyond
the Python standard library."""

import json
import socket
import time


class QMPError(Exception):
    pass


class QMP:
    def __init__(self, path: str, timeout: float = 10.0):
        deadline = time.monotonic() + timeout
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise QMPError(f"cannot connect to QMP socket {path}")
                time.sleep(0.05)
        self.sock.settimeout(timeout)
        self.file = self.sock.makefile("rwb")
        self.events = []
        greeting = self._read()
        if "QMP" not in greeting:
            raise QMPError(f"unexpected QMP greeting: {greeting}")
        self.command("qmp_capabilities")

    def _read(self) -> dict:
        line = self.file.readline()
        if not line:
            raise QMPError("QMP connection closed")
        return json.loads(line)

    def command(self, name: str, **arguments):
        msg = {"execute": name}
        if arguments:
            msg["arguments"] = arguments
        self.file.write(json.dumps(msg).encode() + b"\n")
        self.file.flush()
        while True:
            reply = self._read()
            if "event" in reply:
                self.events.append(reply)
            elif "error" in reply:
                raise QMPError(f"{name}: {reply['error'].get('desc', reply['error'])}")
            elif "return" in reply:
                return reply["return"]

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass

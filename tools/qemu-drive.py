#!/usr/bin/env python3
"""Run a built image in QEMU with no window, and drive it.

`west build -t run` opens a cocoa window and offers no way in but a hand, which
makes every check that needs input a manual one -- and a manual check is one
nobody notices they ran against the previous binary. This runs the same image
headless, captures the console, and clicks and types at it through QEMU's QMP
socket, so "launch Notes, drag its grip, open the File menu, choose Save" is a
script. For questions about pixels rather than console output, use shot.py.

    tools/qemu-drive.py -d build-smoke -t 12
    tools/qemu-drive.py -d build 'wait:2' 'click:20,258' 'wait:1' 'click:30,200'
    tools/qemu-drive.py -d build 'key:ctrl-s' 'type:hello world'

Actions, in order, each as one argument:

    wait:<seconds>            let the desktop run
    click:<x>,<y>             press and release at a screen pixel
    press:<x>,<y>             press and hold
    move:<x>,<y>              move while held (or not)
    release                   let go
    drag:<x1>,<y1>,<x2>,<y2>  press, several intermediate moves, release
    key:<name>                one key, QEMU qcode names, e.g. a, ctrl-s, shift-2
    type:<text>               one key per character, ASCII only
    expect:<substring>        fail unless the console has shown this by now

An implicit settle follows every action, because the desktop only reaps and
repaints once per loop iteration and a click delivered into the middle of one
is not the click a finger would have made.

Exit status is 1 if any expect: failed or QEMU died early, so this is usable
from a shell && chain.

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import os
import re
import select
import socket
import subprocess
import sys
import time

SDK = os.environ.get("ZEPHYR_SDK_INSTALL_DIR", os.path.expanduser("~/zephyr-sdk-1.0.1"))
QEMU = os.environ.get("QEMU", f"{SDK}/hosttools/usr/bin/qemu-system-aarch64")

# QEMU absolute pointer axes are always this range, whatever the panel is.
ABS_MAX = 32767

# Enough of the qcode table for a US keyboard. QEMU's names, not Linux's.
QCODE = {
    " ": "spc", "\n": "ret", "\t": "tab",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    "\\": "backslash", ",": "comma", ".": "dot", "/": "slash",
}
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
    "&": "7", "*": "8", "(": "9", ")": "0", "_": "-", "+": "=",
    "{": "[", "}": "]", ":": ";", '"': "'", "~": "`", "|": "\\",
    "<": ",", ">": ".", "?": "/",
}


def qcode(ch):
    """QEMU qcode names for one character, plus whether shift is needed."""
    if ch in SHIFTED:
        return QCODE.get(SHIFTED[ch], SHIFTED[ch]), True
    if ch.isupper():
        return ch.lower(), True
    return QCODE.get(ch, ch), False


class Qmp:
    def __init__(self, port, timeout=20):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 1)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.buf = b""
        self.recv()  # the greeting
        self.cmd("qmp_capabilities")

    def recv(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("qmp closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, execute, **args):
        msg = {"execute": execute}
        if args:
            msg["arguments"] = args
        self.sock.sendall((json.dumps(msg) + "\n").encode())
        while True:
            reply = self.recv()
            # Events (RESET, SHUTDOWN, ...) interleave with replies.
            if "return" in reply or "error" in reply:
                if "error" in reply:
                    raise RuntimeError(reply["error"])
                return reply["return"]

    def send_events(self, events):
        self.cmd("input-send-event", events=events)

    def abs_to(self, x, y, width, height):
        return [
            {"type": "abs", "data": {"axis": "x",
                                     "value": x * ABS_MAX // max(width - 1, 1)}},
            {"type": "abs", "data": {"axis": "y",
                                     "value": y * ABS_MAX // max(height - 1, 1)}},
        ]

    def btn(self, down):
        return [{"type": "btn", "data": {"down": down, "button": "left"}}]

    def key(self, name):
        keys = [{"type": "qcode", "data": part} for part in name.split("-")]
        self.cmd("send-key", keys=keys)


class Console:
    """Reads QEMU's serial output without blocking the driver."""

    def __init__(self, proc):
        self.proc = proc
        self.text = ""

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            left = max(end - time.time(), 0)
            ready, _, _ = select.select([self.proc.stdout], [], [], min(left, 0.2))
            if ready:
                chunk = os.read(self.proc.stdout.fileno(), 65536)
                if not chunk:
                    return
                self.text += chunk.decode("utf-8", "replace")


def run(args):
    elf = os.path.join(args.build_dir, "zephyr", "zephyr.elf")
    if not os.path.isfile(elf):
        sys.exit(f"no image at {elf}")
    if not os.access(QEMU, os.X_OK):
        sys.exit(f"no qemu at {QEMU} (set $QEMU)")

    port = 45000 + os.getpid() % 10000
    cmd = [
        QEMU,
        "-cpu", "cortex-a53",
        "-machine", "virt,secure=on,gic-version=3",
        "-global", "virtio-mmio.force-legacy=false",
        "-device", "virtio-tablet-device,bus=virtio-mmio-bus.3",
        "-device", "virtio-keyboard-device,bus=virtio-mmio-bus.4",
        "-device", "ramfb", "-vga", "none", "-display", "none", "-net", "none",
        "-serial", "stdio",
        "-qmp", f"tcp:127.0.0.1:{port},server,nowait",
        "-kernel", elf,
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    console = Console(proc)
    failures = []

    try:
        qmp = Qmp(port)
        console.pump(args.boot)

        for action in args.actions:
            do_action(qmp, console, action, args, failures)

        console.pump(args.tail)
    finally:
        proc.terminate()
        try:
            proc.wait(3)
        except subprocess.TimeoutExpired:
            proc.kill()
        console.pump(0.3)

    sys.stdout.write(console.text)

    for bad in failures:
        print(f"DRIVE FAIL: {bad}", file=sys.stderr)
    return 1 if failures else 0


def do_action(qmp, console, action, args, failures):
    verb, _, rest = action.partition(":")
    size = (args.width, args.height)

    if verb == "wait":
        console.pump(float(rest))
        return

    if verb == "expect":
        if rest not in console.text:
            failures.append(f"never saw {rest!r}")
        return

    if verb in ("click", "press", "move"):
        x, y = (int(v) for v in rest.split(","))
        events = qmp.abs_to(x, y, *size)
        if verb == "press":
            events += qmp.btn(True)
        elif verb == "click":
            qmp.send_events(events + qmp.btn(True))
            console.pump(args.settle)
            qmp.send_events(qmp.btn(False))
            console.pump(args.settle)
            return
        qmp.send_events(events)

    elif verb == "release":
        qmp.send_events(qmp.btn(False))

    elif verb == "drag":
        x1, y1, x2, y2 = (int(v) for v in rest.split(","))
        qmp.send_events(qmp.abs_to(x1, y1, *size) + qmp.btn(True))
        console.pump(args.settle)
        # Several steps, not one: a drag is a gesture, and the WM only sees
        # the samples it is given.
        for i in range(1, args.steps + 1):
            qmp.send_events(qmp.abs_to(x1 + (x2 - x1) * i // args.steps,
                                       y1 + (y2 - y1) * i // args.steps, *size))
            console.pump(args.settle)
        qmp.send_events(qmp.btn(False))

    elif verb == "key":
        qmp.key(rest)

    elif verb == "type":
        for ch in rest:
            name, shift = qcode(ch)
            qmp.key(f"shift-{name}" if shift else name)
            console.pump(args.settle)
        return

    else:
        failures.append(f"unknown action {action!r}")
        return

    console.pump(args.settle)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("actions", nargs="*", help="wait:/click:/key:/type:/expect: ...")
    ap.add_argument("-d", "--build-dir", default="build")
    ap.add_argument("-b", "--boot", type=float, default=3.0,
                    help="seconds to let the image boot before the first action")
    ap.add_argument("-t", "--tail", type=float, default=1.0,
                    help="seconds to keep reading after the last action")
    ap.add_argument("-s", "--settle", type=float, default=0.25,
                    help="seconds after each event; one desktop loop is ~30 ms")
    ap.add_argument("--steps", type=int, default=6, help="samples per drag:")
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--height", type=int, default=272)
    sys.exit(run(ap.parse_args()))


if __name__ == "__main__":
    main()

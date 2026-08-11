#!/usr/bin/env python3
"""Boot a build under QEMU, drive the pointer, and screenshot the framebuffer.

This is how milestones get verified. `west build -t run` opens a cocoa window a
human has to look at; this drives the same binary headlessly over QMP, injects
virtio-tablet events, and writes PNGs plus the serial console, so a change can be
checked without a human in the loop.

    tools/shot.py -d build -o /tmp/out                       # just boot + shoot
    tools/shot.py -d build -o /tmp/out --click 0.5,0.5       # click centre, shoot
    tools/shot.py -d build -o /tmp/out \
        --shot boot --click 0.1,0.95 --shot menu \
        --drag 0.3,0.2:0.6,0.5 --shot dragged

Actions run in order. Every --shot writes <name>.png and reports how many pixels
changed since the previous shot, which is usually the assertion you actually
want ("did clicking the launcher change anything?").
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

QEMU = os.path.expanduser("~/zephyr-sdk-1.0.1/hosttools/usr/bin/qemu-system-aarch64")
ABS_MAX = 32767  # virtio-tablet reports absolute axes over this range


class Qmp:
    def __init__(self, port):
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.25)
        else:
            raise SystemExit("could not connect to QMP")
        self.f = self.s.makefile("rw", encoding="utf-8", newline="\n")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if "event" in reply:
                continue
            if "error" in reply:
                raise SystemExit(f"QMP {name} failed: {reply['error']}")
            return reply.get("return")

    def move(self, fx, fy):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(fx * ABS_MAX)}},
            {"type": "abs", "data": {"axis": "y", "value": int(fy * ABS_MAX)}},
        ])

    def button(self, down):
        self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": down, "button": "left"}}])


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos:pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        fh.write(chunk(b"IEND", b""))


def point(text):
    fx, fy = text.split(",")
    return float(fx), float(fy)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-d", "--build", default="build")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--boot-delay", type=float, default=6.0)
    ap.add_argument("--port", type=int, default=4455)
    ap.add_argument("--shot", action="append", default=[], dest="actions_shot")
    ap.add_argument("--click", action="append", default=[], dest="actions_click")
    ap.add_argument("--move", action="append", default=[], dest="actions_move")
    ap.add_argument("--drag", action="append", default=[], dest="actions_drag")
    # press/release exist so a shot can be taken mid-gesture: pressed button
    # states and half-finished drags are only observable while held.
    ap.add_argument("--press", action="append", default=[], dest="actions_press")
    ap.add_argument("--release", action="append_const", const="",
                    default=[], dest="actions_release")
    args, _ = ap.parse_known_args()

    # Rebuild the action list in the order the flags actually appeared, which
    # argparse discards.
    known = ("--shot", "--click", "--move", "--drag", "--press", "--release")
    order = [tok[2:] for tok in sys.argv[1:] if tok in known]
    queues = {"shot": list(args.actions_shot), "click": list(args.actions_click),
              "move": list(args.actions_move), "drag": list(args.actions_drag),
              "press": list(args.actions_press), "release": list(args.actions_release)}
    actions = [(kind, queues[kind].pop(0)) for kind in order]
    if not actions:
        actions = [("shot", "frame")]

    os.makedirs(args.out, exist_ok=True)
    proc = subprocess.Popen([
        QEMU, "-cpu", "cortex-a53",
        "-device", "virtio-tablet-device,bus=virtio-mmio-bus.3",
        "-machine", "virt,secure=on,gic-version=3",
        "-global", "virtio-mmio.force-legacy=false",
        "-serial", f"file:{args.out}/console.log",
        "-device", "ramfb", "-vga", "none", "-display", "none", "-net", "none",
        "-qmp", f"tcp:127.0.0.1:{args.port},server,nowait",
        "-kernel", f"{args.build}/zephyr/zephyr.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    rc = 0
    try:
        q = Qmp(args.port)
        time.sleep(args.boot_delay)
        prev = None
        for kind, arg in actions:
            if kind == "move":
                q.move(*point(arg))
                time.sleep(0.3)
            elif kind == "click":
                q.move(*point(arg))
                time.sleep(0.3)
                q.button(True)
                time.sleep(0.25)
                q.button(False)
                time.sleep(0.6)
            elif kind == "press":
                q.move(*point(arg))
                time.sleep(0.3)
                q.button(True)
                time.sleep(0.5)
            elif kind == "release":
                q.button(False)
                time.sleep(0.5)
            elif kind == "drag":
                src, dst = arg.split(":")
                sx, sy = point(src)
                dx, dy = point(dst)
                q.move(sx, sy)
                time.sleep(0.3)
                q.button(True)
                time.sleep(0.3)
                steps = 12
                for i in range(1, steps + 1):
                    q.move(sx + (dx - sx) * i / steps, sy + (dy - sy) * i / steps)
                    time.sleep(0.08)
                q.button(False)
                time.sleep(0.6)
            elif kind == "shot":
                ppm = f"{args.out}/{arg}.ppm"
                q.cmd("screendump", filename=ppm)
                time.sleep(1.2)
                w, h, rgb = read_ppm(ppm)
                write_png(f"{args.out}/{arg}.png", w, h, rgb)
                os.remove(ppm)
                colours = len({rgb[i:i + 3] for i in range(0, len(rgb) - 2, 3)})
                msg = f"{arg}: {w}x{h}, {colours} distinct colours"
                if prev is not None and len(prev) == len(rgb):
                    diff = sum(1 for i in range(0, len(rgb) - 2, 3)
                               if prev[i:i + 3] != rgb[i:i + 3])
                    msg += f", {100.0 * diff / (len(rgb) // 3):.2f}% pixels changed"
                print(msg)
                prev = rgb
    except SystemExit as exc:
        print(exc, file=sys.stderr)
        rc = 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return rc


if __name__ == "__main__":
    sys.exit(main())

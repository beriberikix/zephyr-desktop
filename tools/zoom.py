#!/usr/bin/env python3
"""Crop and magnify a region of a shot.py PNG, optionally stacking several.

Bevels are two one-pixel rings on a 480x272 display; at 1:1 they are impossible
to judge. This blows a region up so the retro chrome can actually be reviewed,
and stacks multiple inputs vertically so before/after states sit side by side.

    tools/zoom.py -o out.png -r 0,240,80,32 -z 6 boot.png pressed.png

INPUT MUST BE A shot.py PNG: 8-bit RGB with filter 0 on every row. The reader
here is about thirty lines and understands nothing else -- not RGBA, not
palettes, not the adaptive row filters every general-purpose encoder emits. A
screenshot from your operating system will be rejected rather than mangled. To
crop one of those, `sips -c H W --cropOffset Y X in.png --out out.png` is
already on any Mac.

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import struct
import sys
import zlib


def read_png(path):
    """Read a PNG written by shot.py: 8-bit RGB, filter 0 on every row."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", data[pos + 8:pos + 18])
            if (depth, ctype) != (8, 2):
                raise SystemExit(f"{path}: expected 8-bit RGB, got depth={depth} type={ctype}")
        elif tag == b"IDAT":
            idat += data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    rows = []
    for y in range(h):
        if raw[y * stride] != 0:
            raise SystemExit(f"{path}: row {y} uses PNG filter {raw[y * stride]}, unsupported")
        rows.append(raw[y * stride + 1:(y + 1) * stride])
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        fh.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("-r", "--rect", required=True, help="x,y,w,h in source pixels")
    ap.add_argument("-z", "--zoom", type=int, default=6)
    ap.add_argument("--gap", type=int, default=4, help="rows of separator between stacked inputs")
    args = ap.parse_args()

    cx, cy, cw, ch = (int(v) for v in args.rect.split(","))
    z = args.zoom
    out_rows = []

    for n, path in enumerate(args.inputs):
        w, h, rows = read_png(path)
        if cx + cw > w or cy + ch > h:
            raise SystemExit(f"{path}: rect {args.rect} outside {w}x{h}")
        if n and args.gap:
            out_rows.extend([b"\xff\x00\xff" * (cw * z)] * args.gap)
        for y in range(ch):
            src = rows[cy + y]
            up = b"".join(src[(cx + x) * 3:(cx + x) * 3 + 3] * z for x in range(cw))
            out_rows.extend([up] * z)

    write_png(args.out, cw * z, len(out_rows), out_rows)
    print(f"{args.out}: {cw * z}x{len(out_rows)} from {len(args.inputs)} input(s)")


if __name__ == "__main__":
    sys.exit(main())

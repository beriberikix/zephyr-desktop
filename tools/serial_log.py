#!/usr/bin/env python3
"""Follow a board's serial console, surviving resets.

USB-CDC consoles (ESP32-S3 USB-Serial/JTAG, and anything else enumerating over
USB) disappear from the host the instant the board resets, which kills a naive
reader mid-session -- exactly when the interesting output starts. This reopens
the port and keeps going.

    tools/serial_log.py /dev/cu.usbmodem1101 -o out.log --seconds 300 --reset
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not found. Try: ~/.local/share/uv/tools/esptool/bin/python " + __file__)


def open_port(path, baud):
    while True:
        try:
            return serial.Serial(path, baud, timeout=0.2)
        except Exception:
            time.sleep(0.3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--reset", action="store_true", help="pulse RTS to reset on connect")
    args = ap.parse_args()

    end = time.time() + args.seconds
    out = open(args.out, "wb", buffering=0)
    port = None

    while time.time() < end:
        if port is None:
            port = open_port(args.port, args.baud)
            out.write(b"\n--- connected ---\n")
            if args.reset:
                port.setDTR(False)
                port.setRTS(True)
                time.sleep(0.2)
                port.setRTS(False)
                args.reset = False  # only on first connect
        try:
            data = port.read(4096)
            if data:
                out.write(data)
        except Exception:
            # Board reset: the CDC device went away. Wait for it to come back.
            out.write(b"\n--- port lost, reconnecting ---\n")
            try:
                port.close()
            except Exception:
                pass
            port = None
            time.sleep(0.5)

    if port is not None:
        port.close()
    out.close()


if __name__ == "__main__":
    main()

#!/bin/sh
#
# Run a built image in QEMU with no window, and optionally type at it.
#
# `west build -t run` opens a cocoa window and offers no way in but a mouse,
# which makes every check that needs input a manual one. This runs the same
# image headless with the QEMU monitor on a socket, so the console can be
# captured and keys can be sent from a script:
#
#   tools/qemu-headless.sh -d build-smoke -t 20
#   tools/qemu-headless.sh -d build -k 'a,shift-b,ctrl-s,left,f1,ret'
#
# Keys are QEMU `sendkey` names, comma-separated. The monitor port is chosen
# per run so two of these do not collide.
#
# The device list must stay in step with what the board and app/CMakeLists.txt
# ask for -- there is no way to ask ninja for the command without also asking it
# to open a window.
#
# SPDX-License-Identifier: Apache-2.0

set -eu

BUILD_DIR=build
SECS=15
KEYS=
DELAY=8
PORT=$((45000 + $$ % 10000))

usage() {
	echo "usage: $0 [-d build_dir] [-t seconds] [-k key,key,...] [-w delay]" >&2
	exit 2
}

while getopts d:t:k:w: opt; do
	case "$opt" in
	d) BUILD_DIR=$OPTARG ;;
	t) SECS=$OPTARG ;;
	k) KEYS=$OPTARG ;;
	w) DELAY=$OPTARG ;;
	*) usage ;;
	esac
done

ELF="$BUILD_DIR/zephyr/zephyr.elf"
[ -f "$ELF" ] || { echo "no image at $ELF" >&2; exit 1; }

QEMU="${QEMU:-$HOME/zephyr-sdk-1.0.1/hosttools/usr/bin/qemu-system-aarch64}"
[ -x "$QEMU" ] || { echo "no qemu at $QEMU (set \$QEMU)" >&2; exit 1; }

"$QEMU" \
	-cpu cortex-a53 \
	-machine virt,secure=on,gic-version=3 \
	-global virtio-mmio.force-legacy=false \
	-device virtio-tablet-device,bus=virtio-mmio-bus.3 \
	-device virtio-keyboard-device,bus=virtio-mmio-bus.4 \
	-device ramfb -vga none -display none -net none \
	-serial stdio \
	-monitor "telnet:127.0.0.1:$PORT,server,nowait" \
	-kernel "$ELF" &
QEMU_PID=$!

trap 'kill "$QEMU_PID" 2>/dev/null || true' EXIT INT TERM

if [ -n "$KEYS" ]; then
	sleep "$DELAY"
	{
		echo "$KEYS" | tr ',' '\n' | while read -r key; do
			[ -n "$key" ] && echo "sendkey $key"
		done
	} | nc 127.0.0.1 "$PORT" >/dev/null 2>&1 || true
fi

sleep "$SECS"

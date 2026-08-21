#!/bin/sh
# Screenshot clippy: the desktop, the launcher, and the zapp's own window.
#
# These are the images docs/clippy.md refers to, so they are produced by a build
# rather than pasted in from someone's laptop and never updated again.
#
#   tools/shot-clippy.sh -d build-ci/clippy -o build-ci/shots
#
# The build must have been configured with -DCONFIG_ZD_CLIPPY=y. Networking is
# not required and is not used: shot.py runs QEMU with `-net none`, so even on a
# ZD_NET build the zapp cannot reach a proxy from here. That is the point of the
# "base shot" -- it is clippy's UI, with the request failing honestly, and it
# does not depend on a Space that takes tens of seconds on CPU.
#
# SPDX-License-Identifier: Apache-2.0
set -u

build=build
out=shots
while [ $# -gt 0 ]; do
	case $1 in
	-d) build=$2; shift 2 ;;
	-o) out=$2; shift 2 ;;
	*) echo "usage: $0 [-d builddir] [-o outdir]" >&2; exit 2 ;;
	esac
done

[ -f "$build/zephyr/zephyr.elf" ] || { echo "no image at $build/zephyr/zephyr.elf" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Where the clippy row is.
#
# shot.py takes fractions of the screen, so this has to be derived rather than
# eyeballed. Every number below is a constant in the source, named here so a
# change to one of them shows up as a wrong click rather than as a mystery:
#
#   ramfb default            480 x 272   (drivers/display/display_qemu_ramfb.c)
#   ZD_TASKBAR_H             28          (app/src/shell/desktop.h)
#   MENU_W / MENU_PAD        140 / 4     (app/src/shell/launcher.c)
#   ITEM_H                   18 + slop   (ditto; slop is 0 on qemu_cortex_a53)
#   launcher button x1       EDGE_PAD 3  (app/src/shell/taskbar.c)
#
# The panel is bottom-anchored above the taskbar and grows upward with the zapp
# count, so the row position depends on how many zapps were discovered. Seven
# with clippy seeded, and clippy is sixth (index 5) because seed.c installs it
# after mines and before badabi, and LittleFS hands readdir() back in creation
# order.
#
# If that assumption is ever wrong the console says so outright -- the launcher
# logs `launch '<name>'` -- which is what the check at the bottom reads.
# ---------------------------------------------------------------------------
ZAPPS=7
ROW=5

geom=$(ZAPPS=$ZAPPS ROW=$ROW python3 -c '
import os
W, H = 480, 272
TASKBAR, MENU_W, PAD, ITEM = 28, 140, 4, 18
n, row = int(os.environ["ZAPPS"]), int(os.environ["ROW"])
top = H - TASKBAR - (2 * PAD + n * ITEM)
print("%.4f,%.4f" % ((3 + PAD + (MENU_W - 2 * PAD) / 2) / W,
                     (top + PAD + row * ITEM + ITEM / 2) / H))
') || exit 1

echo "clippy row at $geom (of $ZAPPS zapps, index $ROW)"

mkdir -p "$out"
python3 tools/shot.py -d "$build" -o "$out" \
	--boot-delay 8 \
	--shot desktop \
	--click 0.04,0.95 --shot launcher \
	--click "$geom" --shot clippy
rc=$?

# shot.py reports pixels; the console is the only thing that can say *which*
# zapp the click actually hit. A wrong row is otherwise a perfectly good
# screenshot of the wrong window.
if grep -q "launch 'clippy'" "$out/console.log" 2>/dev/null; then
	echo "ok: the launcher says it launched clippy"
else
	echo "WARNING: the console does not show clippy being launched."
	echo "         what the launcher actually reported:"
	grep -h "launch '" "$out/console.log" 2>/dev/null || echo "         (nothing)"
	echo "         zapps discovered:"
	grep -h 'discovered .* zapp' "$out/console.log" 2>/dev/null || echo "         (nothing)"
fi

exit $rc

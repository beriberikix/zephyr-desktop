#!/usr/bin/env bash
#
# Assemble what a release actually ships.
#
# A source tag is enough for anyone running QEMU, because they will build it.
# The thing a release can give that a tag cannot is the SD-card tree: six .llext
# files, per architecture, in the layout the desktop expects. That is the part
# nobody can produce without the whole toolchain, and the part that is easiest
# to get wrong -- an aarch64 zapp on a CoreS3 card is refused at relocation,
# which is the correct outcome and an easy half hour to lose.
#
# What this deliberately does NOT produce is a one-click flashable image for the
# CoreS3. The ESP32-S3 needs a bootloader and a partition table alongside the
# application, both assembled by the Espressif HAL at flash time from a
# workspace this bundle does not contain. Shipping zephyr.bin and calling it a
# firmware image would be a lie that fails at 0x0. Use `west flash`; see
# docs/hardware.md.
#
# Usage:
#   tools/mkrelease.sh              # builds pristine, stamps from git describe
#   ZD_REL_VERSION=v0.1.0 tools/mkrelease.sh
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

version=${ZD_REL_VERSION:-$(git describe --abbrev=12 --always --tags 2>/dev/null || echo unknown)}
out=${ZD_REL_OUT:-$root/release/$version}
work=${ZD_REL_WORK:-$root/build-release}

if [ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ]; then
	sdk_version=$(cat zephyr/SDK_VERSION 2>/dev/null)
	export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-${sdk_version:-1.0.1}"
fi

# A release is cut from a clean tree or it is not a release: the boot banner
# takes its build id from git describe, and a dirty tree makes that banner name
# a commit whose contents nobody else can obtain.
if [ -n "$(git status --porcelain)" ]; then
	echo "the working tree is dirty -- commit or stash first, or the version"
	echo "banner in the image will name a commit that does not contain it:"
	git status --short
	exit 1
fi

echo "zephyr-desktop release $version"
echo "  zephyr  $(git -C zephyr rev-parse --short=12 HEAD)"
echo "  sdk     $(cat zephyr/SDK_VERSION)"
echo "  out     $out"
echo

rm -rf "$out"
mkdir -p "$out"

# board:label:nickname
boards="qemu_cortex_a53:qemu_cortex_a53:qemu
m5stack_cores3/esp32s3/procpu:m5stack_cores3:cores3
mimxrt1060_evk/mimxrt1062/qspi:mimxrt1060_evk:rt1060"

while IFS=: read -r board label nick; do
	dir="$work/$nick"
	bundle="$out/zephyr-desktop-$version-$label"

	echo "=== $board ==="
	west build -p always -b "$board" app -d "$dir"

	mkdir -p "$bundle/system/zapps"
	cp "$dir/zephyr/zephyr.elf" "$bundle/"
	cp "$dir/zephyr/zephyr.bin" "$bundle/"
	cp "$dir"/*.llext "$bundle/system/zapps/"

	# The empty directories the desktop expects to find, so that a card
	# unpacked from this bundle boots to a populated desktop rather than to
	# one that has to create them on first write.
	mkdir -p "$bundle/system/share" "$bundle/home/user/zapps" "$bundle/tmp"
	touch "$bundle/system/share/.keep" "$bundle/home/user/zapps/.keep" "$bundle/tmp/.keep"

	cat > "$bundle/README.txt" <<EOF
zephyr-desktop $version -- $label

  system/zapps/*.llext   the zapps, built for THIS board's architecture.
                         They are not interchangeable between boards; the
                         desktop refuses the wrong one at relocation.
  zephyr.elf             the desktop image, unstripped.
  zephyr.bin             the same, raw. NOT a complete flashable image on the
                         CoreS3, which also needs a bootloader and a partition
                         table -- use \`west flash\`.

To put the zapps on a card, copy system/, home/ and tmp/ to the root of a
FAT-formatted card. On the CoreS3 the mount point is /SD:, and the paths above
sit directly beneath it.

Built from $(git rev-parse HEAD)
against Zephyr $(git -C zephyr rev-parse HEAD)
with Zephyr SDK $(cat zephyr/SDK_VERSION).

See docs/hardware.md for the runbooks.
EOF

	(cd "$out" && tar czf "$(basename "$bundle").tar.gz" "$(basename "$bundle")")
	rm -rf "$bundle"
	echo
done <<< "$boards"

(cd "$out" && shasum -a 256 ./*.tar.gz > SHA256SUMS)

echo "=== $out ==="
ls -la "$out"
echo
cat "$out/SHA256SUMS"

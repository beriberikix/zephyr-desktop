#!/usr/bin/env sh
# Reapply patches/zephyr/ on top of the pinned Zephyr.
#
# `west update` resets zephyr/ to the manifest revision, which deletes anything
# applied here. That is the right default -- the pin is load-bearing -- so the
# patches live in this repo and get replayed rather than the pin being moved.
#
# Run after every `west update`. It is idempotent: if the branch is already
# there and current, it does nothing.
#
#   tools/zephyr-patches.sh          apply
#   tools/zephyr-patches.sh --drop   go back to the bare pin
set -eu

topdir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
zephyr="$topdir/zephyr"
patches="$topdir/patches/zephyr"
branch=zd-p4-ppa

# The base is no longer the manifest pin. Upstream PR 117658 carries the
# ESP32-P4 DSI stack, the indexed framebuffer API and the PPA driver -- work
# this project had been carrying its own version of until that PR appeared --
# so we track it and keep only what is genuinely ours on top.
#
# manifest/west.yml still pins Zephyr for `west update`; this fetches over it.
base_remote=https://github.com/zephyrproject-rtos/zephyr
base_ref=pull/117658/head
pin=FETCH_HEAD

if [ "${1:-}" = "--drop" ]; then
	git -C "$zephyr" fetch -q "$base_remote" "$base_ref"
	git -C "$zephyr" checkout -q "$pin"
	git -C "$zephyr" branch -qD "$branch" 2>/dev/null || true
	echo "zephyr back at the bare PR base"
	exit 0
fi

if [ -n "$(git -C "$zephyr" status --porcelain)" ]; then
	echo "zephyr/ has uncommitted changes; refusing to touch it" >&2
	exit 1
fi

git -C "$zephyr" fetch -q "$base_remote" "$base_ref"
git -C "$zephyr" branch -qD "$branch" 2>/dev/null || true
git -C "$zephyr" checkout -q -b "$branch" "$pin"
git -C "$zephyr" am -q "$patches"/*.patch

echo "applied $(ls "$patches"/*.patch | wc -l | tr -d ' ') patches onto $pin"
git -C "$zephyr" log --oneline "$pin"..HEAD

#!/usr/bin/env bash
#
# Every check this project has, on every board it claims to support.
#
# The checks live here rather than in a workflow file so that the machine which
# will have to fix a failure can also reproduce it. .github/workflows/build.yml
# is a thin caller; anything only expressible in YAML is glue, not a check.
#
# Four things it does that `west build` does not, each of which has already cost
# this project a milestone:
#
#   - it reads the build output UNFILTERED and fails on Kconfig's "was assigned
#     the value ... but got the value ''". A knob that does not exist on a target
#     is a knob that silently does nothing, and the build says so every time.
#     CONFIG_LLEXT_HEAP_SIZE does not exist on the Harvard ESP32-S3, and raising
#     it for ARM did nothing there for a whole milestone while Kconfig said so
#     on every build.
#   - it RUNS the image rather than trusting that it linked. A grep for "error"
#     hid a failure in milestone J and QEMU then happily ran the previous ELF and
#     reported a false pass.
#   - it checks every .llext's undefined symbols. Zapps have no libc, and the
#     compiler emits calls nobody wrote: memset from an ordinary struct
#     assignment on every target, __divdi3 from a 64-bit divide on 32-bit
#     Xtensa. Both build clean and fail at load.
#   - it reads the smoke test's REASONS, not its counts. badabi is last in the
#     seed order, so an exhausted llext heap refuses the one entry that is
#     supposed to be refused -- with -ENOMEM, while the summary still says
#     "1 refused" and PASSes.
#
# Usage:
#   tools/ci-check.sh                # everything, pristine builds
#   ZD_CI_PRISTINE=0 tools/ci-check.sh   # incremental, for iterating locally
#   ZD_CI_BOARDS=qemu tools/ci-check.sh  # just the one that can be run
#                                        # (qemu|cores3|rt1060|presto|all)
#
# Exit status is the number of failed checks, capped at 250. Every check runs
# even after one fails: a report that stops at the first broken board tells you
# least when the most is broken.
#
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 250

pristine=${ZD_CI_PRISTINE:-1}
want=${ZD_CI_BOARDS:-all}
out=${ZD_CI_OUT:-$root/build-ci}
mkdir -p "$out" || exit 250

# Only if it is actually there. Zephyr finds the SDK through its own CMake
# package registry and does not need this; a guess at $HOME/zephyr-sdk-<version>
# is a convenience on a developer's machine and a wrong answer on a CI runner,
# where the SDK is unpacked wherever the setup action cached it.
if [ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ]; then
	sdk_version=$(cat zephyr/SDK_VERSION 2>/dev/null)
	guess="$HOME/zephyr-sdk-${sdk_version:-1.0.1}"
	[ -d "$guess" ] && export ZEPHYR_SDK_INSTALL_DIR="$guess"
fi

# ---------------------------------------------------------------------------
# Ask the build where its tools are, rather than guessing.
#
# Every host tool this script needs is already recorded in the build's own
# CMakeCache.txt, by the CMake run that produced the artifacts being checked --
# so nm is the nm that emitted the symbols, and qemu is the one `west build -t
# run` would use. Hardcoding $ZEPHYR_SDK_INSTALL_DIR/<triple>/bin/<triple>-nm
# was wrong twice over: the SDK also ships a gnu/<triple>/bin copy and that is
# the one CMake picks, and on a CI runner the variable is not even set.
# ---------------------------------------------------------------------------
cache_get() {
	local dir=$1 key=$2
	sed -n "s|^${key}:[A-Z]*=||p" "$dir/CMakeCache.txt" 2>/dev/null | head -1
}

failed=0
declare -a report

check_fail() {
	failed=$((failed + 1))
	report+=("FAIL  $1")
	printf '\n!!!! FAIL: %s\n\n' "$1"
}

check_ok() {
	report+=("ok    $1")
	printf '     ok: %s\n' "$1"
}

section() { printf '\n========== %s ==========\n' "$1"; }

# ---------------------------------------------------------------------------
# Build one board, and read what the build said.
#
# The log is kept whole and grepped afterwards rather than piped through a
# filter, so a human reading the CI output sees what the build actually
# printed. That is the rule this project keeps relearning.
# ---------------------------------------------------------------------------
build_board() {
	local board=$1 dir=$2
	shift 2
	local log="$dir.log"
	local args=(-b "$board" app -d "$dir")

	[ "$pristine" = 1 ] && args=(-p always "${args[@]}")

	section "build $board"
	west build "${args[@]}" "$@" 2>&1 | tee "$log"
	local rc=${PIPESTATUS[0]}

	if [ "$rc" -ne 0 ]; then
		check_fail "$board: build exited $rc"
		return 1
	fi
	check_ok "$board: builds"

	# Kconfig assignments that did not take. One check, not two, because
	# Kconfig says the same sentence for both failures this project has hit:
	#
	#   warning: LLEXT_HEAP_SIZE (defined at subsys/llext/Kconfig:91) was
	#   assigned the value '384' but got the value ''. Check these unsatisfied
	#   dependencies: (!HARVARD) (=n).
	#
	# -- a symbol that does not exist on this board [M] -- and the same shape
	# for a value overridden by a select, which is how CONFIG_FS_FATFS_MKFS=n
	# came to protect nothing at all [H].
	#
	# Match on "was assigned the value" and nothing after it. The message is
	# WRAPPED at about 100 columns, so "but got the value" lands on the next
	# line and a grep for the whole phrase silently never matches -- which is
	# how the first version of this check passed on a build that was warning.
	if grep -q "was assigned the value" "$log"; then
		grep -n -A2 "was assigned the value" "$log"
		check_fail "$board: a Kconfig assignment did not take"
	else
		check_ok "$board: every Kconfig assignment took"
	fi

	return 0
}

# ---------------------------------------------------------------------------
# What a zapp is allowed to import: nothing but zd_get_host_api.
#
# Xtensa artifacts are shared objects, so the ordinary symbol table is empty and
# the check is `nm -D -u`. That build also reports a pre-existing undefined
# memset which does not stop the board loading zapps -- it is tolerated by name
# here rather than by disabling the check, so a SECOND undefined symbol on that
# target still fails.
# ---------------------------------------------------------------------------
check_llexts() {
	local dir=$1 dynamic=$2 tolerate=${3:-}
	local flags=(-u)
	local bad=0 count=0
	local name nm
	name=$(basename "$dir")
	nm=$(cache_get "$dir" CMAKE_NM)

	[ "$dynamic" = 1 ] && flags=(-D -u)

	if [ -z "$nm" ] || [ ! -x "$nm" ]; then
		check_fail "$name: the build did not record a usable CMAKE_NM (got '${nm:-nothing}')"
		return
	fi

	for f in "$dir"/*.llext; do
		[ -e "$f" ] || continue
		count=$((count + 1))
		local undef
		undef=$("$nm" "${flags[@]}" "$f" 2>/dev/null |
			awk '{print $NF}' |
			grep -v '^zd_get_host_api$')
		[ -n "$tolerate" ] && undef=$(printf '%s\n' "$undef" | grep -v "^${tolerate}$")
		undef=$(printf '%s\n' "$undef" | grep -v '^$')

		if [ -n "$undef" ]; then
			printf '  %s imports:\n%s\n' "$(basename "$f")" "$undef"
			bad=$((bad + 1))
		fi
	done

	if [ "$count" -eq 0 ]; then
		check_fail "$name: no .llext artifacts were produced at all"
	elif [ "$bad" -gt 0 ]; then
		check_fail "$name: $bad of $count zapp(s) import something that is not there"
	else
		check_ok "$name: all $count zapp(s) import nothing but zd_get_host_api"
	fi
}

run_qemu() {
	local desc=$1 dir=$2
	shift 2
	local qemu
	qemu=$(cache_get "$dir" QEMU)

	# The SDK's own qemu is the right one -- it is what `west build -t run`
	# uses, so a run here is the run a human would get. But an SDK installed
	# without hosttools records QEMU-NOTFOUND, and a distribution
	# qemu-system-aarch64 is a fine second choice: everything this project
	# needs from it (virtio-mmio, ramfb, virtio-tablet, QMP) is upstream and
	# has been for years.
	if [ -z "$qemu" ] || [ ! -x "$qemu" ]; then
		qemu=$(command -v qemu-system-aarch64)
	fi

	section "run $desc"
	if [ -z "$qemu" ] || [ ! -x "$qemu" ]; then
		check_fail "$desc: no usable qemu -- neither the build's QEMU nor one on PATH"
		return
	fi
	printf '  qemu: %s\n' "$qemu"

	if QEMU=$qemu python3 tools/qemu-drive.py -d "$dir" "$@"; then
		check_ok "$desc"
	else
		check_fail "$desc"
	fi
}

# ---------------------------------------------------------------------------

printf 'zephyr-desktop ci-check\n'
printf '  tree     %s\n' "$(git describe --abbrev=12 --always --dirty 2>/dev/null || echo 'not a git tree')"
printf '  sdk      %s\n' "${ZEPHYR_SDK_INSTALL_DIR:-<unset; CMake will find it>}"
printf '  pristine %s\n' "$pristine"
printf '  boards   %s\n' "$want"

if [ "$want" = all ] || [ "$want" = qemu ]; then
	if build_board qemu_cortex_a53 "$out/qemu"; then
		check_llexts "$out/qemu" 0

		# The boot checks. 224 of them at ABI 0.7, and the count is not
		# asserted here on purpose -- adding a check should not break CI,
		# and the summary line only prints if every one of them passed.
		run_qemu "boot selftests" "$out/qemu" \
			'wait:5' \
			'expect:selftest: all checks passed' \
			'expect:selftest (wm): all checks passed'

		# Discovery, through a click, from a filesystem. The one check
		# here that exercises the pointer.
		run_qemu "launcher enumerates the zapps on the filesystem" "$out/qemu" \
			'wait:3' 'click:20,258' 'expect:discovered 6 zapp(s)'
	fi

	# Load every zapp, close every window, prove the counters came back --
	# and that badabi was refused by the ABI gate rather than by a full heap.
	if build_board qemu_cortex_a53 "$out/smoke" -- -DEXTRA_CONF_FILE=smoke.conf; then
		run_qemu "smoke: no leaks, and the ABI gate still bites" "$out/smoke" \
			'wait:30' 'expect:SMOKE: PASS'
	fi
fi

if [ "$want" = all ] || [ "$want" = cores3 ]; then
	# Xtensa's spurious memset is pre-existing and does not stop the board
	# loading zapps. Named, so that anything else still fails.
	if build_board m5stack_cores3/esp32s3/procpu "$out/cores3"; then
		check_llexts "$out/cores3" 1 memset
	fi
fi

if [ "$want" = all ] || [ "$want" = rt1060 ]; then
	if build_board mimxrt1060_evk/mimxrt1062/qspi "$out/rt1060"; then
		check_llexts "$out/rt1060" 0
	fi
fi

if [ "$want" = all ] || [ "$want" = presto ]; then
	# The board support is out of tree (beriberikix/presto-zephyr, pinned in
	# manifest/west.yml); app/CMakeLists.txt points BOARD_ROOT at it when the
	# board name matches. If this fails to configure at all, the usual cause
	# is a workspace that has not been `west update`d since that entry
	# arrived, not anything in the board files.
	#
	# Cortex-M33, so the same arm-zephyr-eabi toolchain and the same REL
	# llext as the RT1060 -- and the same expectation of zero stray imports.
	if build_board presto/rp2350b/m33 "$out/presto"; then
		check_llexts "$out/presto" 0
	fi
fi

section 'summary'
printf '%s\n' "${report[@]}"
printf '\n%d check(s), %d failed\n' "${#report[@]}" "$failed"

[ "$failed" -gt 250 ] && failed=250
exit "$failed"

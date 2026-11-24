#!/usr/bin/env bash
# check.sh: the gate, local and CI (docker): CHECK_FAST=1 builds incrementally and skips the test CIA and
# packaging; CHECK_EXPORT=1 gates HEAD's tracked files alone, in a fresh repository.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
cd "$ROOT"
step() { echo "== $* ($SECONDS s)"; }
hostRoot=$(hostPath "$ROOT") # the bind source of the two docker runs below (lib.sh)

if [ "${CHECK_EXPORT:-0}" = 1 ]; then
	# Under the home folder, which colima shares with its containers, unlike the system's temp.
	copy=$(mktemp -d "$(cacheDir)/export.XXXXXX")
	git archive HEAD | tar -x -C "$copy"
	[ ! -d tools/bin ] || cp -R tools/bin "$copy/tools/" # the packaging tools, never tracked
	git -C "$copy" init -q && git -C "$copy" add -A      # lint lists the files git tracks
	step "export of $(git rev-parse --short HEAD) in $copy"
	CHECK_EXPORT=0 "$copy/tools/check.sh"
	rm -rf "$copy" # kept when the check fails, so it can be looked at
	exit 0
fi

# The lint image extends the pinned devkitARM image every build runs in, so there is one toolchain for both.
image=$(tools/lint.sh image)
[ "${CHECK_FAST:-0}" = 1 ] || rm -rf build build-bench
step "bench build"
THREEDROP_TEST_VERBS=1 tools/build.sh >/dev/null
step "release build"
tools/build.sh >/dev/null
# The bench build's tables, especially its mb rows, stay under the limit checked below because a large static
# starves the drain ring's heap.
bss() { sed -n 's/^\.bss  *0x[0-9a-f]*  *\(0x[0-9a-f]*\).*/\1/p' "$1/3drop.map"; }
bench=$(bss build-bench) release=$(bss build)
{ [ -n "$bench" ] && [ -n "$release" ]; } || die "no .bss line in the link maps"
[ $((bench - release)) -lt 131072 ] ||
	die "the bench build's .bss is $((bench - release)) bytes past release's: a large static starves the drain ring"
# One pass over both ELFs reads each one's __stacksize__ word, then its symbols.
scratch
# A container outlives the shell that started it. A run of this script that dies or is stopped takes its own
# containers down, its legs first, so none starts one after.
gate=3drop-gate-$$
trap 'killChildren; docker rm -f "$gate-lint" "$gate-linux" >/dev/null 2>&1 || :; _scratchEnd' EXIT
# A Ctrl-C stops the script. run.sh, waiting on tests in the background, ends normally on a Ctrl-C, and bash
# would take that for a Ctrl-C handled and carry on.
trap 'exit 130' INT
MSYS2_ARG_CONV_EXCL='*' docker run --rm --volume "$hostRoot:/work:ro" --workdir /work "$image" bash -c '
	bin=/opt/devkitpro/devkitARM/bin/arm-none-eabi
	for flavor in build build-bench; do
		at=$($bin-nm "$flavor/3drop.elf" | awk "\$3 == \"__stacksize__\" { print \$1 }")
		[ -z "$at" ] || $bin-objdump -s --start-address="0x$at" --stop-address="$(printf 0x%x $((0x$at + 4)))" \
			"$flavor/3drop.elf" | awk -v f="$flavor" "END { print \"stack\", f \"/3drop.elf\", \$2 }"
		$bin-nm "$flavor/3drop.elf" | awk -v f="$flavor" "{ print \"symbol\", f, \$NF }"
	done' >"$SCRATCH/elves"
# The frame warning is half the main stack, which libctru sizes from each ELF's __stacksize__ word.
warn=$(sed -n 's/.*-Wframe-larger-than=\([0-9]*\).*/\1/p' Makefile)
elves=0
while read -r elf word; do
	[[ "$word" =~ ^[0-9a-f]{8}$ ]] || die "no __stacksize__ word in $elf"
	stack=$((16#${word:6:2}${word:4:2}${word:2:2}${word:0:2}))
	[ "$stack" = $((2 * warn)) ] || die "$elf's main stack is $stack bytes, not twice the $warn-byte frame warning"
	elves=$((elves + 1))
done < <(awk '$1 == "stack" { print $2, $3 }' "$SCRATCH/elves")
[ "$elves" = 2 ] || die "the stack check read $elves of 2 ELFs"
# The release build carries no bench code, that is, no bench, capture or knob name and no discoverFormat (the
# bench build's reader of discovery's counters). libctru's display capture and the QR scan's camera capture
# (src/scan.c) are release code.
benchNames() {
	awk -v f="$1" '$1 == "symbol" && $2 == f { print $3 }' "$SCRATCH/elves" | grep -Ei 'bench|capture|knob|discoverFormat' |
		grep -vxE 'aptConvertScreenForCapture|GSPGPU_ImportDisplayCaptureInfo|CAMU_StartCapture|CAMU_StopCapture' || :
}
[ -n "$(benchNames build-bench)" ] || die "no bench names in the bench build: the release's check would prove nothing"
leaked=$(benchNames build)
[ -z "$leaked" ] || die "the release build carries bench code: ${leaked//$'\n'/ }"
if [ "${CHECK_FAST:-0}" != 1 ]; then
	# This runs before the tests because testMakeFixtureUid builds the tool too, in this build folder
	# (buildTestingTool).
	step "Testing Tool"
	tools/bench/makeFixture.sh 0 ff3e1 >/dev/null
fi
step "lint and tests"
# Lint runs beside the host leg. The Linux leg runs beside both where its container has CPUs of its own
# (colima's VM on a Mac, Docker Desktop's on Windows), so none waits on another. On a Linux host the container
# shares this machine's cores, and a run of this script on 4 of them held every test in a queue past its
# limit, so the Linux leg goes after the host leg there; nothing is skipped.
{
	st=0
	LINT_CONTAINER=$gate-lint tools/lint.sh >"$SCRATCH/lint.out" 2>&1 || st=$?
	step "lint ended" >>"$SCRATCH/lint.out"
	exit "$st"
} &
lint=$!
# Counted here, not by the runner, so a test it stops finding, or a failure it stops reporting, fails the
# check.
# The Linux leg runs bash 5.2 and GNU tools on the tree read-only, so a test that writes into it fails.
linuxLeg() {
	MSYS2_ARG_CONV_EXCL='*' docker run --rm --name "$gate-linux" --user "$(id -u):$(id -g)" --env HOME=/tmp --env THREEDROP_KEY=not-the-tests-key \
		--volume "$hostRoot:/work:ro" "$image" /work/tools/tests/run.sh --linux >"$SCRATCH/linux.out" 2>&1 || :
	step "Linux leg ended" >"$SCRATCH/linux.end"
}
# Run under a key that is not the tests' own, because a run of this script from a shell that sourced .env must
# still pass.
hostLeg() {
	THREEDROP_KEY=not-the-tests-key tools/tests/run.sh | tee "$SCRATCH/tests.out" || :
	step "host leg ended"
}
linux=""
if [ "$(uname -s)" = Linux ]; then
	hostLeg
	linuxLeg
else
	linuxLeg &
	linux=$!
	hostLeg
fi
st=0
wait "$lint" || st=$?
cat "$SCRATCH/lint.out"
[ "$st" = 0 ] || die "lint failed"
[ -z "$linux" ] || wait "$linux" || :
cat "$SCRATCH/linux.out" "$SCRATCH/linux.end"
grep -qx 'tests: 39 passed, 0 failed' "$SCRATCH/tests.out" || die "expected 'tests: 39 passed, 0 failed'"
grep -qx 'tests: 16 passed, 0 failed' "$SCRATCH/linux.out" || die "expected 'tests: 16 passed, 0 failed' on Linux"
# A check that starts skipping passes unseen, so a leg may skip only what needs the network, or a tool its
# host lacks (the Linux image has no jq or nc; a Mac may have no jq, and its dd counts no bytes).
onlySkips() { # <leg's output> <the leg's host, as uname says it>
	local extra allowed='testSeeds: example\.com unreachable|testBridge: no address but 127\.0\.0\.1|testTools: no jq'
	case $2 in
	Darwin) allowed+='|testWire: no dd here that counts bytes' ;;
	*) allowed+='|testAnswers: no nc' ;;
	esac
	extra=$(grep '^SKIPPED: ' "$SCRATCH/$1" | grep -Ev "^SKIPPED: ($allowed)") || :
	[ -z "$extra" ] || die "$1 skipped a check it should make: ${extra//$'\n'/; }"
}
onlySkips tests.out "$(uname)"
onlySkips linux.out Linux
if [ "${CHECK_FAST:-0}" != 1 ]; then
	step "package"
	cmp -s 3drop.elf build/3drop.elf || die "3drop.elf is not the release build's (build/3drop.elf): the CIA would ship another flavor"
	tools/makeCIA.sh >/dev/null
	tools/tests/testCIA.sh 3drop.cia
fi
step "check: PASS"

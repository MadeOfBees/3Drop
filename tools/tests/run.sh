#!/usr/bin/env bash
# run.sh [--linux | test...]: every test, the shell tests with no `# host-only` line, or those named, in
# parallel (TEST_SERIAL=1: one at a time, since Git Bash starts processes too slowly for the tests' short
# bounds with every test at once), each killed after TEST_TIMEOUT; check.sh compares the summary line with
# the count it expects.
set -uo pipefail
. "$(dirname "$0")/lib.sh"
# Controls that must fail, run exactly as the tests are. A runner that reported one of them as
# passing would report a failing test as passing too. Two say PASS and then exit 1, like a test whose
# cleanup or sanitizer fails after it passes. One uses an unset variable under an EXIT trap, as every
# test has (lib.sh), which makes bash 3.2 exit 0. One exits 0 without ever saying PASS.
mkdir "$TMP/controls"
printf '. "%s/lib.sh"\necho "controlSh: PASS"\nexit 1\n' "$TESTS" >"$TMP/controls/controlSh.sh"
# shellcheck disable=SC2016 # expanded when the control runs
printf 'set -euo pipefail\n. "%s/lib.sh"\necho "$controlUnset"\n' "$TESTS" >"$TMP/controls/controlUnset.sh"
printf '#include <stdio.h>\nint main(void) {\n\tputs("controlC: PASS");\n\treturn 1;\n}\n' >"$TMP/controls/controlC.c"
printf 'int main(void) {\n\treturn 0;\n}\n' >"$TMP/controls/controlCQuiet.c"
# A skip that a runner doesn't list passes for a check that ran, so this one must be listed.
printf '. "%s/lib.sh"\nskip "a check it cannot make"\n' "$TESTS" >"$TMP/controls/controlSkip.sh"
tests=("$TESTS"/test*.sh "$TESTS"/test*.c)
if [ "${1:-}" = --linux ]; then
	tests=()
	for t in "$TESTS"/test*.sh; do grep -q '^# host-only' "$t" || tests+=("$t"); done
elif [ "$#" != 0 ]; then
	tests=()
	for n in "$@"; do if [ -e "$TESTS/$n.sh" ]; then tests+=("$TESTS/$n.sh"); else tests+=("$TESTS/$n.c"); fi; done
fi
for t in "${tests[@]}" "$TMP"/controls/*; do
	n=${t%.*}
	n=${n##*/}
	{
		began=$SECONDS
		# A test passes when it says PASS and exits 0. An exit status of 0 alone isn't enough, because it
		# can come from bash 3.2 after an unset variable, or from an early return.
		{ if [[ $t == *.sh ]]; then withTimeout "$TEST_TIMEOUT" bash "$t"; else ccRun "$t"; fi; } >"$TMP/$n.log" 2>&1 &&
			grep -q "^$n: PASS" "$TMP/$n.log" && touch "$TMP/$n.ok"
		echo $((SECONDS - began)) >"$TMP/$n.s"
	} &
	[ -z "${TEST_SERIAL:-}" ] || wait
done
wait
skipped() { sed -n "s/^$1: SKIP /SKIPPED: $1: /p" "$TMP/$1.log"; }
passed=0 failed=0
for n in controlSh controlUnset controlC controlCQuiet; do
	if [ ! -e "$TMP/$n.log" ] || [ -e "$TMP/$n.ok" ]; then
		failed=$((failed + 1))
		echo "--- $n did not fail: this runner would pass a failing test"
	fi
done
if [ "$(skipped controlSkip)" != "SKIPPED: controlSkip: a check it cannot make" ]; then
	failed=$((failed + 1))
	echo "--- controlSkip was not listed: this runner would hide a skip"
fi
for log in "$TMP"/test*.log; do
	n=${log##*/}
	n=${n%.log}
	if [ -e "$TMP/$n.ok" ]; then passed=$((passed + 1)); else
		failed=$((failed + 1))
		echo "--- $n FAILED"
		cat "$log"
	fi
	skipped "$n"
done
echo "tests: $passed passed, $failed failed"
# Slowest first. Every test starts at once, so the slowest one sets how long the leg takes.
for s in "$TMP"/test*.s; do
	n=${s##*/}
	echo "$(<"$s") ${n%.s}"
done | sort -rn | awk '{ print "time:", $2, $1, "s" }'
[ "$failed" = 0 ]

#!/usr/bin/env bash
# shellcheck source=/dev/null # fakeDevice.sh and wire.sh are linted alone, not again in each test
# The fake's FIRM check against the device's, src/sdXfer.c's firmCheck compiled here: every plant of
# one corpus gets the same verdict from both at every gate, each verdict pinned.
# host-only: ccTest links mbedtls, which the Linux image lacks.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
# shellcheck disable=SC2046 # a word list
ccTest "$TMP/firmProbe" tools/tests/firmProbe.c $(ccArgs "$TESTS/firmProbe.c")
mkdir -p "$TMP/probe/sdmc:/firm"
while read -r want plant; do
	fakeFirmBuild "$TMP/probe/sdmc:/firm/$plant" "$plant"
	echo "/firm/$plant" >>"$TMP/paths"
	echo "$want $plant" >>"$TMP/pinned"
done <<'EOF'
ok valid
ok other
ok arm11zero
system magic
system bounds
system byte
system arm9
system arm11
system arm9zero
system offset
system length
system self
system cut
system header
system arm9end
ok emptyodd
system none
system unaligned
system wrap
system region
system regionlow
system regionpast
ok regionend
system short
system verifier
ok luma
ok gm9
ok safeb9s
ok ntrboot
ok hwtest
ok bmbt
EOF
(cd "$TMP/probe" && "$TMP/firmProbe") <"$TMP/paths" >"$TMP/verdicts" || fail "the probe stopped: $(cat "$TMP/verdicts")"
paste -d' ' "$TMP/verdicts" "$TMP/paths" | sed 's| /firm/| |' >"$TMP/byDevice"
while read -r path; do
	if fakeFirm "$TMP/probe/sdmc:$path"; then echo "ok ${path#/firm/}"; else echo "system ${path#/firm/}"; fi
done <"$TMP/paths" >"$TMP/byFake"
cmp -s "$TMP/byDevice" "$TMP/pinned" || fail "the device's verdicts are not the pinned ones: $(diff "$TMP/pinned" "$TMP/byDevice" | tr '\n' ' ')"
cmp -s "$TMP/byFake" "$TMP/byDevice" || fail "the fake's FIRM check is not the device's: $(diff "$TMP/byDevice" "$TMP/byFake" | tr '\n' ' ')"
echo "testFirmRules: PASS ($(wc -l <"$TMP/paths" | tr -d ' ') plants)"

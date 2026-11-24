#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# drop.sh's ghost retry against the fake device.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
cia="$TMP/big.cia"
fakeCia "$cia" 0004000001234500 1 17825792
size=$(fileSize "$cia")
ghost=(THREEDROP_GHOST_FLOOR=1000000000000 THREEDROP_GHOST_GRACE=0)
# The real floor: above the rate of a stalled segment and below the rate of a loaded one.
ghost2=(THREEDROP_GHOST_FLOOR=1500000 THREEDROP_GHOST_GRACE=0)

# drop <want-exit> [env...]: the install must exit as wanted. The forced-ghost checks pin the exact
# nonce/receipt order, and a wall-clock step can age a kept nonce into another stats request. Retry that
# sequence only after such a step, and start each try with no kept nonce, pause marker or put id file left
# from the preceding run. Assertions follow the loop so a real defect still fails.
drop() {
	local want=$1 st
	shift
	if [[ " $* " == *" ${ghost[0]} "* ]]; then
		until steadyRun; do dropOnce "$@"; done
	else
		dropOnce "$@"
	fi
	[ "$st" = "$want" ] || fail "drop.sh $* exited $st, want $want: $(tail -1 "$TMP/err")"
}
dropOnce() {
	rm -f "$(_wireNonceFile 127.0.0.1 "$fakePort")" "$FAKE/paused" "$TMP/final"
	: >"$FAKE/log"
	: >"$FAKE/bytes"
	: >"$FAKE/installed"
	st=0
	# The knocks that ask for the ghost's receipt wait 3 s for the fake's answer. The fake takes 0.6 s to
	# answer on a quiet Git Bash, and over 3 s once a process start costs 0.3 s more. Then all the knocks come
	# back busy, which takes 60 s and ends with exit 4. The control timeout gets its own 15 s.
	env THREEDROP_TIMEOUT=15 "$@" "$ROOT/tools/drop.sh" --new "$cia" 127.0.0.1 >"$TMP/out" 2>"$TMP/err" || st=$?
}
distinct() { grep '^put ' "$FAKE/log" | cut -d' ' -f"$1" | sort -u | grep -c .; }

# A segment's rate and the whole send's grace/max window share one monotonic clock. Move
# SECONDS independently at the writer seam: backward steps cannot hide a ghost, forward steps
# cannot invent one, and the first sample and later segment origins must use the same clock.
(
	unset SECONDS # an ordinary test variable now, not bash's wall clock
	# shellcheck disable=SC2317,SC2034 # _wireEmit calls this and reads wireMono
	_wireMono() { wireMono=${mono[index]}; }
	# shellcheck disable=SC2317 # this probe needs only the body's logical size
	fileSize() { echo "$emitSize"; }
	# shellcheck disable=SC2317 # completing a segment advances the two independent clocks
	_wireWriter() {
		index=$((index + 1))
		SECONDS=${wall[index]}
	}
	clockedEmit() {
		local want=$1 label=$2 index=0 st=0 emitSize=${7:-17825792}
		local -a wall mono
		read -r -a wall <<<"$3"
		read -r -a mono <<<"$4"
		SECONDS=${wall[0]}
		THREEDROP_GHOST_FLOOR=1500000 \
			THREEDROP_GHOST_GRACE=${5:-0} \
			THREEDROP_GHOST_MAX_ELAPSED=${6:-150} \
			_wireEmit request mac "$cia" >/dev/null || st=$?
		expect "$st" "$want" "$label"
	}
	clockedEmit 0 healthy '100 101 102 103' '100 101 102 103'
	clockedEmit 42 two-slow '100 112 124 124' '100 112 124 124'
	clockedEmit 42 backward-step '100 112 109 109' '100 112 124 124'
	clockedEmit 42 different-origins '115 127 139 139' '100 112 124 124'
	clockedEmit 0 forward-steps '100 116 132 148' '100 101 102 103'
	clockedEmit 0 one-slow '100 112 113 114' '100 112 113 114'
	clockedEmit 0 one-slow-backward-step '100 97 98 99' '100 112 113 114'
	clockedEmit 0 last-slow '100 101 102 114' '100 101 102 114'
	clockedEmit 0 last-two-slow '100 101 113 125' '100 101 113 125'
	clockedEmit 0 inside-grace '100 112 124 124' '100 112 124 124' 30
	clockedEmit 0 past-max '100 112 124 124' '100 112 124 124' 0 23
	clockedEmit 42 grace-boundary '100 112 124 124' '100 112 124 124' 24
	clockedEmit 42 max-boundary '100 112 124 124' '100 112 124 124' 0 24
	clockedEmit 0 total-rate '100 101 107 113 114' '100 101 107 113 114' 0 150 33554432
	clockedEmit 0 slow-fast-slow '100 112 113 125 126' '100 112 113 125 126' 0 150 33554432
	clockedEmit 42 total-so-far '100 112 124 124 124 124 124 124 124' \
		'100 112 124 124 124 124 124 124 124' 0 150 67108864
	clockedEmit 0 same-second '100 100 100 100' '100 100 100 100'
	# A clock that cannot be read never stops a send: the check is dropped and the whole body goes.
	clocklessEmit() {
		local want=$1 works=$2 label=$3 index=0 st=0 emitSize=33554432
		local -a wall=(100 100 100 100 100) mono=(100 112 124 136 148)
		SECONDS=100
		THREEDROP_GHOST_FLOOR=1500000 THREEDROP_GHOST_GRACE=0 \
			_wireEmit request mac "$cia" >/dev/null || st=$?
		expect "$st" "$want" "$label"
		expect "$index" "$4" "$label: segments sent"
	}
	# shellcheck disable=SC2317,SC2034 # _wireEmit calls this and reads wireMono
	_wireMono() {
		[ "$index" -lt "$works" ] || return 1
		wireMono=${mono[index]}
	}
	clocklessEmit 42 5 clock-read-control 2
	clocklessEmit 0 0 clock-never-read 4
	clocklessEmit 0 1 clock-lost-mid-send 4
) || exit 1

drop 42 "${ghost[@]}"
# stats fetches the first put's nonce, then each put's receipt, whose reply carries the next nonce.
[ "$(awk '{ printf "%s ", $1 }' "$FAKE/log")" = "stats put stats put stats " ] ||
	fail "a ghost must be sent once more, after its receipt: $(awk '{ printf "%s ", $1 }' "$FAKE/log")"
[ "$(distinct 2)" = 2 ] || fail "the retry reused the request id"
[ "$(distinct 3)" = 2 ] || fail "the retry re-presented the consumed nonce"
grep -q 'ghost detected' "$TMP/err" || fail "drop.sh did not announce the retry"
drop 42 "${ghost[@]}" THREEDROP_GHOST_RETRY=0
[ "$(count put)" = 1 ] || fail "THREEDROP_GHOST_RETRY=0 still retried"
drop 0 THREEDROP_GHOST_FLOOR=1 THREEDROP_GHOST_GRACE=9999
{ [ "$(cat "$FAKE/bytes")" = "$size" ] && grep -q 'put committed ok' "$TMP/out"; } || fail "a segmented send lost bytes"
# A ghost is sent again only once its receipt says the console canceled it: a console silent for a
# while is waited for, and one whose receipt is gone gets no second put.
(
	# shellcheck disable=SC2317 # called by drop.sh and wire.sh
	sleep() { [ "$1" = "$WIRE_KNOCK" ] || "$realSleep" "$@"; }
	export -f sleep
	faulted slow-cancel drop 42 "${ghost[@]}"
)
[ "$(count put)" = 2 ] || fail "a ghost the console took a while to cancel was not sent again"
faulted lost-receipt drop 4 "${ghost[@]}" # its outcome unknown: no answer to it
[ "$(count put)" = 1 ] || fail "a ghost whose receipt was lost was sent again"
faulted ghost-commits drop 0 "${ghost[@]}"
{ [ "$(count put)" = 1 ] && grep -q 'put committed ok' "$TMP/out"; } || fail "a ghost the console committed was sent again"
# A stall in one segment (a paused tool, a Mac asleep) with the next segment fast again is not a ghost. The
# body is long enough that a socket buffer cannot swallow the rest of it, so the host must block in the
# segment the stall is in. The floor sits below the rate of a loaded box's own segment (8 MiB in 3 s is 2.8
# MB/s) and above the stalled segment's rate (8 MiB in 12 s). That is a margin of about 2x, not a bound: a
# segment that took 5.6 s would count as a second slow one.
pauseCia="$TMP/pause.cia"
fakeCia "$pauseCia" 0004000001234500 1 41943040
cia=$pauseCia faulted pause-once drop 0 "${ghost2[@]}"
{ [ "$(count put)" = 1 ] && grep -q 'put committed ok' "$TMP/out" && ! grep -qi ghost "$TMP/err"; } ||
	fail "one slow segment was taken for a ghost"
fakeCia "$TMP/small.cia" 0004000001234500 1 8388608
cia=$TMP/small.cia drop 0 "${ghost[@]}"
{ [ "$(count put)" = 1 ] && grep -q 'put committed ok' "$TMP/out" && ! grep -qi ghost "$TMP/err"; } ||
	fail "a body in one segment was taken for a ghost"
# A ghost the retry commits: the id file holds one line per put sent, the ghost's and then the retry's own,
# which is what the bench arm ties its receipt and stats to; the two lines are how it sees the ghost.
faulted ghost-then-fast drop 0 "${ghost2[@]}" THREEDROP_PUT_ID_FILE="$TMP/final"
grep -q 'ghost detected' "$TMP/err" || fail "a put the first attempt stalled was not a ghost"
[ "$(count put)" = 2 ] || fail "the ghost was not sent again: $(count put) puts"
[ "$(grep '^put ' "$FAKE/log" | cut -d' ' -f2 | tr '\n' ' ')" = "$(tr '\n' ' ' <"$TMP/final")" ] ||
	fail "the put id file holds $(tr '\n' ' ' <"$TMP/final"), not each put's id in turn"

# /dev/tcp: the ghost closes the socket, so the device sees EOF before the declared size. Guard the
# nonce exchange against a wall-clock step, as in `drop`.
prefix=$(ciaPrefixSize "$cia")
until steadyRun; do
	: >"$FAKE/bytes"
	: >"$FAKE/installed"
	# This put's own receipt says the device finished reading the body it kept.
	rm -f "$FAKE/receipt" "$(_wireNonceFile 127.0.0.1 "$fakePort")"
	nonce=$(devtcp _wireNonce 127.0.0.1 "$fakePort")
	request="put $(newRequestID) $nonce new $size $prefix $(unitDigest "$cia" "$prefix")"
	# shellcheck disable=SC2016 # the child shell expands its own arguments
	status=$(env "${ghost[@]}" bash -c '. "$FAKE_LIB"; devtcp _wireTransport 127.0.0.1 "$1" 5 "$2" "$3" "$4" >/dev/null; echo $?' _ \
		"$fakePort" "$request" "$(macRequest "$request")" "$cia")
	# The device names the title and hashes what it kept after the close, and its receipt says that read is
	# done. Going on before the receipt (a rerun, or the end of the test) takes $FAKE/body away mid-read, and
	# the device's od reports that on stderr.
	[ "$status" != 42 ] || waitFor "$FAKE/receipt"
done
[ "$status" = 42 ] || fail "/dev/tcp ghost status '$status'"
[ "$(cat "$FAKE/bytes")" -lt "$size" ] || fail "/dev/tcp ghost sent the whole body"
grep -q '^op .* put rejected short ' "$FAKE/receipt" || fail "the device did not refuse the body the ghost cut"
echo "testGhostRetry: PASS"

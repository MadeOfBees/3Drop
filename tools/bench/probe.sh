#!/usr/bin/env bash
# probe.sh <sub> <ip> [args]: an exchange with a bench build (src/bench/bench.c), or a run of them (tool,
# --smoke), its reply printed, with its rate where it has one; a refusal on stderr, exiting as device.sh does.
#   state <ip>                           every knob's applied value by name, capture, mb_rows (mb's capacity)
#   rx <ip> [units=32] [body|-]          receive rate, no card or AM in the way (a unit is 1048576 bytes); a body file (`-`: stdin) goes as it comes
#   tx <ip> [units=32]                   send rate, no card or AM in the way, read as get is
#                                        rx, tx: bytes ms; busy_ms calls in recv or send; idle_ms eagain in EAGAIN waits
#                                        The window misses up to a socket buffer (rcvbuf at the start of rx, sndbuf at
#                                        the end of tx): compare buffer knobs at 32 units or more
#   file <ip> <units> [prealloc 0|1] [chunk in 1024-byte blocks] [flush 0|1] [read 0|1]   SD write floor without
#                                        AM; read 1 then times reading the file back in the same chunks: bytes chunk
#                                        writes prealloc_ms first_ms steady_ms steady_max_ms flush_ms bytes_done reads
#                                        read_first_ms read_steady_ms read_steady_max_ms read_done
#   mb <ip>                              the last transfer's rows: req dir rows cols, then wall_ms wait_ms eagain sd_kb per MB
#                                        (wall_ms, wait_ms and sd_kb are running totals, eagain the row's own; sd_kb is a
#                                        put's written bytes, a get's the pump's, a buffer ahead of the wire, an install's
#                                        AM's import position)
#   hw <ip>                              the last timed request's timeline: req rows calls gaps gap_min_ms, then a line per call
#                                        (<kind> start_us end_us k; d a digest call, w a write, r a read) and per wire
#                                        gap of 30 ms or more (g quiet_us call_us end_us), in us from the request's start
#   mem <ip>                             heap and linear memory with tier 1 taken first, so lin_free is what is left:
#                                        lin_heap lin_free heap heap_touched_max tier1
#   rcvbuf|sndbuf|sendsize|nohash|nodisc <ip> [value]   read a knob, or set it (0 = the default): applied default,
#                                        and for rcvbuf and sndbuf client, what this request's own socket took (on a
#                                        set, a socket that predates the new listener: read it again to see one under it).
#                                        A set spends a nonce, as every change does: one captured on the network cannot
#                                        set it again (refused, `challenge`), after a relaunch too, until the key
#                                        changes; a read spends none (`nohash 1` leaves later SD puts unverified,
#                                        `nodisc 1` silences discovery).
#   fscalls <ip>                         the last change's file steps, each in us (- where it took none): req orphan
#                                        open setsize flush close publish aside swap drop delete rename log
#   capture <ip> [on|off]                capture-on-change: capture frames held dropped slots (on|off is a set: see the
#                                        knobs; capture.sh on|off sets it and checks that it holds)
#   led <ip>                             the LED now (rgb, MCU register 0x7F), the last write (shown breathes), gap_max_ms,
#                                        the longest gap after a lit write, writes failed; read and reset: once the reply
#                                        has gone out, gap_max_ms, writes and failed start over
#   disc <ip>                            discovery's counters: disc name asked answered announced probed drained opened unsent
#   tool <ip> <title-id>                 run the Testing Tool unattended, wait for 3Drop, print its evidence
#   evidence <file> [title-id]           check and print a Testing Tool evidence file
#   --smoke <ip>                         state, mem, rx 1, mb, tx 1 and file 1, checked by value
set -euo pipefail
. "$(dirname "$0")/lib.sh"

sub="${1:-}"
ip="${2:-}"
if [ -z "$sub" ] || [ -z "$ip" ]; then usage; fi
shift 2

# show <reply>: prints an ok reply with its rate. The line carries inst=, the host instrument it was
# read with, because this host is in the loop and readings under another instrument never pool. Any
# other reply is reported.
show() {
	wireFields "$1"
	[ "$wireOutcome" = ok ] || {
		wireReport "$1"
		return
	}
	if isPositive "$(kv "$1" ms)"; then
		echo "$1  ($(mbps "$(kv "$1" bytes)" "$(kv "$1" ms)") MB/s) inst=$(inst)"
	else
		echo "$1"
	fi
}

# verdict <name> <reply> <ok if it passed>: prints one line of --smoke's output,
# and returns 1 for a FAIL.
# shellcheck disable=SC2317 # called by smoke
verdict() {
	if [ "$3" = ok ]; then
		echo "ok   $1"
		return
	fi
	echo "FAIL $1: ${2:-no answer}"
	return 1
}

# Checks what a bench build fresh from `run self` must answer. 3Drop's own allocations hold part of
# the heap, so a malloc whose every page was written stays below its size, and an equal value is not
# a measurement.
# shellcheck disable=SC2317 # called by bounded
smoke() {
	local failed=0 reply id state
	# The defaults and sizes are read from the device's source, never copied.
	state="state rcvbuf=$(cDefine net.h LISTENER_WINDOW_BYTES)"
	state="$state sndbuf=$(cDefine net.h LISTENER_SEND_BUFFER) sendsize=$(cDefine net.h SEND_CHUNK_SIZE)"
	state="$state nohash=0 nodisc=0 capture=[01] mb_rows=$(cDefine bench/bench.c MB_ROWS) "
	reply=$(wireRead "$ip" "$port" bench 2>&1) || :
	# A release build refuses this bare `bench` itself, so there is no second exchange.
	benchBuild "$ip" "$port" "$reply"
	# The pattern is open at the end, as a reply is (a key may be appended, per bench.c:2). The space
	# ends mb_rows' number.
	# $state is a pattern, so capture may be either.
	verdict state "$reply" "$(benchOk "$reply " "$state" && echo ok)" || failed=1
	reply=$(wireRead "$ip" "$port" bench mem 2>&1) || :
	verdict mem "$reply" "$([ "$(kv "$reply" tier1)" = "$(cDefine ring.h RING_TIER1_BYTES)" ] && isPositive "$(kv "$reply" lin_free)" &&
		isPositive "$(kv "$reply" heap_touched_max)" && isPositive "$(kv "$reply" heap)" &&
		[ "$(kv "$reply" heap_touched_max)" -lt "$(kv "$reply" heap)" ] && echo ok)" || failed=1
	reply=$(benchRx "$ip" "$port" 1 2>&1) || :
	read -r _ id _ <<<"$reply"
	verdict "rx 1" "$reply" "$(benchOk "$reply" "rx bytes=$WIRE_UNIT " && echo ok)" || failed=1
	reply=$(benchMb "$ip" "$port" 2>&1) || :
	verdict "mb names rx 1" "$reply" "$([ "$(kv "$reply" req) $(kv "$reply" dir) $(kv "$reply" rows)" = "$id rx 1" ] && echo ok)" || failed=1
	reply=$(benchTx "$ip" "$port" 1 2>&1) || :
	verdict "tx 1" "$reply" "$(benchOk "$reply" "tx bytes=$WIRE_UNIT " && echo ok)" || failed=1
	reply=$(benchFile "$ip" "$port" 1 2>&1) || :
	verdict "file 1" "$reply" "$(benchOk "$reply" "file " &&
		[ "$(kv "$reply" bytes) $(kv "$reply" writes) $(kv "$reply" bytes_done)" = "$WIRE_UNIT 1 $WIRE_UNIT" ] && echo ok)" || failed=1
	return "$failed"
}

# toolEvidence <file> [title-id]: prints the file, then a verdict; returns non-zero unless the tool
# finished as that title and read a color at least once (a read is `<ms> rrggbb`, a failed one
# `<ms> err <result>`).
toolEvidence() {
	local file=$1 title good all
	cat "$file"
	title=$(sed -n 's/^title \([0-9a-fA-F]\{16\}\)$/\1/p' "$file")
	good=$(grep -c '^[0-9]* [0-9a-f]\{6\}$' "$file" || :)
	all=$(grep -c '^[0-9]* \([0-9a-f]\{6\}\|err [0-9a-f]*\)$' "$file" || :)
	if ! grep -q '^build .' "$file" || [ -z "$title" ] || ! grep -qx 'done' "$file"; then
		echo "$TOOL: no complete evidence (build, title and done lines)" >&2
		return 1
	fi
	if [ -n "${2:-}" ] && [ "$(wireTitleID "$title")" != "$(wireTitleID "${2#title:}")" ]; then
		echo "$TOOL: it ran as $title, not $2" >&2
		return 1
	fi
	if [ "$good" = 0 ]; then
		echo "$TOOL: it read no color (reads=0 of $all)" >&2
		return 1
	fi
	echo "tool ok title=$title reads=$good of $all"
}

toolMarker=/3ds/3drop/testing-tool.auto
toolEvidenceFile=/3ds/3drop/testing-tool.txt
# The tool removes the marker as it starts (testingTool.c), so from the put it stands on the card
# until then. Left there, it makes the next launch, by hand, unattended. A launch the console refused
# never started the tool, so the marker is this command's to take back. After any other end (a lost
# reply, a tool that has not come up, an interrupt) it is unknown whether the tool is running, and a
# delete could race it, so the marker is named on the way out and left.
# shellcheck disable=SC2317 # called by runTool's EXIT trap
toolMarkerNote() {
	echo "$TOOL: if the Testing Tool did not start, $toolMarker is still on the card and makes its next launch unattended: remove it with  tools/device.sh delete $ip $toolMarker" >&2
}

# runTool <title-id, or title:<id>>: the marker makes the tool run unattended, and the old evidence
# is deleted first so a stale file cannot pass.
# This is a subshell because bash 3.2 does not run an EXIT trap that a function set when the function
# returns as an async command.
# shellcheck disable=SC2317 # called by bounded
runTool() (
	local id marker="$work/marker" evidence="$work/evidence" reply st=0
	id=$(wireTitleID "${1#title:}") || return # a bad id is refused before the marker goes anywhere
	echo 1 >"$marker"
	reply=$(wireDelete "$ip" "$port" "$toolEvidenceFile" 2>&1) || st=$?
	case "$st:$reply" in
	0:* | *" delete rejected notfound "*) ;;
	*)
		echo "$TOOL: the old evidence stayed: $reply" >&2
		return "$st"
		;;
	esac
	wireFilePut "$ip" "$port" "$marker" "$toolMarker" >/dev/null || return
	trap toolMarkerNote EXIT
	st=0 # the delete of the old evidence left its own status (1: notfound)
	wireRun "$ip" "$port" "$id" >/dev/null || st=$?
	if [ "$st" = 1 ]; then # refused, so nothing started
		if wireDelete "$ip" "$port" "$toolMarker" >/dev/null 2>&1; then trap - EXIT; fi
		return 1
	fi
	[ "$st" = 0 ] || return "$st"
	wireWaitUp "$ip" "$port" "" 180 >/dev/null || return # it says why
	wireGet "$ip" "$port" "$toolEvidenceFile" "$evidence" >/dev/null || return
	trap - EXIT # the evidence is the tool's, and the tool took the marker as it started
	toolEvidence "$evidence" "$id"
)

if [ "$sub" = evidence ]; then
	file=$ip # the second word is a file here, not an address
	toolEvidence "$file" "$@"
	exit
fi
benchConsole "$ip" # sets ip and port, the address every exchange below uses
scratch
# An interrupt ends the deadlines a pipeline ran, whose variables this shell never sees.
trap 'deadlineStopAll; _scratchEnd' EXIT
work=$SCRATCH
# bounded <seconds> <command...>: runs the command under a deadline (BENCH_DEADLINE where set), which
# that trap ends. It never runs in a $(...) of this shell, because bash 3.2 reads one to its end
# before it runs the TERM trap.
bounded() { withDeadline "${BENCH_DEADLINE:-$1}" "${@:2}"; }
# shellcheck disable=SC2317 # called by bounded
showReply() {
	local reply
	reply=$("$@") && show "$reply"
}
control=$((3 * $(wireTimeout) + WIRE_MARGIN)) # wireRead's three tries
listing=$(($(wireTimeout) + WIRE_SILENCE))
if [ "$sub" = tool ]; then # a title of its own, with no bench verb, so there is no bench build to ask for
	[ -n "${1:-}" ] || usage
	bounded $((180 + 4 * control)) runTool "$1" # wireWaitUp's 180 s and four exchanges
	exit
fi

# The subs fall into three shapes, by what each can be wrapped in: an exchange shown by showReply
# (state, mem, led, disc, fscalls, capture, the knobs); a listing that prints itself (mb, hw); and rx,
# tx and file, whose reply goes through a file, since benchRx and benchTx run under deadlines of their
# own, scaled to their size, which `bounded` cannot wrap.
dispatch() {
	case "$sub" in
	state) bounded "$control" showReply wireRead "$ip" "$port" bench ;;
	rx | tx | file)
		[ "$sub" != file ] || [ "$#" -gt 0 ] || usage 2>&3
		st=0
		# The reply goes into a file because bash 3.2 reads a $(...) to its end before it runs the TERM trap.
		case "$sub" in
		rx) benchRx "$ip" "$port" "${1:-32}" "${2:-}" >"$work/reply" || st=$? ;;
		tx) benchTx "$ip" "$port" "${1:-32}" >"$work/reply" || st=$? ;;
		file) bounded $(($(wireTimeout 600) + WIRE_MARGIN)) benchFile "$ip" "$port" "$*" >"$work/reply" || st=$? ;;
		esac
		reply=$(<"$work/reply")
		[ -n "$reply" ] || return "$st" # what failed is said
		show "$reply"
		;;
	mb) bounded "$listing" benchMb "$ip" "$port" ;;
	hw) bounded "$listing" benchHw "$ip" "$port" ;;
	mem | led | disc | fscalls) bounded "$control" showReply wireRead "$ip" "$port" bench "$sub${*:+ $*}" ;;
	capture)
		# on|off is a set and spends a nonce, as every change does; a bare capture is a read.
		if [ "$#" -gt 0 ]; then
			bounded "$control" showReply wireMutation "$ip" "$port" "$(wireTimeout)" \
				bench "capture $1"
		else
			bounded "$control" showReply wireRead "$ip" "$port" bench capture
		fi
		;;
	rcvbuf | sndbuf | sendsize | nohash | nodisc) bounded "$control" showReply benchKnob "$ip" "$port" "$sub" "$@" ;;
	--smoke) bounded $((2 * control + 3 * listing + $(wireTimeout 600))) smoke ;; # state, mem, rx, mb, tx, file
	*) usage 2>&3 ;;
	esac
}

# What a failed sub said goes to a file, not the screen, because a release build refuses any of them
# (as `error rejected`, which wire.sh blames on an older 3Drop), and benchBuild says so, asking the
# console only then. A bad call is reported at once, through fd 3, which is the screen.
exec 3>&2
st=0
dispatch "$@" 2>"$work/err" || st=$?
if [ "$st" != 0 ]; then
	said=$(<"$work/err")
	benchBuild "$ip" "$port" "$said"
	[ -z "$said" ] || printf '%s\n' "$said" >&2
fi
exit "$st"

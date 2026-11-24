# shellcheck shell=bash
BENCH="$(CDPATH='' cd "${BASH_SOURCE[0]%/*}" && pwd)"
# shellcheck source=tools/wire.sh
. "$BENCH/../wire.sh"

BENCH_RESULTS="${BENCH_RESULTS:-$PWD/bench-results.tsv}"
# A test builds its fixtures in a folder of its own, never the repo's, which a series may be using.
# shellcheck disable=SC2034 # read by bench.sh and makeFixture.sh
FIXTURES="${THREEDROP_FIXTURE_DIR:-$ROOT/buildFixture}"

# romfsKept <size>: prints the path of the RomFS 3dstool made once for a size, keyed by the size and the rsf's
# hash.
romfsKept() {
	local digest
	digest=$(sha256File "$ROOT/res/fixtureCFA.rsf") || return
	echo "$FIXTURES/content1-${1}m-${digest:0:12}.ncch"
}

# The reply cells hold replies raw, read by key name (kv), never by position, because a reply only ever gains
# keys.
BENCH_COLUMNS="series rep time uid title size_bytes wall_s rate_mbs outcome ver inst knobs capture
am_titles am_tickets free_bytes air card_before card_after stats mb"

benchHeader() {
	# shellcheck disable=SC2086 # word-split the column list
	echo $BENCH_COLUMNS | tr ' ' '\t'
}

# benchLayout <results>: for a file with another column layout, whose rows would be misread, prints a message
# and returns 1.
benchLayout() {
	[ ! -s "$1" ] || [ "$(head -1 "$1")" = "$(benchHeader)" ] || {
		echo "$TOOL: $1 has another column layout than these tools write: point BENCH_RESULTS at a new file, or score it with the tools that wrote it" >&2
		return 1
	}
}

tsvAppend() {
	benchLayout "$1" || return
	[ -s "$1" ] || benchHeader >"$1"
	printf '%s\n' "$2" >>"$1"
}

# benchRow <results> <name=value>...: writes one row, each cell given by its column's name and written in
# BENCH_COLUMNS order, so no cell is ever placed by position. A name that is not a column, a column named
# twice or a column left out is refused (a message, and status 1) and nothing is written. An empty value is
# written as `-`, as kv reads an empty value. The encoder starts no process and uses only the shell's own
# tests (bash 3.2 has no associative arrays, so each column looks for its cell). tsvAppend, which then writes
# the line, still starts a head and a tr to read the file's layout.
benchRow() {
	local results=$1 cell name n value why line="" tab=$'\t'
	shift
	for cell; do
		for name in $BENCH_COLUMNS; do [ "${cell%%=*}" != "$name" ] || continue 2; done
		echo "$TOOL: a row has no column ${cell%%=*}" >&2
		return 1
	done
	for name in $BENCH_COLUMNS; do
		n=0
		for cell; do
			[ "${cell%%=*}" != "$name" ] || n=$((n + 1)) value=${cell#*=}
		done
		case "$n" in
		1)
			line+=${line:+$tab}${value:--}
			continue
			;;
		0) why="has no cell for" ;;
		*) why="names twice the column" ;;
		esac
		echo "$TOOL: a row $why $name" >&2
		return 1
	done
	tsvAppend "$results" "$line"
}

# One awk library for bench.sh, seriesScore.sh and reclaimTestTitles.sh: how a row counts (rowClass),
# which rows may hold a title (holdsTitle), and the bad-run rule (bad), each written once here.
# shellcheck disable=SC2016 # awk's own $i, not the shell's
BENCH_AWK='
function columns(   i) { for (i = 1; i <= NF; i++) col[$i] = i }
# An empty value reads `-`, as kv in wire.sh reads it.
function kv(text, key,   s, at) {
	s = " " text " "
	at = index(s, " " key "=")
	if (!at) return "-"
	s = substr(s, at + length(key) + 2)
	s = substr(s, 1, index(s, " ") - 1)
	return s == "" ? "-" : s
}
function mbps(bytes, ms) { return ms + 0 > 0 ? sprintf("%.4f", bytes / 1e6 / (ms / 1000)) : "-" }
# rowClass(outcome, wall): how a row counts, by one rule for every reader. ok: committed, ranked by its wall.
# fail: ranked past every wall, since its wall is a lower bound, not a result. void and skip: counted, never
# ranked. A committed row whose wall is not a positive number is void (void:clock), because a clock that
# stepped gave it no time, and it must never rank as the fastest.
function rowClass(outcome, wall) {
	if (outcome ~ /^skipped:/) return "skip"
	if (outcome ~ /^void:/) return "void"
	if (outcome !~ /^committed:/) return "fail"
	return wall ~ /^[0-9]+(\.[0-9]+)?$/ && wall + 0 > 0 ? "ok" : "void"
}
# holdsTitle(outcome): whether a rep that ended this way may have left its title installed, so reclaim looks
# for the title at its id (and takes it only as the fixture of its row, in reclaimTestTitles.sh). committed
# and void: the put committed (void reps are committed ones that no stats could vouch for). unknown, deadline
# and noreply: the console never said, so the put may have gone through. rejected:am-write and am-cancel: AM
# had begun, and a failed cancel leaves the import. Every other refusal installed nothing, so a title at its
# id belongs to another client. void:ver and void:knobs are older rows, written over any outcome (a refused
# put is one of them).
function holdsTitle(outcome) {
	if (outcome ~ /^void:(ver|knobs)$/) return 0
	return outcome ~ /^(committed:|void:|unknown:|rejected:am-write$|rejected:am-cancel$|deadline$|noreply$)/
}
function order(key, n, idx,   i, j, t) {
	for (i = 1; i <= n; i++) idx[i] = i
	for (i = 2; i <= n; i++)
		for (j = i; j > 1 && key[idx[j]] < key[idx[j - 1]]; j--) { t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t }
}
# An even count averages its middle pair, but if the upper one is FAILED (half or more failed), that value is
# the median.
function median(key, n, idx,   lo, hi) {
	lo = key[idx[int((n + 1) / 2)]]
	if (n % 2) return lo
	hi = key[idx[n / 2 + 1]]
	return hi < FAILED ? (lo + hi) / 2 : hi
}
function bad(key, med) { return key >= FAILED || key > 1.25 * med }
BEGIN { FAILED = 1e12 }
'

# What bench.sh times an install by: `perl -e "$BENCH_TIMER" <file> <command...>`, run under withDeadline. It
# reads its clock in its own process, around the command's fork and exit, so the deadline's setup and teardown
# (mktemp, the watchdog, killTree's ps, rm) and starting a separate process to ask the clock are not part of
# the interval, and nothing is spawned inside it. It uses CLOCK_MONOTONIC where this perl has one, because the
# wall clock steps (WSL's moved back 1-3 s about every 30 s) and a wall clock that stepped gives no time.
# Otherwise it uses the wall clock, and benchClock says which before rep 1. <file> gets `<seconds> <clock>`.
# The status is the command's (128 + the signal for a killed one). TERM does not end it, because a deadline's
# TERM ends the command first and this lives on to write what it took. With no file it runs nothing and prints
# the clock it would read, so benchClock's answer comes from this same rule.
# shellcheck disable=SC2016,SC2034 # perl's own $; bench.sh and testBenchRep.sh read it
BENCH_TIMER='
use strict; use warnings;
use Time::HiRes ();
my ($out, @cmd) = @ARGV;
my $mono = eval { Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC()); 1 };
if (!defined $out) { print STDOUT $mono ? "monotonic" : "wall"; exit 0 }
my $now = $mono ? sub { Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC()) } : sub { Time::HiRes::time() };
$SIG{TERM} = sub { };
my $t0 = $now->();
my $rc = system { $cmd[0] } @cmd;
my $t1 = $now->();
my $status = $rc == -1 ? 127 : $? & 127 ? 128 + ($? & 127) : $? >> 8;
print STDERR "$cmd[0]: $!\n" if $rc == -1;
open(my $fh, ">", $out) or die "$out: $!\n";
printf $fh "%.6f %s\n", $t1 - $t0, $mono ? "monotonic" : "wall";
close($fh) or die "$out: $!\n";
exit $status;
'
# benchClock: prints `monotonic` or `wall`, the clock BENCH_TIMER reads, as BENCH_TIMER prints it when given
# no file.
benchClock() { perl -e "$BENCH_TIMER"; }

# withDeadline <whole seconds> <command...>: runs the command under a deadline, because one hung exchange must
# not stall a series. It returns 124 when the deadline ended it, as timeout(1) does, and DEADLINE_FIRED is
# then 1, since the command may time out on its own. The command keeps the caller's stdin, because a
# non-interactive bash gives an async command /dev/null unless it names one. Meanwhile DEADLINE_PID and
# DEADLINE_DOG name the command and its watchdog.
withDeadline() {
	local secs=$1 rc=0 armed
	shift
	# The watchdog counts the seconds down, and bash reads a leading 0 as octal, so `08` would stop the count.
	{ isPositive "$secs" && [ "${#secs}" -le 9 ]; } || dieWith 2 "a deadline of whole seconds (at most 9 digits), not '$secs'"
	secs=$((10#$secs))
	DEADLINE_FIRED=0
	# The file is removed before the kill, so a deadline reads as one even while the watchdog is still
	# exiting.
	armed=$(mktemp "${SCRATCH:-${TMPDIR:-/tmp}}/3drop-deadline.XXXXXX") || return
	"$@" <&0 &
	DEADLINE_PID=$!
	# It also fires once this shell is gone, which a trap may never have seen (a KILL, or the Ctrl-C of a
	# $(...)).
	watchOwner "$DEADLINE_PID" _deadlineTick _deadlineFire
	DEADLINE_DOG=$!
	# Both pids are written to the file as well, for the deadlineStopAll that a trap calls.
	echo "$DEADLINE_DOG $DEADLINE_PID" >"$armed"
	# Kept quiet because bash 3.2 prints "Terminated: 15" as it reaps a command the deadline killed.
	wait "$DEADLINE_PID" 2>/dev/null || rc=$?
	DEADLINE_PID="" # reaped, so the pid may belong to another process now
	if [ "$rc" != 0 ] && [ ! -e "$armed" ]; then
		# shellcheck disable=SC2034 # bench.sh reads it
		rc=124 DEADLINE_FIRED=1
	fi
	deadlineStop
	rm -f "$armed"
	return "$rc"
}
# The watchdog's per-second step and its end, both working on withDeadline's local variables.
_deadlineTick() {
	secs=$((secs - 1))
	[ "$secs" -gt 0 ]
}
_deadlineFire() {
	rm -f "$armed"
	killTree "$DEADLINE_PID"
}
# deadlineStop: ends withDeadline's command and watchdog, also from an interrupt's trap, because an async
# command ignores Ctrl-C and a watchdog left behind would later signal whatever took the pid.
deadlineStop() {
	local pid
	for pid in ${DEADLINE_DOG:-} ${DEADLINE_PID:-}; do
		killTree "$pid"
		wait "$pid" 2>/dev/null || :
	done
	DEADLINE_DOG="" DEADLINE_PID=""
}
# deadlineStopAll: runs deadlineStop for a trap, and also ends each withDeadline that the scratch directory
# names, including any that a $(...) or a pipeline of the tool ran, because a trap never sees a subshell's
# variables.
deadlineStopAll() {
	local file pids pid
	for file in ${SCRATCH:+"$SCRATCH"/3drop-deadline.??????}; do
		read -r pids 2>/dev/null <"$file" || continue
		# shellcheck disable=SC2086 # two pids
		for pid in $pids; do if isUint "$pid"; then killTree "$pid"; fi; done
	done
	deadlineStop
}

mbps() {
	awk -v b="$1" -v ms="$2" "$BENCH_AWK"'BEGIN { print mbps(b, ms) }'
}

# The host instrument's identity, so rows sent by different scripts never pool. It is the first 16 digits of
# its digest, cut by the shell. A block reads it once, and every rep reads it again.
inst() {
	local digest
	digest=$(cat "$ROOT"/tools/*.sh "$ROOT"/tools/bench/*.sh | openssl dgst -sha256) || return
	digest=${digest##* }
	echo "${digest:0:16}"
}

# The host is the sender, so a container or gate beside a series shares its CPU and network. A gate is
# tools/check.sh as ps -ef's command (its eighth field on), or as a shell's first word past its options and
# their operands (-o or -O anywhere in a cluster, their + forms, --rcfile, --init-file; -- ends them). A
# process that only names tools/check.sh, as an editor or a grep does, is not a gate.
macQuiet() {
	if [ -n "$(docker ps -q 2>/dev/null)" ]; then
		echo "$TOOL: docker has running containers: a series needs this PC quiet" >&2
		return 1
	fi
	if ps -ef | awk '{ i = 8 }
		$i ~ /(^|\/)(ba|da|k|z)?sh$/ {
			for (i++; $i ~ /^[-+]/; i++) {
				if ($i == "--") { i++; break }
				if ($i ~ /^[-+][A-Za-z]*[oO][A-Za-z]*$/ || $i ~ /^--(rcfile|init-file)$/) i++
			}
		}
		$i ~ /(^|\/)tools\/check\.sh$/ { found = 1 } END { exit !found }'; then
		echo "$TOOL: a gate (tools/check.sh) is running: a series needs this PC quiet" >&2
		return 1
	fi
}

# repVerdict <fact>=<value>...: prints the rep's outcome from its facts, by the first rule that holds, in one
# place. The facts: put, drop.sh's put line as <word>:<detail> (empty if it said none); fired, 1 when the
# rep's deadline ended drop.sh; sent, the puts drop.sh sent (the lines of THREEDROP_PUT_ID_FILE); own, the
# last of their ids; receipt, the put id `stats op` names; stats, the `stats` reply's fields; bytes, the
# fixture's size; timed, the class rowClass gives the rep's wall_s cell (`ok` means a clock gave it a time). A
# committed rep counts only if it was one put, and its receipt and stats describe that put; any other is
# `void:<cause>`, counted and never ranked. A ghost retry is two installs in one wall, with the stats of the
# second.
repVerdict() {
	local fact put="" fired=0 sent=1 own="" receipt="" stats="" bytes="" timed=void
	for fact; do
		case "$fact" in
		put=*) put=${fact#*=} ;;
		fired=*) fired=${fact#*=} ;;
		sent=*) sent=${fact#*=} ;;
		own=*) own=${fact#*=} ;;
		receipt=*) receipt=${fact#*=} ;;
		stats=*) stats=${fact#*=} ;;
		bytes=*) bytes=${fact#*=} ;;
		timed=*) timed=${fact#*=} ;;
		*)
			echo "$TOOL: repVerdict has no fact ${fact%%=*}" >&2
			return 2
			;;
		esac
	done
	if [ -z "$put" ]; then
		if [ "$fired" = 1 ]; then echo deadline; else echo noreply; fi
	elif [[ "$put" != committed:* ]]; then
		echo "$put"
	elif [ "$sent" != 1 ] && [ "$sent" != 0 ]; then
		echo void:ghost
	elif [ -n "$own" ] && [ "$receipt" != "$own" ]; then
		# A foreign put replaces the receipt and the transfer records together, so comparing those
		# two with each other can never see it: only the rep's own put id does.
		echo void:req
	elif [ -z "$stats" ] || [ "$stats" = - ]; then
		echo void:stats
	elif [ "$(kv "$stats" req)" != "${own:-$receipt}" ]; then
		echo void:req # another request reached the console between the put and this read
	elif [ "$(kv "$stats" bytes)" != "$bytes" ]; then
		echo void:bytes
	elif [[ "$(kv "$stats" am_write_n)" == [-0] ]]; then
		echo void:am-writes
	elif [ "$timed" != ok ]; then
		echo void:clock # no clock gave it a time (rowClass)
	else
		echo "$put"
	fi
}

# The token goes to THREEDROP_REP_OWNER, so the rep's own exchanges, and the tools it runs, pass.
repLock() {
	local lock=$WIRE_RUN_DIR/rep-$1
	if ! { _wireRepFree "$1" && mkdir "$lock" 2>/dev/null; }; then
		echo "$TOOL: could not take the rep lock $lock" >&2
		return 1
	fi
	THREEDROP_REP_OWNER=$(openssl rand -hex 8)
	export THREEDROP_REP_OWNER
	_wireNow
	printf '%s %s %s\n' "$THREEDROP_REP_OWNER" "$$" "$wireNow" >"$lock/owner"
	REP_LOCK=$lock
}
repUnlock() {
	[ -z "${REP_LOCK:-}" ] || rm -rf "$REP_LOCK"
	REP_LOCK=""
}

# requireLabel <label>: refuses it (a message, and status 2) unless it is a TSV cell a reader can find again:
# not empty, and no tab, newline or other control character. The awks read it from BENCH_LABEL, never `-v`,
# which decodes a backslash in it, and compare it as text (`$col["series"] ""`), never as a number, so any
# other text is a label as written.
requireLabel() {
	local LC_ALL=C
	[[ -n "$1" && "$1" != *[[:cntrl:]]* ]] || {
		echo "$TOOL: a label is a cell of the results file: not empty, and no tab, newline or other control character" >&2
		return 2
	}
}

# repSlow <results> <label> <wall>: true when <wall> is bad (bad(), above) against the median of the label's
# rows so far, not counting void and skipped reps; false when fewer than two rows are left. It is bench.sh's
# in-run test of a rep, as seriesScore counts it.
repSlow() {
	[ -s "$1" ] && BENCH_LABEL=$2 awk -F'\t' -v w="$3" "$BENCH_AWK"'
		NR == 1 { columns(); next }
		$col["series"] "" == ENVIRON["BENCH_LABEL"] "" {
			c = rowClass($col["outcome"], $col["wall_s"])
			if (c == "ok") key[++k] = $col["wall_s"] + 0
			else if (c == "fail") key[++k] = FAILED
		}
		END { if (k < 2) exit 1; order(key, k, idx); exit !bad(w + 0, median(key, k, idx)) }' "$1"
}

# freshPrefix <results>: prints the uid prefix the next block takes, the first from c000 up that no row of the
# file used. The rep is a fifth hex digit after it, since makerom stamps at most 0xFFFFF. It is one awk, never
# a pipe into an early-exiting grep, because under pipefail a long file's SIGPIPE reads as "not found".
freshPrefix() {
	local results=$1
	[ -s "$results" ] || results=/dev/null
	awk -F'\t' "$BENCH_AWK"'NR == 1 { columns(); next }
		col["uid"] { used[substr($col["uid"], 1, 4)] = 1 }
		END { n = 49152; while (sprintf("%04x", n) in used) n++; printf "%04x\n", n }' "$results"
}
# labelInUse <label> <results>: true when a row of the file has the label, compared as text (requireLabel).
labelInUse() {
	[ -s "$2" ] && BENCH_LABEL=$1 awk -F'\t' "$BENCH_AWK"'NR == 1 { columns(); next }
		col["series"] && $col["series"] "" == ENVIRON["BENCH_LABEL"] "" { found = 1; exit }
		END { exit !found }' "$2"
}

# requireUnits <n>: refuses a size given in 1048576-byte units, with a message and status 2, unless it is a
# decimal of at most 9 digits, which $((10#$n * WIRE_UNIT)) reads whole (a leading 0 is not octal there) and
# cannot wrap.
requireUnits() {
	{ isUint "$1" && [ "${#1}" -le 9 ]; } || {
		echo "$TOOL: a size counts 1048576-byte units, as a decimal of at most 9 digits; got '$1'" >&2
		return 2
	}
}

# benchConsole [ip]: sets ip and port from the address as the console's screen shows it, or, with none given,
# as discover finds it.
benchConsole() {
	wireConsoleAt "${1:-}"
	ip=$wireIP port=$wirePort
}

# benchBuild <ip> <port> <said>: for use after a bench request failed with <said>, its stderr. It stops with a
# message (status 1) when the console is not a bench build. A release build answers a bare `bench` with
# `unsupported`, and any sub of it with a grammar rejection that wire.sh blames on an older 3Drop, which is
# not the cause, so that case is asked the bare `bench`. Any other failure is the caller's own to report.
benchBuild() {
	local said=$3
	case "$said" in
	*" error rejected grammar "*) said=$(wireRead "$1" "$2" bench 2>&1) || : ;;
	esac
	case "$said" in
	*" error rejected unsupported "*) die "not a bench build: THREEDROP_TEST_VERBS=1 tools/deploy.sh $1" ;;
	esac
}

# benchOk <reply> <text>: whether the reply is a bench build's ok one, `<id> bench ok ok 00000000 ` followed
# by text, which is a pattern (a state's `capture=[01]` fits). The frame is typed once, here.
benchOk() {
	# shellcheck disable=SC2053 # $2 is a pattern
	[[ "$1" == *" bench ok ok 00000000 "$2* ]]
}

# benchKnob <ip> <port> <knob> [value]: prints the reply once the console says what the knob applied;
# otherwise it prints a message and returns the exchange's status, or 1 for a refusal. A set spends a nonce,
# as every change does, so a captured set is refused; a read spends none and can be repeated.
benchKnob() {
	local reply st=0
	if [ -n "${4:-}" ]; then
		reply=$(wireMutation "$1" "$2" "$(wireTimeout)" bench "$3 ${*:4}" 2>&1) || st=$?
	else
		reply=$(wireRead "$1" "$2" bench "$3" 2>&1) || st=$?
	fi
	benchOk "$reply" "$3 applied=" || {
		echo "$TOOL: knob $3${4:+=${*:4}} did not take (${reply:-no answer})" >&2
		return $((st ? st : 1))
	}
	echo "$reply"
}

# benchRx <ip> <port> <units> [body|-]: sends zeros, or a body file (`-` means stdin) to plant a short or
# paused body. Its status is the exchange's, since a sender that a refusal cut short is not a failure.
benchRx() {
	local request bytes units
	requireUnits "$3" || return
	units=$((10#$3))
	bytes=$((units * WIRE_UNIT))
	request="bench $(newRequestID) rx $units" || return
	if [ -z "${4:-}" ]; then
		head -c "$bytes" /dev/zero
	else
		cat -- "$4"
	fi | withDeadline "${BENCH_DEADLINE:-$((WIRE_SILENCE + bytes / 250000))}" wireExchange "$1" "$2" \
		"$request" - "$WIRE_SILENCE"
	return "${PIPESTATUS[1]}"
}

# benchTx <ip> <port> <units>: reads the way a get does.
benchTx() {
	local units
	requireUnits "$3" || return
	units=$((10#$3))
	withDeadline "${BENCH_DEADLINE:-$((WIRE_SILENCE + units * WIRE_UNIT / 250000))}" _wireWork _benchTx \
		"$1" "$2" "$units"
}
# Prints the trailer, whatever it says (the trailer of a stopped tx too). A count that does not match returns
# 4, with no trailer printed.
_benchTx() {
	local request header trailer bytes
	request="bench $(newRequestID) tx $4" || return
	header=$(_wireStream "$2" "$3" "$request" "$1") || return
	trailer=$(_wireStreamClose "$1" "$request") || return
	bytes=$(fileSize "$1/body") || return
	wireFields "$trailer"
	if [ "$wireOutcome" = ok ] &&
		{ [ "$(kv "$header" bytes)" != "$bytes" ] || [ "$(kv "$trailer" bytes)" != "$bytes" ]; }; then
		echo "$TOOL: tx brought $bytes bytes; its header said $(kv "$header" bytes), its trailer $(kv "$trailer" bytes)" >&2
		return 4
	fi
	echo "$trailer"
	[ "$wireOutcome" = ok ]
}

# _benchListing <work> <ip> <port> <verb>: prints the header's fields and leaves the rows in <work>/body. A
# row count that the header does not match is refused.
_benchListing() {
	local request header
	request="bench $(newRequestID) $4" || return
	_wireListing "$1" "$2" "$3" "$request" || return
	header=$(wireNonceless "$(cat "$1/header")")
	if [ "$(kv "$header" rows)" != $(($(wc -l <"$1/body"))) ]; then
		echo "$TOOL: $4 said rows=$(kv "$header" rows) and sent $(($(wc -l <"$1/body")))" >&2
		return 1
	fi
	echo "${header#*" $4 "}" # its fields, past the frame and the sub's name
}

# benchMb <ip> <port>: prints one line, the TSV's mb cell.
benchMb() { _wireWork _benchMb "$@"; }
_benchMb() {
	local fields
	fields=$(_benchListing "$1" "$2" "$3" mb) || return
	printf '%s' "$fields"
	# shellcheck disable=SC2046 # one word per row
	set -- $(tr ' ' ',' <"$1/body")
	[ $# = 0 ] || printf ' %s' "$@"
	echo
}

# benchHw <ip> <port>: prints the timeline of the last timed request, its header's fields on the first line,
# then each call or gap as the console sent it.
benchHw() { _wireWork _benchHw "$@"; }
_benchHw() {
	_benchListing "$1" "$2" "$3" hw || return
	cat "$1/body"
}

# benchFile <ip> <port> <args>: never retries, because it writes the card.
benchFile() {
	wireExchange "$1" "$2" "bench $(newRequestID) file $3" "" "$(wireTimeout 600)"
}

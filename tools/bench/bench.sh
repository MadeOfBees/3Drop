#!/usr/bin/env bash
# bench.sh <label> [reps=8, at most 15] [size=500] [ip]: runs one arm of a series, then prints its
# score. With no ip, it uses the first address where `device.sh discover` finds this key's console.
# Each rep installs the Testing Tool with size x 1048576 bytes of RomFS (500 gives a 528 MB CIA).
#   bench.sh --prefix-only [results]      print the uid prefix the next arm would use
# BENCH_KNOBS (`rcvbuf=32768 sendsize=16384`) sets the arm's knobs; BENCH_DEADLINE bounds a rep.
# Each guard keeps a measurement honest: the console must be on its charger (a draining battery
# looks like a treatment), the host must be quiet (it is the sender), the console must run the build
# the arms ledger last records for it (otherwise unknown code is under test), knobs are read back
# (otherwise the arm tests nothing), each rep gets a fresh title id (a title already there is
# someone else's), and each rep's own title and ticket are removed after it (AM's first-write hold
# grows with the title count). A rep the device cannot account for is `void:<cause>`, which is
# counted and never dropped.
set -euo pipefail
# shellcheck source=tools/bench/lib.sh
. "$(dirname "$0")/lib.sh"

# An interrupt mid-rep ends its install before the rep lock is released, and names the title it may
# have left, which no row will name. The message goes to fd 3 (this stderr) because bash 5 runs the
# trap under the redirections of the call it interrupted.
# shellcheck disable=SC2317 # called by main's EXIT trap
benchStopped() {
	deadlineStopAll
	[ -z "${repTitle:-}" ] ||
		echo "bench: stopped in rep $rep, whose title $repTitle may be installed: tools/device.sh uninstall $ip:$port --ticket $repTitle removes it" >&3
}

# Prints `release` for a console without the bench verb, and `?` when no bench reply came, which
# skips a rep (skipped:knobs).
benchState() {
	local reply
	reply=$(wireRead "$ip" "$port" bench 2>&1) || :
	if benchOk "$reply" "state "; then
		echo "${reply#*" state "}"
	elif [[ "$reply" == *" error rejected unsupported "* ]]; then
		echo release
	else
		echo '?'
	fi
}
# cardFloor prints the card floor's reply, or `-` for none. A refused floor wrote nothing to the
# card, so it is not a floor and lengthens no first write.
cardFloor() {
	local reply
	[ "$flavor" = bench ] || {
		echo -
		return
	}
	reply=$(benchFile "$ip" "$port" 16 2>/dev/null) || reply=""
	if benchOk "$reply" "file "; then echo "${reply#*" bench "}"; else echo -; fi
}

# repSkip <title> <ver> <knobs> <self> prints why the rep must not install, as its outcome word, or
# nothing when it may. When it cannot tell whether its title is installed, it says so and returns 1.
# It checks first for unknown code or an unknown instrument, because such a rep, if installed, could
# only end void, a word that hides what its put did (rejected:exists too), which reclaim reads.
# Then, as before rep 1, it checks the charger and a quiet PC, because a charger pulled or
# tools/check.sh started mid-arm would affect the timing of every rep after it. Last, it checks its
# fresh title id, whose absence is only ever `notfound`; anything else leaves the rep without an
# owner.
repSkip() {
	local listing st=0
	if [ "$2" = '?' ]; then
		echo skipped:self
	elif [ "$3" = '?' ]; then
		echo skipped:knobs
	elif [ "$(kv "$4" ac)" != 1 ]; then
		echo skipped:ac
	elif ! macQuiet; then
		echo skipped:host
	else
		listing=$(wireTitleVersion "$ip" "$port" "$1" 2>&1) || st=$?
		case "$st:$listing" in
		0:) ;;
		0:-) echo skipped:am-info ;;
		0:*) echo skipped:exists ;;
		1:*" ls rejected "*) # a refusal the rep still counts, by its word
			listing=${listing#*" ls rejected "}
			echo "skipped:${listing%% *}"
			;;
		*)
			echo "bench: could not tell whether $1 is installed: $listing" >&2
			return 1
			;;
		esac
	fi
}

# repRow <rep> <name=value>... writes the rep's row. The block's last rep writes it with the block's
# last card floor (card_after), whichever way that rep ended; this is the one place the last floor
# is taken. A row that cannot be written stops the block.
repRow() {
	local cardAfter=-
	[ "$1" != "$reps" ] || cardAfter=$(cardFloor)
	benchRow "$BENCH_RESULTS" "${@:2}" time="$(date '+%F %T')" card_after="$cardAfter" || die "no row for rep $1"
}

# runRep <uid> <rep> returns 3 when the rep was skipped (repSkip, or a step before the install that
# failed), and 4 when its own title stayed installed. cardBefore is the card floor its row starts
# from. A rep that installed leaves the next one its own floor, taken after it was bad, or none; a
# skipped rep leaves it as it was.
runRep() {
	local uid=$1 n=$2 title bytes=- instNow self="" ver=- knobs=- capture=- deadline elapsed clock="" wall rate
	local timed outcome="" put sent receipt putId ownId stats mb=- air=- reply="" left="" bad=0 ring cells line
	local last
	# A row names the tools the block began with (inst). If they were edited since, the block stops
	# before this rep, as the ring check does, so no row names code that did not run.
	instNow=$(inst)
	[ "$instNow" = "$blockInst" ] ||
		die "tools/ changed since the block began (inst $blockInst, now $instNow): the block stops before rep $n"
	title=$(uidTitleID "$uid")
	if [ ! -f "$fixture" ] || [ "$(ciaTitleID "$fixture")" != "$title" ]; then
		"$BENCH/makeFixture.sh" "$size" "$uid" >&2 || outcome=skipped:fixture
	fi
	# A step before the install that fails (its fixture, the rep lock, the console's word on its
	# title) skips the rep, as the step said. The rep installed nothing, and its row still holds
	# what a row holds.
	if [ -n "$outcome" ] || ! bytes=$(fileSize "$fixture"); then
		outcome=skipped:fixture
	elif ! repLock "$ip"; then
		outcome=skipped:lock
	else
		self=$(wireRead "$ip" "$port" stats self 2>/dev/null) || self=""
		knobs=$(benchState)
		capture=$(kv "$knobs" capture)
		[ "$flavor" = bench ] || capture=0
		ver=$(kv "$self" ver)
		[ "$ver" != - ] || ver='?'
		outcome=$(repSkip "$title" "$ver" "$knobs" "$self") || outcome=skipped:listing
	fi
	# The cells every row of this rep holds, each by its column's name (benchRow places them).
	cells=(series="$label" rep="$n" uid="$uid" title="$title" size_bytes="$bytes" ver="$ver" inst="$instNow"
		knobs="$knobs" capture="$capture" am_titles="$(kv "$self" am_titles)" am_tickets="$(kv "$self" am_tickets)"
		free_bytes="$(kv "$self" free)" card_before="$cardBefore")
	if [ -n "$outcome" ]; then
		repUnlock
		repRow "$n" "${cells[@]}" outcome="$outcome" wall_s=- rate_mbs=- air=- stats=- mb=-
		case "$outcome" in
		skipped:exists | skipped:am-info) echo "bench: title $title is already there (${outcome#skipped:}): it is someone else's" >&2 ;;
		skipped:self) echo "bench: rep $n skipped: its stats self named no build (no answer, or no ver=): unknown code under test" >&2 ;;
		skipped:knobs) echo "bench: rep $n skipped: no answer to a bare bench: the knobs it would run under are unknown" >&2 ;;
		skipped:ac) echo "bench: rep $n skipped: the console is on battery (ac=$(kv "$self" ac)): plug in its charger" >&2 ;;
		skipped:host) echo "bench: rep $n skipped: this PC is not quiet" >&2 ;;
		skipped:fixture | skipped:lock | skipped:listing) ;; # said above, by the step that failed
		*) echo "bench: rep $n skipped: the console refused to say whether $title is installed (${outcome#skipped:})" >&2 ;;
		esac
		return 3
	fi

	deadline="${BENCH_DEADLINE:-$((120 + bytes / 250000))}"
	repTitle=$title
	: >"$SCRATCH/wall"
	rm -f "$SCRATCH/putid"
	# With --no-seed, the wall is the install alone, never a fetch from Nintendo's server.
	# BENCH_TIMER reads the clock around drop.sh itself, so the deadline's own setup and teardown
	# are not in it. THREEDROP_PUT_ID_FILE gets drop.sh's put ids, a line per put sent (two for a
	# ghost retry), so the rep's receipt and stats can be tied to the put it actually sent. Nothing
	# is printed or asked for it.
	THREEDROP_PUT_ID_FILE="$SCRATCH/putid" withDeadline "$deadline" perl -e "$BENCH_TIMER" "$SCRATCH/wall" \
		"$ROOT/tools/drop.sh" --new --no-seed "$fixture" "$ip" "$port" \
		>"$SCRATCH/out" 2>"$SCRATCH/err" || :
	read -r elapsed clock <"$SCRATCH/wall" || elapsed=""
	# The seconds are used as read, for the rate; only the displayed wall is rounded. A missing
	# value, or one not past 0, gives `-`, because no clock gave this rep a time. The verdict gets
	# the class rowClass gives the wall as shown (timed), so a wall that rounds to 0.00 is not a
	# time either.
	read -r wall rate timed <<<"$(awk -v e="$elapsed" -v n="$bytes" "$BENCH_AWK"'BEGIN {
		if (e !~ /^[0-9]+(\.[0-9]+)?$/ || e + 0 <= 0) { print "-", "-", "void"; exit }
		w = sprintf("%.2f", e)
		print w, mbps(n, e * 1000), rowClass("committed:", w) }')"
	# A committed put's line is on drop.sh's stdout, a refused one on its stderr.
	put=$(awk '{ sub(/^drop: /, "") } $1 == "put" && $2 ~ /^(committed|rejected|unknown)$/ { o = $2 ":" $3 } END { print o }' "$SCRATCH/out" "$SCRATCH/err")
	receipt=$(wireRead "$ip" "$port" stats op 2>/dev/null) || receipt=""
	putId=${receipt#*" op "}
	putId=${putId%% *}
	# Reads drop.sh's put ids, a line each (none when no put was sent), to get how many there are
	# and the last.
	sent=0 ownId=""
	if [ -f "$SCRATCH/putid" ]; then
		while IFS= read -r line; do sent=$((sent + 1)) ownId=$line; done <"$SCRATCH/putid"
	fi
	stats=$(wireRead "$ip" "$port" stats 2>/dev/null) || stats=""
	stats=${stats#*" stats ok ok 00000000 "}
	if [ "$flavor" = bench ]; then
		mb=$(benchMb "$ip" "$port" 2>/dev/null) || mb=-
		# The curve of another request (one that reached the console after the stats) is not this
		# rep's. It is dropped, and the drop is reported; the rep's stats are still its own.
		if [ "$mb" != - ] && [ "$(kv "$mb" req)" != "${ownId:-$putId}" ]; then
			echo "bench: rep $n: its mb curve is of request $(kv "$mb" req), not its own: not kept" >&2
			mb=-
		fi
		reply=$(benchRx "$ip" "$port" 16 2>/dev/null) || :
		[ -z "$reply" ] || air=${reply#*" bench "}
	fi
	# The rep's own title is uninstalled, whatever drop.sh said, because the exists check showed
	# nothing at its fresh id. A refusal that proves this rep installed nothing (the console said
	# the title is already there) must not delete another client's title or its ticket.
	if [ "$put" != rejected:exists ] &&
		! reply=$(wireUninstall "$ip" "$port" "$title" ticket 2>&1); then
		case "$reply" in *" delete rejected notfound "*) ;; *) left=$reply ;; esac
	fi
	repUnlock

	outcome=$(repVerdict put="$put" fired="$DEADLINE_FIRED" sent="$sent" own="$ownId" receipt="$putId" \
		stats="$stats" bytes="$bytes" timed="$timed")
	# A rep that did not complete moved no fixture's worth of bytes in its wall, so it has no rate.
	case "$outcome" in committed:* | void:*) ;; *) rate=- ;; esac
	# Shows what drop.sh said, which no row keeps: the last line for a put with no outcome, and
	# every line for a ghost retry (which install the wall holds).
	case "$outcome" in
	noreply | void:ghost)
		last=""
		while IFS= read -r line || [ -n "$line" ]; do
			[ "$outcome" = noreply ] || echo "bench: rep $n ($outcome): $line" >&2
			last=$line line=""
		done <"$SCRATCH/err"
		[ "$outcome" != noreply ] || [ -z "$last" ] || echo "bench: rep $n ($outcome): $last" >&2
		;;
	esac
	# A rep is bad if its outcome is anything but committed (a void too, which the score counts
	# separately but which may still have disturbed the card), or if its wall is past 1.25 times the
	# median so far (repSlow: the score's bad()).
	[[ "$outcome" == committed:* ]] || bad=1
	if [ "$bad" = 0 ] && repSlow "$BENCH_RESULTS" "$label" "$wall"; then
		bad=1
	fi
	# The card floor lengthens the next first write, so after a bad rep it goes on the next rep's
	# row (card_before) and that row says what ran before it. After the last rep, repRow takes the
	# block's last.
	cardBefore=-
	if [ "$bad" = 1 ] && [ "$n" != "$reps" ]; then
		cardBefore=$(cardFloor)
	fi
	repRow "$n" "${cells[@]}" outcome="$outcome" wall_s="$wall" rate_mbs="$rate" air="$air" stats="$stats" mb="$mb"
	repTitle=""
	# If the wall clock timed it (no CLOCK_MONOTONIC), say so on its line, because no column does.
	case "$clock" in wall) clock="  clock wall" ;; *) clock="" ;; esac
	printf '%s r%s  %s  wall %ss  %s MB/s  first_write_ms %s  stalls %s (%s ms)%s\n' "$label" "$n" \
		"$outcome" "$wall" "$rate" "$(kv "$stats" first_write_ms)" "$(kv "$stats" stall_n)" \
		"$(kv "$stats" stall_ms)" "$clock"
	# Tier 2 can fail to allocate without failing the transfer, so every rep's ring must be the
	# shipped one. A transfer that fits tier 1 gets no tier 2. Each tier is read once a block
	# (ringTier1 and ringTier2), at the first rep that needs it.
	if [ "$(kv "$stats" ring_cap)" != - ]; then
		[ -n "${ringTier1:-}" ] || ringTier1=$(cDefine ring.h RING_TIER1_BYTES)
		ring=$ringTier1
		if [ "$bytes" -gt "$ring" ]; then
			[ -n "${ringTier2:-}" ] || ringTier2=$(($(cDefine ring.c DRAIN_RING2_UNITS) * $(cDefine ring.h RING_UNIT)))
			ring=$((ring + ringTier2))
		fi
		[ "$(kv "$stats" ring_cap)" = "$ring" ] ||
			die "rep $n's ring_cap=$(kv "$stats" ring_cap), not the shipped $ring: this is not the ring under test; the series stops"
	fi
	if [ -n "$left" ]; then
		echo "bench: rep $n's title $title was not uninstalled: $left" >&2
		return 4
	fi
}

# main <args> runs the block. bash reads a running script by byte offset, so an edit of it mid-block
# would derail it. But bash parses a whole function, and a whole line, before running either, so
# with the block in this function and the line that calls it ending the script, no edit of bench.sh
# is read. main declares no local, because runRep and the trap read the block's settings as globals.
main() {
	case "${1:-}" in
	--prefix-only)
		freshPrefix "${2:-$BENCH_RESULTS}"
		return 0
		;;
	-* | "") usage ;; # any other flag, or no label, since a label never starts with a dash
	esac
	scratch
	exec 3>&2
	trap 'benchStopped; repUnlock; _scratchEnd' EXIT
	label=$1
	requireLabel "$label" || exit
	reps="${2:-8}"
	size="${3:-500}"
	{ isPositive "$reps" && [ "${#reps}" -le 9 ]; } || dieWith 2 "reps must be a positive integer, got '$reps'"
	reps=$((10#$reps)) # a leading 0 isn't octal, so the banner, loop and last-rep test see the same reps
	[ "$reps" -le 15 ] || dieWith 2 "at most 15 reps per arm (a rep's unique id is one hex digit after the prefix)"
	requireUnits "$size" || exit
	size=$((10#$size))
	# An invalid BENCH_DEADLINE is refused here, not inside rep 1's timed call, where withDeadline
	# would refuse it unseen.
	[ -z "${BENCH_DEADLINE:-}" ] || { isPositive "$BENCH_DEADLINE" && [ "${#BENCH_DEADLINE}" -le 9 ]; } ||
		dieWith 2 "BENCH_DEADLINE is whole seconds (at most 9 digits), not '$BENCH_DEADLINE'"
	# Each rep needs them; without perl no rep has a wall, and every one would read noreply.
	for tool in perl openssl; do
		command -v "$tool" >/dev/null || dieWith 2 "bench needs $tool, which this PC does not have"
	done
	# A reused label would pool two series into one score.
	! labelInUse "$label" "$BENCH_RESULTS" || die "label '$label' already has rows in $BENCH_RESULTS"
	benchLayout "$BENCH_RESULTS" || exit 1
	# The PC is the sender, and a busy one is refused before any work of the block's own.
	macQuiet || exit 1
	# Each rep builds its fixture, the Testing Tool at its own title, unless the cached one is
	# already that title, as only rep 1's can be. makeFixture.sh's tools are found here the way it
	# finds them, before the console is touched.
	prefix=$(freshPrefix "$BENCH_RESULTS")
	fixture="$FIXTURES/fixture${size}m.cia"
	if [ "$reps" -gt 1 ] || [ ! -f "$fixture" ] || [ "$(ciaTitleID "$fixture")" != "$(uidTitleID "${prefix}1")" ]; then
		resolveCIATools ||
			dieWith 2 "a block that builds its fixtures needs makerom and bannertool (on PATH, in tools/bin, or named by MAKEROM and BANNERTOOL)"
		[ "$size" = 0 ] || [ -x "$ciaThreeDsTool" ] || [ -f "$(romfsKept "$size")" ] ||
			dieWith 2 "a block that builds its fixtures needs 3dstool for their RomFS (on PATH, in tools/bin, or named by THREEDSTOOL), not $ciaThreeDsTool"
	fi
	# This is said before rep 1 and before the console is asked anything; BENCH_TIMER (lib.sh) reads
	# the walls.
	[ "$(benchClock)" = monotonic ] ||
		echo "bench: this perl has no CLOCK_MONOTONIC: walls come from the wall clock, and a rep it stepped through (a wall not past 0) is void:clock" >&2
	# The tools every row will name are recorded before the console is touched (runRep holds each
	# rep to it).
	blockInst=$(inst)
	benchConsole "${4:-}"

	self=$(wireRead "$ip" "$port" stats self) || dieWith $? "no answer from 3Drop at $ip"
	[ "$(kv "$self" ac)" = 1 ] ||
		die "the console is on battery (ac=$(kv "$self" ac)): plug in its charger; a draining battery looks like a treatment effect"
	ver=$(kv "$self" ver)
	# The build must be the last one deployed here, not any build, because every plain makeCIA.sh
	# build is v0, as an old deploy's line may be. The comparison is as text, so a console the
	# ledger has no line for ("") is never version 0.
	[ "$(armsLast "$ip")" = "$ver" ] ||
		die "build $ver on $ip is not the arms ledger's last for it ($(armsLedger)): unknown code under test (deploy it with tools/deploy.sh)"

	echo "bench: $label reps=$reps size=$size ($((size * WIRE_UNIT)) bytes of RomFS) uid prefix $prefix results $BENCH_RESULTS"
	case "$(benchState)" in
	release) flavor=release ;;
	'?') dieWith 4 "no answer to a bare bench from $ip" ;;
	*) flavor=bench ;;
	esac
	# A relaunch resets every knob to its default. The card floor lengthens the next first write, so it
	# runs before the relaunch, not before rep 1.
	cardBefore=$(cardFloor)
	wireRun "$ip" "$port" self >/dev/null || dieWith $? "run self did not go"
	wireWaitUp "$ip" "$port" "$ver" >/dev/null || dieWith $? "3Drop did not come back after run self"
	for knob in ${BENCH_KNOBS:-}; do
		[ "$flavor" = bench ] || die "BENCH_KNOBS needs a bench build"
		reply=$(benchKnob "$ip" "$port" "${knob%%=*}" "${knob#*=}") || dieWith $? "the arm is void before rep 1"
		[ "$(kv "$(benchState)" "${knob%%=*}")" = "$(kv "$reply" applied)" ] ||
			die "knob $knob reads back otherwise in bench's state: the arm is void before rep 1"
	done

	for rep in $(seq 1 "$reps"); do
		uid="$prefix$(printf '%x' "$rep")"
		rc=0
		runRep "$uid" "$rep" || rc=$?
		[ "$rc" != 4 ] || die "rep $rep's title stayed installed; the arm stops (reclaimTestTitles.sh $BENCH_RESULTS $ip --delete removes it)"
		[ "$rc" = 0 ] || echo "bench: rep $rep ended early (exit $rc)" >&2
	done
	# The last rep's uninstall is in none of the rows' AM counts (each row's counts are its rep's
	# from before the install), so the score also gets the ticket gap after it, from one control
	# read outside every wall.
	after=$(wireRead "$ip" "$port" stats self 2>/dev/null) || after=""
	afterGap=-
	if isUint "$(kv "$after" am_titles)" && isUint "$(kv "$after" am_tickets)"; then
		afterGap=$(($(kv "$after" am_tickets) - $(kv "$after" am_titles)))
	fi
	"$BENCH/seriesScore.sh" "$label" "$BENCH_RESULTS" "$reps" "$afterGap"
}
# This is one group, parsed whole before main runs, so an edit to this file during a block never
# runs.
{
	main "$@"
	exit
}

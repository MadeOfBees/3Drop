#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# The bench kit against the fake device: an arm refused before anything is sent, a rep that
# uninstalls only its own title, void reps, the rep lock, probe.sh, and deadlines on a mute console.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
# shellcheck source=tools/bench/lib.sh
. "$ROOT/tools/bench/lib.sh"
export BENCH_RESULTS="$TMP/results.tsv" THREEDROP_ARMS="$TMP/arms.tsv"
printf '127.0.0.1\t52\t-\t-\tbench\t-\n' >"$THREEDROP_ARMS" # the fake answers as build 52
# This PC is quiet as far as bench.sh can see (the gate that runs this test is a check.sh). ps leaves that
# gate out and keeps the process rows the deadlines need. For what counts as a gate, see macQuiet
# (bench/lib.sh).
printf '#!/bin/sh\n' >"$FAKE/bin/docker"
ps=$(type -P ps)
# shellcheck disable=SC2016 # the stub expands it
printf '#!/bin/sh\necho "UID PID PPID C STIME TTY TIME CMD"\ncat "$FAKE/ps-rows"\n' >"$FAKE/bin/ps"
chmod +x "$FAKE/bin/docker" "$FAKE/bin/ps"
for gate in "/bin/bash -e ./tools/check.sh" "/home/user/3Drop/tools/check.sh --fast" \
	"/bin/bash -o pipefail ./tools/check.sh" "/bin/bash --noprofile --rcfile /dev/null ./tools/check.sh" \
	"bash +o pipefail tools/check.sh" "bash --init-file /dev/null tools/check.sh" "bash -euo pipefail tools/check.sh" \
	"bash -O extglob tools/check.sh" "bash -oe pipefail tools/check.sh" "sh -- ./tools/check.sh"; do
	echo "user 4242 1 0 10:28 pts/0 00:00:01 $gate" >"$FAKE/ps-rows"
	run 1 macQuiet
	saidLine "$TOOL: a gate (tools/check.sh) is running: a series needs this PC quiet" "a running gate ($gate)"
done
printf 'user 4243 1 0 10:28 pts/1 00:00:00 %s\n' "vim tools/check.sh" "bash -c echo tools/check.sh" \
	"grep -n tools/check.sh notes" "bash -o pipefail notes tools/check.sh" \
	"bash --rcfile tools/check.sh notes" "bash -- -x tools/check.sh" >"$FAKE/ps-rows"
run 0 macQuiet
# shellcheck disable=SC2016 # the shim's own arguments
printf '#!/usr/bin/env bash\n%q "$@" | awk '\''!/[t]ools\\/check\\.sh/'\''\n' "$ps" >"$FAKE/bin/ps"
export THREEDROP_FIXTURE_DIR=$TMP/fixtures
fixture=$THREEDROP_FIXTURE_DIR/fixture3m.cia
mkdir -p "$THREEDROP_FIXTURE_DIR"
# arm <label> [fake mode]: runs a one-rep arm of the size-3 fixture on a fresh results file, whose first rep
# is uid c0001 (ARM_REPS and ARM_SIZE set the reps and the size otherwise). Its status is left in st. Its
# files are named by the label with each backslash made an underscore, because Windows takes a backslash for a
# folder.
arm() {
	fakeCia "$fixture" 000400000c000100 1 3145728
	: >"$FAKE/log"
	BENCH_RESULTS="$TMP/${1//\\/_}.tsv"
	st=0
	faulted "${2:-valid}" "$ROOT/tools/bench/bench.sh" "$1" "${ARM_REPS:-1}" "${ARM_SIZE:-3}" 127.0.0.1 \
		>"$TMP/${1//\\/_}.out" 2>&1 || st=$?
}
column() { # <label> <name>: that column of the arm's one row
	awk -F'\t' -v c="$2" "$BENCH_AWK"'NR == 1 { columns(); next } { print $col[c] }' "$TMP/${1//\\/_}.tsv"
}
ownDelete() { grep '^delete ' "$FAKE/log" | cut -d' ' -f4-; }

# Refused before anything is sent.
echo 0 >"$FAKE/ac"
arm battery
rm "$FAKE/ac"
expect "$st:$(count put):$(count delete)" "1:0:0" "an arm on battery"
grep -q 'on battery' "$TMP/battery.out" || fail "the battery refusal did not say why: $(cat "$TMP/battery.out")"
printf '127.0.0.1\t51\t-\t-\tbench\t-\n10.0.0.9\t52\t-\t-\tbench\t-\n' >"$TMP/other.tsv"
THREEDROP_ARMS="$TMP/other.tsv" arm unknown # build 51 here, and 52 on another console
expect "$st:$(count put)" "1:0" "an arm on a build the ledger does not name"
printf '127.0.0.1\t52\t-\t-\tbench\t-\n127.0.0.1\t53\t-\t-\tbench\t-\n' >"$TMP/later.tsv"
THREEDROP_ARMS="$TMP/later.tsv" arm superseded # 52 deployed here once, 53 since
expect "$st:$(count put)" "1:0" "an arm on a build the ledger's last line here does not name"
# A console the ledger has no line for is unknown code even at version 0, which an unset last compared
# equal to.
echo "$FAKE_SELF 0" >"$FAKE/installed"
printf '10.0.0.9\t0\t-\t-\tbench\t-\n' >"$TMP/elsewhere.tsv"
THREEDROP_ARMS="$TMP/elsewhere.tsv" arm noline # build 0 deployed elsewhere, none here
: >"$FAKE/installed"
expect "$st:$(count put)" "1:0" "an arm at version 0 on a console the ledger has no line for"
grep -q "build 0 on 127.0.0.1 is not the arms ledger's last" "$TMP/noline.out" || fail "the no-line refusal: $(cat "$TMP/noline.out")"
# No label, or a flag (a label never starts with a dash, --help after -- included), is a call it cannot take.
# It prints the usage on stderr, exits 2, and starts nothing. A deadline that isn't a whole number of seconds
# is refused before the console is asked anything, not inside rep 1's timed call.
: >"$FAKE/log"
run 2 "$ROOT/tools/bench/bench.sh"
said "bench.sh <label> [reps=8, at most 15]" "bench.sh with no label"
run 2 "$ROOT/tools/bench/bench.sh" --helpme
said "bench.sh <label> [reps=8, at most 15]" "bench.sh with a flag it does not know"
run 2 "$ROOT/tools/bench/bench.sh" -- --help
BENCH_DEADLINE=08x run 2 "$ROOT/tools/bench/bench.sh" deadlined 1 3 127.0.0.1
said "BENCH_DEADLINE is whole seconds (at most 9 digits), not '08x'" "a deadline that is no number"
expect "$(awk 'END { print NR }' "$FAKE/log")" 0 "what bench.sh sent for no label, a bad flag or a bad deadline"
# A block that builds its fixtures (more than one rep, or one whose cached fixture is another title)
# finds makeFixture.sh's tools before the console is asked anything: with no makerom, or no 3dstool
# for a RomFS, it is refused, and nothing is sent.
anyTool=$(type -P true) # stands in for a tool bench.sh only finds
BENCH_RESULTS=$TMP/nobuilder.tsv MAKEROM=$TMP/no-makerom BANNERTOOL=$anyTool THREEDSTOOL=$anyTool \
	run 2 "$ROOT/tools/bench/bench.sh" nobuilder 2 3 127.0.0.1
said "not found or not executable: $TMP/no-makerom" "a block with no makerom"
said "a block that builds its fixtures needs makerom and bannertool" "a block with no makerom"
BENCH_RESULTS=$TMP/nobuilder.tsv MAKEROM=$anyTool BANNERTOOL=$anyTool THREEDSTOOL=$TMP/no-3dstool \
	run 2 "$ROOT/tools/bench/bench.sh" nobuilder 2 3 127.0.0.1
said "needs 3dstool for their RomFS (on PATH, in tools/bin, or named by THREEDSTOOL), not $TMP/no-3dstool" "a block with no 3dstool"
expect "$(awk 'END { print NR }' "$FAKE/log")" 0 "what bench.sh sent with no fixture builder"
# 3dstool makes a size's RomFS once, and a block of that size needs no 3dstool while the RomFS is kept. With
# the file that makeFixture.sh keeps it in (named by the size and the rsf's hash) present, a block with no
# 3dstool gets past that check, as far as the arms ledger's refusal here (no build 52 is its last at this
# address).
keptRomFS=$THREEDROP_FIXTURE_DIR/content1-3m-$(openssl dgst -sha256 <"$ROOT/res/fixtureCFA.rsf" | awk '{ print substr($NF, 1, 12) }').ncch # romfsKept's name for it (bench/lib.sh)
: >"$keptRomFS"
BENCH_RESULTS=$TMP/keptromfs.tsv THREEDROP_ARMS=$TMP/other.tsv MAKEROM=$anyTool BANNERTOOL=$anyTool THREEDSTOOL=$TMP/no-3dstool \
	run 1 "$ROOT/tools/bench/bench.sh" keptromfs 2 3 127.0.0.1
said "build 52 on 127.0.0.1 is not the arms ledger's last" "a block with no 3dstool, whose RomFS is kept"
rm -f "$keptRomFS"
printf '#!/bin/sh\necho 0123abcd\n' >"$FAKE/bin/docker"
arm busy
printf '#!/bin/sh\n' >"$FAKE/bin/docker"
expect "$st:$(count put)" "1:0" "an arm with docker running"
# Refused before any work of its own: no hash of the tools (openssl) and no check of the cached fixture (od).
# Both programs log their calls through a shim of the same name.
for shim in openssl od; do
	# shellcheck disable=SC2016 # the shim expands them when it runs
	printf '#!/bin/sh\necho %s >>"$FAKE/shim.log"\nexec %s "$@"\n' "$shim" "$(type -P "$shim")" >"$FAKE/bin/$shim"
	chmod +x "$FAKE/bin/$shim"
done
: >"$FAKE/shim.log"
printf '#!/bin/sh\necho 0123abcd\n' >"$FAKE/bin/docker"
st=0
BENCH_RESULTS="$TMP/busyshim.tsv" faulted valid "$ROOT/tools/bench/bench.sh" busyshim 1 3 127.0.0.1 >"$TMP/busyshim.out" 2>&1 || st=$?
printf '#!/bin/sh\n' >"$FAKE/bin/docker"
rm "$FAKE/bin/openssl" "$FAKE/bin/od"
expect "$st:$(cat "$FAKE/shim.log")" "1:" "an arm with docker running, and the tools it started before it refused"
grep -q 'docker has running containers' "$TMP/busyshim.out" || fail "the arm on a busy PC did not say why: $(cat "$TMP/busyshim.out")"
# Each rep checks both again: if a charger is pulled, or a container started, after the arm's checks, the rep
# is skipped with no install.
printf '1\n0\n' >"$FAKE/ac" # the arm's stats self, then rep 1's
arm unplugged
rm "$FAKE/ac"
expect "$(count put):$(column unplugged outcome)" "0:skipped:ac" "a rep after the charger was pulled"
grep -q 'rep 1 skipped: the console is on battery' "$TMP/unplugged.out" || fail "the rep off its charger did not say why: $(cat "$TMP/unplugged.out")"
# shellcheck disable=SC2016 # the stub expands it: quiet for the arm's check, a container for rep 1's
printf '#!/bin/sh\n[ ! -e "$FAKE/docker-seen" ] || echo 0123abcd\n: >"$FAKE/docker-seen"\n' >"$FAKE/bin/docker"
arm loaded
printf '#!/bin/sh\n' >"$FAKE/bin/docker"
expect "$(count put):$(column loaded outcome)" "0:skipped:host" "a rep after a container started"
grep -q 'docker has running containers' "$TMP/loaded.out" || fail "the rep on a busy Mac did not say why: $(cat "$TMP/loaded.out")"
BENCH_KNOBS=rcvbuf=70000 arm range
expect "$st:$(count put)" "1:0" "an arm whose knob is out of range"
grep -q 'did not take.* bench rejected range 00000000 rcvbuf ' "$TMP/range.out" ||
	fail "a knob out of range was not said by its word: $(cat "$TMP/range.out")"

# The rep's own title only.
echo "000400000c000100 1" >"$FAKE/installed"
arm listfails bad-trailer # the title is there, and its listing does not authenticate
expect "$(count delete):$(count put)" "0:0" "a rep that could not tell whether its title was there"
# Skipped, with its row written all the same: it is the block's last rep, so it holds the block's last floor.
expect "$(column listfails outcome):$(grep -c '^bench [0-9a-f]* file 16$' "$FAKE/log")" "skipped:listing:2" \
	"a last rep that could not tell whether its title was there, and the block's floors"
[[ "$(column listfails card_after)" == "ok ok 00000000 file bytes=16777216 "* ]] ||
	fail "a last rep that could not tell kept no floor: $(column listfails card_after)"
arm exists
expect "$(count delete):$(count put):$(column exists outcome)" "0:0:skipped:exists" "a rep over an installed title"
printf '000400000c000100 -\n0004000001234500 5\n' >"$FAKE/installed" # AM has no info, so it is someone else's
arm noinfo
expect "$(count delete):$(count put):$(column noinfo outcome)" "0:0:skipped:am-info" "a rep over a no-info title"
: >"$FAKE/installed"
BENCH_KNOBS=rcvbuf=32768 MAKEROM=$TMP/no-makerom arm own # its cached fixture is rep 1's: no builder
expect "$st:$(ownDelete):$(column own outcome)" "0:title:000400000c000100 ticket:committed:ok" "a committed rep"
[ ! -s "$FAKE/installed" ] || fail "a committed rep left its title installed"
putId=$(awk '$1 == "put" { print $2 }' "$FAKE/log")
# Every column of a committed rep's row, read back by its name. A cell written under another column's name, or
# by position, fails here. AM's counts are stats self's counts from before the put, under its own keys.
for name in $BENCH_COLUMNS; do
	case "$name" in
	series) want=own ;;
	rep) want=1 ;;
	time) want='[0-9][0-9][0-9][0-9]-[01][0-9]-[0-3][0-9] [0-2][0-9]:[0-5][0-9]:[0-6][0-9]' ;;
	uid) want=c0001 ;;
	title) want=000400000c000100 ;;
	size_bytes) want=3145728 ;;
	wall_s) want='[0-9]*.[0-9][0-9]' ;;
	rate_mbs) want='[0-9]*.[0-9][0-9][0-9][0-9]' ;;
	outcome) want=committed:ok ;;
	ver) want=52 ;;
	inst) want=$(cat "$ROOT"/tools/*.sh "$ROOT"/tools/bench/*.sh | openssl dgst -sha256 | awk '{ print substr($NF, 1, 16) }') ;; # inst's rule (bench/lib.sh)
	knobs) want='rcvbuf=32768 sndbuf=65536 sendsize=65536 nohash=0 nodisc=0 capture=0 mb_rows=4096' ;;
	capture | am_titles) want=0 ;;
	am_tickets) want=40 ;;
	free_bytes) want=1000000 ;;
	air) want='ok ok 00000000 rx bytes=16777216 *' ;;
	card_before | card_after) want='ok ok 00000000 file bytes=16777216 *' ;; # the block's first floor, and its last
	stats) want="bytes=3145728 * req=$putId" ;;
	mb) want="req=$putId dir=rx rows=3 cols=wall_ms,wait_ms,eagain,sd_kb * 2100,30,1,3000" ;;
	*) fail "the column $name has no expectation here: every column is read back" ;;
	esac
	# shellcheck disable=SC2053 # want is a pattern
	[[ "$(column own "$name")" == $want ]] || fail "a committed rep's $name cell: '$(column own "$name")', want '$want'"
done
# The rep's line names its first write and stalls from the fake's stats (40 ms; none).
grep -q '^own r1  committed:ok  wall [0-9.]*s  [0-9.]* MB/s  first_write_ms 40  stalls 0 (0 ms)$' "$TMP/own.out" ||
	fail "a committed rep's line: $(grep '^own r1 ' "$TMP/own.out")"
# Its wall is the timed install's, and its rate is the fixture's bytes over that wall, in MB/s. The rate uses
# the seconds as read, so it matches only within the two decimals the wall shows.
awk -v w="$(column own wall_s)" -v r="$(column own rate_mbs)" 'BEGIN {
	m = 3145728 / 1e6
	exit !(w > 0 && r + 0 >= m / (w + 0.005) - 0.00006 && r + 0 <= m / (w - 0.005) + 0.00006) }' ||
	fail "the wall and rate columns: $(column own wall_s) s, $(column own rate_mbs) MB/s"
# A ticket that the last rep's uninstall kept: every row's AM counts are its pre-install ones, so only the
# control read after the last uninstall sees it, and a one-rep block is a block that checks.
arm kept keep-ticket
expect "$st:$(column kept outcome)" "0:committed:ok" "an arm whose uninstall kept a ticket"
grep -q 'ticket gap +1 over reps 1-1  TICKET-GAP: a rep left a ticket' "$TMP/kept.out" ||
	fail "a ticket kept by the last rep's uninstall was not named: $(cat "$TMP/kept.out")"
rm -f "$FAKE/tickets-kept" # the arms after it are not to see this one's leak
# A committed rep's row and a skipped rep's are as wide as the header, because a field one gained would be a
# column no reader can name.
for results in "$TMP/own.tsv" "$TMP/exists.tsv"; do
	expect "$(awk -F'\t' '{ print NF }' "$results" | sort -u)" "$(benchHeader | awk -F'\t' '{ print NF }')" "the width of every line of $results"
done
# Reps and a size given with a leading zero are the decimals they spell, never octal. The fixture is the
# size-3 one, its title is the rep's, and the last rep, whose number is compared as text with the reps, still
# reads its card floor.
ARM_REPS=01 ARM_SIZE=003 arm padded
expect "$st:$(column padded size_bytes)" "0:3145728" "an arm of 01 reps of 003 units"
grep -q 'reps=1 size=3 (3145728 bytes' "$TMP/padded.out" || fail "the padded arm's banner: $(head -1 "$TMP/padded.out")"
[[ "$(column padded card_after)" == "ok ok 00000000 file bytes=16777216 "* ]] || fail "the last of 01 reps read no card floor: $(column padded card_after)"
run 2 requireUnits 1234567890 # ten digits: a multiply by a unit could wrap
saidLine "$TOOL: a size counts 1048576-byte units, as a decimal of at most 9 digits; got '1234567890'" "an oversized size"
# BENCH_TIMER's status is its command's, and the file it writes names the seconds and the clock.
timer=$TMP/timer
for leg in "7:sh -c 'exit 7'" "137:sh -c 'kill -9 \$\$'" "127:$TMP/no-such-command"; do
	rm -f "$timer"
	st=0
	eval "perl -e \"\$BENCH_TIMER\" \"\$timer\" ${leg#*:}" 2>/dev/null || st=$?
	expect "$st" "${leg%%:*}" "the timer's status for ${leg#*:}"
	read -r took clock <"$timer" || fail "the timer wrote no time for ${leg#*:}"
	[[ "$took" =~ ^[0-9]+\.[0-9]{6}$ && "$clock" == "$(benchClock)" ]] || fail "the timer wrote '$took $clock' for ${leg#*:}"
done
# A step in the wall clock (StepClock: Time::HiRes::time reads 100 s back after its first call, in the timer's
# perl alone) does not show in CLOCK_MONOTONIC, which the timer reads. On a perl with no CLOCK_MONOTONIC
# (no-mono), the wall clock is the fallback, which is said before rep 1, and a rep it stepped is void:clock.
mkdir -p "$TMP/clk"
cat >"$TMP/clk/StepClock.pm" <<'EOF'
package StepClock;
use strict;
use warnings;
use Time::HiRes ();
BEGIN {
	my %flag = map { $_ => 1 } split /,/, $ENV{STEP_CLOCK} // "";
	# BENCH_TIMER's perl (its first argument is the wall file) and benchClock's (none): no other perl
	if ($0 eq "-e" && (!@ARGV || $ARGV[0] =~ m{/wall$})) {
		no warnings "redefine";
		*Time::HiRes::CLOCK_MONOTONIC = sub { die "Your vendor has not defined Time::HiRes macro CLOCK_MONOTONIC\n" } if $flag{"no-mono"};
		if ($flag{"wall-back"}) {
			my ($real, $calls) = (\&Time::HiRes::time, 0);
			*Time::HiRes::time = sub { my $t = $real->(); $calls++ ? $t - 100 : $t };
		}
		if ($flag{"wall-tiny"}) { # each call after the first 4 ms on from it
			my ($real, $t0) = (\&Time::HiRes::time);
			*Time::HiRes::time = sub { defined $t0 ? $t0 + 0.004 : ($t0 = $real->()) };
		}
	}
}
# spy: BENCH_TIMER's perl stamps CLOCK_MONOTONIC on $FAKE/stamps as it starts and as it ends, before
# and after its own clock reads.
my $spy = ($ENV{STEP_CLOCK} // "") =~ /(^|,)spy(,|$)/ && $0 eq "-e" && @ARGV && $ARGV[0] =~ m{/wall$};
sub stamp {
	open(my $fh, ">>", "$ENV{FAKE}/stamps") or die "$ENV{FAKE}/stamps: $!\n";
	printf $fh "%s %.6f\n", $_[0], Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC());
	close($fh);
}
stamp("timer-start") if $spy;
END { stamp("timer-end") if $spy }
1;
EOF
stepped="-I$TMP/clk -MStepClock"
# The wall is drop.sh's alone, read by BENCH_TIMER in the process that runs it. The deadline's setup (its
# mktemp) and teardown (its rm) are not part of it. This is checked on one clock, not by how long either took:
# shims stamp CLOCK_MONOTONIC when the install's deadline makes its temp file (its setup, between the title
# check and the put) and when it removes it (its teardown, the first after the put), and StepClock's spy
# stamps when the timer's perl starts and ends. Timed around the whole deadline, as it used to be, the setup
# would come after the timer's start and the teardown before its end.
mkdir -p "$TMP/slow"
cat >"$TMP/slow/stamp" <<'EOF'
#!/bin/sh
# stamp <name>: `<name> <CLOCK_MONOTONIC seconds>` on $FAKE/stamps.
exec perl -MTime::HiRes -e 'printf "%s %.6f\n", $ARGV[0], Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC())' "$1" >>"$FAKE/stamps"
EOF
chmod +x "$TMP/slow/stamp"
for tool in mktemp rm; do
	# shellcheck disable=SC2016 # the shim's own expressions, expanded when it runs
	case "$tool" in
	mktemp) gate='grep -q "^ls " "$FAKE/log" && ! grep -q "^put " "$FAKE/log"' ;;
	rm) gate='grep -q "^put " "$FAKE/log" && ! grep -q "^rm " "$FAKE/stamps"' ;;
	esac
	# shellcheck disable=SC2016 # the shim's own arguments
	printf '#!/bin/sh\ncase "$*" in *3drop-deadline*) if %s 2>/dev/null; then %q %s; fi ;; esac\nexec %q "$@"\n' \
		"$gate" "$TMP/slow/stamp" "$tool" "$(type -P "$tool")" >"$TMP/slow/$tool"
	chmod +x "$TMP/slow/$tool"
done
rm -f "$FAKE/stamps"
PATH="$TMP/slow:$PATH" PERL5OPT=$stepped STEP_CLOCK=spy arm supervised
expect "$st:$(column supervised outcome)" "0:committed:ok" "a rep whose deadline and timer were stamped"
awk '!($1 in t) { t[$1] = $2 + 0 } END {
	exit !(("mktemp" in t) && ("timer-start" in t) && ("timer-end" in t) && ("rm" in t) &&
		t["mktemp"] <= t["timer-start"] && t["timer-end"] <= t["rm"]) }' "$FAKE/stamps" ||
	fail "the wall holds the deadline's own setup or teardown: $(tr '\n' ' ' <"$FAKE/stamps" 2>&1)"
# A rep's wall is one drop.sh run: a second or so on a Mac, 7 to 10 s on Git Bash and up to 33 s on a loaded
# one (a process there costs 70 to 250 ms). These bounds, here and at wallok, tell a real wall from a stepped
# clock's (under 0, or 100 s off). On Git Bash they are 90 s, over any rep seen there and under a step.
repMax=$((gitBash ? 90 : 10))
PERL5OPT=$stepped STEP_CLOCK=wall-back arm monostep
expect "$st:$(column monostep outcome)" "0:committed:ok" "a rep whose wall clock stepped back 100 s"
awk -v w="$(column monostep wall_s)" -v max="$repMax" 'BEGIN { exit !(w > 0 && w < max) }' || fail "a stepped wall clock reached the wall: $(column monostep wall_s) s"
! grep -q 'no CLOCK_MONOTONIC' "$TMP/monostep.out" || fail "a perl with CLOCK_MONOTONIC said it had none: $(cat "$TMP/monostep.out")"
PERL5OPT=$stepped STEP_CLOCK=no-mono,wall-back arm wallstep
expect "$(column wallstep outcome):$(column wallstep wall_s):$(column wallstep rate_mbs)" "void:clock:-:-" "a rep whose only clock stepped back"
awk '/no CLOCK_MONOTONIC/ { said = NR } /^wallstep r1 / { rep = NR } END { exit !(said && rep && said < rep) }' "$TMP/wallstep.out" ||
	fail "the wall clock fallback was not said before rep 1: $(cat "$TMP/wallstep.out")"
# A real step of the wall clock inside this rep (WSL's: 1-3 s about every 30 s) voids it, as it should. The
# rep then runs again (steadyRun, lib.sh) on a fresh results file, with its checks after the loop. The others
# are safe from a real step: monostep is timed by CLOCK_MONOTONIC, and the verdicts of wallstep and walltiny
# are StepClock's (100 s back, 4 ms), which a real step of seconds cannot undo.
until steadyRun; do
	rm -f "$TMP/wallok.tsv"
	PERL5OPT=$stepped STEP_CLOCK=no-mono arm wallok
done
expect "$st:$(column wallok outcome)" "0:committed:ok" "a rep on a perl with no CLOCK_MONOTONIC"
awk -v w="$(column wallok wall_s)" -v max="$repMax" 'BEGIN { exit !(w > 0 && w < max) }' || fail "a wall from the wall clock: $(column wallok wall_s) s"
grep -q 'no CLOCK_MONOTONIC' "$TMP/wallok.out" || fail "the wall clock fallback was not said: $(cat "$TMP/wallok.out")"
# Which clock timed a rep is on its line when it was the wall clock, which no column says.
grep -q '^wallok r1 .* ms)  clock wall$' "$TMP/wallok.out" || fail "a rep the wall clock timed did not say so: $(grep '^wallok r1' "$TMP/wallok.out")"
! grep -q 'clock wall' "$TMP/monostep.out" || fail "a rep CLOCK_MONOTONIC timed named the wall clock: $(cat "$TMP/monostep.out")"
# A wall that rounds to 0.00 (wall-tiny: 4 ms) does not count as a time either. The verdict reads the class
# rowClass gives the wall as shown.
PERL5OPT=$stepped STEP_CLOCK=no-mono,wall-tiny arm walltiny
expect "$(column walltiny outcome):$(column walltiny wall_s)" "void:clock:0.00" "a rep whose wall rounds to 0.00"
# Rows an older instrument wrote with a stepped clock hold a committed wall that is not a time. They are
# counted void and never ranked as the fastest (or at all).
awk -F'\t' -v OFS='\t' "$BENCH_AWK"'NR == 1 { print; columns(); next }
	{ for (i = 1; i <= 4; i++) {
		$col["series"] = "old"; $col["rep"] = i
		$col["wall_s"] = (i == 1 ? "1.20" : i == 2 ? "-2.50" : i == 3 ? "-" : "0.00")
		$col["rate_mbs"] = (i == 1 ? "2.6214" : "-")
		print } }' "$TMP/own.tsv" >"$TMP/old.tsv"
out=$("$ROOT/tools/bench/seriesScore.sh" old "$TMP/old.tsv" 4 2>&1) || fail "scoring rows of an older instrument: $out"
[[ "$out" == *"fastest 1.20 s (2.6214 MB/s)"* && "$out" == *"failed 0  void 3  skipped 0"* ]] ||
	fail "rows with no wall were ranked: $out"
[[ "$(grep -c ' void:clock ' <<<"$out")" = 3 ]] || fail "rows with no wall were not named void:clock: $out"
# A net of zero can hide a ticket kept by one rep and lost by a later one: the scorer names the step.
tab=$'\t'
for gaps in "40 41 40:tk" "40 40 40:tkflat"; do
	BENCH_GAPS=${gaps%%:*} awk -F"$tab" -v OFS="$tab" "$BENCH_AWK"'NR == 1 { print; columns(); next }
		{ split(ENVIRON["BENCH_GAPS"], g, " ")
		for (i = 1; i <= 3; i++) { $col["series"] = "tk"; $col["rep"] = i; $col["am_titles"] = 0; $col["am_tickets"] = g[i]; print } }' \
		"$TMP/own.tsv" >"$TMP/${gaps##*:}.tsv"
done
out=$("$ROOT/tools/bench/seriesScore.sh" tk "$TMP/tk.tsv" 3 2>&1) || fail "scoring a ticket gap that moved and came back: $out"
[[ "$out" == *"ticket gap +0 over reps 1-2"*"ticket gap moved up between rep 1 and rep 2 and was back by rep 3  TICKET-GAP"* ]] ||
	fail "a ticket gap that moved and came back was not named: $out"
out=$("$ROOT/tools/bench/seriesScore.sh" tk "$TMP/tkflat.tsv" 3 2>&1) || fail "scoring a ticket gap that held: $out"
[[ "$out" == *"ticket gap +0 over reps 1-2"* && "$out" != *"moved"* ]] || fail "a ticket gap that held was named: $out"
# A label is a cell of the results file, found again as written. A backslash in it is not an escape that awk
# decodes (a`\tb` is not a, a tab, b), and a tab or a newline, which no cell holds, is refused before the
# console is asked anything.
label='a\tb'
arm "$label"
expect "$st:$(column "$label" outcome)" "0:committed:ok" "an arm whose label holds a backslash"
arm "$label"
expect "$st:$(count put)" "1:0" "a second block under the same label"
grep -qF "label '$label' already has rows" "$TMP/${label//\\/_}.out" || fail "a label in use was not said: $(cat "$TMP/${label//\\/_}.out")"
BENCH_LABEL=$label awk -F'\t' -v OFS='\t' "$BENCH_AWK"'NR == 1 { print; columns(); next }
	{ for (i = 1; i <= 3; i++) { $col["series"] = ENVIRON["BENCH_LABEL"]; $col["rep"] = i; $col["wall_s"] = "1.00"; print } }' \
	"$TMP/own.tsv" >"$TMP/slow.tsv" # three reps of one second
repSlow "$TMP/slow.tsv" "$label" 2.00 || fail "a rep twice the median of a label with a backslash was not slow"
! repSlow "$TMP/slow.tsv" "$label" 1.10 || fail "a rep near the median of a label with a backslash was slow"
for badLabel in $'a\tb' $'a\nb'; do
	: >"$FAKE/log"
	rm -f "$TMP/refused.tsv"
	st=0
	BENCH_RESULTS="$TMP/refused.tsv" faulted valid "$ROOT/tools/bench/bench.sh" "$badLabel" 1 3 127.0.0.1 >"$TMP/refused.out" 2>&1 || st=$?
	expect "$st:$(awk 'END { print NR }' "$FAKE/log"):$([ -e "$TMP/refused.tsv" ] && echo made || echo none)" "2:0:none" "an arm whose label holds a tab or a newline"
	grep -q 'a label is a cell of the results file' "$TMP/refused.out" || fail "a label with a tab or a newline was not said: $(cat "$TMP/refused.out")"
	st=0
	"$ROOT/tools/bench/seriesScore.sh" "$badLabel" "$TMP/old.tsv" >/dev/null 2>&1 || st=$?
	expect "$st" 2 "scoring a label that holds a tab or a newline"
done
# Capture is one key, `capture=`. capture.sh arms it by that key, and the arm's row reads it. Each of on and
# off sends the one set and no check before it, so a release build is asked only once it refuses.
: >"$FAKE/log"
"$ROOT/tools/bench/capture.sh" on 127.0.0.1 >/dev/null || fail "capture.sh on did not read the capture key"
expect "$(count bench)" 1 "the bench requests capture.sh on sent"
arm captured
expect "$st:$(column captured capture)" "0:1" "an arm with capture armed"
: >"$FAKE/log"
"$ROOT/tools/bench/capture.sh" off 127.0.0.1 >/dev/null || fail "capture.sh off did not read the capture key"
expect "$(count bench)" 1 "the bench requests capture.sh off sent"
# The reply is the console's RAM, and a later boot arms from the arm file. A card that refuses the file (to
# make it, or to remove it) leaves the capture holding for this boot only, which capture.sh says.
st=0
out=$(faulted capture-nosave "$ROOT/tools/bench/capture.sh" on 127.0.0.1 2>&1) || st=$?
[[ "$st" = 1 && "$out" == *"capture: capture on holds for this boot only: /3ds/3drop/capture/arm is not on the card" ]] ||
	fail "capture.sh on, the card refusing its arm file (exit $st): $out"
"$ROOT/tools/bench/capture.sh" on 127.0.0.1 >/dev/null || fail "capture.sh on, the card taking its arm file"
st=0
out=$(faulted capture-nosave "$ROOT/tools/bench/capture.sh" off 127.0.0.1 2>&1) || st=$?
[[ "$st" = 1 && "$out" == *"capture: capture off holds for this boot only: /3ds/3drop/capture/arm is still on the card" ]] ||
	fail "capture.sh off, the card keeping its arm file (exit $st): $out"
"$ROOT/tools/bench/capture.sh" off 127.0.0.1 >/dev/null || fail "capture.sh off, the card taking its arm file away"
# probe.sh's capture arm is a set too, so it spends a nonce as capture.sh's does; a bare one reads.
out=$("$ROOT/tools/bench/probe.sh" capture 127.0.0.1 on) || fail "probe.sh capture on: $out"
[[ "$out" == *" capture capture=1 "* ]] || fail "probe.sh capture on said: $out"
out=$("$ROOT/tools/bench/probe.sh" capture 127.0.0.1 off) || fail "probe.sh capture off: $out"
[[ "$out" == *" capture capture=0 "* ]] || fail "probe.sh capture off said: $out"
out=$("$ROOT/tools/bench/probe.sh" capture 127.0.0.1) || fail "probe.sh capture: $out"
[[ "$out" == *" capture capture=0 "* ]] || fail "probe.sh capture said: $out"
arm release release # a release build: no knobs, rows, air or card floor to read
expect "$st:$(column release outcome):$(column release knobs):$(column release mb):$(column release air)" \
	"0:committed:ok:release:-:-" "an arm on a release build"
: >"$FAKE/locked"
arm stranger foreign-req # a probe reached the console between the put and its `stats`
for sent in '^put ' '^stats [0-9a-f]* op$' '^delete '; do
	grep -q "$sent" "$FAKE/locked" || fail "the rep's $sent was not sent under its lock"
done
expect "$(column stranger outcome):$(ownDelete)" "void:req:title:000400000c000100 ticket" "a rep whose stats name another request"
# A foreign put replaces the receipt and the transfer records together: comparing each with the other
# is satisfied by both moving, so only the rep's own put id (THREEDROP_PUT_ID_FILE) sees it.
arm foreign foreign-both
expect "$(column foreign outcome):$(ownDelete)" "void:req:title:000400000c000100 ticket" \
	"a rep whose receipt and stats both name one foreign request"
# A curve the console holds for another request (one that reached it after the stats were read) is not this
# rep's curve. Its mb cell is `-`, the tool says so, and the rep, whose stats are its own, counts.
arm foreignmb foreign-mb
expect "$st:$(column foreignmb outcome):$(column foreignmb mb)" "0:committed:ok:-" "a rep whose mb curve names another request"
grep -qx "bench: rep 1: its mb curve is of request ffffffffffffffffffffffffffffffff, not its own: not kept" "$TMP/foreignmb.out" ||
	fail "a foreign mb curve was not said: $(cat "$TMP/foreignmb.out")"
# A put refused `exists` proves this rep installed nothing: it must not uninstall the title.
arm refused-exists put-exists
expect "$(column refused-exists outcome):$(count delete)" "rejected:exists:0" \
	"a rep whose put was refused exists, with no uninstall of another client's title"
# The other verdicts are repVerdict's alone (the stranger arm runs the plumbing). Its rules, in order: no put
# line, a refusal, a receipt or stats of another put, stats of other bytes, none, no AM write, no wall. Stats
# of another put and of other bytes are void:req. The first rule that holds applies.
stats="bytes=3145728 total_ms=1000 am_write_n=1 req=$putId"
verdict() { repVerdict put=committed:ok own="$putId" receipt="$putId" bytes=3145728 timed=ok stats="$1" "${@:2}"; }
expect "$(verdict "$stats")" committed:ok "a rep whose stats describe its put"
expect "$(verdict "${stats/bytes=3145728/bytes=3145727}")" void:bytes "a rep whose stats moved other bytes"
expect "$(verdict -)" void:stats "a rep whose stats went unanswered"
expect "$(verdict "${stats/am_write_n=1/am_write_n=0}")" void:am-writes "a rep whose stats show no AM write"
expect "$(verdict "bytes=3145727 total_ms=1000 am_write_n=1 req=0123")" void:req "a rep whose stats name another put, of other bytes"
expect "$(verdict "$stats" receipt=0123)" void:req "a rep whose receipt names another put"
expect "$(verdict "$stats" timed=void)" void:clock "a committed rep no clock timed"
expect "$(verdict "$stats" put=rejected:space)" rejected:space "a refused put"
expect "$(verdict "$stats" put=):$(verdict "$stats" put= fired=1)" noreply:deadline "a put with no line, then one its deadline ended"
# A rep whose build or knobs go unread installs nothing: it is skipped before its title check (and a
# stats self that went unanswered no longer passes the charger check). Each mode starts at the arm's
# relaunch, so the arm's own reads pass.
rm -f "$FAKE/relaunched"
arm nover self-nover # stats self names no build
expect "$(count put):$(count ls):$(column nover outcome):$(column nover ver)" "0:0:skipped:self:?" "a rep whose stats self named no build"
grep -q 'rep 1 skipped: its stats self named no build' "$TMP/nover.out" || fail "a rep with no build did not say why: $(cat "$TMP/nover.out")"
rm -f "$FAKE/relaunched"
arm silent mute-state # the bare bench goes unanswered
expect "$(count put):$(column silent outcome)" "0:skipped:knobs" "a rep whose knobs could not be read"
grep -q 'rep 1 skipped: no answer to a bare bench' "$TMP/silent.out" || fail "a rep with no knobs did not say why: $(cat "$TMP/silent.out")"
arm amb ambiguous # no reply, no receipt, and the title not installed: the put's outcome is unknown
expect "$st:$(column amb outcome):$(ownDelete)" "1:noreply:title:000400000c000100 ticket" "a rep whose put has no outcome (its median FAILED: 1)"
grep -q "^bench: rep 1 (noreply): drop: the put's outcome is unknown" "$TMP/amb.out" || fail "a rep with no outcome did not say drop.sh's reason: $(cat "$TMP/amb.out")"
arm refusedput refuse-early # drop.sh says the refusal on stderr, after its `drop: `
expect "$st:$(column refusedput outcome):$(ownDelete)" "1:rejected:space:title:000400000c000100 ticket" "a rep whose put was refused (its median FAILED: 1)"
expect "$(column refusedput rate_mbs)" - "a refused rep's rate" # it moved no fixture's worth of bytes
# A floor the console refused wrote nothing, so it is not a floor, and it marks no rep after card floor. The
# refusal is on the arm's second bench request (its bare bench, then its first floor), as faultedOn puts it.
echo "bench 2" >"$FAKE/fault-on"
rm -f "$FAKE/fault-seen"
arm floorless rejected-busy
rm -f "$FAKE/fault-on" "$FAKE/fault-seen"
expect "$st:$(column floorless card_before)" "0:-" "an arm whose first card floor was refused"
# A ghost retry is two installs in one wall, with the second's stats: void:ghost, with what drop.sh said.
fakeCia "$THREEDROP_FIXTURE_DIR/fixture17m.cia" 000400000c000100 1 17825792
rm -f "$FAKE/paused"
THREEDROP_GHOST_FLOOR=1500000 THREEDROP_GHOST_GRACE=0 ARM_SIZE=17 arm ghosted ghost-then-fast
rm -f "$THREEDROP_FIXTURE_DIR/fixture17m.cia" "$FAKE/paused"
expect "$st:$(count put):$(column ghosted outcome)" "1:2:void:ghost" "a rep a ghost retry sent twice (no rep counted: 1)"
grep -q '^bench: rep 1 (void:ghost): drop: ghost detected' "$TMP/ghosted.out" || fail "a ghost retry did not say drop.sh's lines: $(cat "$TMP/ghosted.out")"
BENCH_DEADLINE=3 arm late mute-put # the put goes unanswered past the rep's deadline
expect "$(column late outcome)" deadline "a rep its deadline ended"
# Its wall is what it took to end. The timer outlives the deadline's TERM (which lands on it last, after the
# command's) and writes the seconds the command ran. A kill on Git Bash costs a ps -ef for each process of the
# tree (the 3 s deadline took 8 to 10 s to end on a loaded PC), so its bound there is 25 s, under the 30 s the
# mute console would hold the put. A deadline that ended nothing shows the hold.
endMax=$((gitBash ? 25 : 10))
awk -v w="$(column late wall_s)" -v max="$endMax" 'BEGIN { exit !(w >= 2.5 && w < max) }' ||
	fail "a rep its deadline ended has no wall of its own: '$(column late wall_s)' s"
# A deadline of 08 s is eight, not an octal the watchdog's countdown cannot take, so a command that outlives
# it is ended at 8 s. It runs in the background beside the checks below (collected at the end) and under an
# alarm, so a deadline that never fires fails there and does not hang check.sh.
(
	st08=0 # not st: the checks below read this shell's own
	# shellcheck disable=SC2016 # the child shell expands its own
	perl -e 'alarm 25; exec @ARGV' bash -c '. "$1/tools/bench/lib.sh"; rc=0; withDeadline 08 sleep 30 || rc=$?; echo "$rc $DEADLINE_FIRED"' \
		_ "$ROOT" >"$TMP/dead08" 2>&1 || st08=$?
	echo "$st08" >"$TMP/dead08.st"
) &
dead08=$!
# The deadline's kill is its own, however slowly the watchdog then exits (a loaded host).
(
	original killTree
	# shellcheck disable=SC2317 # called by withDeadline
	killTree() {
		real_killTree "$1"
		sleep 0.3
	}
	withDeadline 1 sleep 30 || :
	[ "$DEADLINE_FIRED" = 1 ] || fail "a deadline whose watchdog was slow to exit was not its own"
)
# A command that ended is never signalled after: once reaped, its pid may be another process's.
(
	original killTree
	# shellcheck disable=SC2317 # called by withDeadline
	killTree() {
		echo "$1" >>"$TMP/signalled"
		real_killTree "$1"
	}
	# shellcheck disable=SC2016 # the child shell expands its own
	withDeadline 30 sh -c 'echo $$ >"$1"' _ "$TMP/ended"
	{ read -r ended <"$TMP/ended" && isPositive "$ended"; } || fail "the command never named its pid: the check below would prove nothing"
	! grep -qxF "$ended" "$TMP/signalled" || fail "withDeadline signalled its command's pid once it ended"
)
# A watchdog whose owner is gone, which no trap saw (a KILL), ends the command with it and never signals a pid
# it no longer names later. And deadlineStopAll, which a trap calls, ends at once a watchdog that a subshell
# ran.
# shellcheck disable=SC2016 # the child shells expand their own
bash -c '. "$1/tools/bench/lib.sh"; withDeadline 30 sh -c "echo \$\$ >\"\$1\"; exec sleep 30" _ "$2"' _ "$ROOT" "$TMP/owned" &
owner=$!
waitFor "$TMP/owned" 10
kill -KILL "$owner"
tryFor 60 gone "$(cat "$TMP/owned")" || fail "a deadline's command outlived its KILLed owner by 3 s"
(
	SCRATCH=$TMP/armed
	mkdir "$SCRATCH"
	# shellcheck disable=SC2016
	(withDeadline 30 sh -c 'echo $$ >"$1"; exec sleep 30' _ "$TMP/sub") &
	waitFor "$TMP/sub" 10
	deadlineStopAll
	tryFor 10 gone "$(cat "$TMP/sub")" || fail "deadlineStopAll left a subshell's deadline running"
	wait
)
# Ctrl-C in the middle of an install, or of the rx after it, whose deadline runs in a $(...) pipeline (INT to
# bench.sh's process group, which their commands ignore, as every async command does), ends both and their
# watchdogs before the rep lock goes, sends nothing more, and names the title the rep may have left.
for stage in "mute-put ^put " "mute-rx ^bench [0-9a-f]* rx "; do
	fakeCia "$fixture" 000400000c000100 1 3145728
	: >"$FAKE/installed"
	: >"$FAKE/log"
	echo "${stage%% *}" >"$FAKE/mode"
	BENCH_RESULTS="$TMP/${stage%% *}.tsv" BENCH_DEADLINE=60 perl -e '$SIG{INT} = "DEFAULT"; setpgrp(0, 0); exec(@ARGV) or die' \
		"$ROOT/tools/bench/bench.sh" stopped 1 3 127.0.0.1 >"$TMP/stopped.out" 2>&1 &
	bencher=$!
	tryFor 600 grep -q "${stage#* }" "$FAKE/log" || :
	sleep 0.5
	sent=$(wc -l <"$FAKE/log")
	kill -INT -- "-$bencher"
	wait "$bencher" || :
	tryFor 100 gone "-$bencher" || { # 5 s: a loaded host is slow to run the traps
		kill -KILL -- "-$bencher" 2>/dev/null || :
		fail "Ctrl-C (${stage%% *}) left bench.sh's command or its watchdog running: $(cat "$TMP/stopped.out")"
	}
	rm -f "$FAKE/mode"
	expect "$(tail -n +$((sent + 1)) "$FAKE/log")" "" "what bench.sh sent after a Ctrl-C (${stage%% *})"
	[ ! -d "$WIRE_RUN_DIR/rep-127.0.0.1" ] || fail "Ctrl-C (${stage%% *}) left the rep lock"
	grep -qF "tools/device.sh uninstall 127.0.0.1:$fakePort --ticket 000400000c000100" "$TMP/stopped.out" ||
		fail "Ctrl-C (${stage%% *}) did not name the rep's title: $(cat "$TMP/stopped.out")"
done
: >"$FAKE/installed"
# bench.sh is one function, read whole before it starts (main "$@"; exit). Text appended to it while rep 1 is
# held is never run, and the block still ends 0 and scores. An edit of the tools the rows name (inst)
# mid-block stops the block before the next rep, so no row names code that did not run. This runs from a copy
# of the tree, which the edits change instead of this one.
kit=$TMP/kit
mkdir -p "$kit"
cp -a "$ROOT/tools" "$ROOT/src" "$ROOT/res" "$kit/"
# makeFixture.sh's tools, which a block that builds its fixtures must find first, are stand-ins that are never
# run, since the copy's blocks make theirs with the fake's CIA (below).
mkdir -p "$kit/tools/bin"
for tool in makerom bannertool 3dstool; do
	printf '#!/bin/sh\nexit 1\n' >"$kit/tools/bin/$tool"
	chmod +x "$kit/tools/bin/$tool"
done
# An nc in front of the fake's that counts puts: on put $PUT_AT it makes put-held, then waits $PUT_SLEEP s
# inside the rep's wall, or sets the fake's mode to $PUT_MODE from that put on.
mkdir -p "$TMP/putshim"
cat >"$TMP/putshim/nc" <<'EOF'
#!/usr/bin/env bash
IFS= read -r line
if [[ "$line" == "put "* ]]; then
	n=$(($(cat "$FAKE/puts-seen" 2>/dev/null || echo 0) + 1))
	echo "$n" >"$FAKE/puts-seen"
	if [ "$n" = "${PUT_AT:-}" ]; then
		[ -z "${PUT_MODE:-}" ] || echo "$PUT_MODE" >"$FAKE/mode"
		: >"$FAKE/put-held"
		sleep "${PUT_SLEEP:-0}"
	fi
fi
{ printf '%s\n' "$line"; exec cat; } | "$FAKE/bin/nc" "$@"
EOF
chmod +x "$TMP/putshim/nc"
heldBlock() { # <label> <reps>: from the copy, in the background ($bencher), once rep 1's put is held
	fakeCia "$fixture" 000400000c000100 1 3145728
	: >"$FAKE/log"
	echo mute-put >"$FAKE/mode"
	BENCH_RESULTS="$TMP/$1.tsv" BENCH_DEADLINE=3 "$kit/tools/bench/bench.sh" "$1" "$2" 3 127.0.0.1 >"$TMP/$1.out" 2>&1 &
	bencher=$!
	tryFor 600 grep -q '^put ' "$FAKE/log" || :
}
# The held rep commits once its put goes through, so main returns 0 and the group's `exit` is what
# ends the shell: without it bash would read on into the appended lines and exit 7. (A deadline's
# rep scores a FAILED median, and main's non-zero return ends the shell by set -e, `exit` or not.)
fakeCia "$fixture" 000400000c000100 1 3145728
rm -f "$FAKE/puts-seen" "$FAKE/put-held"
: >"$FAKE/log"
PATH="$TMP/putshim:$PATH" PUT_AT=1 PUT_SLEEP=2 BENCH_RESULTS="$TMP/appended.tsv" "$kit/tools/bench/bench.sh" appended 1 3 127.0.0.1 >"$TMP/appended.out" 2>&1 &
bencher=$!
tryFor 600 test -e "$FAKE/put-held" || :
printf 'echo "bench: an appended line ran" >&2\nexit 7\n' >>"$kit/tools/bench/bench.sh"
st=0
wait "$bencher" || st=$?
expect "$st:$(column appended outcome)" "0:committed:ok" "a block whose bench.sh was appended to during rep 1 (the appended exit is 7)"
grep -q '^appended n=1 ' "$TMP/appended.out" || fail "a block whose bench.sh was appended to did not score: $(cat "$TMP/appended.out")"
! grep -q 'an appended line ran' "$TMP/appended.out" || fail "bench.sh ran text appended to it mid-block"
cp "$ROOT/tools/bench/bench.sh" "$kit/tools/bench/bench.sh"
heldBlock relib 2
echo '# an edit mid-block' >>"$kit/tools/bench/lib.sh"
st=0
wait "$bencher" || st=$?
rm -f "$FAKE/mode"
expect "$st:$(count put):$(column relib rep)" "1:1:1" "a block whose lib.sh was edited during rep 1"
grep -q 'tools/ changed since the block began (inst [0-9a-f]*, now [0-9a-f]*): the block stops before rep 2' "$TMP/relib.out" ||
	fail "an edit of lib.sh mid-block did not stop it: $(cat "$TMP/relib.out")"
cp "$ROOT/tools/bench/lib.sh" "$kit/tools/bench/lib.sh"
# Blocks of more than one rep, from the copy, whose makeFixture.sh makes the fake's CIA at each rep's
# own title, through the put-counting nc.
cat >"$kit/tools/bench/makeFixture.sh" <<'EOF'
#!/usr/bin/env bash
# makeFixture.sh <units> <uid>: the fake's CIA at the uid's title, for the tests' blocks; none for the
# uid FIXTURE_FAIL names.
set -euo pipefail
fail() { echo "makeFixture: $*" >&2; exit 1; }
. "$FAKE_LIB"
[ "$2" != "${FIXTURE_FAIL:-}" ] || fail "no fixture for uid $2"
fakeCia "$THREEDROP_FIXTURE_DIR/fixture${1}m.cia" "$(uidTitleID "$2")" 1 $((10#$1 * 1048576))
EOF
chmod +x "$kit/tools/bench/makeFixture.sh"
block() { # <label> <reps> [fault-on args...]: a block from the copy through the put-counting nc
	rm -f "$FAKE/puts-seen" "$fixture"
	: >"$FAKE/log"
	st=0
	if [ "$#" -gt 2 ]; then
		PATH="$TMP/putshim:$PATH" BENCH_RESULTS="$TMP/$1.tsv" faultedOn "${@:3}" "$kit/tools/bench/bench.sh" "$1" "$2" 3 127.0.0.1 \
			>"$TMP/$1.out" 2>&1 || st=$?
	else
		PATH="$TMP/putshim:$PATH" BENCH_RESULTS="$TMP/$1.tsv" "$kit/tools/bench/bench.sh" "$1" "$2" 3 127.0.0.1 >"$TMP/$1.out" 2>&1 || st=$?
	fi
	rm -f "$FAKE/mode"
}
cells() { awk -F'\t' -v c="$2" "$BENCH_AWK"'NR == 1 { columns(); next } { printf "%s%s", (NR > 2 ? " " : ""), $col[c] }' "$TMP/$1.tsv"; }
floors() { awk -F'\t' -v c="$2" "$BENCH_AWK"'NR == 1 { columns(); next } { printf "%s%s", (NR > 2 ? " " : ""), ($col[c] ~ /^ok ok 00000000 file / ? "F" : $col[c]) }' "$TMP/$1.tsv"; }
# Six reps, each with a uid made of the prefix and the rep number. Rep 3's put is refused (no rate, because it
# did not complete), rep 4 is over an installed title (skipped), and rep 5 is slow (its put is held, measured
# against the median so far, of reps 1-3). A card floor goes on the row after each bad rep, and a skip carries
# it over: rep 4's and rep 5's rows start from rep 3's floor, rep 6's from rep 5's; rep 1's is the block's
# first, and rep 6 takes its last. Slow means past 1.25 times that median, which with rep 3 failed is the
# slower of reps 1 and 2. A 3 s hold does it against a Mac's 1 s reps; Git Bash's reps reach 33 s (repMax,
# above), so there the hold is 45 s.
echo "000400000c000400 1" >"$FAKE/installed"
PUT_AT=4 PUT_SLEEP=$((gitBash ? 45 : 3)) block six 6 put 3 refuse-early
: >"$FAKE/installed"
expect "$(cells six uid)" "c0001 c0002 c0003 c0004 c0005 c0006" "a six-rep block's uids"
expect "$(cells six title)" "000400000c000100 000400000c000200 000400000c000300 000400000c000400 000400000c000500 000400000c000600" "a six-rep block's titles"
expect "$(cells six outcome)" "committed:ok committed:ok rejected:space skipped:exists committed:ok committed:ok" "a six-rep block's outcomes"
expect "$(cells six rate_mbs | cut -d' ' -f3-4)" "- -" "the rates of a refused rep and a skipped one"
expect "$(floors six card_before):$(floors six card_after)" "F - - F F F:- - - - - F" "a six-rep block's card floors"
awk -v w="$(cells six wall_s | cut -d' ' -f5)" 'BEGIN { exit !(w >= 3) }' || fail "the slow rep's wall: $(cells six wall_s)"
grep -q '^six n=6 ' "$TMP/six.out" || fail "a six-rep block did not score: $(cat "$TMP/six.out")"
# A ring other than the shipped one stops the block at any rep, not only at the first that reports one.
PUT_AT=2 PUT_MODE=small-ring block ring2 2
expect "$st:$(cells ring2 rep):$(count put)" "1:1 2:2" "a block whose rep 2 ran on another ring"
grep -q "rep 2's ring_cap=16777216, not the shipped .*; the series stops" "$TMP/ring2.out" || fail "rep 2's ring was not checked: $(cat "$TMP/ring2.out")"
# A last rep that cannot start (no fixture made for it) is skipped, on a row of its own that holds the
# block's last floor. It never began, so it did not fail: the block still scores, and exits 0.
FIXTURE_FAIL=c0002 block lastfix 2
expect "$st:$(cells lastfix outcome):$(floors lastfix card_after):$(count put)" "0:committed:ok skipped:fixture:- F:1" \
	"a block whose last rep had no fixture"
# Rep 10's uid is the prefix and a hex digit (c000a, never c00010); and a last rep that is skipped
# still takes the block's last floor. Every title is there already, so nothing installs.
for rep in 1 2 3 4 5 6 7 8 9 a; do echo "000400000c000${rep}00 1"; done >"$FAKE/installed"
block ten 10
: >"$FAKE/installed"
expect "$st:$(count put):$(cells ten uid | cut -d' ' -f10):$(cells ten title | cut -d' ' -f10)" "1:0:c000a:000400000c000a00" "a ten-rep block's last uid"
expect "$(floors ten card_after | cut -d' ' -f10)" F "the floor after a block whose last rep was skipped"
: >"$FAKE/relaunched"
arm nobench mute-state # no answer to the bare bench before the arm: no answer, 4
expect "$st:$(count put)" "4:0" "an arm whose bench state went unanswered"
arm smallring small-ring
if [ "$st" = 0 ] || ! grep -q 'not the ring under test' "$TMP/smallring.out"; then
	fail "a ring other than the shipped one did not stop the series: $(cat "$TMP/smallring.out")"
fi
arm refused refuse-uninstall
expect "$st:$(count put)" "1:1" "an arm whose rep's own uninstall was refused"
grep -q 'stayed installed' "$TMP/refused.out" || fail "the refused uninstall did not stop the arm"
# The reclaim the stop names removes it: the console sizes the title as its row's fixture.
out=$("$ROOT/tools/bench/reclaimTestTitles.sh" "$TMP/refused.tsv" 127.0.0.1 --delete 2>&1) || fail "reclaim of the title a refused uninstall left: $out"
expect "$(awk '{ print $1 }' "$FAKE/installed"):$(ownDelete | tail -n 1)" ":title:000400000c000100 ticket" "what reclaim removed of a rep's own title"
: >"$FAKE/installed"

# The rep lock: a live holder's lock refuses other exchanges; a dead holder's is removed, and so is one
# made before this boot (the run directory outlives a restart), whatever process has its pid since. The
# boot time is the wall clock less the uptime, so a step of the wall clock (WSL's: 1-3 s) moves it: this
# one is made 5 s before it, not 1.
lock="$WIRE_RUN_DIR/rep-127.0.0.1"
sleep 30 &
holder=$!
boot=$(_wireBoot) || fail "no boot time on this host"
mkdir "$lock"
echo "token $holder $((boot - 5))" >"$lock/owner"
"$ROOT/tools/device.sh" stats 127.0.0.1 >/dev/null || fail "a lock made before this boot still refused"
[ ! -d "$lock" ] || fail "a lock made before this boot was left"
repLock 127.0.0.1 || fail "repLock on a free console"
read -r _ _ made <"$lock/owner"
repUnlock
unset THREEDROP_REP_OWNER
{ isUint "$made" && [ "$made" -ge "$boot" ]; } || fail "repLock's owner line names no making time in this boot: '$made'"
mkdir "$lock"
echo "token $holder $made" >"$lock/owner"
st=0
out=$("$ROOT/tools/device.sh" stats 127.0.0.1 2>&1) || st=$?
if [ "$st" != 3 ] || [[ "$out" != *"a benchmark run on this PC holds the console at 127.0.0.1 (bench.sh pid "* ]]; then fail "a live rep's lock let an exchange through (exit $st): $out"; fi
THREEDROP_REP_OWNER=token "$ROOT/tools/device.sh" stats 127.0.0.1 >/dev/null || fail "the rep's own exchange was refused"
kill "$holder"
wait "$holder" 2>/dev/null || :
"$ROOT/tools/device.sh" stats 127.0.0.1 >/dev/null || fail "a dead rep's lock still refused"
[ ! -d "$lock" ] || fail "a dead rep's lock was left"

# reclaimTestTitles removes, with their tickets, the titles of reps that reached AM or never learned their
# outcome. The id of a skipped or refused rep is someone else's title, and so is that of a void:ver or
# void:knobs row, which an older bench.sh wrote over any outcome (a put refused exists too). Of those it takes
# only a title the console sizes as its row's fixture, within a unit either way (the fake's 3-unit one is
# 3130252 bytes past its TMD, which AM might round up). A title installed since at an id that a noreply put
# never filled, or that a committed rep's own uninstall emptied, is another client's. It, and a title AM gives
# no size for, are listed with the command that removes them, and left. A refused uninstall is reported, the
# rest still go, and the exit says one stayed. Without --delete it only lists. Its rows are a results file's
# (another layout is refused), with every cell `-` but these three.
blank=()
for name in $BENCH_COLUMNS; do case "$name" in outcome | title | size_bytes) ;; *) blank+=("$name=-") ;; esac done
rm -f "$TMP/ledger.tsv"
: >"$FAKE/installed"
for row in committed:ok:000400000c001000:3130252 rejected:exists:000400000c001100:3130252 \
	unknown:am-finish:000400000c001200:- deadline:000400000c001300:3178496 \
	rejected:am-write:000400000c001400:3130252 void:req:000400000c001500:3130252 \
	skipped:exists:000400000c001600:3130252 skipped:am-info:000400000c001700:3130252 \
	rejected:am-info:000400000c001800:3130252 void:ver:000400000c001900:3130252 \
	void:knobs:000400000c001a00:3130252 noreply:000400000c001b00:3130252 \
	rejected:am-cancel:000400000c001c00:3130252 noreply:000400000c001d00:52428800 \
	committed:ok:000400000c001e00:4194304 deadline:000400000c001f00:2097152; do
	listed=${row##*:} row=${row%:*} # <outcome>:<title>:<the console's size for it, `-` for none>
	benchRow "$TMP/ledger.tsv" "${blank[@]}" outcome="${row%:*}" title="${row##*:}" size_bytes=3145728
	if [ "$listed" = - ]; then
		echo "${row##*:} -" >>"$FAKE/installed"
	else
		echo "${row##*:} 1" >>"$FAKE/installed"
		echo "${row##*:} $listed" >>"$FAKE/sizes"
	fi
done
: >"$FAKE/log"
out=$("$ROOT/tools/bench/reclaimTestTitles.sh" "$TMP/ledger.tsv" 127.0.0.1) || fail "reclaim's listing: $out"
expect "$(count delete):$(grep '^ours: ' <<<"$out" | tr '\n' ' ')" "0:ours: 000400000c001000 ours: 000400000c001300 ours: 000400000c001400 ours: 000400000c001500 ours: 000400000c001b00 ours: 000400000c001c00 " "what reclaim lists, deleting nothing"
for untied in 000400000c001200:- 000400000c001d00:52428800 000400000c001e00:4194304 000400000c001f00:2097152; do
	grep -qxF "untied: ${untied%:*}: the console's size for it, ${untied#*:}, is not its row's fixture's (3145728 bytes): left in place; if it is the bench's, tools/device.sh uninstall 127.0.0.1:$fakePort --ticket ${untied%:*} removes it" <<<"$out" ||
		fail "reclaim's listing of ${untied%:*}, which it cannot tie to its row's fixture: $out"
done
# When the first uninstall is refused, it is reported and the exit says the title stayed. A second run removes
# it, and still exits 1 for the untied titles it leaves.
st=0
faultedOn delete 1 refuse-uninstall "$ROOT/tools/bench/reclaimTestTitles.sh" "$TMP/ledger.tsv" 127.0.0.1 --delete >"$TMP/out" 2>"$TMP/err" || st=$?
if [ "$st" = 0 ] || ! grep -q '000400000c001000: its removal is unconfirmed' "$TMP/err"; then fail "reclaim hid an unconfirmed uninstall (exit $st)"; fi
grep -q '^reclaimTestTitles: 10 installed title(s) from .* (--delete), 4 untied, 1 unconfirmed$' "$TMP/out" || fail "reclaim's summary: $(cat "$TMP/out")"
st=0
"$ROOT/tools/bench/reclaimTestTitles.sh" "$TMP/ledger.tsv" 127.0.0.1 --delete >"$TMP/out" 2>&1 || st=$?
expect "$st:$(tail -n 1 "$TMP/out")" "1:reclaimTestTitles: 5 installed title(s) from $TMP/ledger.tsv (--delete), 4 untied, 0 unconfirmed" "a second reclaim, which leaves only untied titles"
expect "$(awk '{ printf "%s ", $1 }' "$FAKE/installed")" "000400000c001100 000400000c001200 000400000c001600 000400000c001700 000400000c001800 000400000c001900 000400000c001a00 000400000c001d00 000400000c001e00 000400000c001f00 " "what reclaim left installed"
expect "$(ownDelete | tr '\n' ',')" "title:000400000c001000 ticket,title:000400000c001300 ticket,title:000400000c001400 ticket,title:000400000c001500 ticket,title:000400000c001b00 ticket,title:000400000c001c00 ticket,title:000400000c001000 ticket," "reclaim's uninstalls"
# A third word other than --delete is not a silent listing; a file of another layout is not a clean "0
# titles".
: >"$FAKE/log"
run 2 "$ROOT/tools/bench/reclaimTestTitles.sh" "$TMP/ledger.tsv" 127.0.0.1 --delte
printf 'outcome\ttitle\ncommitted:ok\t000400000c001200\n' >"$TMP/twocol.tsv"
run 1 "$ROOT/tools/bench/reclaimTestTitles.sh" "$TMP/twocol.tsv" 127.0.0.1 --delete
said "has another column layout" "reclaim over a file of another layout"
expect "$(awk 'END { print NR }' "$FAKE/log")" 0 "what reclaim sent for a wrong flag or layout"
: >"$FAKE/installed"
rm -f "$fixture"

# capture.sh fetch: every capture comes down from the device's CAPTURE_DIR and leaves; a failed listing fails it.
captures=$(fakeSd "$(fakeAppFile src/bench/capture.c CAPTURE_DIR)")
mkdir -p "$captures"
printf 'bmp' >"$captures/001-x-top.bmp"
: >"$FAKE/log"
out=$("$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/captures") || fail "capture.sh fetch: $out"
# It sends no bench verb (ls, get, delete), so it fetches from a release build's card as well.
expect "$(count bench)" 0 "the bench requests capture.sh fetch sent"
printf 'bmp' >"$captures/002-x-top.bmp"
out=$(faulted release "$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/relcaptures") || fail "capture.sh fetch from a release build: $out"
[ -f "$TMP/relcaptures/002-x-top.bmp" ] || fail "capture.sh fetch from a release build brought nothing: $out"
{ [ -f "$TMP/captures/001-x-top.bmp" ] && [ ! -e "$captures/001-x-top.bmp" ]; } || fail "capture.sh fetch did not move the capture"
if faulted bad-trailer "$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/captures" >/dev/null 2>&1; then
	fail "capture.sh fetch hid a listing that failed"
fi
rm -r "$captures" # never armed
expect "$("$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/captures")" "capture: 0 files in $TMP/captures" "a fetch where capture was never armed"
# A capture's name is the boot's count and its screen's, and the next boot makes the same names, so a fetch
# never replaces a file already in the folder, nor deletes the console's copy for it. Once the old file is
# moved aside, the fetch goes through.
mkdir -p "$captures" "$TMP/kept"
printf 'old-capture' >"$TMP/kept/001-x-top.bmp"
printf 'new-capture' >"$captures/001-x-top.bmp"
st=0
out=$("$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/kept" 2>&1) || st=$?
expect "$st:$(cat "$TMP/kept/001-x-top.bmp"):$(cat "$captures/001-x-top.bmp")" "1:old-capture:new-capture" "a fetch over a capture already fetched"
[[ "$out" == *"already holds 001-x-top.bmp: move or delete them"* ]] || fail "a fetch that would replace a capture did not say so: $out"
mv "$TMP/kept/001-x-top.bmp" "$TMP/kept/001-x-top.old"
out=$("$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/kept") || fail "capture.sh fetch once the old capture was aside: $out"
expect "$(cat "$TMP/kept/001-x-top.bmp"):$([ -e "$captures/001-x-top.bmp" ] && echo left || echo gone)" "new-capture:gone" "a fetch once the old capture was aside"
# Where sips exists (macOS) it writes a PNG beside each capture: one already in the folder is a conflict too,
# and a fetch makes it. A sips of our own goes in front, and it copies the BMP.
mkdir -p "$TMP/png" "$TMP/sipsbin"
# shellcheck disable=SC2016 # the script's own $4 and $6, which expand when sips runs
printf '#!/bin/sh\ncp "$4" "$6"\n' >"$TMP/sipsbin/sips"
chmod +x "$TMP/sipsbin/sips"
printf 'png' >"$TMP/png/001-x-top.png"
printf 'new-capture' >"$captures/001-x-top.bmp"
st=0
out=$(PATH="$TMP/sipsbin:$PATH" "$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/png" 2>&1) || st=$?
expect "$st:$([ -e "$captures/001-x-top.bmp" ] && echo left || echo gone)" "1:left" "a fetch over a PNG already in the folder, with sips"
[[ "$out" == *"already holds 001-x-top.png: move or delete them"* ]] || fail "a fetch that would replace a PNG did not say so: $out"
rm "$TMP/png/001-x-top.png"
out=$(PATH="$TMP/sipsbin:$PATH" "$ROOT/tools/bench/capture.sh" fetch 127.0.0.1 "$TMP/png") || fail "capture.sh fetch with sips: $out"
expect "$(cat "$TMP/png/001-x-top.bmp"):$(cat "$TMP/png/001-x-top.png")" "new-capture:new-capture" "a capture and the PNG sips made beside it"
rm -r "$captures"

# probe.sh: rx moves its bytes, and a body cut short is `short`; mb and hw name the transfer, and hw
# refuses a listing that miscounts its rows; tx is read as get is, over /dev/tcp from a real port
# and never through nc; --smoke checks values.
probe="$ROOT/tools/bench/probe.sh"
mkdir "$TMP/nonc"
printf '#!/bin/sh\nexit 1\n' >"$TMP/nonc/nc"
chmod +x "$TMP/nonc/nc"
out=$("$probe" rx 127.0.0.1 2) || fail "rx: $out"
# 2097152 bytes in 1000 ms: 2.0972 decimal MB/s (not MiB, not over its busy_ms).
[[ "$out" == "response "*" bench ok ok 00000000 rx bytes=2097152 "*"  (2.0972 MB/s) inst=$(inst)" ]] || fail "rx said: $out"
rxId=$(awk '{ print $2 }' <<<"$out")
out=$("$probe" mb 127.0.0.1) || fail "mb: $out"
[[ "$out" == "req=$rxId dir=rx rows=2 "* ]] || fail "mb after rx said: $out"
out=$("$probe" hw 127.0.0.1) || fail "hw: $out"
[ "$out" = "req=$rxId rows=3 calls=2 gaps=1 gap_min_ms=30 call_cols=kind,start_us,end_us,k gap_cols=g,quiet_us,call_us,end_us
w 1000 178000 1
d 178100 204000 1
g 5000 6000 45000" ] || fail "hw after rx said: $out"
st=0
out=$(faulted hw-rows "$probe" hw 127.0.0.1 2>&1 >/dev/null) || st=$?
[[ "$st" = 1 && "$out" == "probe: hw said rows=4 and sent 3" ]] || fail "hw passed a listing short of its rows (exit $st): $out"
st=0
out=$(head -c 1000000 /dev/zero | "$probe" rx 127.0.0.1 1 - 2>&1 >/dev/null) || st=$?
[[ "$st" = 4 && "$out" == "probe: "*" bench rejected short d963f802 rx bytes=1000000 "* ]] || fail "a short rx body (exit $st): $out"
# A body on stdin goes over /dev/tcp too (Git Bash has no nc).
out=$(head -c 1048576 /dev/zero | BENCH_DEADLINE=20 devtcp benchRx 127.0.0.1 "$fakePort" 1 -) || fail "an rx body on stdin over /dev/tcp: $out"
[[ "$out" == *" bench ok ok 00000000 rx bytes=1048576 "* ]] || fail "an rx body on stdin over /dev/tcp: $out"
out=$(PATH="$TMP/nonc:$PATH" "$probe" tx 127.0.0.1 1) || fail "tx: $out"
[[ "$out" == "trailer "*" bench ok ok 00000000 tx bytes=1048576 "*" MB/s) inst=$(inst)" ]] || fail "tx said: $out"
# 010 units are ten, as rx and tx ask for them and as rx sends them: bash would read 010 as eight.
: >"$FAKE/log"
out=$("$probe" rx 127.0.0.1 010) || fail "rx 010: $out"
[[ "$out" == *" bench ok ok 00000000 rx bytes=10485760 "* ]] || fail "rx 010 moved other than ten units: $out"
grep -q '^bench [0-9a-f]* rx 10$' "$FAKE/log" || fail "rx 010 was asked for as: $(cat "$FAKE/log")"
out=$(PATH="$TMP/nonc:$PATH" "$probe" tx 127.0.0.1 010) || fail "tx 010: $out"
[[ "$out" == "trailer "*" bench ok ok 00000000 tx bytes=10485760 "* ]] || fail "tx 010 moved other than ten units: $out"
grep -q '^bench [0-9a-f]* tx 10$' "$FAKE/log" || fail "tx 010 was asked for as: $(cat "$FAKE/log")"
out=$("$probe" file 127.0.0.1 1) || fail "file: $out"
[[ "$out" == *" bench ok ok 00000000 file bytes=1048576 chunk=1048576 writes=1 "*" bytes_done=1048576 reads=0 read_first_ms=0 read_steady_ms=0 read_steady_max_ms=0 read_done=0" ]] || fail "file said: $out"
out=$("$probe" --smoke 127.0.0.1) || fail "--smoke against a sound device: $out"
[ "$(grep -c '^ok ' <<<"$out")" = 6 ] || fail "--smoke checked: $out"
# --smoke reads each value: a tx of half the unit asked fails it, though its header, body and trailer
# agree (so benchTx's status is 0), and a key the device appended to state does not.
st=0
out=$(faulted tx-short "$probe" --smoke 127.0.0.1) || st=$?
[[ "$st" = 1 && "$out" == *"FAIL tx 1: "* && "$(grep -c '^ok ' <<<"$out")" = 5 ]] || fail "--smoke passed a tx of half a unit for one (exit $st): $out"
out=$(faulted state-extra "$probe" --smoke 127.0.0.1) || fail "--smoke failed a state with a key appended: $out"
[ "$(grep -c '^ok ' <<<"$out")" = 6 ] || fail "--smoke checked a state with a key appended: $out"
printf 'unverified' >"$TMP/unverified"
"$probe" nohash 127.0.0.1 1 >/dev/null || fail "nohash 1"
out=$("$ROOT/tools/device.sh" put 127.0.0.1 "$TMP/unverified" /unverified 2>&1) || fail "a nohash put: $out"
[[ "$out" == *" digest=skipped"* && "$out" == *unverified* ]] || fail "a nohash put was not said unverified: $out"
"$probe" nohash 127.0.0.1 0 >/dev/null || fail "nohash 0"
echo 70000 >"$FAKE/knob-rcvbuf" # a knob left set
"$probe" --smoke 127.0.0.1 >/dev/null && fail "--smoke passed a knob off its default"
rm "$FAKE/knob-rcvbuf"
# Every knob the device takes is one probe.sh reads: the fake's list is src/bench/bench.c's own.
while read -r knob def _; do
	out=$("$probe" "$knob" 127.0.0.1 2>&1) || fail "probe.sh $knob: $out"
	[[ "$out" == *" bench ok ok 00000000 $knob applied=$def default=$def"* ]] || fail "probe.sh $knob said: $out"
done <"$FAKE/knobs"
# A set of a knob with a word too many is refused as a grammar, naming the knob as the device does.
out=$("$probe" rcvbuf 127.0.0.1 1 2 2>&1) || :
[[ "$out" == *" bench rejected grammar 00000000 rcvbuf"* ]] || fail "a set of rcvbuf with two values was refused as: $out"
# Checks led, disc and fscalls through probe.sh, against the fake's replies, whose keys are the device's
# format strings'.
out=$("$probe" led 127.0.0.1) || fail "led: $out"
expect "$(fakeKeys "$out")" "$(fakeKeys "$(fakeFormat bench/bench.c ' led rgb=')")" "the keys probe.sh led shows"
out=$("$probe" disc 127.0.0.1) || fail "disc: $out"
expect "$(fakeKeys "$out")" "$(fakeKeys "$(fakeFormat discover.c ' disc=%s ')")" "the keys probe.sh disc shows"
out=$("$probe" fscalls 127.0.0.1) || fail "fscalls: $out"
[[ "$out" == *" bench ok ok 00000000 fscalls req=- orphan_us=- open_us=- "*" rename_us=- log_us=-" ]] || fail "fscalls said: $out"
# Each sub sends its own request and nothing before it, so a release build is told from a bench build only
# once it refuses (below), never by a bare `bench` ahead of every command. --smoke runs six subs.
for leg in mem "rx 1" "tx 1" "file 1" state rcvbuf --smoke; do
	# shellcheck disable=SC2086 # the sub and its arguments
	set -- $leg
	: >"$FAKE/log"
	"$probe" "$1" 127.0.0.1 "${@:2}" >/dev/null 2>&1 || fail "probe.sh $leg"
	expect "$(count bench)" "$([ "$leg" = --smoke ] && echo 6 || echo 1)" "the bench requests probe.sh $leg sent"
done

# With no address given, an arm runs at the first remembered one this key's console answers at, on
# its port; with none remembered it is refused before anything goes, the console's screen last.
fakeCia "$fixture" 000400000c000100 1 3145728
printf '127.0.0.1 1 1\n127.0.0.1 %s 1\n' "$fakePort" >"$WIRE_BOOK"
: >"$FAKE/log"
st=0
BENCH_RESULTS="$TMP/remembered.tsv" THREEDROP_PORT=1 "$ROOT/tools/bench/bench.sh" remembered 1 3 >"$TMP/remembered.out" 2>&1 || st=$?
expect "$st:$(count put):$(column remembered outcome):$(column remembered mb | cut -c1-4)" "0:1:committed:ok:req=" "an arm at a remembered address"
rm -f "$WIRE_BOOK"
: >"$FAKE/log"
st=0
BENCH_RESULTS="$TMP/unremembered.tsv" "$ROOT/tools/bench/bench.sh" unremembered 1 3 >"$TMP/unremembered.out" 2>&1 || st=$?
expect "$st:$(count put):$(tail -n 1 "$TMP/unremembered.out")" "4:0:bench: $WIRE_SCREEN" "an arm with no address"

# probe.sh evidence: a finished Testing Tool run passes, and each missing piece or another title fails.
good="$TMP/evidence.txt"
printf 'build 3Drop Testing Tool\ntitle 000400000ff3e100\n# launch\n0 ff00ff\n100 err d8a0abfa\n# opens\n0 00000000 812 us\n12000 ff00ff\ndone\n' >"$good"
out=$("$probe" evidence "$good" 000400000FF3E100) || fail "a complete evidence file: $out"
[[ "$out" == *"tool ok title=000400000ff3e100 reads=2 of 3" ]] || fail "evidence said: $out"
for cut in '^done$' '^build ' '^title '; do
	grep -v "$cut" "$good" >"$TMP/cut.txt"
	st=0
	out=$("$probe" evidence "$TMP/cut.txt" 2>&1 >/dev/null) || st=$?
	[[ "$st" = 1 && "$out" == "probe: no complete evidence"* ]] || fail "evidence without $cut exited $st, saying: $out"
done
st=0
"$probe" evidence "$good" 0004000003d37200 >/dev/null || st=$?
[ "$st" = 1 ] || fail "evidence of another title exited $st"
# A tool that read no color has not done its job, though it finished; `title:<id>` names a title too.
printf 'build 3Drop Testing Tool\ntitle 000400000ff3e100\n0 err d8a0abfa\ndone\n' >"$TMP/noread.txt"
st=0
out=$("$probe" evidence "$TMP/noread.txt" 2>&1 >/dev/null) || st=$?
expect "$st:$out" "1:probe: it read no color (reads=0 of 1)" "evidence of a tool that read no color"
out=$("$probe" evidence "$good" title:000400000FF3E100) || fail "evidence under title:<id>: $out"
# probe.sh tool puts the marker, and reads the evidence, where the Testing Tool has them.
echo "000400000ff3e100 1" >"$FAKE/installed"
out=$("$probe" tool 127.0.0.1 000400000ff3e100 2>&1) || fail "tool: $out"
[[ "$out" == *"tool ok title=000400000ff3e100 reads=1 of 1" ]] || fail "tool said: $out"
[[ "$out" != *"testing-tool.auto"* ]] || fail "a tool run that finished named its marker: $out"
out=$("$probe" tool 127.0.0.1 title:000400000FF3E100 2>&1) || fail "tool under title:<id>: $out"
[[ "$out" == *"tool ok title=000400000ff3e100 reads=1 of 1" ]] || fail "tool under title:<id> said: $out"
# The Testing Tool is a title of its own and takes no bench verb: a release build runs it too.
out=$(faulted release "$probe" tool 127.0.0.1 000400000ff3e100 2>&1) || fail "tool against a release build: $out"
[[ "$out" == *"tool ok title=000400000ff3e100 reads=1 of 1" ]] || fail "tool against a release build said: $out"
: >"$FAKE/installed"
# A malformed id is refused before the marker goes to the card: no put, no run, and none left behind.
: >"$FAKE/log"
st=0
"$probe" tool 127.0.0.1 000400000ff3e10 >/dev/null 2>&1 || st=$?
expect "$st:$(count put)$(count run):$([ -e "$(fakeSd "$FAKE_TOOL_MARKER")" ] && echo marker || echo none)" "2:00:none" "a tool run for a malformed title id"
# The tool takes its marker as it starts. A launch the console refused never started it: the marker
# is taken back. One it accepted that did not come up leaves it unknown whether the tool runs, so the
# marker stays, named, with the command that removes it.
marker=$(fakeSd "$FAKE_TOOL_MARKER")
st=0
out=$("$probe" tool 127.0.0.1 000400000ff3e100 2>&1) || st=$?
expect "$st:$([ -e "$marker" ] && echo marker || echo none)" "1:none" "a tool launch the console refused"
[[ "$out" != *"testing-tool.auto"* ]] || fail "a refused tool launch named a marker it had taken back: $out"
echo "000400000ff3e100 1" >"$FAKE/installed"
st=0
out=$(faulted tool-stuck "$probe" tool 127.0.0.1 000400000ff3e100 2>&1) || st=$?
expect "$([ "$st" != 0 ] && echo failed):$([ -e "$marker" ] && echo marker || echo none)" "failed:marker" "a tool that did not come up"
[[ "$out" == *"/3ds/3drop/testing-tool.auto is still on the card"*"tools/device.sh delete 127.0.0.1 /3ds/3drop/testing-tool.auto"* ]] ||
	fail "a tool that did not come up did not name its marker: $out"
rm -f "$marker"
: >"$FAKE/installed"
# Run by relative paths under an exported CDPATH (cd searches it before the current folder), the kit
# still finds its own tree.
mkdir -p "$TMP/decoy/tools/bench"
out=$(cd "$ROOT" && CDPATH="$TMP/decoy" tools/bench/bench.sh --prefix-only "$TMP/none.tsv" 2>&1) || fail "bench.sh under CDPATH: $out"
expect "$out" c000 "bench.sh under CDPATH"
out=$(cd "$ROOT" && CDPATH="$TMP/decoy" tools/bench/probe.sh evidence "$good" 2>&1) || fail "probe.sh under CDPATH: $out"

# A refusal is said on stderr, as by every tool: a tx the console stopped, and an rx refused before
# its body, whose sender the refusal cut short.
st=0
out=$(faulted stopped "$probe" tx 127.0.0.1 1 2>&1 >/dev/null) || st=$?
[[ "$st" = 1 && "$out" == "probe: trailer "*" bench rejected read c8804464 "* ]] || fail "a stopped tx (exit $st): $out"
st=0
: >"$FAKE/log"
out=$(faulted rejected-busy "$probe" rx 127.0.0.1 1 2>&1 >/dev/null) || st=$?
[[ "$st" = 1 && "$out" == "probe: response "*" bench rejected busy 00000000"* ]] || fail "an rx refused before its body (exit $st): $out"
# A refusal that is not a release build's is said as it is, with nothing asked after it: probe.sh's and capture.sh's.
expect "$(count bench)" 1 "the bench requests an rx refused as busy sent"
: >"$FAKE/log"
st=0
out=$(faulted rejected-busy "$ROOT/tools/bench/capture.sh" on 127.0.0.1 2>&1) || st=$?
[[ "$st" = 1 && "$out" == "capture: response "*" bench rejected busy 00000000"* ]] || fail "capture.sh on refused as busy (exit $st): $out"
expect "$(count bench)" 1 "the bench requests capture.sh on, refused as busy, sent"
# A release build is not a bench build: it is told so once, with how to get one (its refusals of a sub read as
# an old 3Drop, which is not the cause). It is asked the bare `bench` only once a sub is refused: that is the
# second request of rx and mem. state and --smoke send the bare one themselves, which a release build refuses
# as `unsupported`: they ask nothing more.
for leg in "rx 1:2" "mem:2" "state:1" "--smoke:1"; do
	# shellcheck disable=SC2086 # the sub and its arguments
	set -- ${leg%:*}
	: >"$FAKE/log"
	st=0
	out=$(faulted release "$probe" "$1" 127.0.0.1 "${@:2}" 2>&1 >/dev/null) || st=$?
	expect "$st:$out:$(count bench)" "1:probe: not a bench build: THREEDROP_TEST_VERBS=1 tools/deploy.sh 127.0.0.1:${leg#*:}" \
		"probe.sh ${leg%:*} against a release build, and the bench requests it sent"
done
: >"$FAKE/log"
st=0
out=$(faulted release "$ROOT/tools/bench/capture.sh" on 127.0.0.1 2>&1) || st=$?
expect "$st:$out:$(count bench)" "1:capture: not a bench build: THREEDROP_TEST_VERBS=1 tools/deploy.sh 127.0.0.1:2" "capture.sh on against a release build"

# A console that accepts and never answers holds each sub only until its deadline, never for its own
# timeouts (60 s each here). All at once: each holds its own connection, which the fake serves alone. On Git Bash
# ten subs ending their deadlines at once took 16 to 39 s on a PC in use (each kill runs ps -ef, and a process
# costs 70 to 250 ms there), so the timeouts, the alarm and the bound below are five times as long there.
muteScale=$((gitBash ? 5 : 1))
echo mute >"$FAKE/mode"
subs=("rx 1" "tx 1" state mem "sndbuf 2" mb hw "file 1" "tool 000400000ff3e100" --smoke) probers=()
for leg in "${subs[@]}"; do
	(
		# shellcheck disable=SC2086 # the sub and its arguments
		set -- $leg
		t0=$SECONDS st=0
		# alarm: a regression fails here instead of hanging check.sh.
		THREEDROP_TIMEOUT=$((60 * muteScale)) BENCH_DEADLINE=2 perl -e 'alarm '$((30 * muteScale))'; exec @ARGV' "$probe" "$1" 127.0.0.1 "${@:2}" >/dev/null 2>&1 || st=$?
		echo "$st $((SECONDS - t0))"
	) >"$TMP/mute.${leg%% *}" &
	probers+=("$!")
done
wait "${probers[@]}"
for leg in "${subs[@]}"; do
	read -r st took <"$TMP/mute.${leg%% *}"
	case "$st" in 0 | 142) fail "probe.sh $leg against a mute device exited $st" ;; esac # 142: the alarm
	[ "$took" -le $((20 * muteScale)) ] || fail "a mute device held probe.sh $leg $took s"
done
# Ctrl-C or a TERM to probe.sh alone ends the exchange and the watchdog that its $(...) runs, which no trap
# sees. A stop walks each leg's process tree a level at a time, each level a ps -ef. On Git Bash that took 3
# to 5 s a level with the 14 legs at once, so a loop seven levels down (the nonce fetch of tool and sndbuf)
# was reached 20 to 30 s in and the whole stop took 30 to 55 s. A request the mute fake lets go of before the
# walk reaches that loop ends its exchange on its own, and the loop, not yet stopped, asks again (`stats <new
# id> op` is how an INT or a TERM check failed on a loaded PC). What this checks is the stop's order, a loop
# stopped before its exchange is killed, not how fast a PC walks. So there the fake holds each request, and
# each leg runs, ten times as long as the 30 and 60 s elsewhere.
holdScale=$((gitBash ? 10 : 1))
legs=("INT:rx 2" "INT:tx 2" INT:state "INT:tool 000400000ff3e100" "TERM:rx 2" "TERM:tx 2" TERM:state TERM:mem
	"TERM:sndbuf 2" TERM:mb TERM:hw "TERM:file 1" "TERM:tool 000400000ff3e100" TERM:--smoke) probers=() left=""
: >"$FAKE/log"
echo $((30 * holdScale)) >"$FAKE/hold"
for leg in "${legs[@]}"; do
	# shellcheck disable=SC2086 # the sub and its arguments
	set -- ${leg#*:}
	BENCH_DEADLINE=$((60 * holdScale)) perl -e '$SIG{INT} = "DEFAULT"; setpgrp(0, 0); exec(@ARGV) or die' "$probe" "$1" 127.0.0.1 "${@:2}" \
		>/dev/null 2>&1 &
	probers+=("$!")
done
# Each sends one request, which the mute fake holds.
tryFor 600 hasLines "$FAKE/log" "${#legs[@]}" || :
sleep 0.5
sent=$(wc -l <"$FAKE/log")
for i in "${!legs[@]}"; do
	# A leg gone before its signal is said, not a silent end of the test by set -e.
	if [ "${legs[i]%%:*}" = INT ]; then kill -INT -- "-${probers[i]}"; else kill -TERM "${probers[i]}"; fi ||
		fail "probe.sh ${legs[i]} had ended before its signal"
done
# Every leg's trap runs its ps -ef at once, which is slow: 6.8 s of 10 under the load of a check.sh run, and
# on Git Bash 30 to 55 s for the whole stop. The wait counts pauses, each a process there, so it outlasts
# that.
tryFor 600 gone "${probers[@]/#/-}" || :
for i in "${!legs[@]}"; do
	! kill -KILL -- "-${probers[i]}" 2>/dev/null || left+=" ${legs[i]}"
done
[ -z "$left" ] || fail "probe.sh left it, its exchange or its watchdog running 30 s on after:$left"
wait "${probers[@]}" || :
rm -f "$FAKE/hold"
expect "$(tail -n +$((sent + 1)) "$FAKE/log")" "" "what probe.sh sent after an INT or a TERM"
rm -f "$FAKE/mode"
wait "$dead08" || :
expect "$(cat "$TMP/dead08") $(cat "$TMP/dead08.st")" "124 1 0" "a deadline of 08 seconds, alarmed at 25 s"
echo "testBenchRep: PASS"

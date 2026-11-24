# shellcheck shell=bash
ROOT="$(CDPATH='' cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TESTS="$ROOT/tools/tests"
STUBS="$TESTS/stubs"
TEST_NAME="$(basename "$0" .sh)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || :' EXIT # under set -e a failed cleanup would turn a pass into exit 1
# Only the tests' own settings apply (a shell that sourced .env holds the key), and no search reaches a real
# console.
# BENCH_* is unset too because a shell's BENCH_KNOBS would set the knobs of every test arm.
unset "${!THREEDROP_@}" "${!BENCH_@}"
export THREEDROP_KEY=abcd-efgh-2345 THREEDROP_NAME='' THREEDROP_PROBE=127.0.0.1
# A failed exchange is retried 0.1 s later instead of after wire.sh's 1 s, which held up the slowest tests the
# most (testAnswers and testPieces time the default).
export _THREEDROP_RETRY_PAUSE=0.1
# realSleep: the sleep binary by its path, for a test's own sleep() to call. On bash 3.2 (macOS's) a `command sleep`
# from a function ends the watchdog loop (watchOwner) at its first tick, and no timeout fires.
realSleep=$(type -P sleep)
export realSleep

fail() {
	echo "$TEST_NAME: FAIL -- $*" >&2
	exit 1
}

# msNow: the monotonic clock in milliseconds, for bounds that SECONDS' whole seconds would blur and that a
# step of the wall clock (an NTP or VM time sync) cannot stretch or shrink. A perl without CLOCK_MONOTONIC
# reads the wall clock.
msNow() { perl -MTime::HiRes -e 'my $t = eval { Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC()) } // Time::HiRes::time(); printf "%d\n", $t * 1000'; }

# clockOffset: the wall clock minus the monotonic clock, in ms, which moves only when the wall clock is
# stepped (bash has no monotonic clock to read, so each read runs one perl). It is 0 for a perl without
# CLOCK_MONOTONIC.
clockOffset() { perl -MTime::HiRes -e 'my $m = eval { Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC()) }; printf "%d\n", defined $m ? (Time::HiRes::time() - $m) * 1000 : 0' || echo 0; }
# steadyRun: the condition of `until steadyRun; do <exchanges>; done`. It goes around a sequence in which a
# step of the wall clock would upset the exact order or count in its log, or its exit status. The tools stamp
# a kept nonce with the wall clock, and a nonce stamped ahead reads as stale, which adds an extra stats.
# steadyRun is false before the first run, and after a run the clock was stepped across (its offset to the
# monotonic clock moved over 0.2 s), so the run goes again, three runs at most. It is true after a run that
# held steady. A defect fails every run, but a clock step fails only one. A run sets up its own state (the
# log, the nonce) so that it can run again, and its checks follow the loop, because a `fail` inside it would
# end the test before the run could go again.
clockMark="" clockRuns=0
steadyRun() {
	local at moved
	at=$(clockOffset)
	if [ -n "$clockMark" ]; then
		moved=$((at - clockMark))
		if [ "${moved#-}" -le 200 ] || [ "$clockRuns" -ge 3 ]; then
			clockMark="" clockRuns=0
			return 0
		fi
	fi
	clockMark=$at clockRuns=$((clockRuns + 1))
	return 1
}

# original <function>: saves the function's definition as it is now under the name real_<function>
# (real__wireLs for _wireLs), for the wrapper that a test then defines under the same name to call. A wrapper
# defined in a subshell stays in that subshell.
original() { eval "real_$(declare -f "$1")"; }

# skip <what>: a check this host cannot make, which run.sh lists beside its summary.
skip() { echo "$TEST_NAME: SKIP $*"; }

# expect <got> <want> <what>
expect() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }

# run <status> <command...>: its stdout in $TMP/out and stderr in $TMP/err.
run() {
	local st=0
	"${@:2}" >"$TMP/out" 2>"$TMP/err" || st=$?
	[ "$st" = "$1" ] || fail "$* exited $st, want $1: $(cat "$TMP/err")"
}

# waitFor <file> [seconds]: waits until the file holds something. It fails, naming the file, once the bound
# (30 s) has passed.
waitFor() {
	local end=$(($(msNow) + ${2:-30} * 1000))
	while [ ! -s "$1" ]; do
		[ "$(msNow)" -lt "$end" ] || fail "$1 was still empty after ${2:-30} s"
		sleep 0.05
	done
}
# tryFor <n> <command...>: runs the command every 0.05 s until it succeeds, with n pauses at most. Returns 1
# if it never succeeded.
tryFor() {
	local n=$1
	shift
	until "$@"; do
		((n--)) || return 1
		sleep 0.05
	done
}
# gone <pid|-pgid...>: none of them runs.
gone() {
	local p
	for p; do ! kill -0 -- "$p" 2>/dev/null || return 1; done
}
# hasLines <file> <n>: it holds n lines or more.
hasLines() { [ $(($(wc -l <"$1"))) -ge "$2" ]; }

# landed <file> <copy> <what> <command...>: the command succeeds and leaves <copy> holding <file>'s bytes.
landed() {
	local st=0
	"${@:4}" >/dev/null || st=$?
	[ "$st" = 0 ] || fail "$3: exited $st"
	cmp -s "$1" "$2" || fail "$3: $2 is not $1 byte for byte"
}

# opensslShim: writes $TMP/shim/openssl, an openssl that logs each call's arguments to $TMP/openssl.log. A
# dgst call's line then continues with ` |` and the sizes of its inputs: its files', or its stdin's, which the
# shim spools to count.
opensslShim() {
	mkdir -p "$TMP/shim"
	{
		printf '#!/usr/bin/env bash\nlog=%q real=%q\n' "$TMP/openssl.log" "$(type -P openssl)"
		cat <<'EOF'
sizes=""
if [ "$1" = dgst ]; then
	for a in "${@:2}"; do [[ "$a" == -* ]] || sizes+=" $(($(wc -c <"$a")))"; done
	if [ -z "$sizes" ]; then
		in=$(mktemp "${log%/*}/shim/in.XXXXXX") && cat >"$in" && sizes=" $(($(wc -c <"$in")))" && exec <"$in"
	fi
fi
echo "$*${sizes:+ |$sizes}" >>"$log"
exec "$real" "$@"
EOF
	} >"$TMP/shim/openssl"
	chmod +x "$TMP/shim/openssl"
}

# silent <command...>: it fails and prints nothing.
silent() {
	local out
	if out=$("$@" 2>/dev/null) || [ -n "$out" ]; then fail "$* answered '$out'"; fi
}

# said <text> [what]: checks that $TMP/err, a command's stderr, holds the text. saidLine checks for it as a
# whole line.
said() { grep -qF -- "$1" "$TMP/err" || fail "${2:+$2: }it never said '$1', but: $(cat "$TMP/err")"; }
saidLine() { grep -qxF -- "$1" "$TMP/err" || fail "${2:+$2: }it never said '$1', but: $(cat "$TMP/err")"; }

# A test that runs past this fails instead of hanging check.sh. It is a hang limit, not a speed bound. The
# heaviest tests take 2 to 4 minutes in a busy check.sh run on 8 cores and about twice that on 4. Git Bash
# runs them longer still (testWire alone takes 18 minutes there on 8 cores, and testBenchRep and testBridge
# take 13 and 12), and longer again beside other work (testBenchRep needed over an hour, with a process
# costing 70 to 200 ms there).
case $OSTYPE in msys* | cygwin*) gitBash=1 ;; *) gitBash=0 ;; esac # a test's own bounds may need it too
TEST_TIMEOUT=$((gitBash ? 7200 : 900))
# withTimeout <seconds> <command...>: at the deadline the command is killed along with every process it
# started, and the status is 124.
withTimeout() {
	perl -MPOSIX=:signal_h -e '
		my $seconds = shift;
		my %number = (HUP => 1, INT => 2, QUIT => 3, TERM => 15);
		# The signals handled below are held from before the fork until their handlers are in, so one sent as
		# the command starts is handled late, never lost (INT and QUIT arrive ignored, and a HUP or TERM would
		# end this wrapper and leave the command running). This relies on a blocked signal staying pending
		# even when its action is to ignore it, as Linux has it; macOS may drop an ignored INT or QUIT.
		my ($held, $was) = (POSIX::SigSet->new(SIGHUP, SIGINT, SIGQUIT, SIGTERM), POSIX::SigSet->new);
		sigprocmask(SIG_BLOCK, $held, $was);
		my $pid = fork() // die "fork: $!";
		if ($pid == 0) { setpgrp(0, 0); sigprocmask(SIG_SETMASK, $was); exec(@ARGV) or die "exec $ARGV[0]: $!" }
		# Its own group never hears the terminal, so a stop of this wrapper ends it, an ignored Ctrl-C too.
		# The pid is killed as well, for a signal handled before the command has made its group.
		$SIG{$_} = sub { kill("KILL", -$pid, $pid); exit(128 + $number{$_[0]}) } for keys %number;
		$SIG{ALRM} = sub { kill("KILL", -$pid); print STDERR "killed after $seconds s\n"; exit 124 };
		sigprocmask(SIG_SETMASK, $was);
		alarm($seconds);
		waitpid($pid, 0);
		print STDERR "died of signal ", $? & 127, "\n" if $? & 127; # else a crash is an empty log and a 139
		exit($? & 127 ? 128 + ($? & 127) : $? >> 8);' "$@"
}

# inGroup <signal> <settle> <file> <n> <prefix> <command...>: signals the command's group once the file has n
# lines starting with the prefix. A group still running 10 s later fails and is killed. Its output goes to
# $TMP/inGroup.out.
inGroup() {
	perl -MPOSIX=WNOHANG -e '
		my ($sig, $settle, $file, $lines, $prefix, $out) = splice(@ARGV, 0, 6);
		my $pid = fork() // die "fork: $!";
		# INT and QUIT taken back: bash starts a background test with them ignored.
		if ($pid == 0) {
			$SIG{INT} = $SIG{QUIT} = "DEFAULT";
			setpgrp(0, 0);
			open(STDOUT, ">", $out) && open(STDERR, ">&", \*STDOUT) or die "$out: $!";
			exec(@ARGV) or die "exec $ARGV[0]: $!";
		}
		sub held { open(my $f, "<", $file) or return 0; return grep({ index($_, $prefix) == 0 } <$f>) >= $lines }
		for (1 .. 1200) { last if held(); select(undef, undef, undef, 0.1) }
		select(undef, undef, undef, $settle);
		kill($sig, -$pid);
		# Timed from the signal, the leader reaped as it goes: an orphan can hold the leader up too.
		for (1 .. 100) { waitpid($pid, WNOHANG); exit 0 unless kill(0, -$pid); select(undef, undef, undef, 0.1) }
		print STDERR grep({ (split)[1] == $pid } `ps -A -o pid= -o pgid= -o args= 2>/dev/null`);
		kill("KILL", -$pid);
		exit 1' "${@:1:5}" "$TMP/inGroup.out" "${@:6}" || fail "$1 left the processes above running 10 s on: ${*:6}"
}

# ccTest <out> <cc args...>: builds from ROOT against the stubs, the weak fakes and mbedtls (2.28 on Linux,
# keg-only mbedtls@3 on macOS via stubs/mbedtlsCompat.h), with the Makefile's SELF_TITLE_ID and with TESTS_DIR
# defined.
ccTest() {
	local out="$1" mbed host=("-Wl,-dead_strip") self
	shift
	# shellcheck source=tools/cia.sh
	self=$(. "$ROOT/tools/cia.sh" && selfTitleID) || fail "no 3Drop title id"
	if [ "$(uname)" = Darwin ]; then
		mbed="$(brew --prefix mbedtls@3 2>/dev/null)" || :
		[ -e "$mbed/include/mbedtls/sha256.h" ] || fail "brew install mbedtls@3"
		host+=(-I"$mbed/include" -L"$mbed/lib")
	else # Linux/gcc: LP64 u64 is `unsigned long`, so the device's %llu formats do not type-check,
		# and glibc hides fakes.c's pread, pwrite and ftruncate under -std=c11.
		host=("-Wl,--gc-sections" -Wno-format -Wno-format-truncation -D_DEFAULT_SOURCE)
	fi
	# -Wno-format-zero-length is passed because a reply with no fields formats "" (src/protocol.h reply()).
	(cd "$ROOT" && cc -std=c11 -Wall -Wextra -Werror -pedantic -Wno-unused-parameter \
		-Wno-format-zero-length -ffunction-sections -fdata-sections "${host[@]}" -I"$STUBS" \
		-DSELF_TITLE_ID="0x${self}ULL" -DTESTS_DIR="\"$TESTS\"" \
		-include mbedtlsCompat.h -Isrc "$STUBS/fakes.c" "$@" -lmbedcrypto -o "$out")
}

# ccArgs <test.c>: its `// cc:` words and those of each test it includes.
ccArgs() {
	sed -n 's|^// cc: ||p' "$1"
	sed -n 's|^#include "\(test[A-Za-z]*\.c\)"$|\1|p' "$1" | while read -r inc; do ccArgs "$TESTS/$inc"; done
}

# ccRun <test.c>: builds the test with ccArgs and runs it under TEST_TIMEOUT, with ./sdmc: as the SD card.
ccRun() {
	local name dir
	name="$(basename "$1" .c)"
	dir="$TMP/$name.run"
	mkdir -p "$dir/sdmc:/3ds/3drop"
	# shellcheck disable=SC2046 # a word list
	ccTest "$dir/$name" "$1" $(ccArgs "$1") &&
		(cd "$dir" && withTimeout "$TEST_TIMEOUT" "./$name")
}

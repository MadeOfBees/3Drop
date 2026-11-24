#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2034,SC2153,SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# Finding the console: remembered addresses, its name and the probe, each proved before it is taken.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
device=$ROOT/tools/device.sh
# The probe's UDP socket uses the same port number as the fake's TCP port (the console's 1337 is both).
# Windows keeps ranges of ports away from programs (Hyper-V and WSL reserve ranges of 100 ports each, 4% of
# the ports a listener is given on one PC). A fake whose port lies in one of those ranges has no probe ("udp:
# Permission denied", and the test waits 30 s for it). So its listener is restarted on another port until the
# port binds on UDP as well.
udpFree() { perl -MIO::Socket::INET -e 'exit(IO::Socket::INET->new(Proto => "udp", LocalAddr => "127.0.0.1", LocalPort => $ARGV[0]) ? 0 : 1)' "$1"; }
for tries in 1 2 3 4 5 6 7 8 9 10 11 12; do
	udpFree "$fakePort" && break
	[ "$tries" -lt 12 ] || fail "none of 12 ports of 127.0.0.1 binds on UDP as well as TCP; the last was $fakePort"
	killTree "$fakeListenPid"
	fakeListen
	export THREEDROP_PORT=$fakePort
done
fake="127.0.0.1 $fakePort"
other=another-key-1234
# book [file] prints the remembered addresses in order, each as `<ip> <port>|`.
book() { cut -d' ' -f1,2 "${1:-$WIRE_BOOK}" 2>/dev/null | tr '\n' '|'; }
# remember <ip> <port> [epoch] stores the address the way a verified reply does.
remember() { _wireRemember "$1" "$2" "${3:-$(date +%s)}"; }
# answering <text> sets answerPort to a 127.0.0.1 port where the first client to connect gets the text and
# then a close. The listener is gone after that, or after a minute. It answers only once it has the request's
# first line, as a console does. A banner and a close the instant it accepts can make a connect on Git Bash
# fail with "Connection reset by peer" when the host is loaded, and the proof then says that nothing answers.
answering() {
	rm -f "$TMP/answerPort"
	perl -MIO::Socket::INET -e '
		my $s = IO::Socket::INET->new(Listen => 1, LocalAddr => "127.0.0.1", LocalPort => 0) or die "listen: $!";
		open(my $f, ">", $ARGV[1]) or die; print $f $s->sockport; close $f;
		alarm 60;
		my $c = $s->accept or exit;
		my $request = <$c>;
		print $c "$ARGV[0]\n";
		shutdown($c, 1); # Windows drops a banner when a close with the request unread resets the connection
		1 while sysread($c, my $b, 65536);' "$1" "$TMP/answerPort" &
	waitFor "$TMP/answerPort"
	answerPort=$(cat "$TMP/answerPort")
}
screenLast() { expect "$(tail -n 1 "$TMP/err")" "device: $WIRE_SCREEN" "the last word of $1"; }

# With nothing remembered and no name or probe, discover says so and then prints $WIRE_SCREEN.
run 4 "$device" discover 0
[ ! -s "$TMP/out" ] || fail "a discover that found nothing printed: $(cat "$TMP/out")"
expect "$(cat "$TMP/err")" "device: no address has answered this key on this PC yet"$'\n'"device: $WIRE_SCREEN" "a discover with nothing to try"

# An address pasted the way the screen shows it is accepted, and its port takes precedence over
# THREEDROP_PORT. A reply from it makes the tools remember it. An address that no exchange could use is a
# usage error, and so are two ports that differ.
THREEDROP_PORT=1 run 0 "$device" stats "127.0.0.1:$fakePort"
expect "$(book)" "$fake|" "the address a reply came from"
for address in 127.0.0.1: 127.0.0.1:0 127.0.0.1:65536 127.0.0.1:01337 127.0.0.1:x :1337 -x 1.2.3.4:5:6 'a b'; do
	run 2 "$device" stats "$address"
done
run 1 "$ROOT/tools/drop.sh" "$TMP/none.cia" 127.0.0.1:1337 1337
saidLine "drop: '$TMP/none.cia' is missing or empty"
run 2 "$ROOT/tools/drop.sh" "$TMP/none.cia" 127.0.0.1:1337 1338
run 2 "$ROOT/tools/sync.sh" 000400000ff4e500 "$TMP" 127.0.0.1:1337 1338
# dropBatch hands drop.sh the pasted port, and sync.sh accepts it. Their streams use /dev/tcp and go to that
# port alone.
rm -f "$WIRE_BOOK"
fakeCia "$TMP/batch.cia" 0004000001234500 1
THREEDROP_PORT=1 run 0 "$ROOT/tools/dropBatch.sh" "127.0.0.1:$fakePort" "$TMP/batch.cia"
expect "$(book)" "$fake|" "the address dropBatch's install came from"
mkdir "$TMP/stub"
printf 'code' >"$TMP/stub/code.bin"
echo "000400000ff4e500 1" >>"$FAKE/installed"
THREEDROP_PORT=1 run 0 "$ROOT/tools/sync.sh" 000400000ff4e500 "$TMP/stub" "127.0.0.1:$fakePort"

# The file is named by a tag, never the key, and holds neither the key nor its stretch.
dk=$(deriveDK)
tag=${WIRE_BOOK##*/addresses-}
[[ "$tag" =~ ^[0-9a-f]{16}$ && "$dk" != *"$tag"* ]] || fail "the remembered addresses' file is named '${WIRE_BOOK##*/}'"
if grep -rqiF -e "$THREEDROP_KEY" -e "${THREEDROP_KEY//-/}" -e "$dk" "$TMP/cache"; then fail "a cache file holds the key"; fi

# The most recent address comes first, at most 8 are kept, and an address appears once. A reply less than a
# minute after the last one leaves its entry unchanged.
rm -f "$WIRE_BOOK"
t0=$(date +%s)
for port in 1 2 3 4 5 6 7 8 9; do remember 127.0.0.1 "$port" "$t0"; done
expect "$(book)" "127.0.0.1 9|127.0.0.1 8|127.0.0.1 7|127.0.0.1 6|127.0.0.1 5|127.0.0.1 4|127.0.0.1 3|127.0.0.1 2|" "nine remembered"
wireRead 127.0.0.1 "$fakePort" stats >/dev/null || fail "a stats read"
expect "$(book)" "$fake|127.0.0.1 9|127.0.0.1 8|127.0.0.1 7|127.0.0.1 6|127.0.0.1 5|127.0.0.1 4|127.0.0.1 3|" "a reply's address"
remember 127.0.0.1 5 "$t0"
expect "$(book)" "127.0.0.1 5|$fake|127.0.0.1 9|127.0.0.1 8|127.0.0.1 7|127.0.0.1 6|127.0.0.1 4|127.0.0.1 3|" "an address that answered again"
remember 127.0.0.1 5 $((t0 + 59))
expect "$(head -n 1 "$WIRE_BOOK")" "127.0.0.1 5 $t0" "a reply under a minute after the last"
remember 127.0.0.1 5 $((t0 + 60))
expect "$(head -n 1 "$WIRE_BOOK")" "127.0.0.1 5 $((t0 + 60))" "a reply a minute after the last"
# A line it never wrote is passed over and dropped, and its text never runs.
# shellcheck disable=SC2016 # the planted text, never expanded here
printf '%s\n' '127.0.0.1 3 x[$(touch${IFS}'"$TMP"'/canary)]' 'x[$(touch${IFS}'"$TMP"'/canary)] 1 1' \
	'127.0.0.1 01 1' '127.0.0.1 2 3 4' >"$WIRE_BOOK"
remember 127.0.0.1 3
[ ! -e "$TMP/canary" ] || fail "a planted line ran"
expect "$(book)" "127.0.0.1 3|" "a book with planted lines"
# If the book cannot be written, the exchange doesn't fail and nothing is said.
chmod 500 "${WIRE_BOOK%/*}"
if [ ! -w "${WIRE_BOOK%/*}" ]; then
	run 0 wireRead 127.0.0.1 "$fakePort" stats
	chmod 700 "${WIRE_BOOK%/*}"
	expect "$(cat "$TMP/err")" "" "an exchange whose book could not be written"
else # as root, or on a filesystem with no modes, the check would prove nothing
	chmod 700 "${WIRE_BOOK%/*}"
	skip "a book this user cannot write"
fi

# discover proves each remembered address with one signed `stats`, and prints this key's console's address,
# with `-` as the model, since the model comes from a probe. A proof waits 3 s to connect and for its reply
# (wire.sh's WIRE_PROVE_WAIT; a console that is up answers at once). The fake is a bash started for each
# request, which takes 0.6 s on a quiet Git Bash and over 3 s on a loaded one. THREEDROP_TIMEOUT is the one
# setting for that wait (15 s is also wire.sh's control timeout). A console that never answers is still
# reported as such, at that bound.
[ "$gitBash" = 0 ] || export THREEDROP_TIMEOUT=15
printf '%s\n' "$fake 1" >"$WIRE_BOOK"
: >"$FAKE/log"
run 0 "$device" discover 0
expect "$(cat "$TMP/out")" "$fake -" "discover from a remembered address"
expect "$(cut -d' ' -f1 "$FAKE/log" | tr '\n' ' ')" "stats " "the requests a discover sent"
# The first address that answers is the one a tool takes when it is given none; an address that does not
# answer is skipped.
printf '%s\n' "127.0.0.1 1 1" "$fake 1" >"$WIRE_BOOK"
wireFind >"$TMP/out" || fail "wireFind past an address that refuses"
expect "$wireIP $wirePort:$(cat "$TMP/out")" "$fake:" "the address wireFind took"

# Each address that fails says why and stays remembered. The cases are a console busy with another PC, one
# that refuses connections, and one where nothing answers. Then it prints $WIRE_SCREEN.
printf '%s\n' "$fake 1" "127.0.0.1 1 1" "192.0.2.1 $fakePort 1" >"$WIRE_BOOK"
bound=1
# Windows refuses a closed port after about 2 s, so a 1 s bound hears silence.
[ "$gitBash" = 0 ] || bound=$THREEDROP_TIMEOUT
THREEDROP_TIMEOUT=$bound run 4 faulted mute "$device" discover 0
saidLine "device: 127.0.0.1:$fakePort took the connection but did not answer in $bound s: the console is busy with another PC or a transfer"
saidLine "device: 127.0.0.1:1 refuses connections: 3Drop is not running there, or your console moved"
saidLine "device: nothing answers at 192.0.2.1:$fakePort: the console is off or asleep, on another network, or on a Wi-Fi that keeps its devices apart (a guest network), or your console moved"
screenLast "a discover where each remembered address failed"
expect "$(book)" "$fake|127.0.0.1 1|192.0.2.1 $fakePort|" "addresses that failed, kept"
# A bench rep here that holds the console gives status 3, as for every exchange.
printf '%s\n' "$fake 1" >"$WIRE_BOOK"
mkdir "$WIRE_RUN_DIR/rep-127.0.0.1"
echo "someone $$" >"$WIRE_RUN_DIR/rep-127.0.0.1/owner"
run 3 "$device" discover 0
rm -r "$WIRE_RUN_DIR/rep-127.0.0.1"
saidLine "device: a benchmark run on this PC holds 127.0.0.1:$fakePort"

# Another key's 3Drop at a remembered address is not your console. That is never the key's fault, and the
# address is forgotten. An address that was typed keeps the key's message, which names the address.
otherBook=$(
	THREEDROP_KEY=$other
	wireKey
	remember 127.0.0.1 "$fakePort"
	echo "$WIRE_BOOK"
)
[ "$otherBook" != "$WIRE_BOOK" ] || fail "two keys share a book"
THREEDROP_KEY=$other run 4 "$device" discover 0
saidLine "device: 127.0.0.1:$fakePort is a 3Drop holding another key: not your console, or its key was regenerated (put the new one in THREEDROP_KEY); this address is forgotten"
if grep -q 'not the key on its screen' "$TMP/err"; then fail "another console's answer blamed the key: $(cat "$TMP/err")"; fi
expect "$(book "$otherBook")" "" "another key's 3Drop's address, forgotten"
# Something that is not 3Drop at a remembered address means the console moved, so the address is forgotten. A
# 3Drop that closes without a word (relaunching), cannot read the request, or signs something that does not
# verify is kept. Forgetting is a write that the tool may fail to make: the book is best effort (a write it
# cannot make never fails an exchange and never says a word), and the sentence is said either way. On Git Bash
# a run has left the address in the book with the sentence said, and no cause was found. So there the section
# is repeated with a new listener, up to 3 times. A tool that never forgets fails all three; elsewhere there
# is one try.
for attempt in 1 2 3; do
	answering "SSH-2.0-OpenSSH_9.6"
	printf '%s\n' "127.0.0.1 $answerPort 1" "$fake 1" >"$WIRE_BOOK"
	run 4 faulted hangup "$device" discover 0
	saidLine "device: 127.0.0.1:$answerPort answers as something that is not 3Drop (your console moved, so this address is forgotten)"
	saidLine "device: 127.0.0.1:$fakePort took the connection and closed it without answering: 3Drop may be relaunching, so try again"
	[ "$(book)" != "$fake|" ] || break
	[ "$gitBash" != 0 ] || break
done
expect "$(book)" "$fake|" "the address something else answers at, forgotten"
answering "ERR malformed"
printf '%s\n' "127.0.0.1 $answerPort 1" "$fake 1" >"$WIRE_BOOK"
run 4 faulted bad-mac "$device" discover 0
saidLine "device: 127.0.0.1:$answerPort is a 3Drop that cannot read this request: update it, or these tools"
saidLine "device: 127.0.0.1:$fakePort answered as 3Drop, but its reply does not verify: a 3Drop from another release, or something posing as one"
expect "$(book)" "127.0.0.1 $answerPort|$fake|" "3Drops that did not answer this request, kept"

# The console's name is a MAC of a fixed label, pinned by the vector that testCryptoKat.c pins src/auth.c's
# authNameTag to. THREEDROP_NAME stands in for it.
computed() (
	unset THREEDROP_NAME
	_wireName
	echo "$wireName"
)
expect "$(computed)" 3drop-1938bccf.local "this key's console's name"
# Each address the name resolves to is proved at THREEDROP_PORT, printed and remembered. A remembered address
# that the name also gives is proved once.
rm -f "$WIRE_BOOK"
THREEDROP_NAME=localhost run 0 "$device" discover 0
expect "$(cat "$TMP/out"):$(book)" "$fake -:$fake|" "discover by the name"
: >"$FAKE/log"
THREEDROP_NAME=localhost run 0 "$device" discover 0
expect "$(cat "$TMP/out"):$(grep -c '^stats ' "$FAKE/log")" "$fake -:1" "a remembered address the name gives too"
rm -f "$WIRE_BOOK"
THREEDROP_NAME=localhost wireFind || fail "wireFind by the name"
expect "$wireIP $wirePort" "$fake" "the address wireFind took from the name"
# A name that does not resolve is reported, and the search goes on.
unresolved() (
	# shellcheck disable=SC2317 # called by _wireSeek
	_wireResolve() { return 1; }
	THREEDROP_NAME=3drop-1938bccf.local wireDiscover "$fakePort" 0
)
rm -f "$WIRE_BOOK"
run 4 unresolved
saidLine "$TEST_NAME: 3drop-1938bccf.local, the console's name, does not resolve: the console is off, asleep or busy, or on another network, or this PC does not look .local names up"

# probed <line...> listens on UDP 127.0.0.1:$fakePort, where the probe goes in these tests. Each datagram that
# is exactly 3Drop's probe (pinned as testResponder.c pins src/discover.h's) gets the lines back, one datagram
# per line. A line `elsewhere <line>` is sent from another port. What arrives is kept in $TMP/probes, one
# datagram per line, in hex. The listener is gone after a minute.
probed() {
	rm -f "$TMP/probes" "$TMP/probes.ready"
	perl -MIO::Socket::INET -e '
		my ($port, $log, @lines) = @ARGV;
		my $s = IO::Socket::INET->new(Proto => "udp", LocalAddr => "127.0.0.1", LocalPort => $port) or die "udp: $!";
		my $other = IO::Socket::INET->new(Proto => "udp", LocalAddr => "127.0.0.1") or die "udp: $!";
		open(my $f, ">", "$log.ready") or die;
		print $f 1;
		close $f;
		alarm 60;
		while (defined(my $from = $s->recv(my $probe, 2048))) {
			open(my $l, ">>", $log) or die;
			print $l unpack("H*", $probe), "\n";
			close $l;
			next unless $probe eq pack("a40", "3drop-probe/1");
			for (@lines) {
				if (/\Aelsewhere (.*)/) { $other->send("$1\n", 0, $from) } else { $s->send("$_\n", 0, $from) }
			}
		}' "$fakePort" "$TMP/probes" "$@" &
	prober=$!
	waitFor "$TMP/probes.ready"
}
# The sender of an answer is the address, whatever address the answer names, and each answer is proved before
# it is printed or remembered. A malformed answer is never tried. The probe is sent again and again because
# the console hears few of them. The fake is a process like any other, which a loaded host can hold off the
# CPU or a disk for more than a 1 s window. That window hears no answer, which says nothing about the tools.
# So a window with no answer is repeated, five times at most, on a clean log; a probe that never finds the
# fake fails all five.
probeFinds() {
	rm -f "$WIRE_BOOK" "$TMP/probes"
	st=0
	"$device" discover 1 >"$TMP/out" 2>"$TMP/err" || st=$?
	[ "$st" = 0 ]
}
probed "3drop 192.0.2.9 $fakePort n3dsxl" "3drop 127.0.0.1 1 o3ds" "3drop 127.0.0.1" "garbage"
tryFor 4 probeFinds || fail "$device discover 1 exited $st in each of 5 windows, want 0: $(cat "$TMP/err")"
expect "$(cat "$TMP/out"):$(book)" "$fake n3dsxl:$fake|" "discover by the probe"
# The fake logs each probe as it gets to it, so the count waits for the probes the window sent.
tryFor 100 hasLines "$TMP/probes" 3 || fail "the probe went $(($(wc -l <"$TMP/probes"))) times in a second"
expect "$(sort -u "$TMP/probes")" "$(printf '3drop-probe/1' | od -An -v -tx1 | tr -d ' \n')$(printf '%054d' 0)" "the probe's bytes"
: >"$FAKE/log"
run 0 "$device" discover 1
expect "$(cat "$TMP/out"):$(grep -c '^stats ' "$FAKE/log")" "$fake -:1" "a remembered address the probe names too"
# A search for the console (a tool given no address) takes the first answer that proves, and the probe stops
# there instead of waiting out its window, which is 20 s here.
rm -f "$WIRE_BOOK"
t0=$(msNow)
(
	WIRE_PROBE_SECONDS=20
	wireFind >/dev/null 2>"$TMP/found.err" && echo "$wireIP $wirePort" >"$TMP/found"
) || fail "wireFind by the probe"
[ $(($(msNow) - t0)) -lt 10000 ] || fail "wireFind waited $(($(msNow) - t0)) ms on a probe answered at once"
expect "$(cat "$TMP/found")" "$fake" "the address wireFind took from the probe"
sleep 0.5
probes=$(wc -l <"$TMP/probes")
sleep 0.6
expect "$(wc -l <"$TMP/probes")" "$probes" "probes sent after wireFind took the console"
# Its probe stopped with nothing said (bash 3.2 announces a process that a TERM ends).
expect "$(cat "$TMP/found.err")" "" "what wireFind said as it stopped its probe"
kill "$prober"
# When none answers, that is reported; an empty THREEDROP_PROBE sends no probe.
rm -f "$WIRE_BOOK"
run 4 "$device" discover 1
saidLine "device: no console answered a probe in 1 s: it is off, asleep or busy, or on another network, or a firewall here drops the answer (Linux ufw or firewalld); with a VPN up, THREEDROP_PROBE=<this network's broadcast address> sends it on this network; under WSL 2's default network the probe stays in the VM, so give the console's address once"
probed "3drop $fake o3ds"
THREEDROP_PROBE='' run 4 "$device" discover 1
[ ! -e "$TMP/probes" ] || fail "a probe went with THREEDROP_PROBE empty"
kill "$prober"
# An answer from a port other than the one probed doesn't count, and neither does one that names no port.
probed "elsewhere 3drop 127.0.0.1 $fakePort o3ds" "3drop 127.0.0.1 0 o3ds" "3drop 127.0.0.1 70000 o3ds"
: >"$FAKE/log"
run 4 "$device" discover 1
saidLine "device: no console answered a probe in 1 s: it is off, asleep or busy, or on another network, or a firewall here drops the answer (Linux ufw or firewalld); with a VPN up, THREEDROP_PROBE=<this network's broadcast address> sends it on this network; under WSL 2's default network the probe stays in the VM, so give the console's address once"
expect "$(grep -c '^stats ' "$FAKE/log")" 0 "proofs of answers that were none"
kill "$prober"
# Each answer costs a proof, so past 8 none is taken, and forged answers can't hold a search up for long.
forged=()
for port in 1 2 3 4 5 6 7 8; do forged+=("3drop 127.0.0.1 $port o3ds"); done
probed "${forged[@]}" "3drop $fake o3ds"
forgedHeard() {
	st=0
	"$device" discover 1 >"$TMP/out" 2>"$TMP/err" || st=$?
	[ "$st:$(grep -c 'refuses connections' "$TMP/err")" = 4:8 ]
}
# As above, a window in which the fake was held off the CPU hears no answer. The checks follow.
tryFor 4 forgedHeard || :
expect "$st:$(grep -c 'refuses connections' "$TMP/err"):$(grep -c '^stats ' "$FAKE/log")" "4:8:0" "the answers proved, of 9 (exit status, refused, stats)"
kill "$prober"

# With no perl, the tool says that neither the name nor the probe was tried, and a remembered address still
# answers.
noPerl() { THREEDROP_NAME=localhost without perl wireDiscover "$fakePort" 1; }
printf '%s\n' "$fake 1" >"$WIRE_BOOK"
run 0 noPerl
expect "$(cat "$TMP/out")" "$fake -" "a remembered address with no perl"
rm -f "$WIRE_BOOK"
run 4 noPerl
saidLine "$TEST_NAME: no perl, so neither the console's name nor a probe was tried"
run 2 "$device" discover x
run 2 "$device" discover 1 2
echo "testDiscover: PASS"

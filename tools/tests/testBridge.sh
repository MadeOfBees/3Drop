#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2034,SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# The GUI bridge against the fake device, in pieces of one 1048576-byte unit unless told.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
bridge=$ROOT/tools/bridge.sh
unit=$WIRE_UNIT
temp=$WIRE_PUT_TEMP
sd=$FAKE/sd

# Every bridge and client stops with the test, before fakeStop removes $TMP. One left running would
# keep serving, or keep waiting.
kids=()
trap 'kill ${kids[@]+"${kids[@]}"} 2>/dev/null || :; fakeStop' EXIT

# start <name> [address] [VAR=value...]: runs bridge.sh in the background on a free port, in pieces of
# 1 unit unless told otherwise. Its output goes in $TMP/<name>.out and its log in $TMP/<name>.log. Once
# it has printed its address U and password PW, bridgePid holds its pid.
start() {
	local name=$1 address=${2:-}
	# It gets a process group of its own, as a terminal's job has, and a hang-up or Ctrl-C reaches all of it.
	# INT and QUIT are reset to their defaults, because bash starts a background job with them ignored.
	perl -e '$SIG{INT} = $SIG{QUIT} = "DEFAULT"; setpgrp(0, 0); exec(@ARGV) or die "exec: $!"' \
		env THREEDROP_PIECE="$unit" THREEDROP_BRIDGE_PORT=0 "${@:3}" "$bridge" ${address:+"$address"} \
		>"$TMP/$name.out" 2>"$TMP/$name.log" &
	bridgePid=$!
	kids+=("$bridgePid")
	while [ ! -s "$TMP/$name.out" ] && kill -0 "$bridgePid" 2>/dev/null; do sleep 0.05; done
	read -r U user who pass PW <"$TMP/$name.out" || fail "bridge.sh did not start: $(cat "$TMP/$name.log")"
	expect "$user $who $pass" "user 3drop password" "the line bridge.sh prints"
}
stopBridge() {
	kill "$bridgePid"
	wait "$bridgePid" 2>/dev/null || :
}
# cu <path> [curl args]: runs curl at the bridge's path, logged in, sending each of Q's commands first.
# Its config goes in a file, so that no UTF-8 byte passes through Windows' argv. The fake's curl is the
# seed server's.
Q=()
cu() {
	local st=0 q
	{
		printf 'url = "%s%s"\nuser = "3drop:%s"\n' "$U" "$1" "$PW"
		for q in ${Q[@]+"${Q[@]}"}; do printf 'quote = "%s"\n' "$q"; done
	} >"$TMP/curlrc"
	Q=()
	command curl -sS -K "$TMP/curlrc" "${@:2}" || st=$?
	return "$st"
}
# refused <code> <what> <command...>: the command must fail, and the last reply it got (from curl -v)
# must have that code. That reply is left in `last`.
refused() {
	if "${@:3}" -v >/dev/null 2>"$TMP/v"; then fail "$2 was taken"; fi
	last=$(tr -d '\r' <"$TMP/v" | sed -n 's/^< \([0-9][0-9][0-9] .*\)/\1/p' | tail -n 1)
	[ "${last:0:3}" = "$1" ] || fail "$2: got '$last', want a $1"
}
# raw <step...>: runs one FTP session through the bridge, taking the steps in turn and printing each reply
# line. It ends with QUIT, then reads until the bridge closes, so the bridge closes first. A step is one
# of: a command (after PASV or EPSV the data connection opens at once, at RAW_DATA_HOST if set);
# `get <n|all>`, which reads that much of the data connection and prints `got <bytes>`;
# `put <file> <n>`, which sends the file's first n bytes; `close`; `reply`;
# `until <file> <bytes>`, which waits until the file holds that many bytes; `mark <file>`, which makes
# the file; `wait <file>`, which waits for it; or `quit`, which ends at once, as a client that hangs up
# would.
raw() {
	perl -MIO::Socket::INET -e '
		my ($port, @steps) = @ARGV;
		$| = 1;
		alarm 120;
		my $c = IO::Socket::INET->new("127.0.0.1:$port") or die "connect: $!\n";
		my $d;
		sub reply { while (defined(my $l = <$c>)) { $l =~ s/\r?\n\z//; print "$l\n"; return $l if $l =~ /^\d{3} / } die "no reply\n" }
		reply();
		for (@steps, "QUIT") {
			if (/^get (\S+)$/) {
				my ($n, $got) = ($1, 0);
				while ($n eq "all" || $got < $n) { $got += sysread($d, my $b, $n eq "all" ? 65536 : $n - $got) || last }
				print "got $got\n";
			} elsif (/^put (\S+) (\d+)$/) {
				open(my $f, "<:raw", $1) or die "$1: $!\n";
				read($f, my $b, $2);
				print $d $b;
			} elsif (/^until (\S+) (\d+)$/) {
				my ($f, $n) = ($1, $2);
				select(undef, undef, undef, 0.05) until (-s $f // 0) >= $n;
			} elsif (/^mark (\S+)$/) {
				open(my $f, ">", $1) or die "$1: $!\n";
			} elsif (/^wait (\S+)$/) {
				select(undef, undef, undef, 0.05) until -e $1;
			} elsif ($_ eq "close") {
				close $d;
			} elsif ($_ eq "reply") {
				reply();
			} elsif ($_ eq "quit") {
				exit 0;
			} else {
				print $c "$_\r\n";
				my $l = reply();
				next unless /^(PASV|EPSV)$/;
				my ($at) = $l =~ /\|(\d+)\|/ ? $1 : $l =~ /,(\d+),(\d+)\)/ ? $1 * 256 + $2 : die "no data port: $l\n";
				$d = IO::Socket::INET->new(PeerAddr => ($ENV{RAW_DATA_HOST} || "127.0.0.1") . ":$at", Timeout => 3) or die "data: $!\n";
			}
		}
		1 while <$c>;' "${U##*:}" "$@"
}
login() { raw "USER 3drop" "PASS $PW" "$@"; }
codes() { sed -n 's/^\([0-9][0-9][0-9]\) .*/\1/p' "$1" | tr '\n' ' '; }
# pidOf <log> <device.sh's words>: prints the pid the bridge logged for the device.sh run with those words.
pidOf() { grep -F " device.sh $2: pid " "$1" | tail -n 1 | sed 's/.*: pid //'; }
# stopped <log> <pid>: checks that the bridge stopped it, that its whole process group ended and that its
# traps removed its scratch. The stop ends with a KILL to the group, but a member still counts until it
# is reaped, which a loaded host does a few ms late, so the check allows a second's grace.
stopped() {
	local line left
	tryFor 200 grep -q "^bridge: [0-9]* stopped device.sh $2" "$1" || fail "the bridge never stopped device.sh $2"
	line=$(grep "^bridge: [0-9]* stopped device.sh $2" "$1")
	[[ "$line" != *" by KILL"* ]] || fail "device.sh $2 outlived its TERM: $line"
	tryFor 20 gone "-$2" || fail "device.sh $2's process group outlived its stop by 1 s"
	left=$(find "$TMPDIR" -maxdepth 1 -name "3drop.$2.*")
	[ -z "$left" ] || fail "device.sh $2 left its scratch: $(find "$left" | tr '\n' ' ')"
}
puts() { sed -n 's/^put .* range \([0-9]*\) \([0-9]*\) .*/\1+\2/p' "$FAKE/log" | tr '\n' ' '; }
gets() { sed -n 's/^get .* range \([0-9]*\) \([0-9]*\)$/\1+\2/p' "$FAKE/log" | tr '\n' ' '; }

head -c $((3 * unit + unit / 2)) /dev/urandom >"$TMP/a"
head -c $((3 * unit + unit / 2)) /dev/urandom >"$TMP/b"
printf 'line\r\nline\nend' >"$TMP/text" # what a TYPE A transfer would rewrite
: >"$TMP/empty"
mkdir -p "$sd/gui"

# The bridge takes one address at most, a port, and pieces of whole units; its usage is its header.
# Each call is bounded by a timeout and runs on a free port, so one that serves instead fails.
bounded() { withTimeout 60 env THREEDROP_BRIDGE_PORT=0 "$@"; }
run 2 bounded "$bridge" 127.0.0.1 127.0.0.2
[[ "$(head -n 1 "$TMP/err")" == "bridge.sh [ip]: "* ]] || fail "bridge.sh's usage: $(head -n 1 "$TMP/err")"
run 2 bounded THREEDROP_BRIDGE_PORT=65536 "$bridge" 127.0.0.1
run 2 bounded THREEDROP_PIECE=1000 "$bridge" 127.0.0.1
expect "$(cat "$TMP/err")" "bridge: THREEDROP_PIECE=1000: a whole number of 1048576-byte units, 33554432 bytes at most" "pieces of no whole unit"
run 2 bounded THREEDROP_PIECE=x[\$\(touch\ "$TMP"/canary\)] "$bridge" 127.0.0.1
[ ! -e "$TMP/canary" ] || fail "THREEDROP_PIECE ran as code"

# With no address it looks for the console the way discover does. Where nothing answers it says why, then
# points to the console's screen, and serves nothing.
rm -f "$WIRE_BOOK"
run 4 bounded THREEDROP_PROBE= "$bridge" # the probe is testDiscover's
[ ! -s "$TMP/out" ] || fail "a bridge with no console printed $(cat "$TMP/out")"
expect "$(tail -n 1 "$TMP/err")" "bridge: $WIRE_SCREEN" "the last word of a bridge that found no console"
_wireRemember 127.0.0.1 "$fakePort" "$(date +%s)"
start found
grep -qxF "bridge: the console at 127.0.0.1:$fakePort" "$TMP/found.log" || fail "the bridge found no console: $(cat "$TMP/found.log")"
cu / >/dev/null || fail "a listing through a bridge that found the console"
firstPW=$PW
stopBridge

start main "127.0.0.1:$fakePort"
mainU=$U mainPW=$PW mainPid=$bridgePid
[[ "$PW" =~ ^[0-9a-f]{24}$ && "$PW" != "$firstPW" ]] || fail "a run's password is '$PW' after '$firstPW'"
# The bridge answers at 127.0.0.1 alone, so this PC's other address, where it has one, reaches nothing.
other=$(perl -MIO::Socket::INET -e 'my $s = IO::Socket::INET->new(Proto => "udp", PeerAddr => "192.0.2.1:9"); print $s->sockhost if $s' 2>/dev/null) || :
if [ -z "$other" ] || [ "$other" = 127.0.0.1 ]; then
	skip "no address but 127.0.0.1 here, so the bridge's 127.0.0.1 alone is unchecked"
else
	! perl -MIO::Socket::INET -e 'exit !IO::Socket::INET->new(PeerAddr => $ARGV[0], Timeout => 3)' "$other:${U##*:}" ||
		fail "the bridge answered at $other"
	# Check the data port too, because it carries a transfer's bytes with no password.
	! RAW_DATA_HOST=$other login PASV >/dev/null 2>"$TMP/v" || fail "the bridge's PASV data port answered at $other"
	grep -q '^data: ' "$TMP/v" || fail "a PASV data port at $other: $(cat "$TMP/v")"
fi
# Before the password, only the login is accepted. There is no active mode, and an upload can only
# start from byte 0.
raw "LIST" "USER 3drop" "PASS nope" "PWD" "PASS $PW" "PORT 127,0,0,1,4,1" "EPRT |1|127.0.0.1|1025|" "APPE x" \
	"REST 5" "STOR x" >"$TMP/raw"
expect "$(codes "$TMP/raw")" "220 530 331 530 530 230 502 502 502 350 502 221 " "a session's replies"
login "CWD /gui" DELE RMD RNFR >"$TMP/raw"
expect "$(codes "$TMP/raw")" "220 331 230 250 501 501 501 221 " "pathless changes"
[ -d "$sd/gui" ] || fail "a pathless change removed the current folder"

# The steps of a file client: make a folder, upload files (an empty one, one with a name outside ASCII),
# list a folder, download the bytes back as they went, resume a download, rename a file and see the console
# refuse a rename, and delete.
Q=("MKD /gui/made")
cu / -o /dev/null || fail "MKD"
[ -d "$sd/gui/made" ] || fail "MKD made no folder"
{ cu /gui/a.bin -T "$TMP/a" && cmp -s "$TMP/a" "$sd/gui/a.bin"; } || fail "STOR of a file in pieces"
{ cu /gui/text -T "$TMP/text" && cmp -s "$TMP/text" "$sd/gui/text"; } || fail "STOR of a file's line endings"
{ cu /gui/empty -T "$TMP/empty" && [ -f "$sd/gui/empty" ] && [ ! -s "$sd/gui/empty" ]; } || fail "STOR of an empty file"
{ cu "/gui/%C3%A9%E6%97%A5.txt" -T "$TMP/text" && cmp -s "$TMP/text" "$sd/gui/é日.txt"; } || fail "STOR of a name outside ASCII"
cu /gui/ -v >"$TMP/list" 2>"$TMP/v" || fail "LIST"
tr -d '\r' <"$TMP/list" >"$TMP/listed"
{ grep -q "^-rw-r--r-- 1 3ds 3ds *$((3 * unit + unit / 2)) .* a\.bin$" "$TMP/listed" && grep -q " é日\.txt$" "$TMP/listed" &&
	grep -q "^drwxr-xr-x .* made$" "$TMP/listed"; } || fail "LIST: $(cat "$TMP/listed")"
grep -q '^> TYPE A' "$TMP/v" || fail "curl's listing came in Binary: TYPE A went untested"
{ cu /gui/a.bin -o "$TMP/got" && cmp -s "$TMP/a" "$TMP/got"; } || fail "RETR of a file in pieces"
{ cu /gui/text -o "$TMP/got" && cmp -s "$TMP/text" "$TMP/got"; } || fail "RETR of a file's line endings"
head -c $((unit + unit / 2)) "$TMP/a" >"$TMP/got"
: >"$FAKE/log"
{ cu /gui/a.bin -C - -o "$TMP/got" && cmp -s "$TMP/a" "$TMP/got"; } || fail "RETR resumed"
expect "$(gets)" "$((unit + unit / 2))+$unit $((2 * unit + unit / 2))+$unit " "a resumed RETR's pieces"
Q=("RNFR /gui/empty" "RNTO /gui/renamed")
{ cu / -o /dev/null && [ -f "$sd/gui/renamed" ] && [ ! -e "$sd/gui/empty" ]; } || fail "RNFR, RNTO"
Q=("RNFR /gui/renamed" "RNTO /gui/text")
refused 550 "a rename onto a name that is there" cu / -o /dev/null
[[ "$last" == "550 rename rejected exists "* ]] || fail "the console's words of a refused rename: $last"
Q=("DELE /gui/renamed" "RMD /gui/made")
{ cu / -o /dev/null && [ ! -e "$sd/gui/renamed" ] && [ ! -e "$sd/gui/made" ]; } || fail "DELE, RMD"

# ASCII is for listings. A transfer in it would rewrite line endings, so the bridge refuses one.
: >"$FAKE/log"
refused 550 "a RETR in ASCII" cu /gui/text -B -o "$TMP/ascii"
expect "$last" "550 ASCII would rewrite line endings: set the client to Binary" "a RETR in ASCII"
refused 550 "a STOR in ASCII" cu /gui/ascii -B -T "$TMP/text"
{ [ ! -e "$sd/gui/ascii" ] && ! grep -q '^get \|^put ' "$FAKE/log"; } || fail "a transfer in ASCII reached the console"

# The bridge never sends the word `system`, nor a tree's `r`, so the console's boot files and a full
# folder stay.
mkdir -p "$sd/luma/payloads" "$sd/gui/full"
echo payload >"$sd/luma/payloads/x.firm"
echo kept >"$sd/gui/full/kept"
: >"$FAKE/log"
refused 550 "a STOR of /boot.firm" cu /boot.firm -T "$TMP/text"
[[ "$last" == "550 put rejected system "* ]] || fail "the console's words for /boot.firm: $last"
Q=("DELE /luma/payloads/x.firm")
refused 550 "a DELE in /luma/payloads" cu / -o /dev/null
Q=("RMD /gui/full")
refused 550 "an RMD of a folder that holds a file" cu / -o /dev/null
{ [ ! -e "$sd/boot.firm" ] && [ -f "$sd/luma/payloads/x.firm" ] && [ -f "$sd/gui/full/kept" ]; } || fail "a boot file or a full folder went"
if grep -E ' (system|r)$' "$FAKE/log"; then fail "the bridge sent the word system or r"; fi

# The bridge uses pieces of 8 units unless told otherwise, and sends each to the console as it comes in, so
# the upload's first piece goes before the client ends the file.
start eight "127.0.0.1:$fakePort" THREEDROP_PIECE=
head -c $((8 * unit + 1)) /dev/urandom >"$TMP/big"
: >"$FAKE/log"
{ cu /gui/big -T "$TMP/big" && cmp -s "$TMP/big" "$sd/gui/big"; } || fail "STOR in pieces of 8 units"
{ cu /gui/big -o "$TMP/got" && cmp -s "$TMP/big" "$TMP/got"; } || fail "RETR in pieces of 8 units"
expect "$(puts):$(gets)" "0+$((8 * unit)) $((8 * unit))+1 :0+$((8 * unit)) $((8 * unit))+$((8 * unit)) " "pieces of 8 units"
stopBridge
U=$mainU PW=$mainPW bridgePid=$mainPid
: >"$FAKE/log"
{
	head -c $((2 * unit)) "$TMP/a"
	for _ in $(seq 300); do
		if grep -q "^put .* range 0 $unit " "$FAKE/log"; then
			: >"$TMP/streamed"
			break
		fi
		sleep 0.1
	done
	tail -c +$((2 * unit + 1)) "$TMP/a"
} | cu /gui/stream.bin -T - || fail "STOR from a pipe"
cmp -s "$TMP/a" "$sd/gui/stream.bin" || fail "STOR from a pipe landed other bytes"
[ -e "$TMP/streamed" ] || fail "an upload's first piece waited for the file's end"

# A download goes a verified piece at a time: with its third stalled on the console, the client holds
# the first two and none of the third. Meanwhile a second transfer is refused at once, the folder
# changes and sizes before it answered without the console, which would answer only after the
# transfer, and listings (451: curl takes a LIST's 450 for an empty folder) and changes (450) are
# refused at once. ABOR stops device.sh's whole process group and frees the transfer for the next.
cp "$TMP/a" "$sd/gui/stall.bin"
rm -f "$TMP/two" "$TMP/go" "$FAKE/fault-seen"
echo stall-body >"$FAKE/mode"
echo "get 3" >"$FAKE/fault-on"
login "TYPE I" "PASV" "RETR /gui/stall.bin" "get $((2 * unit))" "mark $TMP/two" "wait $TMP/go" "ABOR" "reply" "get all" \
	>"$TMP/raw" &
aborter=$!
kids+=("$aborter")
while [ ! -e "$TMP/two" ] && kill -0 "$aborter" 2>/dev/null; do sleep 0.05; done
echo 20 >"$FAKE/ls-wait"
: >"$FAKE/log"
t0=$SECONDS
refused 450 "a second transfer" cu /gui/a.bin -o "$TMP/second"
expect "$last" "450 one transfer at a time, and another runs: set the client to one connection" "a second transfer"
login "CWD /gui/none" "CDUP" "PWD" "SIZE /gui/a.bin" "MDTM /gui/a.bin" "DELE /gui/a.bin" "RMD /gui/x" \
	"MKD /gui/new" "RNFR /gui/a.bin" "RNTO /gui/b.bin" >"$TMP/busy"
refused 451 "a listing during a transfer" cu /gui/
expect "$last" "451 a transfer holds the console: ask again once it ends" "a listing during a transfer"
[ $((SECONDS - t0)) -le 15 ] || fail "a second transfer and a session beside it took $((SECONDS - t0)) s"
expect "$(codes "$TMP/busy")" "220 331 230 250 250 257 450 450 450 450 450 350 450 221 " "a session's replies during a transfer"
grep -qx '257 "/gui" is the current folder' "$TMP/busy" || fail "CWD and CDUP during a transfer: $(cat "$TMP/busy")"
if grep -E '^(ls|delete|mkdir|rename) ' "$FAKE/log"; then fail "a request during a transfer waited on the console"; fi
{ [ -e "$sd/gui/a.bin" ] && [ ! -e "$sd/gui/new" ]; } || fail "a change during a transfer reached the card"
rm -f "$FAKE/ls-wait"
: >"$TMP/go"
wait "$aborter" || fail "the ABOR session: $(cat "$TMP/raw")"
rm -f "$FAKE/mode" "$FAKE/fault-on" "$FAKE/fault-seen"
expect "$(grep '^got ' "$TMP/raw" | tr '\n' ' ')" "got $((2 * unit)) got 0 " "what a stalled download gave the client"
{ grep -qx '426 the transfer was stopped' "$TMP/raw" && grep -qx '226 ABOR done' "$TMP/raw"; } || fail "ABOR's replies: $(cat "$TMP/raw")"
stopped "$TMP/main.log" "$(pidOf "$TMP/main.log" "get /gui/stall.bin -")"
{ cu /gui/a.bin -o "$TMP/second" && cmp -s "$TMP/a" "$TMP/second"; } || fail "a transfer after an ABOR"
# With no transfer running, the console checks them, so a folder that is not there is still refused.
login "CWD /gui/none" "CWD /gui/a.bin" "CWD /gui" "SIZE a.bin" "SIZE /gui/none" >"$TMP/raw"
expect "$(codes "$TMP/raw")" "220 331 230 550 550 250 213 550 221 " "a session's replies with the console free"
grep -qx "213 $((3 * unit + unit / 2))" "$TMP/raw" || fail "SIZE with the console free: $(cat "$TMP/raw")"

# When a client hangs up mid-upload, its data connection closing first, the upload stops. Nothing is
# published, the console keeps the whole pieces that went, and sending the same file again goes on from
# there.
login "TYPE I" "PASV" "STOR /gui/cut.bin" "put $TMP/a $((2 * unit + unit / 2))" "until $sd/gui/cut.bin$temp $((2 * unit))" \
	"close" "quit" >"$TMP/raw"
stopped "$TMP/main.log" "$(pidOf "$TMP/main.log" "put - /gui/cut.bin")"
{ [ ! -e "$sd/gui/cut.bin" ] && cmp -s "$sd/gui/cut.bin$temp" <(head -c $((2 * unit)) "$TMP/a"); } || fail "a cut upload was kept as other than its whole pieces"
: >"$FAKE/log"
{ cu /gui/cut.bin -T "$TMP/a" && cmp -s "$TMP/a" "$sd/gui/cut.bin"; } || fail "the cut upload sent again"
expect "$(puts)" "$((2 * unit))+$unit $((3 * unit))+$((unit / 2)) " "the cut upload sent again"
# A session that dies mid-upload (here from a perl error planted in its next write) stops the upload first,
# as it does on a signal, because dying closes device.sh's stdin, and the end of that stdin would publish
# the cut file.
mkdir -p "$TMP/perl"
cat >"$TMP/perl/DiesMid.pm" <<'EOF'
package DiesMid;
my $plant = "$ENV{FAKE}/die-mid";
delete @ENV{qw(PERL5OPT PERL5LIB)};
*CORE::GLOBAL::syswrite = sub (*$;$$) {
	die "planted\n" if unlink $plant;
	CORE::syswrite($_[0], $_[1], @_ > 2 ? $_[2] : length $_[1], @_ > 3 ? $_[3] : 0);
};
1;
EOF
start died "127.0.0.1:$fakePort" PERL5LIB="$TMP/perl" PERL5OPT=-MDiesMid
login "TYPE I" "PASV" "STOR /gui/died.bin" "put $TMP/a $((2 * unit + unit / 2))" "until $sd/gui/died.bin$temp $((2 * unit))" \
	"mark $FAKE/die-mid" "put $TMP/a $unit" >"$TMP/raw" || : # no reply to its QUIT, as the session is gone
stopped "$TMP/died.log" "$(pidOf "$TMP/died.log" "put - /gui/died.bin")"
grep -qx 'planted' "$TMP/died.log" || fail "the planted error never came: $(cat "$TMP/died.log")"
[ ! -e "$sd/gui/died.bin" ] || fail "a session that died mid-upload published $(fileSize "$sd/gui/died.bin") bytes"
stopBridge
U=$mainU PW=$mainPW bridgePid=$mainPid
# A session KILLed mid-upload (OOM killer, kill -9) runs no handler. device.sh, on its own, finds the end
# of stdin unmarked, keeps the whole pieces and publishes nothing. The mark of the earlier upload is gone.
rm -f "$TMP/sent" "$TMP/gone"
login "TYPE I" "PASV" "STOR /gui/whole.txt" "put $TMP/text 100" "close" "reply" "PASV" "STOR /gui/killed.bin" \
	"put $TMP/a $((2 * unit + unit / 2))" "until $sd/gui/killed.bin$temp $((2 * unit))" "mark $TMP/sent" "wait $TMP/gone" \
	>/dev/null 2>&1 &
client=$!
kids+=("$client")
while [ ! -e "$TMP/sent" ] && kill -0 "$client" 2>/dev/null; do sleep 0.05; done
session=$(sed -n 's|^bridge: \([0-9]*\) device.sh put - /gui/killed.bin: pid .*|\1|p' "$TMP/main.log" | tail -n 1)
pid=$(pidOf "$TMP/main.log" "put - /gui/killed.bin")
{ [ -n "$session" ] && [ -n "$pid" ]; } || fail "no session ran the killed upload: $(cat "$TMP/main.log")"
kill -KILL "$session"
tryFor 200 gone "-$pid" || :
: >"$TMP/gone"
wait "$client" || : # its QUIT finds the session gone
! kill -0 -- "-$pid" 2>/dev/null || fail "device.sh $pid ran on 10 s after its session was KILLed"
[ ! -e "$sd/gui/killed.bin" ] || fail "a session KILLed mid-upload published $(fileSize "$sd/gui/killed.bin") bytes"
cmp -s "$sd/gui/killed.bin$temp" <(head -c $((2 * unit)) "$TMP/a") || fail "a KILLed session's upload kept other than its whole pieces"
cmp -s "$TMP/text" "$sd/gui/whole.txt" || fail "the upload before the KILLed one landed other bytes"
# ABOR during an upload over a file the card holds stops the upload, and the file stays as it was.
cp "$TMP/b" "$sd/gui/abor.bin"
login "TYPE I" "PASV" "STOR /gui/abor.bin" "put $TMP/a $((2 * unit + unit / 2))" \
	"until $sd/gui/abor.bin$temp $((2 * unit))" "ABOR" >"$TMP/raw"
stopped "$TMP/main.log" "$(pidOf "$TMP/main.log" "put - /gui/abor.bin")"
cmp -s "$TMP/b" "$sd/gui/abor.bin" || fail "an ABOR mid-upload published $(fileSize "$sd/gui/abor.bin") bytes over the card's file"
{ grep -qx '426 the transfer was stopped' "$TMP/raw" && grep -qx '226 ABOR done' "$TMP/raw"; } ||
	fail "ABOR's replies during an upload: $(cat "$TMP/raw")"

# When the bridge itself is stopped mid-upload, by Ctrl-C, Ctrl-\ or its terminal closing (each reaches
# the bridge's process group, never device.sh's), it stops device.sh first, so the card's file stays as
# it was.
for sig in INT QUIT HUP; do
	cp "$TMP/b" "$sd/gui/kept.bin"
	rm -f "$sd/gui/kept.bin$temp" "$TMP/sent" "$TMP/gone"
	start "$sig" "127.0.0.1:$fakePort"
	login "TYPE I" "PASV" "STOR /gui/kept.bin" "put $TMP/a $((2 * unit + unit / 2))" \
		"until $sd/gui/kept.bin$temp $((2 * unit))" "mark $TMP/sent" "wait $TMP/gone" "quit" >/dev/null &
	client=$!
	kids+=("$client")
	while [ ! -e "$TMP/sent" ] && kill -0 "$client" 2>/dev/null; do sleep 0.05; done
	kill -s "$sig" -- "-$bridgePid"
	wait "$bridgePid" 2>/dev/null || :
	: >"$TMP/gone"
	wait "$client" || :
	pid=$(pidOf "$TMP/$sig.log" "put - /gui/kept.bin")
	tryFor 200 gone "-$pid" || :
	cmp -s "$TMP/b" "$sd/gui/kept.bin" || fail "a bridge stopped by $sig mid-upload published $(fileSize "$sd/gui/kept.bin") bytes over the card's file"
	stopped "$TMP/$sig.log" "$pid"
done
U=$mainU PW=$mainPW bridgePid=$mainPid

# A refusal a retry can get past is a 4xx, which FileZilla retries. No room on the card is 552, the console's
# other refusals are 550, and no answer is 451. An upload's temp that is using up the card's room, or holds
# another file's bytes, is deleted.
faulted rejected-stranded refused 450 "a RETR while the console is stranded" cu /gui/a.bin -o /dev/null
faulted rejected-busy refused 450 "a RETR while the console is at a QR screen" cu /gui/a.bin -o /dev/null
faulted rejected-ring refused 450 "a STOR the drain ring could not take" cu /gui/ring -T "$TMP/text"
Q=("DELE /gui/a.bin")
faulted rejected-challenge refused 450 "a DELE whose nonce was spent twice" cu / -o /dev/null
[ -f "$sd/gui/a.bin" ] || fail "a refused DELE deleted"
faulted rejected-size refused 450 "a STOR whose kept pieces no longer line up" cu /gui/size -T "$TMP/text"
# bridge.pl maps three of wire.sh's own sentences too: "put it again" and "put again" give 450, "went
# past" gives 552, and "other bytes" has the temp deleted. Each is pinned by its words, so rewording one
# in wire.sh fails here. Without the pin it would only have changed a code.
faultedOn put 3 rejected-size refused 450 "a STOR whose third piece no longer lines up with the kept two" cu /gui/again.bin -T "$TMP/a"
expect "$last" "450 the console's /gui/again.bin$temp holds less than its listing said: put it again" "a STOR whose third piece no longer lines up with the kept two"
faultedOn put 3 rejected-space refused 552 "a STOR onto a full card" cu /gui/full.bin -T "$TMP/a"
expect "$last" "552 put rejected space 00000000; the card has no room for the file; /gui/full.bin$temp, a cut upload's pieces, is deleted" "a STOR onto a full card"
{ [ ! -e "$sd/gui/full.bin$temp" ] && [ ! -e "$sd/gui/full.bin" ]; } || fail "a STOR onto a full card left its temp"
head -c $((2 * unit)) "$TMP/b" >"$sd/gui/other.bin$temp"
refused 450 "a STOR onto another file's temp" cu /gui/other.bin -T "$TMP/a"
expect "$last" "450 the console's /gui/other.bin$temp keeps other bytes than this stdin's: delete that file, then put again; /gui/other.bin$temp, a cut upload's pieces, is deleted" "a STOR onto another file's temp"
[ ! -e "$sd/gui/other.bin$temp" ] || fail "another file's temp was kept"
{ cu /gui/other.bin -T "$TMP/a" && cmp -s "$TMP/a" "$sd/gui/other.bin"; } || fail "the STOR after another file's temp went"
# A stdin past FAT32's largest file gets 552 (no room), and its temp is deleted. A real one is 4 GiB, so a
# copy of the tools with that largest file lowered to two units stands in for it, serving beside the main
# bridge.
mkdir -p "$TMP/low/tools"
cp "$ROOT"/tools/{bridge.pl,bridge.sh,cia.sh,device.sh,lib.sh} "$TMP/low/tools/"
sed "s/^WIRE_FILE_MAX=.*/WIRE_FILE_MAX=$((2 * unit))/" "$ROOT/tools/wire.sh" >"$TMP/low/tools/wire.sh"
grep -qx "WIRE_FILE_MAX=$((2 * unit))" "$TMP/low/tools/wire.sh" || fail "no WIRE_FILE_MAX line in wire.sh to lower"
bridge=$TMP/low/tools/bridge.sh
start past "127.0.0.1:$fakePort"
bridge=$ROOT/tools/bridge.sh
refused 552 "a STOR of stdin past FAT32's largest file" cu /gui/past.bin -T "$TMP/a"
expect "$last" "552 stdin went past $((2 * unit)) bytes, the most a FAT32 file holds: delete /gui/past.bin$temp; /gui/past.bin$temp, a cut upload's pieces, is deleted" "a STOR of stdin past FAT32's largest file"
[ ! -e "$sd/gui/past.bin$temp" ] || fail "a STOR past FAT32's largest file kept its temp"
stopBridge
U=$mainU PW=$mainPW bridgePid=$mainPid
mkdir -p "$sd/3ds/3drop"
echo secret >"$sd/3ds/3drop/key.txt"
refused 550 "a RETR of the key" cu /3ds/3drop/key.txt -o /dev/null
[[ "$last" == "550 get rejected protected "* ]] || fail "the console's words for the key: $last"
faulted hangup refused 451 "a folder the console never answered for" cu /gui/

# Restarted on the port it just served, the bridge binds at once. While it serves, no other program can
# take the port (Windows' exclusive bind, which ReuseAddr would drop).
port=${U##*:}
stopBridge
start again "127.0.0.1:$fakePort" THREEDROP_BRIDGE_PORT="$port"
expect "${U##*:}" "$port" "the port a restart took"
perl -MIO::Socket::INET -e 'exit !!IO::Socket::INET->new(Listen => 1, LocalAddr => "127.0.0.1", LocalPort => $ARGV[0], ReuseAddr => 1)' \
	"$port" || fail "another listener took the bridge's port"
# A second bridge on that port is refused with status 1 from the exit table, and says so.
run 1 bounded THREEDROP_BRIDGE_PORT="$port" "$bridge" "127.0.0.1:$fakePort"
grep -q "^bridge: 127.0.0.1:$port: " "$TMP/err" || fail "a bridge on a taken port said: $(cat "$TMP/err")"
# A transfer lock it cannot open holds the console, so no listing or transfer reaches it.
mkdir "$(find "$TMPDIR" -maxdepth 1 -name "3drop.$bridgePid.*")/transfer"
refused 451 "a listing with the transfer lock unopenable" cu /gui/
refused 450 "a transfer with the transfer lock unopenable" cu /gui/a.bin -o /dev/null
stopBridge

if grep -h 'bridge\.pl line [0-9]*' "$TMP"/*.log; then fail "perl warned"; fi
echo "testBridge: PASS"

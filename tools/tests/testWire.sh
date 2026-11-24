#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# wire.sh against the fake device: MACs, reply authentication, the kept nonce, lost-reply
# reconcile, byte-exact file verbs, listings, and /dev/tcp, which Windows takes for everything.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
# The fake's listener (not its children) fails a fork while $FAKE/fork-fails exists, as happens under load.
mkdir "$TMP/perl"
cat >"$TMP/perl/ForkFails.pm" <<'EOF'
package ForkFails;
my $plant = "$ENV{FAKE}/fork-fails";
delete @ENV{qw(PERL5OPT PERL5LIB)};
*CORE::GLOBAL::fork = sub () { unlink($plant) ? undef : CORE::fork() };
1;
EOF
PERL5LIB=$TMP/perl PERL5OPT=-MForkFails . "$(dirname "$0")/fakeDevice.sh"

# The MAC domains, pinned by vector (their twin on the device is src/auth.c).
request="info 00112233445566778899aabbccddeeff"
response="response 00112233445566778899aabbccddeeff info 52 000400000ff40a00 52 ready"
expect "$(macRequest "$request")" 7771cf626ba7c87b1cfe00b408a2c38d26637c7bd6e95120b83938479cb16079abedfbc88ea2bac536c1e43976d381ecf70461eac5cbd64d11c5cc2e55403b5a "request-domain vector"
expect "$(macResponse "$request" "$response")" bc03ddf532f7ccde156cdab9e41a7b6142cdb6d3b1a99920381ff2094f8f9d3f0ba6af5c8c86acaacf3c93a49f5dbbdd49b2d7fa5e25f7d94dbb5cf309c7a38b "response-domain vector"
expect "$(macTrailer "get 0123456789abcdef0123456789abcdef /a" "response 0123456789abcdef0123456789abcdef get ok 4" "trailer 0123456789abcdef0123456789abcdef get ok e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")" c8486003023a517914b0c7d1af6355a864f9d44a47415cbd26663b87b20d773c64315048f579071bb33d13f188db44246d66537a9d8c26ced53ef251f97645be "trailer-domain vector"

# macOf is a hand-rolled HMAC-SHA512 so the key never reaches an argv, and no openssl exec sees it.
dk=$(deriveDK)
opensslShim
PATH="$TMP/shim:$PATH" macRequest "$request" >/dev/null
if [ ! -s "$TMP/openssl.log" ] || grep -qF -e "$dk" -e "${THREEDROP_KEY//-/}" "$TMP/openssl.log"; then fail "a key reached openssl's argv"; fi
# wireKey keeps the stretch for its key, and another key's MAC is that key's.
expect "$(THREEDROP_KEY=other-key-1234 macRequest "$request")" "$(WIRE_KEY_FOR="" THREEDROP_KEY=other-key-1234 macRequest "$request")" "another key's MAC"
# A .env line that is not NAME=value followed by at most a comment is refused before any line runs, and is
# never shown. If it ran, the bare key, a key after a blank, a key after a command, or a key after a # that
# follows its value directly (bash reads that # as part of the value) would be a command the shell names on
# stderr, and ${..?..} or $[..] would print what it holds in an error.
mkdir "$TMP/env"
# shellcheck disable=SC2016 # the lines' own $
for bad in '<key>' 'THREEDROP_KEY= <key>' 'THREEDROP_KEY=<key>; <key>' 'THREEDROP_KEY=<key>;<key>' \
	'THREEDROP_KEY=<key>#;<key>' "THREEDROP_KEY='<key>'#;<key>" \
	'X=${UNSET?<key>}' 'X="${UNSET?<key>}"' 'X=$[7<key>]' 'X="$[7<key>]"'; do
	printf 'THREEDROP_PORT=4242\r\n%s\r\n' "${bad//<key>/$THREEDROP_KEY}" >"$TMP/env/.env"
	st=0
	(
		unset THREEDROP_KEY
		ROOT=$TMP/env requireKey
	) >"$TMP/said" 2>&1 || st=$?
	expect "$st:$(cat "$TMP/said")" "2:$TOOL: $TMP/env/.env line 2 is not NAME=value: the key goes in as THREEDROP_KEY=<key>" "the .env line $bad"
	! grep -qF -- "$THREEDROP_KEY" "$TMP/said" || fail "the .env line $bad showed the key"
done
# The lines it runs: LF or CRLF, an export, a value bare or in either quotes, blanks and comments.
# $NAME expands in a bare or "" value, a '' value holds any character, and a # inside a value is part of the
# value.
printf '# 3Drop\n\n export THREEDROP_PORT=4242 # its port\r\nTHREEDROP_NAME="my 3ds"\nTHREEDROP_KEY='\''%s'\'' # the key\n' "$THREEDROP_KEY" >"$TMP/env/.env"
cat >>"$TMP/env/.env" <<'EOF'
SAME=$THREEDROP_PORT
SAID="the port is $THREEDROP_PORT"
LITERAL='${x} $[y] (z);'
HASH=a#b
EMPTY= # an empty value, then a comment
EOF
expect "$(
	unset THREEDROP_KEY
	ROOT=$TMP/env requireKey
	echo "$THREEDROP_KEY:$THREEDROP_PORT:$THREEDROP_NAME:$SAME:$SAID:$LITERAL:$HASH:$EMPTY."
)" "$THREEDROP_KEY:4242:my 3ds:4242:the port is 4242:\${x} \$[y] (z);:a#b:." "the lines a .env may hold"
# A UTF-8 byte-order mark before the first line, as an editor may save one, isn't part of that line. A .env in
# UTF-16, as Windows PowerShell 5.1's > writes one, is reported as UTF-16, and none of it runs.
printf '\357\273\277THREEDROP_KEY=%s\r\nTHREEDROP_PORT=4242\r\n' "$THREEDROP_KEY" >"$TMP/env/.env"
expect "$(
	unset THREEDROP_KEY
	ROOT=$TMP/env requireKey
	echo "$THREEDROP_KEY:$THREEDROP_PORT"
)" "$THREEDROP_KEY:4242" "a .env saved with a UTF-8 byte-order mark"
line="THREEDROP_KEY=$THREEDROP_KEY"
{
	printf '\377\376'
	for ((i = 0; i < ${#line}; i++)); do printf '%s\0' "${line:i:1}"; done
	printf '\r\0\n\0'
} >"$TMP/env/.env"
st=0
(
	unset THREEDROP_KEY
	ROOT=$TMP/env requireKey
) >"$TMP/said" 2>&1 || st=$?
expect "$st:$(cat "$TMP/said")" "2:$TOOL: $TMP/env/.env is UTF-16, as Windows PowerShell 5.1's > writes it: save it as UTF-8" "a .env in UTF-16"

# The wire's digest, pinned by testCryptoKat.c's vectors for src/hash.c, whole and as a prefix. A missing
# file, bytes past the end, no room to split, a bad split or a failed openssl each print nothing.
head -c 3670016 < <(yes 3drop) >"$TMP/b3670016"
: >"$TMP/b0"
for v in 0:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 \
	1:80903da4e6bbdf96e8ff6fc3966b0cfd355c7e860bdd1caa8e4722d9230e40ac \
	1048575:3ba85163b3e9a11197592d073d9e7d57fc2646201366cb9f3a704194325d2488 \
	1048576:0650756fd33c813b4541443e458d2616696ed1b57bd2729fac99bf0f4d9c0292 \
	1048577:2895dda6697b1166b130b69de087eaad792838abe974fc9932b277e597c241a1 \
	3670016:1a123ad2daaf45074139d526e884c29c5c1f3e5c08fb7746ccd08ffbbcbdb43b; do
	n=${v%%:*}
	[ -e "$TMP/b$n" ] || head -c "$n" "$TMP/b3670016" >"$TMP/b$n"
	expect "$(unitDigest "$TMP/b$n")" "u:${v#*:}" "the unit digest of $n bytes"
	expect "$(unitDigest "$TMP/b3670016" "$n")" "u:${v#*:}" "the unit digest of a prefix of $n bytes"
done
silent unitDigest "$TMP/none"
silent unitDigest "$TMP/b1" 2
silent unitDigest <(printf hello) # a pipe read once for its size is never digested as empty
(
	# shellcheck disable=SC2317 # called by unitDigest
	df() { printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\nx 1 1 0 100%% /\n'; }
	expect "$(unitDigest "$TMP/b1048576")" u:0650756fd33c813b4541443e458d2616696ed1b57bd2729fac99bf0f4d9c0292 "one unit, never split, with no room to split it"
	# A CIA's prefix, a few KB, is handled likewise, because drop.sh digests it before its request.
	expect "$(unitDigest "$TMP/b3670016" 1048575)" u:3ba85163b3e9a11197592d073d9e7d57fc2646201366cb9f3a704194325d2488 "a prefix of one unit, never split, with no room to split it"
	silent unitDigest "$TMP/b1048577"
)
(
	# shellcheck disable=SC2317 # called by unitDigest
	split() { command split "$@" && : >"${*: -1}zzzz"; }
	silent unitDigest "$TMP/b1048577"
)
(
	# shellcheck disable=SC2317 # called by unitDigest
	openssl() {
		if [ "${3:-}" = -binary ]; then return 1; fi
		command openssl "$@"
	}
	silent unitDigest "$TMP/b1"
	silent unitDigest "$TMP/b1048577"
)
zeros=$(printf '%064d' 0)
expect "$(requireDigest "u:$zeros" digest)" "u:$zeros" "a unit digest's spelling"
for bad in "U:$zeros" "u:${zeros#0}" "u:${zeros}0" "u:A${zeros#0}" "$zeros"; do
	silent requireDigest "$bad" digest
done

# Only a reply signed for this exact request, in canonical form, counts; it prints without its nonce.
request="stats $(newRequestID)"
expect "$(wireExchange 127.0.0.1 "$fakePort" "$request")" "response ${request#stats } stats ok ok 00000000 bytes=0 total_ms=- stall_n=0 req=-" "a valid signed reply"
for mode in bad-mac uppercase wrong-binding extra-line; do
	faulted "$mode" fails wireExchange 127.0.0.1 "$fakePort" "$request"
done

# A kept nonce's age is read from the system clock, which bash 3.2 can read without starting a process
# (_wireNow).
for _ in 1 2 3; do
	clock=$(date +%s)
	_wireNow
	[ "$(date +%s)" != "$clock" ] || break
done
expect "$wireNow" "$clock" "the clock a kept nonce's age is read by"
# Mutations spend the last reply's nonce with no round trip. A spent nonce is retried once, on the refusal's
# nonce. (steadyRun, in lib.sh, runs a sequence again if a step of the wall clock upset it.)
until steadyRun; do
	: >"$FAKE/log"
	wireRead 127.0.0.1 "$fakePort" stats >/dev/null
	wireMkdir 127.0.0.1 "$fakePort" /n1 >/dev/null
	wireMkdir 127.0.0.1 "$fakePort" /n2 >/dev/null
done
expect "$(count stats)" 1 "reads before two chained mutations"
# A planted stamp comes from _wireNow. Just past a second, `date +%s` can be one ahead of it, which gives an
# age below 0.
_wireNow && printf '%s %064d %s\n' "$wireNow" 0 "$WIRE_PROTO" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
wireMkdir 127.0.0.1 "$fakePort" /n3 >/dev/null || fail "a spent nonce was not retried"
expect "$(count mkdir)" 4 "mkdir requests (the spent nonce's one retry included)"
[ -d "$FAKE/sd/n3" ] || fail "mkdir"
# A put works the same way. A run of puts takes one exchange each after the read that brings the first nonce,
# and a spent nonce, refused before its body is taken, sends the put once more, never twice.
printf 'kept' >"$TMP/k"
until steadyRun; do
	rm -f "$(_wireNonceFile 127.0.0.1 "$fakePort")"
	: >"$FAKE/log"
	for i in 1 2 3; do wireFilePut 127.0.0.1 "$fakePort" "$TMP/k" "/k$i" >/dev/null || fail "put $i of a run"; done
done
expect "$(awk '{ printf "%s ", $1 }' "$FAKE/log")" "stats put put put " "the exchanges of three puts"
_wireNow && printf '%s %064d %s\n' "$wireNow" 0 "$WIRE_PROTO" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
: >"$FAKE/log"
wireFilePut 127.0.0.1 "$fakePort" "$TMP/k" /k4 >/dev/null || fail "a put on a spent nonce was not sent again"
expect "$(awk '{ printf "%s ", $1 }' "$FAKE/log")" "put put " "a put on a spent nonce, sent again on its refusal's"
cmp -s "$TMP/k" "$FAKE/sd/k4" || fail "the put sent again"
st=0
(
	# shellcheck disable=SC2317 # called by wire.sh
	_wireNonce() { printf '%064d\n' 0; } # a nonce another sender always spends first
	: >"$FAKE/log"
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/k" /k5 >/dev/null 2>"$TMP/k5.err"
) || st=$?
expect "$st:$(count put)" 1:2 "a put refused on every nonce: its status and puts sent"
[ ! -e "$FAKE/sd/k5" ] || fail "a put refused on every nonce was written"
[[ "$(cat "$TMP/k5.err")" == *" put rejected challenge 00000000"$'\n'"$TOOL: the console's nonce was spent or expired"* ]] ||
	fail "a put refused twice on its nonce said: $(cat "$TMP/k5.err")"
# Only a young kept nonce is spent. One at the keep bound, or ahead of a clock that was set back, is read
# afresh.
for kept in old:-$WIRE_NONCE_KEEP ahead:3600; do
	until steadyRun; do # the stamp is planted in the run, since a step would move the age it was planted for
		_wireNow && printf '%s %064d %s\n' $((wireNow + ${kept#*:})) 0 "$WIRE_PROTO" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
		: >"$FAKE/log"
		wireFilePut 127.0.0.1 "$fakePort" "$TMP/k" /k6 >/dev/null || fail "a put on an ${kept%%:*} kept nonce"
	done
	expect "$(awk '{ printf "%s ", $1 }' "$FAKE/log")" "stats put " "a put on an ${kept%%:*} kept nonce"
done
# A clock stepped back (WSL's does, 1.4 s every 30 s) leaves the kept nonce stamped ahead of now. Within
# WIRE_CLOCK_STEP the nonce is spent as of age 0, with no read before its request, and past that it is read
# afresh. The keep, the step and a request's control timeout together stay under the half life the device
# hands a nonce out with.
[ $((WIRE_NONCE_KEEP + WIRE_CLOCK_STEP + $(wireTimeout))) -lt $(($(cDefine auth.c NONCE_TTL_MS) / 2000)) ] ||
	fail "the keep, a clock's step and a request's timeout do not fit the half life the device hands a nonce out with"
# The nonce clock is held still so that each stamp is read at the age it was planted with, because a stall
# between the plant and the put's read (a loaded host) would move it.
(
	_wireNow
	fixedNow=$wireNow
	# shellcheck disable=SC2030,SC2317 # this subshell's frozen clock, called by wire.sh
	_wireNow() { wireNow=$fixedNow; }
	for ahead in "2:put " "$((WIRE_CLOCK_STEP + 5)):stats put "; do
		_wireNow && printf '%s %s %s\n' $((wireNow + ${ahead%%:*})) "$(fakeNonce)" "$WIRE_PROTO" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
		: >"$FAKE/log"
		wireFilePut 127.0.0.1 "$fakePort" "$TMP/k" /k7 >/dev/null || fail "a put on a nonce stamped ${ahead%%:*} s ahead"
		expect "$(awk '{ printf "%s ", $1 }' "$FAKE/log")" "${ahead#*:}" "a put on a nonce stamped ${ahead%%:*} s ahead"
	done
) || exit 1
# The kept nonce is data and never code: it is kept in a private directory, and a stamp is checked to be
# decimal before arithmetic.
# Git Bash's mounts take no modes; the profile's ACL closes them.
mkdir "$TMP/modes" && chmod 700 "$TMP/modes"
case "$(ls -ld "$TMP/modes")" in drwx------*) modes=private ;; *) modes=none ;; esac
for dir in "$(cacheDir)" "$WIRE_RUN_DIR"; do
	case "$modes $(ls -ld "$dir")" in "private drwx------"* | none*) ;; *) fail "$dir is open to other users" ;; esac
done
expect "$WIRE_RUN_DIR" "$TMP/run/3drop" "the run directory under XDG_RUNTIME_DIR"
: >"$TMP/notadir" # an XDG_RUNTIME_DIR that cannot hold a run directory; the cache directory is used instead
expect "$(XDG_RUNTIME_DIR="$TMP/notadir" runDir)" "$(cacheDir)" "the run directory's fallback"
# shellcheck disable=SC2016 # the file's text, never expanded here
planted='x[$(touch${IFS}'"$TMP"'/canary)] '"$(printf '%064d' 0)" # ${IFS} because read splits a plain space
printf '%s\n' "$planted" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
wireMkdir 127.0.0.1 "$fakePort" /n4 >/dev/null || fail "a planted nonce file stopped a mutation"
printf '%s\n' "$planted" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
(
	set +u
	wireMkdir 127.0.0.1 "$fakePort" /n5 >/dev/null
) || fail "a planted nonce file stopped a mutation (without set -u)"
[ ! -e "$TMP/canary" ] || fail "a planted nonce file ran a command"
# A kept nonce that is not 64 hex digits never reaches the request line.
printf '%s x delete /\n' "$(date +%s)" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
wireMkdir 127.0.0.1 "$fakePort" /n6 >/dev/null || fail "a malformed kept nonce stopped a mutation"
if grep -q ' x delete ' "$FAKE/log"; then fail "a malformed kept nonce was sent"; fi
# Hex is lowercase whatever the locale, because a range such as a-f takes A-E under en_US.UTF-8.
if (
	LC_ALL=en_US.UTF-8
	requireHex 4 ABCD digits
) >/dev/null 2>&1; then fail "requireHex took uppercase hex"; fi
if (
	caseless A a
	requireHex 4 ABCD digits
) >/dev/null 2>&1; then fail "requireHex took uppercase hex after a caseless match"; fi

# Installs: a refusal is an answer (status 0), and a lost reply is reconciled (receipt, `ls title:`) instead
# of being resent.
cia="$TMP/app.cia"
fakeCia "$cia" 0004000001234500 7
expect "$(wirePut 127.0.0.1 "$fakePort" new "$cia")" "put committed ok 00000000 0004000001234500 7 new" "a committed PUT"
out=$(wirePut 127.0.0.1 "$fakePort" new "$cia") || fail "a rejected PUT is an answer, status 0"
expect "$(cut -d' ' -f1-3 <<<"$out")" "put rejected exists" "a rejected PUT"
fakeCia "$cia" 0004000001234500 8
: >"$FAKE/log"
expect "$(faulted lost-put wirePut 127.0.0.1 "$fakePort" update "$cia")" "put committed ok 00000000 0004000001234500 8 update" "a PUT reconciled from its receipt"
fakeCia "$cia" 0004000001234500 9
expect "$(faulted lost-receipt wirePut 127.0.0.1 "$fakePort" update "$cia")" "put committed ok 00000000 0004000001234500 9 update" "a PUT reconciled from ls title:"
expect "$(count put)" 2 "puts sent (a lost reply must not resend)"
# A lost reply to a refused PUT is reconciled to the refusal in its receipt, never to `committed` from the
# title's version.
expect "$(faulted lost-put wirePut 127.0.0.1 "$fakePort" update "$cia")" "put rejected not-newer 00000000 0004000001234500 9 update" "a refused PUT reconciled from its receipt"
fakeCia "$TMP/self.cia" 0004000003d37200 53
echo "0004000003d37200 52" >>"$FAKE/installed"
expect "$(faulted lost-receipt wirePut 127.0.0.1 "$fakePort" self "$TMP/self.cia")" "put committed ok 00000000 0004000003d37200 53 self" "a self PUT reconciled from the running version"
# The wait is bounded by time, not by tries: the silent requests outlast ten polls. The knock pauses are
# stubbed and the clock is real.
fakeCia "$TMP/self.cia" 0004000003d37200 54
(
	# shellcheck disable=SC2317 # called by wire.sh
	sleep() { [ "$1" = "$WIRE_KNOCK" ] || "$realSleep" "$@"; }
	expect "$(faulted slow-relaunch wirePut 127.0.0.1 "$fakePort" self "$TMP/self.cia")" "put committed ok 00000000 0004000003d37200 54 self" "a self PUT reconciled across a slow relaunch"
)
rm -f "$FAKE/down"
# A committed self put relaunches 3Drop with another nonce, so the host reads before it spends again.
fakeCia "$TMP/self.cia" 0004000003d37200 55
wireRead 127.0.0.1 "$fakePort" stats >/dev/null
: >"$FAKE/log"
wirePut 127.0.0.1 "$fakePort" self "$TMP/self.cia" >/dev/null
wireMkdir 127.0.0.1 "$fakePort" /after-self >/dev/null
expect "$(awk '{ printf "%s ", $1 }' "$FAKE/log")" "put stats mkdir " "a mutation after a self put"
fakeCia "$cia" 0004000001234500 10
run 4 faulted ambiguous wirePut 127.0.0.1 "$fakePort" update "$cia"
wirePut 127.0.0.1 "$fakePort" update "$cia" "" 2 5 1 >/dev/null
case "$(grep '^put ' "$FAKE/log" | tail -1)" in *" update "*" 2 5 1") ;; *) fail "the batch position is not in the MAC-covered request" ;; esac
# A batch position that is given is sent as given, for the console to refuse. It is never dropped to turn the
# request into a plain put.
expect "$(wirePut 127.0.0.1 "$fakePort" update "$cia" "" "" 5 1 2>/dev/null | cut -d' ' -f1-3)" "error rejected grammar" "a batch position with no index"
case "$(wireUninstall 127.0.0.1 "$fakePort" 0004000001234500 ticket)" in "response "*" delete committed ok 00000000 0004000001234500") ;; *) fail "uninstall" ;; esac
grep -q '^delete [0-9a-f]* [0-9a-f]* title:0004000001234500 ticket$' "$FAKE/log" || fail "the ticket opt-in was not sent"
: >"$FAKE/log"
"$ROOT/tools/device.sh" uninstall 127.0.0.1 0004000001234500 >/dev/null 2>&1 || :
grep -q '^delete [0-9a-f]* [0-9a-f]* title:0004000001234500$' "$FAKE/log" || fail "device.sh uninstall asked for the ticket unasked"
"$ROOT/tools/device.sh" uninstall 127.0.0.1 --ticket 0004000001234500 >/dev/null 2>&1 || :
grep -q '^delete [0-9a-f]* [0-9a-f]* title:0004000001234500 ticket$' "$FAKE/log" || fail "device.sh uninstall --ticket"
case "$(wireUninstall 127.0.0.1 "$fakePort" 00040000012345AB 2>&1 >/dev/null)" in "$TOOL: response "*" delete rejected notfound d8a083fa") ;; *) fail "uninstall of an absent title" ;; esac
# Each exchange waits as its own request does, whatever wrapper sends it: a title's delete, a tree's get and a
# lost reply's receipt wait 120 s, and the kept nonce's read waits the control timeout.
echo "0004000001234500 1" >>"$FAKE/installed"
mkdir -p "$FAKE/sd/tree/a"
(
	original _wireTransport
	# shellcheck disable=SC2317 # called by wire.sh
	_wireTransport() {
		printf '%s %s\n' "${4%% *}" "$3" >>"$TMP/waits"
		real__wireTransport "$@"
	}
	rm -f "$(_wireNonceFile 127.0.0.1 "$fakePort")"
	wireUninstall 127.0.0.1 "$fakePort" 0004000001234500 >/dev/null
	echo "0004000001234500 1" >>"$FAKE/installed"
	wireDelete 127.0.0.1 "$fakePort" title:0004000001234500 >/dev/null
	wireDelete 127.0.0.1 "$fakePort" /tree r >/dev/null
	wireMkdir 127.0.0.1 "$fakePort" /tree >/dev/null
	echo "0004000001234500 1" >>"$FAKE/installed"
	faulted lost-reply wireDelete 127.0.0.1 "$fakePort" title:0004000001234500 >/dev/null
)
expect "$(tr '\n' ' ' <"$TMP/waits")" "stats $(wireTimeout) delete 120 delete 120 delete 120 mkdir $(wireTimeout) delete 120 stats 120 " "each exchange's timeout"
# The deadlines. The control timeout outlasts the device's pre-auth timeout, so a close there reads as no
# reply. A quiet exchange outlasts the console's own give-up (on its peer, or on a worker's stuck card or AM
# call) by the margin its word needs. A put's wait is one bound whatever its size, never hours.
[ "$(wireTimeout)" -gt $(($(cDefine net.h PREAUTH_INPUT_TIMEOUT_MS) / 1000)) ] || fail "the control timeout does not outlast the device's pre-auth deadline"
expect "$WIRE_SILENCE" $(($(cDefine net.h TRANSFER_WIRE_STALL_MS) / 1000 + WIRE_MARGIN)) "the silence bound"
[ "$WIRE_SILENCE" -gt $(($(cDefine cmdFs.c LS_WALK_MS) / 1000)) ] || fail "the silence bound does not outlast a listing's walk"
expect "$WIRE_WORKER_SILENCE" $(($(cDefine ring.c WORKER_STALL_MS) / 1000 + WIRE_MARGIN)) "a worker's silence bound"
head -c 16384 /dev/zero >"$TMP/small"
head -c 8388608 /dev/zero >"$TMP/eight"
mkdir -p "$FAKE/save:000400000f800100"
echo "000400000f800100 1" >>"$FAKE/installed"
(
	original _wireTransport
	# shellcheck disable=SC2317 # called by wire.sh
	_wireTransport() {
		echo "${4%% *} $3" >>"$TMP/streamWaits"
		real__wireTransport "$@"
	}
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/small" /small >/dev/null
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/eight" /eight >/dev/null
	wireGet 127.0.0.1 "$fakePort" /small "$TMP/small.got" >/dev/null
	wireLs 127.0.0.1 "$fakePort" / >/dev/null
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/small" save:000400000f800100/x >/dev/null
)
expect "$(grep -v '^stats ' "$TMP/streamWaits" | tr '\n' ' ')" "put $WIRE_WORKER_SILENCE put $WIRE_WORKER_SILENCE get $WIRE_WORKER_SILENCE ls $WIRE_SILENCE put $WIRE_SILENCE " "each body's silence bound"
# A range resumed with no digest of what the temp keeps reads that back first, at a hash's pace.
_wireQuiet "put 0 0 /p - u:0 range 1000000000 33554432 u:0"
expect "$wireQuiet" $((WIRE_WORKER_SILENCE + 1000 * 23 / 100)) "a resumed range's silence bound"
_wireQuiet "put 0 0 /p - u:0 range 0 33554432 u:0"
expect "$wireQuiet" "$WIRE_WORKER_SILENCE" "a first range's silence bound"
# A hash says nothing until it has read the whole file, so its wait depends on the file on the card: the
# measured 11.5 s per 100 MB, twice over, plus the margin. A size that no listing gave is taken as the largest
# file FAT32 allows.
hashWait() ( # <listing line, or none> -> the wait the hash is read with
	# shellcheck disable=SC2317 # called by wire.sh
	wireLs() { [ -n "$listing" ] && echo "$listing"; }
	# shellcheck disable=SC2317
	wireRead() {
		echo "$5" >"$TMP/hashWait"
		echo "response 0 hash ok ok 00000000 0 u:$(printf '%064d' 0)"
	}
	listing=$1
	wireHash 127.0.0.1 "$fakePort" /x >/dev/null
	cat "$TMP/hashWait"
)
expect "$(hashWait "f 100000000 - x")" $((WIRE_MARGIN + 23)) "a 100 MB hash's wait"
expect "$(hashWait "f 0 - x")" "$WIRE_MARGIN" "an empty file's hash's wait"
expect "$(hashWait "")" $((WIRE_MARGIN + 4295 * 23 / 100)) "a hash's wait with no listing"

# SD file operations are binary-safe through hex; first, the paths (a space is outside the request alphabet).
expect "$(wirePath "/Nintendo 3DS")" hex:2f4e696e74656e646f20334453 "a space forces hex"
expect "$(wirePath hex:2f61)" hex:6865783a32663631 "a literal hex: path is encoded"
expect "$(wirePath "save:000400000f800100/a b")" save:000400000f800100hex:2f612062 "a scheme stays, its path goes hex"
expect "$(wirePath extdata:0000055d/x)" extdata:0000055d/x "an extdata path"
# A macOS name goes composed (e and U+0301 become U+00E9). Other names go as given, including bytes that iconv
# refuses.
decomposed=$(printf '/nfc/cafe\314\201') composed=$(printf '/nfc/caf\303\251')
want=$decomposed
[[ "$OSTYPE" != darwin* ]] || want=$composed
expect "$(wirePath "$decomposed")" "hex:$(wireHex "$want")" "a decomposed path"
expect "$(wirePath "save:000400000f800100$decomposed")" "save:000400000f800100hex:$(wireHex "$want")" "a decomposed path in a save"
printf -v refusedBytes '/a\355\240\200'
expect "$(wirePath "$refusedBytes")" hex:2f61eda080 "bytes iconv refuses"
printf -v newline '/caf\303\251\n'
expect "$(wirePath "$newline")" hex:2f636166c3a90a "a path's last newline"
: >"$FAKE/log"
printf 'nfc' >"$TMP/nfc"
wireFilePut 127.0.0.1 "$fakePort" "$TMP/nfc" "$decomposed" >/dev/null || fail "a put to a decomposed path"
grep -q "^put [0-9a-f]* [0-9a-f]* hex:$(wireHex "$want") " "$FAKE/log" || fail "a put to a decomposed path sent: $(cat "$FAKE/log")"
printf 'file\000ops\377\n' >"$TMP/f"
wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" "/My Files/a b.bin" >/dev/null || fail "put to a spaced path"
expect "$(wireLs 127.0.0.1 "$fakePort" "/My Files/a b.bin" d)" "f 10 - a b.bin" "ls d of a file"
fails wireRename 127.0.0.1 "$fakePort" "/My Files/a b.bin" "/3ds/c d.bin" # no /3ds yet
wireMkdir 127.0.0.1 "$fakePort" /3ds/apps >/dev/null || fail "mkdir under /3ds, its parent made too"
case "$(wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" /3ds/apps 2>&1)" in *" put rejected exists 00000000"$'\n'"$TOOL: it is there already: "*) ;; *) fail "a put onto a directory" ;; esac
[ -d "$FAKE/sd/3ds/apps" ] || fail "a put replaced a directory"
case "$(wireMkdir 127.0.0.1 "$fakePort" "/My Files/a b.bin" 2>&1)" in *" mkdir rejected exists c82044be"$'\n'"$TOOL: something there has that name already") ;; *) fail "a mkdir over a file" ;; esac
wireRename 127.0.0.1 "$fakePort" "/My Files/a b.bin" "/3ds/c d.bin" >/dev/null || fail "rename"
landed "$TMP/f" "$TMP/f.out" "a get of a renamed, spaced path" wireGet 127.0.0.1 "$fakePort" "/3ds/c d.bin" "$TMP/f.out"
# Streams are read binary over /dev/tcp, not nc, whose receive loop capped every get on macOS.
(
	# shellcheck disable=SC2317 # what wire.sh would call
	nc() { return 1; }
	landed "$TMP/f" "$TMP/f.nonc" "a get with no nc" wireGet 127.0.0.1 "$fakePort" "/3ds/c d.bin" "$TMP/f.nonc"
	expect "$(wireLs 127.0.0.1 "$fakePort" "/3ds/c d.bin" d)" "f 10 - c d.bin" "ls without nc"
)
# The device's close waits for ours, so after each reply the host closes at once.
: >"$FAKE/drainMs"
for _ in 1 2 3; do wireGet 127.0.0.1 "$fakePort" "/3ds/c d.bin" "$TMP/f.out" >/dev/null; done
# Each line is written once the device's close ends, after the host's. The test waits for that, because
# otherwise an empty file would pass.
tryFor 100 hasLines "$FAKE/drainMs" 3 || :
expect $(($(wc -l <"$FAKE/drainMs"))) 3 "the device's timed closes of three gets"
awk '$1 >= 600 { exit 1 }' "$FAKE/drainMs" || fail "the host held the connection after a reply (ms): $(tr '\n' ' ' <"$FAKE/drainMs")"
for mode in bad-mac short-body bad-trailer stopped corrupt-body; do
	faulted "$mode" fails wireGet 127.0.0.1 "$fakePort" "/3ds/c d.bin" "$TMP/bad"
	[ ! -e "$TMP/bad" ] || fail "get wrote a file under $mode"
done
# A get whose trailer does not vouch for the body lands no file either: a length or a digest that is not the
# body's own, an unhashed body, or one that ended early. A digest that is not its own means a piece was
# damaged on the way, so it is asked for again, three tries in all.
while read -r mode tries; do
	: >"$FAKE/log"
	faulted "$mode" fails wireGet 127.0.0.1 "$fakePort" "/3ds/c d.bin" "$TMP/bad"
	[ ! -e "$TMP/bad" ] || fail "get wrote a file under $mode"
	expect "$(count get)" "$tries" "the tries of a get under $mode"
done <<'EOF'
miscount 1
misdigest 3
unhashed 1
ended-early 1
no-trailer 3
half-trailer 3
range-short 1
EOF
# A get whose trailer never comes lands no file either. The quiet piece is tried again while the console
# answers a stats.
until steadyRun; do
	: >"$FAKE/log"
	WIRE_WORKER_SILENCE=1 faulted stall-trailer fails wireGet 127.0.0.1 "$fakePort" "/3ds/c d.bin" "$TMP/bad"
	[ ! -e "$TMP/bad" ] || fail "get wrote a file whose trailer never came"
done
expect "$(count get):$(count stats)" 3:2 "the tries of a get whose trailer never came, and the stats between"
# Put and delete inside a save, whose title is installed.
mkdir -p "$FAKE/save:000400000f800100"
echo "000400000f800100 1" >>"$FAKE/installed"
"$ROOT/tools/device.sh" put 127.0.0.1 "$TMP/f" "save:000400000f800100/dir/b c.bin" >/dev/null || fail "put into a save"
head -c 131072 /dev/zero >"$TMP/zeros.sav"
THREEDROP_COMPRESSION=auto wireFilePut 127.0.0.1 "$fakePort" "$TMP/zeros.sav" save:000400000f800100/z.sav >/dev/null || fail "an auto put into a save"
expect "$(cat "$FAKE/compression")" none "no gzip into a save"
# When a save's secure value will not go, nothing is committed, on a put and a delete alike, and the status is
# 1.
case "$(faulted secure-value wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" save:000400000f800100/v.sav 2>&1)" in
*" put rejected secure-value c8a04554"$'\n'"$TOOL: the save's secure value would not go, so nothing was committed") ;;
*) fail "a put whose secure value would not go was not said" ;;
esac
[ ! -e "$FAKE/save:000400000f800100/v.sav" ] || fail "a put whose secure value would not go was committed"
faulted secure-value fails "$ROOT/tools/device.sh" delete 127.0.0.1 "save:000400000f800100/dir/b c.bin"
[ -e "$FAKE/save:000400000f800100/dir/b c.bin" ] || fail "a delete whose secure value would not go was committed"
"$ROOT/tools/device.sh" delete 127.0.0.1 "save:000400000f800100/dir/b c.bin" >/dev/null || fail "delete in a save"
[ ! -e "$FAKE/save:000400000f800100/dir/b c.bin" ] || fail "the save's file stayed"
case "$(faulted lost-reply wireMkdir 127.0.0.1 "$fakePort" /lost)" in "response "*" mkdir committed ok 00000000") ;; *) fail "a lost mutation reply read back from the receipt" ;; esac
: >"$FAKE/log"
case "$(faulted lost-reply wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" /lost/f 2>/dev/null)" in "response "*" put committed ok 00000000 10 u:"*) ;; *) fail "a lost file put reply read back from the receipt" ;; esac
cmp -s "$TMP/f" "$FAKE/sd/lost/f" || fail "the file put whose reply was lost"
expect "$(count put)" 1 "file puts sent for one whose reply was lost (it may have run)"
# A refused put says why. A put whose body the transport cut (short, receive, timeout) exits 4, and any other
# refusal exits 1.
while read -r word st words; do
	got=0
	faulted "rejected-$word" wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" /refused.bin >/dev/null 2>"$TMP/err" || got=$?
	expect "$got" "$st" "a put refused $word"
	said "$TOOL: $words" "a put refused $word"
done <<'EOF'
short 4 the body ended before its size
receive 4 the body stopped arriving
timeout 4 the body went quiet past the console's limit
space 1 the card has no room for the file
open 1 the console could not make the file there
write 1 the card refused a write
publish 1 the file came whole, but the card would not give it its name
ring 1 the console had no memory for the transfer
worker-stall 1 the console's card or AM call answered nothing for 600 s
writer 1 the console had no memory to start the install
not-newer 1 the console holds this title at the same version or newer
metadata 1 that is not a CIA 3Drop can read
am-info 1 AM would not say what is installed of this title
exists 1 it is there already: the title is installed
notfound 1 that title, or its save data, is not on the console
path 1 the console cannot use that path
hbldr 1 Luma's 3dsx loader is off, missing
stdio 1 the console could not tell this PC's address
EOF
wireDelete 127.0.0.1 "$fakePort" "/3ds/c d.bin" >/dev/null || fail "delete"
fails wireLs 127.0.0.1 "$fakePort" "/3ds/c d.bin" d
fails wireDelete 127.0.0.1 "$fakePort" "/new dir/x"

# Which bodies go gzip is tested in testGzip. The gets below read /x, which holds random bytes.
head -c 131072 /dev/zero >"$TMP/z.bin"
openssl rand 65536 >"$TMP/r.bin"
cp "$TMP/r.bin" "$FAKE/sd/x"
# A Ctrl-C or a deadline TERM sent to the job in the middle of gzip ends the put, with nothing sent and no
# temp left (the put has no scratch of its own). The job gets INT back, because bash starts a background test
# with it ignored.
mkdir "$TMP/temps"
for sig in INT TERM; do
	rm -f "$TMP/gzipping"
	: >"$FAKE/log"
	# Signaled 0.2 s after the stub's file appears, as the checks below are. Signaled at once, a loaded box
	# could deliver the signal between the stub's `: >` and its sleep.
	# shellcheck disable=SC2016 # expanded when the job runs
	GZIPPING=$TMP/gzipping TMPDIR=$TMP/temps THREEDROP_COMPRESSION=auto inGroup "$sig" 0.2 "$TMP/gzipping" 0 "" \
		bash -c '. "$1/tools/wire.sh"; gzip() { : >"$GZIPPING"; sleep 30; }; wireFilePut 127.0.0.1 "$3" "$2" /z.bin' _ "$ROOT" "$TMP/z.bin" "$fakePort"
	[ -e "$TMP/gzipping" ] || fail "the put never reached its gzip"
	! grep -q '^put ' "$FAKE/log" || fail "a $sig mid-gzip still sent the put"
	[ -z "$(ls -A "$TMP/temps")" ] || fail "a $sig mid-gzip left $(ls -A "$TMP/temps")"
done
# Ctrl-C, Ctrl-\ or a hangup ends a stalled get or /dev/tcp put entirely, so nothing holds the connection.
export -f devtcp without
for sig in INT QUIT HUP; do
	: >"$FAKE/log"
	# shellcheck disable=SC2016 # expanded when the job runs
	faulted stall-body inGroup "$sig" 0.2 "$FAKE/log" 1 "get " \
		bash -c '. "$1/tools/wire.sh"; scratch; wireGet 127.0.0.1 "$2" /x "$3"' _ "$ROOT" "$fakePort" "$TMP/stalled"
	# shellcheck disable=SC2016 # expanded when the job runs
	faulted mute-put inGroup "$sig" 0.2 "$FAKE/log" 1 "put " \
		bash -c '. "$1/tools/wire.sh"; scratch; devtcp wireFilePut 127.0.0.1 "$2" "$3" /muted' _ "$ROOT" "$fakePort" "$TMP/r.bin"
done
# The fake, playing nc in the host's group, ends when that group gets TERM in the middle of its hold, because
# no console would linger.
request="stats $(newRequestID) op"
: >"$FAKE/log"
# shellcheck disable=SC2016 # expanded when the job runs
faulted mute inGroup TERM 0.2 "$FAKE/log" 1 "stats " \
	bash -c '{ printf "%s\n%s\n" "$1" "$2"; sleep 30; } | { . "$FAKE_LIB"; fakeDevice; }' _ "$request" "$(macRequest "$request")"

# Listings stream whole and uncapped, up to their trailer, and a name's control bytes never reach the
# terminal.
for i in $(seq 0 164); do printf '%016x 65535\n' $((0x0004000000010000 + (i << 8))); done >"$FAKE/installed"
titles=$(wireLs 127.0.0.1 "$fakePort" title:) || fail "ls title:"
expect "$(grep -c . <<<"$titles")" 165 "ls title: in one listing"
expect "$(sed -n 65p <<<"$titles")" "t 65535 0 0004000000014000" "a title line"
expect "$("$ROOT/tools/device.sh" titles 127.0.0.1 | grep -c .)" 165 "device.sh titles"
mkdir -p "$FAKE/sd/dir/sub" "$FAKE/sd/evil"
printf 'hello' >"$FAKE/sd/dir/hello.txt"
printf 'x' >"$FAKE/sd/dir/sub/b"
: >"$FAKE/sd/evil/$(printf '\033[2J\007\177x\302\233y')" # ESC [ 2 J (clear screen), BEL, DEL, "x", C1 CSI, "y"
expect "$(wireLs 127.0.0.1 "$fakePort" /dir | sort)" $'d - - sub\nf 5 - hello.txt' "ls names"
expect "$(wireLs 127.0.0.1 "$fakePort" /dir r | grep -c .)" 3 "ls r walks the tree"
expect "$(wireLs 127.0.0.1 "$fakePort" /evil)" "f 0 - ?[2J??x?y" "a device name's control bytes"
mkdir -p "$FAKE/sd/utf"
: >"$FAKE/sd/utf/$(printf 'caf\303\251 menu')"
expect "$(wireLs 127.0.0.1 "$fakePort" /utf)" "f 0 - $(printf 'caf\303\251 menu')" "a UTF-8 name with a space"
# A UTF-8 path goes as hex on every verb, byte-exact. A non-ASCII top folder takes writes, unless it is as
# long as a protected name (Nıntendo 3DS).
saves="/3ds/Checkpoint/saves/0x0055D ポケモン"
wireMkdir 127.0.0.1 "$fakePort" "$saves" >/dev/null || fail "mkdir of a UTF-8 path"
wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" "$saves/セーブ.bin" >/dev/null || fail "put to a UTF-8 path"
expect "$(wireLs 127.0.0.1 "$fakePort" "$saves")" "f 10 - セーブ.bin" "ls of a UTF-8 path"
wireRename 127.0.0.1 "$fakePort" "$saves/セーブ.bin" "$saves/😀.bin" >/dev/null || fail "rename of a UTF-8 path"
landed "$TMP/f" "$TMP/u.out" "get of a UTF-8 path" wireGet 127.0.0.1 "$fakePort" "$saves/😀.bin" "$TMP/u.out"
wireDelete 127.0.0.1 "$fakePort" "$saves" r >/dev/null || fail "delete of a UTF-8 path"
[ ! -e "$FAKE/sd$saves" ] || fail "the UTF-8 directory stayed"
mkdir -p "$FAKE/sd/音楽"
printf 'song' >"$FAKE/sd/音楽/x"
landed "$TMP/f" "$FAKE/sd/音楽/y" "a put under a non-ASCII top folder" wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" /音楽/y
{ wireGet 127.0.0.1 "$fakePort" /音楽/x "$TMP/u.out" >/dev/null && [ "$(cat "$TMP/u.out")" = song ]; } || fail "a get under a non-ASCII top folder"
case "$(wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" "/Nıntendo 3DS/y" 2>&1)" in *" put rejected refused 00000000"*) ;; *) fail "a put under a look-alike of /Nintendo 3DS" ;; esac
for mode in ended-early miscount misdigest; do # a listing its trailer does not vouch for
	faulted "$mode" fails wireLs 127.0.0.1 "$fakePort" /dir
done
case "$(faulted unhashed wireLs 127.0.0.1 "$fakePort" /dir 2>&1)" in
*"no digest to check it by: hash c8804478"*) ;;
*) fail "a listing the console could not digest was not said" ;;
esac

# Screenshot: each screen is a mutation in get's frame, byte-exact, and the requests are chained on each
# reply's nonce.
openssl rand 4096 >"$FAKE/top.bmp"
openssl rand 3000 >"$FAKE/bottom.bmp"
: >"$FAKE/log"
wireScreenshot 127.0.0.1 "$fakePort" "$TMP/shot" >/dev/null || fail "screenshot"
{ cmp -s "$FAKE/top.bmp" "$TMP/shot-top.bmp" && cmp -s "$FAKE/bottom.bmp" "$TMP/shot-bottom.bmp"; } || fail "screenshots were not byte-exact"
expect "$(count screenshot)" 2 "one request per screen"
# A committed run leaves 3Drop, and its nonce goes with it, so the host reads before it spends again.
until steadyRun; do
	"$ROOT/tools/device.sh" run 127.0.0.1 self >/dev/null
	: >"$FAKE/log"
	"$ROOT/tools/device.sh" screenshot 127.0.0.1 "$TMP/again" >/dev/null || fail "a screenshot after a relaunch"
done
expect "$(awk '{ printf "%s ", $1 }' "$FAKE/log")" "stats screenshot screenshot " "a screenshot after a relaunch"
# A relaunch the host never saw leaves the old nonce in place. The request is refused and sent once more on
# the live nonce.
until steadyRun; do
	# shellcheck disable=SC2031 # set by _wireNow just before: the frozen clock stayed in its subshell
	_wireNow && printf '%s %064d %s\n' "$wireNow" 0 "$WIRE_PROTO" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
	: >"$FAKE/log"
	"$ROOT/tools/device.sh" screenshot 127.0.0.1 "$TMP/again" >/dev/null || fail "a screenshot on a stale nonce"
done
expect "$(count screenshot)" 3 "screenshot requests on a stale nonce (its one retry)"
# A screenshot whose reply is lost drops the nonce it sent, as every lost exchange does, so the next change
# reads a live nonce rather than spending a spent one.
# shellcheck disable=SC2031 # set by _wireNow just before: the frozen clock stayed in its subshell
_wireNow && printf '%s %064d %s\n' "$wireNow" 0 "$WIRE_PROTO" >"$(_wireNonceFile 127.0.0.1 "$fakePort")"
st=0
faultedOn screenshot 1 hangup wireScreenshot 127.0.0.1 "$fakePort" "$TMP/lost" >/dev/null 2>&1 || st=$?
expect "$st" 4 "a screenshot whose reply was lost"
[ ! -e "$(_wireNonceFile 127.0.0.1 "$fakePort")" ] || fail "a screenshot whose reply was lost kept the nonce it sent"

# A long path goes as hex and its request runs past 511 bytes. This is tested with a put, a rename of two
# paths and a get.
printf -v long '/a %0240d' 0
wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" "$long" >/dev/null || fail "a put to a ${#long}-byte path"
wireRename 127.0.0.1 "$fakePort" "$long" "${long}b" >/dev/null || fail "a rename of two long paths"
landed "$TMP/f" "$TMP/long" "a get of a long path" wireGet 127.0.0.1 "$fakePort" "${long}b" "$TMP/long"

case "$("$ROOT/tools/device.sh" restart 127.0.0.1)" in "response "*" run committed ok 00000000 self") ;; *) fail "restart is run self" ;; esac
# restart returns once the relaunched 3Drop answers a `stats op` knock, sent every 0.5 s (the pauses are
# counted here, not slept). No version is wanted, so `stats self`, which waits on AM, is never asked.
(
	# shellcheck disable=SC2317 # called by device.sh
	sleep() { if [ "$1" = 0.5 ]; then echo "$1" >>"$TMP/knocks"; else "$realSleep" "$@"; fi; }
	export -f sleep
	export TMP
	rm -f "$FAKE/unanswered"
	: >"$FAKE/log"
	faulted slow-relaunch "$ROOT/tools/device.sh" restart 127.0.0.1 >/dev/null || fail "a slow restart"
)
expect "$(cat "$FAKE/down")" 0 "requests a slow relaunch left unanswered when restart returned"
rm "$FAKE/down"
expect "$(grep -c . "$TMP/knocks"):$(grep -c '^stats [0-9a-f]* op$' "$FAKE/unanswered")" "$FAKE_RELAUNCH_QUIET:$FAKE_RELAUNCH_QUIET" \
	"0.5 s pauses and stats op knocks while a relaunch answered none of its silent requests"
expect "$(grep -c '^stats [0-9a-f]* self$' "$FAKE/log"):$(grep -c '^stats [0-9a-f]* op$' "$FAKE/log")" 0:1 \
	"stats self and stats op asked of the relaunched 3Drop by a restart"
# A knock that the console takes and never answers is given up after 3 s, not after the control timeout's 15.
t0=$(msNow)
out=$(devtcp faulted mute wireWaitUp 127.0.0.1 "$fakePort" "" 1 2>&1) && fail "a wait on a mute console"
expect "$out" "$TOOL: no answer from 3Drop at 127.0.0.1" "a wait on a mute console"
[ $(($(msNow) - t0)) -lt 9000 ] || fail "a knock on a mute console was held $(($(msNow) - t0)) ms"
# The relaunch wait says what answered: another build, or nothing. The version comes in `stats op`, so
# `stats self`, which waits on AM, is asked only of a 3Drop from before that.
: >"$FAKE/log"
ver=$(wireWaitUp 127.0.0.1 "$fakePort" "" 0) || fail "a wait for any build"
[ "$(wireWaitUp 127.0.0.1 "$fakePort" "$ver" 1)" = "$ver" ] || fail "a wait for the running build"
expect "$(grep -c '^stats [0-9a-f]* self$' "$FAKE/log")" 0 "stats self asked by waits that stats op answered"
[ "$(faulted old-op wireWaitUp 127.0.0.1 "$fakePort" "" 0)" = "$ver" ] || fail "a wait on a 3Drop whose stats op has no version"
expect "$(grep -c '^stats [0-9a-f]* self$' "$FAKE/log")" 1 "stats self asked of a 3Drop whose stats op has no version"
out=$(wireWaitUp 127.0.0.1 "$fakePort" 99 0 2>&1) && fail "a wait for a build that is not running"
expect "$out" "$TOOL: 3Drop at 127.0.0.1 answered as build $ver, not 99" "a wait that found another build"
out=$(devtcp wireWaitUp 127.0.0.1 1 99 0 2>&1) && fail "a wait with nothing listening"
expect "$out" "$TOOL: no answer from 3Drop at 127.0.0.1" "a wait that found nothing"
# Only digits count as a version, because deploy.sh adds 1 to it and bash arithmetic would evaluate a name.
cp "$FAKE/installed" "$TMP/installed.kept"
echo "$FAKE_SELF x[0]" >>"$FAKE/installed"
out=$(wireWaitUp 127.0.0.1 "$fakePort" "" 0 2>&1) && fail "a wait took the version 'x[0]': $out"
mv "$TMP/installed.kept" "$FAKE/installed"
: >"$FAKE/log"
"$ROOT/tools/device.sh" reboot 127.0.0.1 >/dev/null 2>&1 && fail "device.sh still has reboot (cut after a device hang)"
[ ! -s "$FAKE/log" ] || fail "device.sh reboot sent a request"
mkdir -p "$FAKE/sd/3ds/my app"
: >"$FAKE/sd/3ds/my app/demo.3dsx"
case "$("$ROOT/tools/device.sh" run 127.0.0.1 "/3ds/my app/demo.3dsx" stdio)" in "response "*" run committed ok 00000000 3dsx 000400000d921e00") ;; *) fail "run of a 3dsx" ;; esac
grep -q "^run [0-9a-f]* [0-9a-f]* hex:2f3364732f6d79206170702f64656d6f2e33647378 stdio$" "$FAKE/log" || fail "run 3dsx request"
fails "$ROOT/tools/device.sh" run 127.0.0.1 "/3ds/my app/none.3dsx"
: >"$FAKE/sd/3ds/App.3Dsx"
case "$(wireRun 127.0.0.1 "$fakePort" /3ds/App.3Dsx)" in "response "*" 3dsx "*) ;; *) fail "a 3dsx name in mixed case" ;; esac
# A title goes in the device's one spelling, `title:<id>`, whatever case or form it was given in.
echo "000400000123abcd 1" >>"$FAKE/installed"
: >"$FAKE/log"
wireRun 127.0.0.1 "$fakePort" title:000400000123ABCD >/dev/null || fail "run title:<id>"
wireRun 127.0.0.1 "$fakePort" 000400000123ABCD >/dev/null || fail "run of an uppercase id"
wireLs 127.0.0.1 "$fakePort" title:000400000123ABCD >/dev/null || fail "ls title: of an uppercase id"
mkdir -p "$FAKE/save:000400000f800101"
echo "000400000f800101 1" >>"$FAKE/installed"
wireLs 127.0.0.1 "$fakePort" save:000400000F800101/ >/dev/null || fail "ls of an uppercase save id"
"$ROOT/tools/device.sh" run 127.0.0.1 SELF >/dev/null || fail "run SELF"
expect "$(awk '$1 != "stats" { print $1, $NF }' "$FAKE/log" | sort -u | tr '\n' ',')" "ls save:000400000f800101/,ls title:000400000123abcd,run self,run title:000400000123abcd," "ids as sent"
# A title with a romfs folder is launched only as a game. --game goes as the request's last word, for a title
# alone. A launch without it is refused with the way to start the title, and nothing is sent twice.
mkdir -p "$FAKE/sd/luma/titles/000400000123abcd/romfs"
: >"$FAKE/log"
"$ROOT/tools/device.sh" run 127.0.0.1 000400000123ABCD >/dev/null 2>"$TMP/err" && fail "run of a title with a romfs folder"
said "run rejected layeredfs" "the console's refusal"
said "tools/device.sh run <ip> --game <title-id>" "the way to start it"
"$ROOT/tools/device.sh" run 127.0.0.1 --game 000400000123ABCD >/dev/null || fail "run --game of a title with a romfs folder"
expect "$(awk '$1 == "run" { print $4, $5 }' "$FAKE/log" | tr '\n' ',')" "title:000400000123abcd ,title:000400000123abcd game," "the two launches as sent"
fails "$ROOT/tools/device.sh" run 127.0.0.1 --game
fails "$ROOT/tools/device.sh" run 127.0.0.1 --game self
fails "$ROOT/tools/device.sh" run 127.0.0.1 --game 000400000123ABCD stdio
rm -r "$FAKE/sd/luma/titles/000400000123abcd/romfs"

# The waits passed to nc depend on the nc there (man nc; ncat's manual). macOS's own nc bounds a connect by -G
# alone, which is given the control timeout. nmap's ncat bounds only the connect, by -w, so it is skipped and
# /dev/tcp is used.
printf 'said' >"$TMP/said.put"
# ncSaid <ostype> <nc's path> <want> <what> checks that the words each nc took, a read's and then a
# ghost-watched put's, are <want>.
ncSaid() (
	OSTYPE=$1 ncAt=$2
	# shellcheck disable=SC2317 # called by wire.sh
	command() { if [ "$*" = "-v nc" ]; then echo "$ncAt"; else builtin command "$@"; fi; }
	# shellcheck disable=SC2317
	nc() { echo "$*" >>"$TMP/nc.said" && fakeDevice; }
	rm -f "$TMP/nc.said"
	_wireNcKind
	wireRead 127.0.0.1 "$fakePort" stats >/dev/null || fail "a read with $ncAt on $OSTYPE"
	THREEDROP_GHOST_FLOOR=1 wireFilePut 127.0.0.1 "$fakePort" "$TMP/said.put" /said.put >/dev/null ||
		fail "a put with $ncAt on $OSTYPE"
	expect "$(cat "$TMP/nc.said" 2>/dev/null || :)" "$3" "$4"
)
plain="-w 15 127.0.0.1 $fakePort"$'\n'"-w $WIRE_WORKER_SILENCE 127.0.0.1 $fakePort"
ncSaid darwin23 /usr/bin/nc "-G 15 -w 15 127.0.0.1 $fakePort"$'\n'"-G 15 -w $WIRE_WORKER_SILENCE 127.0.0.1 $fakePort" \
	"macOS's own nc's words"
ncSaid darwin23 /opt/homebrew/bin/nc "$plain" "another nc's words on macOS"
ncSaid linux-gnu /usr/bin/nc "$plain" "nc's words on Linux"
mkdir "$TMP/ncat"
: >"$TMP/ncat/ncat"
ln "$TMP/ncat/ncat" "$TMP/ncat/nc"
ncSaid linux-gnu "$TMP/ncat/nc" "" "nmap's ncat's words"
# Git Bash reads replies through dd. The test runs that way wherever this dd is GNU's, and checks that a get
# taking many reads lands whole.
head -c 3000000 /dev/urandom >"$TMP/dd.bin"
cp "$TMP/dd.bin" "$FAKE/sd/dd.bin"
(
	OSTYPE=msys
	_wireCountBytes
	if [ -n "$wireCountBytes" ]; then
		landed "$TMP/dd.bin" "$TMP/dd.out" "a get read through dd" devtcp wireGet 127.0.0.1 "$fakePort" /dd.bin "$TMP/dd.out"
	else
		skip "no dd here that counts bytes, so Git Bash's reply reader is unchecked"
	fi
)

# With no nc (Windows), a put's signed reply survives (its fd stays open) and a get is byte-exact.
fakeCia "$cia" 0004000001234600 3 1048576
case "$(devtcp wirePut 127.0.0.1 "$fakePort" new "$cia")" in "put committed ok 00000000 0004000001234600 3 new") ;; *) fail "/dev/tcp put reply" ;; esac
expect "$(tail -1 "$FAKE/bytes")" "$(fileSize "$cia")" "/dev/tcp put body bytes"
landed "$TMP/r.bin" "$TMP/tcp" "/dev/tcp get" devtcp wireGet 127.0.0.1 "$fakePort" /x "$TMP/tcp"
# Every /dev/tcp step is bounded and reports once that it timed out. The cases here are a device that stops
# taking the body and a connect that nothing answers (the control timeout). A device that never answers is
# tested in testAnswers.
request="stats $(newRequestID)"
head -c 33554432 /dev/zero >"$TMP/big"
bounded() { # <what> <said> <command...>
	local t0=$SECONDS st=0
	"${@:3}" >/dev/null 2>"$TMP/said" || st=$?
	[ "$st" != 0 ] || fail "$1 was not stopped"
	[ $((SECONDS - t0)) -le 15 ] || fail "$1 held the client $((SECONDS - t0)) s"
	[ "$(cat "$TMP/said")" = "$2" ] || fail "$1 said: $(cat "$TMP/said")"
}
bounded "a device that stops taking the body" "$TOOL: timed out: 127.0.0.1:$fakePort went quiet for 1 s" \
	faulted deaf devtcp _wireTransport 127.0.0.1 "$fakePort" 1 "$request" "$(macRequest "$request")" "$TMP/big"
THREEDROP_TIMEOUT=1 bounded "a connect nothing answers" "$TOOL: timed out: no connection to 192.0.2.1:$fakePort in 1 s" \
	_wireTransport 192.0.2.1 "$fakePort" 60 "$request" "$(macRequest "$request")" "" "$TMP/reply"
# A bound is in seconds, not watchdog ticks, which a slow host can take well over a second each (Git Bash 1.6
# s or more). A console that takes the request and says nothing is let go 3 to 4 s in under a 3 s timeout,
# never 4 ticks in, and never early. A step of the wall clock, which SECONDS follows, moves that by a tick at
# most. Here the step is a minute either way, made in the watchdog's SECONDS after its first sleep. Going
# forward, that is 2 s at the earliest, never at once, because the stepped tick is credited 2 s (the cap in
# _wireTcpTick), and a SECONDS set late in a second reads one more almost at once, so the tick after it can be
# credited 2 s too. A real step back inside an arm (WSL's) costs it a tick or two more, which the arms that
# assume none would call 4 ticks in. Such an arm is run again (steadyRun, lib.sh).
# Timing-bound: under a loaded gate, run it alone.
for step in 0 60 -60; do
	until steadyRun; do
		t0=$(msNow)
		st=0
		(
			# shellcheck disable=SC2317 # called by watchOwner, in the watchdog
			sleep() {
				"$realSleep" "$@"
				[ -n "${stepped:-}" ] || SECONDS=$((SECONDS + step)) stepped=1
			}
			THREEDROP_TIMEOUT=3 devtcp faulted mute wireExchange 127.0.0.1 "$fakePort" "$request"
		) >/dev/null 2>"$TMP/said" || st=$?
		held=$(($(msNow) - t0))
	done
	expect "$st:$(cat "$TMP/said")" "124:$TOOL: timed out: 127.0.0.1:$fakePort went quiet for 3 s" "a mute console under a 3 s timeout, the clock stepped $step s"
	{ [ "$held" -ge $((step > 0 ? 2000 : 3000)) ] && [ "$held" -lt $((step < 0 ? 8000 : 6000)) ]; } ||
		fail "a mute console under a 3 s timeout, the clock stepped $step s, held the client $held ms"
done
# Once the whole body has gone, a timeout signals only the reader, because the reaped writer's pid may be
# reused.
printf 'body' >"$TMP/body"
: >"$TMP/signalled"
(
	# shellcheck disable=SC2317 # called by wire.sh
	kill() {
		[[ "$1" == -* ]] || echo "$*" >>"$TMP/signalled"
		builtin kill "$@"
	}
	faulted mute devtcp _wireTransport 127.0.0.1 "$fakePort" 1 "$request" "$(macRequest "$request")" "$TMP/body" >/dev/null 2>&1 || :
)
expect "$(awk '{ print NF }' "$TMP/signalled")" 1 "the processes a timeout after a whole body signalled"
# The bound is on silence: a body in five pieces 1.5 s apart arrives whole under a 3 s bound. Its
# nonce is kept as of the request, since the header left before the body.
t0=$(date +%s)
(
	WIRE_WORKER_SILENCE=3
	landed "$TMP/r.bin" "$TMP/slow" "a slow get under its bound" faulted slow-body wireGet 127.0.0.1 "$fakePort" /x "$TMP/slow"
)
read -r stamp _ <"$(_wireNonceFile 127.0.0.1 "$fakePort")"
[ $((stamp - t0)) -le 3 ] || fail "a 6 s stream's nonce was kept as of $((stamp - t0)) s after its request"
# A reply never outgrows what its verb allows plus its trailer. A console that sends on past a get's trailer
# is cut off there, which fails the get, and it never fills this host's disk.
st=0
faulted endless wireGet 127.0.0.1 "$fakePort" /x "$TMP/endless" >/dev/null 2>&1 || st=$?
[ "$st" = 4 ] || fail "a get whose reply went on past its trailer exited $st"
read -r pushed <"$FAKE/endless"
[ "$pushed" -lt 64 ] || fail "the host read $pushed units of 1048576 bytes past a get's trailer"
[ ! -e "$TMP/endless" ] || fail "a get whose reply went on past its trailer landed"
# The same limit applies to a reply over nc, either a short reply or a put's reply on the ghost watch's path.
printf 'small' >"$TMP/small.put"
for path in "a read's reply" "a put's reply on the ghost watch's path"; do
	rm -f "$FAKE/endless"
	case "$path" in
	"a read's"*) faulted endless wireRead 127.0.0.1 "$fakePort" stats >/dev/null 2>&1 || : ;;
	*) THREEDROP_GHOST_FLOOR=1 faulted endless wireFilePut 127.0.0.1 "$fakePort" "$TMP/small.put" /endless.put >/dev/null 2>&1 || : ;;
	esac
	read -r pushed <"$FAKE/endless"
	[ "$pushed" -lt 64 ] || fail "the host read $pushed units of 1048576 bytes past $path"
done
# With no reply, the tool says so once per exchange, whether nothing listens or the device closes without a
# word.
out=$(wireGet 127.0.0.1 1 /x "$TMP/none" 2>&1) && fail "a get with nothing listening"
none="$TOOL: no reply from 127.0.0.1:1"
expect "$out" "$none"$'\n'"$none"$'\n'"$none"$'\n'"$TOOL: the piece at byte 0 failed 3 times" "a get with nothing listening"
out=$(faulted hangup wireLs 127.0.0.1 "$fakePort" /dir 2>&1) && fail "a listing with no reply"
expect "$out" "$TOOL: no reply from 127.0.0.1:$fakePort" "a listing with no reply"
# A connect refused at once never runs the caller's EXIT trap in the exchange's watchdog, which is what bash 5
# does in a subshell that a TERM reaches the instant after its fork. Here that would end the fake device.
mac=$(macRequest "$request")
(
	trap 'echo x >>"$TMP/trapped"' EXIT
	for _ in 1 2 3 4; do (while [ ! -e "$TMP/trapless" ]; do :; done) & done # load, which widens the instant
	for _ in $(seq 1 50); do _wireTransport 127.0.0.1 1 1 "$request" "$mac" "" "$TMP/reply" 2>/dev/null || :; done
	: >"$TMP/trapless"
	wait
	trap - EXIT
)
[ ! -e "$TMP/trapped" ] || fail "an exchange's watchdog ran the caller's EXIT trap $(wc -l <"$TMP/trapped") times"
# A refusal before the body is the answer on both transports, never a failed send of the untaken body.
head -c 1048576 /dev/zero >"$TMP/m"
for transport in "" devtcp; do
	out=$(faulted refuse-early $transport wireFilePut 127.0.0.1 "$fakePort" "$TMP/m" /m 2>&1) && fail "a refused put succeeded"
	[[ "$out" == "$TOOL: response "*" put rejected space 00000000"$'\n'"$TOOL: the card has no room for the file" ]] ||
		fail "an early refusal ${transport:+over /dev/tcp }said: $out"
done

# A fork the listener cannot make drops that request alone, never every one after it.
: >"$FAKE/fork-fails"
st=0
wireLs 127.0.0.1 "$fakePort" / >/dev/null 2>&1 || st=$?
expect "$st:$([ -e "$FAKE/fork-fails" ] && echo planted || echo spent)" 4:spent "the request the listener could not fork for"
wireLs 127.0.0.1 "$fakePort" / >/dev/null || fail "the listener served nothing after a fork it could not make"

# deploy.sh numbers each build one past the installed one, up to the next release's number, N x 1024
# (tools/release.sh), which it never takes. Below the band's edge it goes on to build (a stub that exits 7
# here). At the edge it stops before anything is built, and says how to get a new run.
mkdir -p "$TMP/deployTree/tools"
cp "$ROOT"/tools/{lib,wire,cia,deploy}.sh "$TMP/deployTree/tools/"
printf '#!/bin/sh\nexit 7\n' >"$TMP/deployTree/tools/build.sh"
chmod +x "$TMP/deployTree/tools/"*.sh
cp "$FAKE/installed" "$TMP/installed.kept"
for edge in 1:7 1022:7 1023:1:before 1024:7 2046:7 2047:1:'on top of' 64512:7 65534:7 65535:1:last; do
	IFS=: read -r ver want on <<<"$edge"
	cp "$TMP/installed.kept" "$FAKE/installed"
	echo "$FAKE_SELF $ver" >>"$FAKE/installed"
	st=0
	"$TMP/deployTree/tools/deploy.sh" 127.0.0.1 >/dev/null 2>"$TMP/err" || st=$?
	expect "$st" "$want" "deploy.sh over title version $ver"
	case "$on" in
	before) saidLine "deploy: 3Drop has 1023 builds before release v1, all one release leaves room for: for a new run, install the next release if it is out (scan the update QR in 3Drop, or drop.sh --self), or reinstall a release with FBI" ;;
	last) saidLine "deploy: 3Drop has 1023 builds on top of release v63, all one release leaves room for, and v63 is the last release: for a new run, reinstall a release with FBI" ;;
	on*) saidLine "deploy: 3Drop has 1023 builds on top of release v$((ver / 1024)), all one release leaves room for: for a new run, install the next release if it is out (scan the update QR in 3Drop, or drop.sh --self), or reinstall a release with FBI" ;;
	esac
done
cp "$TMP/installed.kept" "$FAKE/installed"

# These refusals once had no words and now have them: a self-update that is not newer, and a request the
# console's 3Drop does not know.
echo "$FAKE_SELF 52" >>"$FAKE/installed"
fakeCia "$TMP/self.cia" "$FAKE_SELF" 52
run 1 "$ROOT/tools/drop.sh" --self "$TMP/self.cia" 127.0.0.1
saidLine "drop: 3Drop runs this build or a newer one already, and takes only a newer one: tools/deploy.sh numbers its build past the installed one, and release vN installs over every build deployed on the release before it; FBI installs an older release"
run 1 wireRead 127.0.0.1 "$fakePort" nope
saidLine "$TOOL: the console's 3Drop does not know this request: a typo, or 3Drop and these tools may come from different releases; update 3Drop (in 3Drop, press Y and scan the update QR), or check out the tag its top screen names"

# Every signed reply names a protocol epoch, which is kept with the nonce it brought (src/protocol.h). A
# console relaunched as another build (a QR update the tools never saw) mints a new nonce and names its own
# epoch and release.
relaunchAs() { # <epoch|none> [release]
	echo "$1" >"$FAKE/proto"
	echo "${2:-dev}" >"$FAKE/rel"
	rm -f "$FAKE/nonce"
}
nonceFile=$(_wireNonceFile 127.0.0.1 "$fakePort")
newer="3Drop v2 speaks protocol 2, newer than these tools' 1, so only stats and a self-update work with it: update these tools: git fetch --tags && git checkout v2"
# A QR relaunch, then `stats self`, then a change. In separate runs, with a nonce kept from before and without
# one, the change is refused before it goes, and the read the nonce came from says why.
for kept in kept none; do
	relaunchAs "$FAKE_PROTO"
	wireRead 127.0.0.1 "$fakePort" stats >/dev/null
	[ "$kept" = kept ] || rm -f "$nonceFile"
	relaunchAs 2 v2
	: >"$FAKE/log"
	"$ROOT/tools/device.sh" stats 127.0.0.1 self >/dev/null || fail "stats self of a later protocol's 3Drop ($kept)"
	run 1 "$ROOT/tools/device.sh" mkdir 127.0.0.1 /changed
	saidLine "device: $newer" "a change after a QR relaunch and stats self ($kept)"
	expect "$(count mkdir)" 0 "changes sent to a later protocol's 3Drop after stats self ($kept)"
	[ ! -e "$FAKE/sd/changed" ] || fail "a change ran on a later protocol's 3Drop ($kept)"
	read -r _ _ epoch <"$nonceFile"
	expect "$epoch" 2 "the epoch kept with the nonce ($kept)"
done
# With the old nonce kept and no read after the relaunch, the change goes out on that nonce and is refused
# unrun (`challenge`). That refusal's epoch stops the retry, because a request whose meaning changed never
# runs.
relaunchAs "$FAKE_PROTO"
wireRead 127.0.0.1 "$fakePort" stats >/dev/null
relaunchAs 2 v2
: >"$FAKE/log"
run 1 wireMkdir 127.0.0.1 "$fakePort" /changed
saidLine "$TOOL: $newer"
expect "$(count mkdir)" 1 "changes sent on a nonce from before a relaunch"
[ ! -e "$FAKE/sd/changed" ] || fail "a change ran on a later protocol's 3Drop"
# A read's reply is refused as it comes in, and a stream's before its body. `stats` is never refused.
run 1 wireHash 127.0.0.1 "$fakePort" /x
saidLine "$TOOL: $newer"
run 1 wireGet 127.0.0.1 "$fakePort" /x "$TMP/x.later"
[ ! -e "$TMP/x.later" ] || fail "a get from a later protocol's 3Drop landed"
"$ROOT/tools/device.sh" stats 127.0.0.1 >/dev/null || fail "stats of a later protocol's 3Drop"
# An authentication failure is not an incompatibility. It exits 4 and drops the nonce and its epoch.
st=0
faulted bad-mac wireRead 127.0.0.1 "$fakePort" stats >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$([ -e "$nonceFile" ] && echo kept || echo dropped)" 4:dropped "a reply that does not verify"
saidLine "$TOOL: response authentication failed (try 3 of 3)"
# The release is shown, never run. A checkout is named only for a release tag.
relaunchAs 2 'v2-3-gabc1234-dirty'
run 1 wireMkdir 127.0.0.1 "$fakePort" /changed
saidLine "$TOOL: 3Drop v2-3-gabc1234-dirty speaks protocol 2, newer than these tools' 1, so only stats and a self-update work with it: update these tools to the release on the console's top screen"
relaunchAs 2 'v2;touch'
run 1 wireMkdir 127.0.0.1 "$fakePort" /changed
saidLine "$TOOL: 3Drop speaks protocol 2, newer than these tools' 1, so only stats and a self-update work with it: update these tools to the release on the console's top screen"
# For an older console, here one from before epochs, these tools say to update 3Drop.
relaunchAs none v1
run 1 wireMkdir 127.0.0.1 "$fakePort" /changed
saidLine "$TOOL: 3Drop v1 speaks a protocol from before epochs, older than these tools' 1, so only stats and a self-update work with it: update 3Drop (in 3Drop, press Y and scan the update QR, or send a newer release's 3drop.cia with tools/drop.sh --self)"
# The self-update goes to a 3Drop of any epoch, older or newer, with a word that it did so. Then the
# relaunched build answers.
for epoch in 2 none; do
	relaunchAs "$epoch"
	ver=$(wireWaitUp 127.0.0.1 "$fakePort" "" 0) || fail "the version of a protocol $epoch 3Drop"
	fakeCia "$TMP/self.cia" "$FAKE_SELF" $((ver + 1))
	run 0 "$ROOT/tools/drop.sh" --self "$TMP/self.cia" 127.0.0.1
	[[ "$(cat "$TMP/out")" == "put committed ok 00000000 $FAKE_SELF $((ver + 1)) self" ]] || fail "a self-update to a protocol $epoch 3Drop: $(cat "$TMP/out")"
	said "a self-update goes either way, so check that this build is the one you meant" "a self-update to a protocol $epoch 3Drop"
done
relaunchAs "$FAKE_PROTO"
rm -f "$FAKE/proto" "$FAKE/rel"

# The update bootstrap, which every epoch keeps (src/protocol.h), is pinned here as text, so a change to it
# fails a test before a release can strand the tools of every release before it.
expect "$PBKDF2_SALT:$PBKDF2_ITERS" 33447a6b3344ffee:10000 "the key's derivation"
: >"$FAKE/log"
wireRead 127.0.0.1 "$fakePort" stats op >/dev/null
ver=$(wireWaitUp 127.0.0.1 "$fakePort" "" 0)
fakeCia "$TMP/self.cia" "$FAKE_SELF" $((ver + 1))
"$ROOT/tools/drop.sh" --self "$TMP/self.cia" 127.0.0.1 >/dev/null || fail "the frozen self-update"
grep -qxE 'stats [0-9a-f]{32} op' "$FAKE/log" || fail "the frozen stats op request: $(cat "$FAKE/log")"
grep -qxE 'put [0-9a-f]{32} [0-9a-f]{64} self [0-9]+ [0-9]+ u:[0-9a-f]{64}' "$FAKE/log" || fail "the frozen self put request: $(cat "$FAKE/log")"
id=0123456789abcdef0123456789abcdef
expect "$(wireNonceless "response $id $(printf '%064d' 0) proto=7 stats ok ok 00000000 ver=3 rel=v9 op none")" \
	"response $id stats ok ok 00000000 ver=3 rel=v9 op none" "a reply's header, its nonce and epoch after its id"
expect "$(kv "stats ok ok 00000000 ver=3 rel=v9 op $id put committed ok 00000000 $FAKE_SELF 2048 self" ver)" 3 "stats op's version"
expect "$(_wireReceiptOf "stats ok ok 00000000 ver=3 rel=v9 op $id put committed ok 00000000 $FAKE_SELF 2048 self" "$id")" \
	"response $id put committed ok 00000000 $FAKE_SELF 2048 self" "the receipt, read to the reply's end"
echo "testWire: PASS"

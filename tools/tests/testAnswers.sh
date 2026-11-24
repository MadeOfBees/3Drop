#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2034,SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# Every tool answers by one exit table: 0 ok, 1 the console refused, 2 usage (testTools), 3 a bench
# rep holds the console (testBenchRep), 4 no answer, 124 timed out, 42 a send stopped as a ghost
# (testGhostRetry), 141 a `get ... -` whose reader stopped early. stdout carries data only; a
# refusal or error goes to stderr as `<tool>: ...`, with a sentence where its word needs one, and
# an explained refusal is never sent again. Here too: replies lost on the way, `--system` and
# /boot.firm puts, name patterns, hash and screenshot.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
printf 'file\000ops\377\n' >"$TMP/f" # 10 bytes, including a NUL and one byte past ASCII
request="stats $(newRequestID)"
answers() { # <status> <stdout: data|none> <stderr, a pattern> <command...>
	local st=0
	"${@:4}" >"$TMP/answer.out" 2>"$TMP/answer.err" || st=$?
	[ "$st" = "$1" ] || fail "${*:4} exited $st, not $1: $(cat "$TMP/answer.err")"
	if [ "$2" = none ] && [ -s "$TMP/answer.out" ]; then fail "${*:4} put a refusal on stdout: $(cat "$TMP/answer.out")"; fi
	if [ "$2" = data ] && [ ! -s "$TMP/answer.out" ]; then fail "${*:4} printed no data"; fi
	# shellcheck disable=SC2053 # $3 is a pattern
	[[ "$(cat "$TMP/answer.err")" == $3 ]] || fail "${*:4} said: $(cat "$TMP/answer.err")"
}
device=$ROOT/tools/device.sh
answers 0 data "" "$device" mkdir 127.0.0.1 /answers
answers 1 none "device: response * delete rejected notfound 00000000" "$device" delete 127.0.0.1 /answers/none
# A word the console does not take is refused as `grammar`, and the tool prints the one sentence it needs.
answers 1 none "device: response * stats rejected grammar 00000000"$'\n'"device: the console's 3Drop does not take a word of this request: 3Drop and these tools may come from different releases; its top screen names its release: check out that tag, or update 3Drop" \
	"$device" stats 127.0.0.1 bogus
answers 1 none "device: response * ls rejected notfound 00000000" "$device" ls 127.0.0.1 /answers/none
answers 1 none "device: response * ls rejected unmanaged 00000000"$'\n'"device: 3Drop never touches this title"* \
	"$device" ls 127.0.0.1 title:0004003000008f02
fakeCia "$TMP/answer.cia" 0004000001234700 1
echo "0004000001234700 1" >>"$FAKE/installed"
answers 1 none "drop: put rejected exists 00000000 0004000001234700 1 new"$'\n'"drop: it is there already: *" "$ROOT/tools/drop.sh" --new "$TMP/answer.cia" 127.0.0.1
# A commit that names other bytes than these fails, and the tool says the console's file is not this one.
answers 1 none "$TOOL: the reply names other bytes than these: response * put committed ok 00000000 10 u:0000*" \
	faulted other-bytes wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" /other/f
# (It goes over /dev/tcp because the listener's device keeps the tests' key, which an nc child would not.)
otherKey() (
	THREEDROP_KEY=another-key-1234
	devtcp wireRead 127.0.0.1 "$fakePort" stats
)
answers 1 none "$TOOL: the 3Drop at 127.0.0.1:$fakePort holds another key: THREEDROP_KEY is not the key on its screen, or 127.0.0.1:$fakePort is not your console" otherKey
# A put made with another key on a young kept nonce gets an unsigned refusal before anything ran, and
# that is the answer.
otherKeyPut() (
	THREEDROP_KEY=another-key-1234
	devtcp wireFilePut 127.0.0.1 "$fakePort" "$TMP/f" /other-key
)
wireRead 127.0.0.1 "$fakePort" stats >/dev/null
answers 1 none "$TOOL: the 3Drop at 127.0.0.1:$fakePort holds another key: THREEDROP_KEY is not the key on its screen, or 127.0.0.1:$fakePort is not your console" otherKeyPut
# A change done with a caveat is `unknown`, never `committed`. The tool explains it and exits 1.
answers 1 none "device: response * delete unknown ticket d8a08006 0004000001234700"$'\n'"device: the title is gone, but AM could not say its ticket is: it may still be held" \
	faulted unknown-ticket "$device" uninstall 127.0.0.1 --ticket 0004000001234700
# A put's story is the install's, not the uninstall's: here, a canceled `new` install whose ticket would
# not go. Nothing was installed ("the title is gone" would be false), and its ticket may remain.
unknownTicket="response 0 put unknown ticket d8a08004 0004000001234700 1 new"
answers 1 none "$TOOL: $unknownTicket"$'\n'"$TOOL: nothing was installed, but its ticket may remain, and HOME will not show it: the console's screen names the title ID and FBI's way to delete it" \
	wireReport "$unknownTicket"
# At the default pause, a failed read is tried again a second later: three tries, two pauses. Each
# failed try is reported with its number, then what to check is reported once.
tries() { # <tool> <what each try met> <ip:port>
	printf '%s: %s (try %s of 3)\n' "$1" "$2" 1 "$1" "$2" 2 "$1" "$2" 3
	echo "$1: no good answer from $3 in 3 tries: check the address on 3Drop's top screen, that 3Drop is open, and that this PC is on the console's network (tools/device.sh discover looks for it)"
}
t0=$(msNow)
answers 4 none "$(tries device "no reply from 127.0.0.1:$fakePort" "127.0.0.1:$fakePort")" \
	faulted hangup env -u _THREEDROP_RETRY_PAUSE "$device" stats 127.0.0.1
took=$(($(msNow) - t0))
[ "$took" -ge 2000 ] || fail "three tries of a read took $took ms at the default pause"
answers 4 none "$(tries device "response authentication failed" "127.0.0.1:$fakePort")" faulted bad-mac "$device" stats 127.0.0.1
answers 4 none "device: no reply from 127.0.0.1:$fakePort" faulted hangup "$device" ls 127.0.0.1 /
# A /dev/tcp request that is never answered: each try ends at its bound (1 s here) and is reported
# once with its number. The 45 s check below catches a hang.
quiet="$TOOL: timed out: 127.0.0.1:$fakePort went quiet for 1 s"
t0=$SECONDS
THREEDROP_TIMEOUT=1 answers 124 none "$(tries "$TOOL" "${quiet#"$TOOL: "}" "127.0.0.1:$fakePort")" \
	faulted mute devtcp wireRead 127.0.0.1 "$fakePort" stats
[ $((SECONDS - t0)) -le 45 ] || fail "a mute device held the client $((SECONDS - t0)) s"
answers 2 none "device: invalid title ID (16 hex digits)" "$device" uninstall 127.0.0.1 zzz
# An nc that goes quiet for the whole timeout is a timeout too, as on /dev/tcp. _wireTransport reads
# that off SECONDS, which follows the wall clock, and WSL steps the clock back 1.4 s every 30 s. So a
# quiet nc here waits on SECONDS too, until the 1 s timeout has passed on it. It counts from
# _wireTransport's own t0 (its local, which this nc sees through bash's dynamic scope), because a
# sample of its own would miss a step between the two. A step back only makes it wait longer. A sleep
# long enough to ride out a step would read as more than the timeout, and so pass a verdict that wants
# more.
quietUntil() { # <a SECONDS reading> <seconds>
	until [ $((SECONDS - $1)) -ge "$2" ]; do sleep 0.1; done
}
quietNc() (
	# shellcheck disable=SC2317 # what wire.sh calls
	nc() { quietUntil "$t0" 1; }
	_wireTransport 127.0.0.1 "$fakePort" 1 "$request" "$(macRequest "$request")"
)
answers 124 none "$quiet" quietNc
# A ghost-watched put (every drop.sh install over nc) that goes quiet is a timeout too. Here it goes
# through this host's nc to a mute device, with nc's own -w 1 and nothing made up after it, so an nc
# that fails at once is not a timeout. If the wall clock steps while it runs, it is run again
# (steadyRun, lib.sh); its checks come after the loop.
if hostNc=$(PATH=${PATH#"$FAKE/bin:"} type -P nc); then
	quietGhost() (
		# shellcheck disable=SC2317 # what wire.sh calls
		nc() { "$hostNc" "$@"; }
		THREEDROP_GHOST_FLOOR=1 _wireTransport 127.0.0.1 "$fakePort" 1 "$request" "$(macRequest "$request")" "$TMP/f"
	)
	until steadyRun; do
		st=0
		faulted mute quietGhost >"$TMP/answer.out" 2>"$TMP/answer.err" || st=$?
	done
	expect "$st:$(cat "$TMP/answer.out"):$(cat "$TMP/answer.err")" "124::$quiet" "a ghost-watched put through this host's nc to a mute device"
else
	skip "no nc here, so a ghost-watched put that goes quiet is unchecked"
fi
# When a request's reply is lost, its receipt says what became of it. When no receipt can, the status
# is the lost exchange's, never the receipt read's (a rep's lock taken since does not mean "nothing
# sent"). A request queued behind one that held 3Drop past the device's 15 s was refused unrun,
# whatever this host waits: another request's receipt says it did not run, and no answer says that
# 3Drop is busy or gone.
lostAsk() ( # <_wireReceipt's status> <wireExchange's>
	receiptSt=$1 querySt=$2
	# shellcheck disable=SC2317 # what wire.sh calls
	wireExchange() { return "$querySt"; }
	# shellcheck disable=SC2317
	_wireReceipt() { return "$receiptSt"; }
	_wireMutateRecovering 10 127.0.0.1 "$fakePort" "mkdir $(newRequestID) $(printf '%064d' 0) /x"
)
busy="$TOOL: no reply came, and no read of its receipt was answered: 3Drop is busy or gone; one queued behind a request that holds 3Drop past 15 s is refused unrun, and if 3Drop went away, tools/device.sh stats 127.0.0.1:$fakePort op says what became of it once it is back"
for st in 3 4 124; do answers 4 none "$busy" lostAsk "$st" 4; done
THREEDROP_TIMEOUT=1 answers 124 none "$busy" lostAsk 124 124
notRun="$TOOL: no reply came, and 3Drop's receipt is another request's: this one did not run (lost on its way, or queued behind a request that held 3Drop past 15 s, which refuses it unrun), unless it ran and another change has run since"
answers 4 none "$notRun" lostAsk 2 4
# A change the console closed without reading, as it does with a queued request, whose refusal goes
# nowhere: it was never done, and the tool says so.
answers 4 none "*$notRun" faultedOn mkdir 1 hangup wireMkdir 127.0.0.1 "$fakePort" /queued
[ ! -e "$FAKE/sd/queued" ] || fail "a change the console never read was done"
# A put that could not send its request has sent nothing (1); one that went quiet timed out (124).
putBody() ( # <_wireTransport's status>
	# shellcheck disable=SC2317 # what wire.sh calls
	_wireNonce() { printf '%064d\n' 0; }
	# shellcheck disable=SC2317
	_wireTransport() { return "$sent"; }
	# shellcheck disable=SC2317
	_wireReconcile() { return 4; }
	sent=$1
	_wirePutBody 127.0.0.1 "$fakePort" "$TMP/f" "/x 10 u:$(printf '%064d' 0)"
)
answers 1 none "" putBody 1
answers 124 none "" putBody 124
# A wait for a relaunch says at once, without sending anything, that a rep here holds the console.
mkdir "$WIRE_RUN_DIR/rep-127.0.0.1"
echo "someone $$" >"$WIRE_RUN_DIR/rep-127.0.0.1/owner"
: >"$FAKE/log"
answers 3 none "$TOOL: a benchmark run on this PC holds the console at 127.0.0.1" wireWaitUp 127.0.0.1 "$fakePort" "" 30
rm -r "$WIRE_RUN_DIR/rep-127.0.0.1"
expect "$(grep -c . "$FAKE/log")" 0 "requests sent while a rep held the console"
# A commit with any word but ok is not a clean commit: the tool says so and exits 1, with none of the
# sentences that go with a refusal's word.
answers 1 none "$TOOL: response 0 delete committed secure-value c8a04554" wireReport "response 0 delete committed secure-value c8a04554"
expect "$(_wireWhy run rejected hbldr-title)" "Luma's 3dsx loader title is not an installed application 3Drop may launch" "hbldr-title's sentence"
expect "$(_wireWhy run rejected layeredfs 00000000)" "not launched: the title has a romfs folder, which crashes the console in Luma's loader unless the title is a game built with Nintendo's SDK; for such a game, tools/device.sh run <ip> --game <title-id>; for your own app, delete the folder: tools/device.sh delete <ip> /luma/titles/<title-id>/romfs r" "layeredfs's sentence for a launch"
# With a Result, which means the card would not answer, the sentence says so and offers no --game.
expect "$(_wireWhy run rejected layeredfs c8804471)" "not launched: the card could not be checked for a romfs folder; check the card and try again" "layeredfs's sentence for a launch, the card failing"
# When the archive refuses a write for a file under a folder that is not there, the sentence says so,
# not that the card is full. Any other Result keeps the card's sentence.
expect "$(_wireWhy put rejected write c8804471)" "a folder on the file's path is missing, and the console could not make it" "write's sentence, a folder missing"
expect "$(_wireWhy put rejected write c8804478)" "the card refused a write: full, locked or failing" "write's sentence, another not-found Result"
expect "$(_wireWhy put rejected write 00000000)" "the card refused a write: full, locked or failing" "write's sentence, no Result"
expect "$(_wireWhy put rejected layeredfs 00000000)" "not written: an exheader.bin that is not 1024 or 2048 bytes crashes the console in Luma's loader" "layeredfs's sentence for a write"
expect "$(_wireWhy rename rejected layeredfs 00000000)" "not written: an exheader.bin that is not 1024 or 2048 bytes crashes the console in Luma's loader" "layeredfs's sentence for a rename"
expect "$(_wireWhy rename rejected layeredfs c8804028)" "not written: the card could not be checked for an exheader.bin; check the card and try again" "layeredfs's sentence for a rename, the card failing"
# --system goes as the request's last word, with the body sent whole and raw. Only with it does the
# console take a put or a delete of one file directly in /luma/payloads or /luma/sysmodules.
head -c 4096 /dev/zero >"$TMP/payload" # gzip would be worthwhile, but not with --system
fixture=/luma/payloads/3drop-fixture.firm
answers 1 none "device: response * put rejected system 00000000"$'\n'"device: that is what the console boots from: --system *" \
	"$device" put 127.0.0.1 "$TMP/payload" "$fixture"
: >"$FAKE/log"
THREEDROP_COMPRESSION=auto answers 0 data "" "$device" put 127.0.0.1 --system "$TMP/payload" "$fixture"
grep -q "^put [0-9a-f]* [0-9a-f]* $fixture 4096 u:[0-9a-f]* system\$" "$FAKE/log" || fail "a --system put went as: $(cat "$FAKE/log")"
{ [ "$(cat "$FAKE/compression")" = none ] && cmp -s "$TMP/payload" "$FAKE/sd$fixture"; } || fail "a --system put's body"
answers 1 none "device: response * delete rejected system 00000000"$'\n'"device: that is what the console boots from: --system *" \
	"$device" delete 127.0.0.1 "$fixture"
answers 0 data "" "$device" delete 127.0.0.1 --system "$fixture"
[ ! -e "$FAKE/sd$fixture" ] || fail "a --system delete"
answers 1 none "device: response * delete rejected system 00000000"$'\n'"device: that is what the console boots from: --system *" \
	"$device" delete 127.0.0.1 --system /luma/payloads
answers 2 none "device.sh *" "$device" delete 127.0.0.1 --system /luma/payloads r
answers 2 none "device.sh *" "$device" put 127.0.0.1 --system "$TMP/payload"
answers 2 none "device.sh *" "$device" delete 127.0.0.1 --system title:0004000001234500
# /boot.firm takes a --system put only as a FIRM that boot9strap boots. A plant is refused as `system`
# and the old file is kept (testFirmRules checks every plant's verdict, and that the fake's is the
# device's). Its delete stays refused.
fakeFirmBuild "$TMP/boot.firm"
answers 0 data "" "$device" put 127.0.0.1 --system "$TMP/boot.firm" /boot.firm
fakeFirmBuild "$TMP/planted.firm" byte
answers 1 none "device: response * put rejected system d963f809"$'\n'"device: that is what the console boots from: --system * (some of boot9strap's, not all: docs/safety.md)" \
	"$device" put 127.0.0.1 --system "$TMP/planted.firm" /boot.firm
cmp -s "$TMP/boot.firm" "$FAKE/sd/boot.firm" || fail "a FIRM with a byte planted replaced /boot.firm"
# Off its charger the console takes no /boot.firm, and the refusal carries the word that every boot
# input's refusal has.
fakeFirmBuild "$TMP/other.firm" other # another valid FIRM
echo 0 >"$FAKE/ac"
answers 1 none "device: response * put rejected system d963f818"$'\n'"device: the console is off its charger: a /boot.firm goes only on AC, since a power cut between its two renames leaves none" \
	"$device" put 127.0.0.1 --system "$TMP/other.firm" /boot.firm
rm "$FAKE/ac"
cmp -s "$TMP/boot.firm" "$FAKE/sd/boot.firm" || fail "a /boot.firm went off the charger"
answers 1 none "device: response * delete rejected system 00000000"$'\n'"device: that is what the console boots from: --system *" \
	"$device" delete 127.0.0.1 --system /boot.firm
# A path whose last step holds `*` or `?`, which no FAT name holds, lists the names in its folder that
# match, in either ASCII case as FAT takes them; `?` matches one character and `[` matches itself.
# When none match, it lists nothing and exits 0. A pattern takes no flag but t. A get with r takes what
# the pattern names through the tree loop, each folder with its tree, protecting what changed here as
# the loop does; when it names nothing, the exit status is 1.
mkdir -p "$FAKE/sd/pat/sub.bin/deep"
for name in a.BIN b.bin c.txt "Pokémon [USA].cia" "Pokemon U.cia" café.sav cafe.sav caféé.sav sub.bin/x sub.bin/deep/y; do
	printf '%s' "$name" >"$FAKE/sd/pat/$name"
done
expect "$(wireLsMatch 127.0.0.1 "$fakePort" '/pat/*.bin')" $'f 5 - a.BIN\nf 5 - b.bin\nd - - sub.bin' "ls of a pattern"
expect "$(wireLsMatch 127.0.0.1 "$fakePort" '/pat/*[USA]*')" "f 18 - Pokémon [USA].cia" "ls of a pattern holding brackets"
expect "$(wireLsMatch 127.0.0.1 "$fakePort" '/pat/CAF?.sav')" $'f 8 - cafe.sav\nf 9 - café.sav' "ls of a pattern holding ?"
expect "$(wireLsMatch 127.0.0.1 "$fakePort" '/pat/caf??.sav')" "f 11 - caféé.sav" "ls of a pattern holding ??"
# macOS hands out a decomposed name: there a pattern goes composed, as a path does.
want=""
[[ "$OSTYPE" != darwin* ]] || want="f 9 - café.sav"
out=$(wireLsMatch 127.0.0.1 "$fakePort" "$(printf '/pat/cafe\314\201.*')") || fail "ls of a decomposed pattern exited $?"
expect "$out" "$want" "ls of a decomposed pattern"
answers 0 none "" wireLsMatch 127.0.0.1 "$fakePort" '/pat/*.none'
answers 0 none "" wireLsMatch 127.0.0.1 "$fakePort" '/*.none'
expect "$(wireLsMatch 127.0.0.1 "$fakePort" '/pat/?.bin' t | awk '$3 ~ /^[0-9]+$/ { print $4 }' | tr '\n' ' ')" "a.BIN b.bin " "ls t of a pattern"
answers 2 none "$TOOL: a pattern lists the names in one folder: no flag but t" wireLsMatch 127.0.0.1 "$fakePort" '/pat/*.bin' r
answers 0 data "" "$device" ls 127.0.0.1 '/pat/c*.txt'
# The folder's own names are listed, and only a folder the pattern names is walked.
: >"$FAKE/log"
wireGetTree 127.0.0.1 "$fakePort" '/pat/*.BIN' "$TMP/pat" >/dev/null 2>&1 || fail "a get of a pattern"
expect "$(awk '$1 == "ls" { printf "%s %s,", $3, $4 }' "$FAKE/log")" "/pat t,/pat/sub.bin rt," "a get of a pattern's listings"
expect "$(find "$TMP/pat" -type f | sed "s|^$TMP/pat/||" | LC_ALL=C sort | tr '\n' ' ')" "a.BIN b.bin sub.bin/deep/y sub.bin/x " "a get of a pattern"
printf 'mine' >"$TMP/pat/b.bin"
answers 1 none "*skipped 'b.bin': it changed here since the last get; --overwrite replaces it*" wireGetTree 127.0.0.1 "$fakePort" '/pat/*.bin' "$TMP/pat"
expect "$(cat "$TMP/pat/b.bin")" mine "a get of a pattern over a file changed here"
answers 1 none "$TOOL: nothing in '/pat' matches '*.none'" wireGetTree 127.0.0.1 "$fakePort" '/pat/*.none/' "$TMP/pat"
# A named folder this host cannot make is never walked; the loop skips it. A folder whose walk the
# console refuses, or that ends early, is reported and the rest are still fetched; the copy ends with
# 1. A stranded console stops the copy.
mkdir -p "$FAKE/sd/pat2/a.dir" "$FAKE/sd/pat2/b.dir" "$FAKE/sd/pat2/$(printf 'esc\001').dir"
printf 'a' >"$FAKE/sd/pat2/a.dir/x"
printf 'b' >"$FAKE/sd/pat2/b.dir/y"
: >"$FAKE/log"
answers 1 none "*skipped 'esc?.dir': no file on this host can have that name*" wireGetTree 127.0.0.1 "$fakePort" '/pat2/*.dir' "$TMP/pat2"
{ [ "$(cat "$TMP/pat2/a.dir/x")$(cat "$TMP/pat2/b.dir/y")" = ab ] && ! grep -q '^ls .*esc' "$FAKE/log"; } ||
	fail "a pattern get over a folder this host cannot make: $(cat "$FAKE/log")"
(
	original _wireLsLines
	# shellcheck disable=SC2317 # called by wire.sh
	_wireLsLines() { if [[ "$4" == */a.dir ]]; then real__wireLsLines "$1" "$2" "$3" "$4.gone" "${@:5}"; else real__wireLsLines "$@"; fi; }
	answers 1 none "*ls rejected notfound*$TOOL: the listing ended early (notfound *): only what it named was fetched" \
		wireGetTree 127.0.0.1 "$fakePort" '/pat2/?.dir' "$TMP/pat3"
	[ "$(cat "$TMP/pat3/b.dir/y")" = b ] || fail "a pattern get past a folder whose walk was refused"
	# shellcheck disable=SC2317 # called by wire.sh
	_wireLsLines() {
		real__wireLsLines "$@" || return
		[[ "$4" != */a.dir ]] || wireOutcome=rejected wireDetail=deadline wireResult=00000000
	}
	answers 1 none "*$TOOL: the listing ended early (deadline 00000000): only what it named was fetched" \
		wireGetTree 127.0.0.1 "$fakePort" '/pat2/?.dir' "$TMP/pat4"
)
answers 1 none "*ls rejected stranded*" faultedOn ls 2 stranded wireGetTree 127.0.0.1 "$fakePort" '/pat2/?.dir' "$TMP/pat5"
[ ! -e "$TMP/pat5/b.dir" ] || fail "a pattern get went on past a stranded console"
# hash prints the u: digest and size of the console's file, as read there. With a local file, it exits 1
# and says so where they differ (cmp's convention). Key files are `protected`.
printf 'hashed' >"$FAKE/sd/hashed.bin"
printf 'hashed' >"$TMP/same"
printf 'hashes' >"$TMP/other"
expect "$("$device" hash 127.0.0.1 /hashed.bin)" "$(unitDigest "$TMP/same") 6" "hash"
answers 0 data "" wireHash 127.0.0.1 "$fakePort" /hashed.bin "$TMP/same"
answers 1 data "$TOOL: /hashed.bin differs from $TMP/other" wireHash 127.0.0.1 "$fakePort" /hashed.bin "$TMP/other"
answers 1 none "device: response * hash rejected protected 00000000"$'\n'"device: the key files are never read, replaced or moved" \
	"$device" hash 127.0.0.1 /3ds/3drop/key.txt
answers 1 none "device: response * hash rejected notfound *" "$device" hash 127.0.0.1 /none.bin
# A folder, which no file read opens, is reported as a folder, because `notfound` would send the user
# looking for it.
mkdir "$FAKE/sd/folder"
for verb in "get 127.0.0.1 /folder $TMP/folder" "hash 127.0.0.1 /folder"; do
	# shellcheck disable=SC2086 # the verb and its arguments, none with a space
	answers 1 none "device: response * ${verb%% *} rejected isdir *"$'\n'"device: that is a folder, not a file: *" "$device" $verb
done
answers 1 none "device: response * hash rejected stranded 00000000"$'\n'"device: a transfer is still stuck*" \
	faulted stranded "$device" hash 127.0.0.1 /hashed.bin
# A console at a QR screen refuses with its word, and the sentence says what to do about it.
answers 1 none "device: response * hash rejected busy 00000000"$'\n'"device: the console is busy with a QR scan or install: finish or cancel it at the console*" \
	faulted rejected-busy "$device" hash 127.0.0.1 /hashed.bin
# A reader that stops early ends `get ... -` with 141, as it ends any writer to a closed pipe, and nothing
# is said. The file is past what a pipe holds, so the writer is still writing when the reader goes.
head -c 1048576 /dev/zero >"$FAKE/sd/stream.bin"
firstByte() { "$device" get 127.0.0.1 /stream.bin - | head -c1; }
answers 141 data "" firstByte
openssl rand 4096 >"$FAKE/top.bmp" # the console's screens
openssl rand 3000 >"$FAKE/bottom.bmp"
# A link at any of a screenshot's files is refused before anything is asked, on a host that makes links,
# because sips (here a stand-in that copies, as on a host without it) would write a PNG through it.
mkdir "$TMP/sips"
# shellcheck disable=SC2016 # expanded when the stand-in runs
printf '#!/bin/sh\ncp "$4" "$6"\n' >"$TMP/sips/sips"
chmod +x "$TMP/sips/sips"
printf t >"$TMP/shotTarget"
for f in top.png bottom.bmp; do
	ln -s "$TMP/shotTarget" "$TMP/linked-$f" 2>/dev/null || :
	if [ ! -L "$TMP/linked-$f" ]; then
		skip "no links here, so a screenshot's refusal of one is unchecked"
		break
	fi
	: >"$FAKE/log"
	answers 1 none "device: '$TMP/linked-$f' is a link, which a screenshot never writes through" \
		env PATH="$TMP/sips:$PATH" "$device" screenshot 127.0.0.1 "$TMP/linked"
	expect "$(cat "$TMP/shotTarget"):$(count screenshot)" t:0 "a screenshot with a link at linked-$f"
	rm "$TMP/linked-$f"
done
# Otherwise each PNG is made, then moved in beside its BMP.
PATH="$TMP/sips:$PATH" "$device" screenshot 127.0.0.1 "$TMP/png" >/dev/null || fail "a screenshot where sips exists"
{ cmp -s "$TMP/png-top.bmp" "$TMP/png-top.png" && cmp -s "$TMP/png-bottom.bmp" "$TMP/png-bottom.png"; } || fail "the PNGs sips made"
echo "testAnswers: PASS"

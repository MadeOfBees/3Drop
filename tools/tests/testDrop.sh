#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2034,SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# drop.sh and dropBatch.sh routing against the fake device's install policy: the intent picked when
# none is given, a bump past the installed version (an application's only, never past 65535), the
# batch order, skipping what is installed, and the launch once every item is in.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
installed() { if [ -n "$1" ]; then echo "$(ciaTitleID "$cia") $1"; fi >"$FAKE/installed"; }
cia="$TMP/app.cia"
fakeCia "$cia" 0004000001234500 5
filePatch "$cia" $((0x2100)) 5a # what a fixed damage would write where the rebuild below damages it
cp "$cia" "$TMP/original.cia"

sent() { # <installed or ""> [flags...] -> prints "<intent> v<version>" as the console committed it
	local out
	installed "$1"
	out=$("$ROOT/tools/drop.sh" "${@:2}" "$cia" 127.0.0.1 2>"$TMP/err") || fail "drop.sh ${*:2} (installed '$1'): $out $(cat "$TMP/err")"
	awk '{ print $NF, "v" $(NF - 1) }' <<<"$out"
}
refused() { # <installed> [flags...]: drop.sh must fail without sending a PUT; what it said is in $TMP/refused
	installed "$1"
	: >"$FAKE/log"
	: >"$FAKE/seed-log"
	if "$ROOT/tools/drop.sh" "${@:2}" "$cia" 127.0.0.1 >"$TMP/refused" 2>&1; then fail "drop.sh ${*:2} (installed $1) succeeded"; fi
	! grep -q '^put ' "$FAKE/log" || fail "drop.sh ${*:2} (installed $1) sent a PUT"
	[ ! -s "$FAKE/seed-log" ] || fail "a refused drop asked the seed server"
}
expect "$(sent "")" "new v5" "absent title"
expect "$(sent 3)" "update v5" "older installed"
chmod a-w "$cia"
expect "$(sent 5)" "update v6" "same version installed from a read-only CIA"
chmod u+w "$cia"
expect "$(sent 9)" "update v10" "newer installed"
expect "$(sent 3 --no-bump)" "update v5" "--no-bump over an older install"
# Even when run by a relative path under an exported CDPATH (cd searches it before the current folder),
# drop.sh still finds its own tree, which holds the 3Drop title id it routes by.
mkdir -p "$TMP/decoy/tools"
installed ""
out=$(cd "$ROOT" && CDPATH="$TMP/decoy" tools/drop.sh "$cia" 127.0.0.1 2>&1) || fail "drop.sh by a relative path under CDPATH: $out"
refused 5 --no-bump
[[ "$(cat "$TMP/refused")" != *uninstall* ]] || fail "--no-bump steers an application to an uninstall, which takes its save: $(cat "$TMP/refused")"
refused 65535
[[ "$(cat "$TMP/refused")" == *"uninstalling it deletes its save data"* ]] || fail "the 65535 refusal did not warn that an uninstall takes the save: $(cat "$TMP/refused")"
# --no-bump refuses here, before the body goes, whether the intent was given or not.
refused 5 --update --no-bump
expect "$(sent 3 --update --no-bump)" "update v5" "--update --no-bump over an older install"
# An installed version it cannot read is never "not installed": nothing is sent.
installed 3
: >"$FAKE/log"
faulted bad-trailer "$ROOT/tools/drop.sh" "$cia" 127.0.0.1 >/dev/null 2>&1 && fail "drop.sh sent over a listing that did not authenticate"
! grep -q '^put ' "$FAKE/log" || fail "drop.sh sent a put when it could not tell what is installed"
# An explicit intent is sent as it is, even where the console's state would route another way:
# --new over an installed title is the device's to refuse, never quietly an update.
installed 3
out=$("$ROOT/tools/drop.sh" --new "$cia" 127.0.0.1 2>&1) && fail "--new over an installed title succeeded: $out"
[[ "$out" == "drop: put rejected exists "*" new"$'\n'"drop: it is there already: "* ]] || fail "--new over an installed title: $out"
cmp -s "$cia" "$TMP/original.cia" || fail "drop.sh modified the caller's CIA"
# A file whose size is not the total of its header's sections is refused before anything is sent, and the
# refusal is said once.
cp "$cia" "$TMP/junk.cia"
printf x >>"$TMP/junk.cia"
: >"$FAKE/log"
out=$("$ROOT/tools/drop.sh" "$TMP/junk.cia" 127.0.0.1 2>&1) && fail "drop.sh sent a file that is no CIA"
expect "$out" "drop: $TMP/junk.cia is not a CIA laid out as its header says" "drop.sh on a file that is no CIA"
out=$("$ROOT/tools/dropBatch.sh" 127.0.0.1 "$cia" "$TMP/junk.cia" 2>&1) && fail "a batch with a file that is no CIA went"
expect "$out" "dropBatch: $TMP/junk.cia is not a CIA laid out as its header says" "a batch with a file that is no CIA"
[ ! -s "$FAKE/log" ] || fail "a file that is no CIA reached the console: $(cat "$FAKE/log")"
fakeCia "$TMP/self.cia" 0004000003d37200 5
echo "0004000003d37200 4" >"$FAKE/installed"
case "$("$ROOT/tools/drop.sh" "$TMP/self.cia" 127.0.0.1)" in *" committed ok "*" self") ;; *) fail "3Drop's own id did not go as self" ;; esac
# A patch or DLC is routed the same way but never bumped, because a forged version would shadow the real
# release.
for id in 0004000e01234500 0004008c01234502; do
	cia="$TMP/$id.cia"
	fakeCia "$cia" "$id" 5
	expect "$(sent "")" "new v5" "absent $id"
	expect "$(sent 3)" "update v5" "$id over an older install"
	refused 5
done
cia="$TMP/app.cia"
# A patch or DLC installs, then its game launches (DLC's low byte is its index). If the game is not
# installed, the device refuses the launch and drop.sh says so.
for id in 0004000e01234500 0004008c01234502; do
	echo "0004000001234500 1" >"$FAKE/installed"
	: >"$FAKE/log"
	"$ROOT/tools/drop.sh" --run "$TMP/$id.cia" 127.0.0.1 >/dev/null 2>"$TMP/err" || fail "--run of $id: $(cat "$TMP/err")"
	expect "$(awk '$1 == "put" || $1 == "run" { printf "%s %s ", $1, $4 }' "$FAKE/log")" "put new run title:0004000001234500 " "--run of $id"
done
: >"$FAKE/installed"
out=$("$ROOT/tools/drop.sh" --run "$TMP/0004000e01234500.cia" 127.0.0.1 2>"$TMP/err") && fail "--run of a patch whose game is not installed succeeded: $out"
[[ "$out" == "put committed ok "* && "$(cat "$TMP/err")" == *" run rejected notfound "*"installed, but not launched" ]] || fail "--run of a patch whose game is not installed: $out $(cat "$TMP/err")"
# --run has no word to mark a title as a game, so a title with a romfs folder is refused its launch, and
# the console's sentence, which both tools print, says how to start it.
mkdir -p "$FAKE/sd/luma/titles/0004000001234500/romfs"
echo "0004000001234500 1" >"$FAKE/installed"
"$ROOT/tools/drop.sh" --run "$cia" 127.0.0.1 >/dev/null 2>"$TMP/err" && fail "--run of a title with a romfs folder launched"
said "tools/device.sh run <ip> --game <title-id>" "drop.sh's way to start it"
echo "0004000001234500 1" >"$FAKE/installed"
"$ROOT/tools/dropBatch.sh" --run 1 127.0.0.1 "$cia" >/dev/null 2>"$TMP/err" && fail "a batch --run of a title with a romfs folder launched"
said "tools/device.sh run <ip> --game <title-id>" "dropBatch.sh's way to start it"
rm -r "$FAKE/sd/luma/titles/0004000001234500/romfs"
# An application's seed is requested from the server as its install begins, alongside it, and imported after
# it, before its launch; --no-seed, a patch, DLC and 3Drop ask the server for nothing. A seed that fails
# only warns: the install stands, and the exit status is 0. The server's curl logs its request among the
# console's requests.
seed=00112233445566778899aabbccddeeff
fakeSeed 0004000001234500 200 "$seed"
eval "serverCurl() $(declare -f curl | tail -n +2)"
# shellcheck disable=SC2317 # drop.sh's curl
curl() {
	echo curl >>"$FAKE/log"
	serverCurl "$@"
}
export -f curl serverCurl
seeded() { # [flags...] -> prints the requests that follow drop.sh's own reads, in order
	installed ""
	: >"$FAKE/log"
	: >"$FAKE/seed-log"
	"$ROOT/tools/drop.sh" "$@" "$cia" 127.0.0.1 >"$TMP/out" 2>"$TMP/err" || fail "drop.sh $* with a seed: $(cat "$TMP/err")"
	awk '$1 != "stats" && $1 != "ls" { printf "%s ", $1 }' "$FAKE/log"
}
expect "$(seeded --run)" "curl put seed run " "a seeded application"
grep -qx "seed [0-9a-f]* [0-9a-f]* title:0004000001234500 $seed" "$FAKE/log" || fail "the seed sent is not the one fetched"
expect "$(sed -n 2p "$TMP/out")" "seed committed ok 00000000 0004000001234500 added" "drop.sh's seed line"
expect "$(seeded --no-seed)" "put " "--no-seed"
[ ! -s "$FAKE/seed-log" ] || fail "--no-seed asked the seed server"
# A fetch that fails, or that has no curl, is reported once, along with the offline way.
fetchFailed="drop: the seed was not fetched (%s), so nothing went to the console: if this title is seed-encrypted, it will not boot until it has its seed (device.sh seed <ip> 0004000001234500 <seed-file> takes it from a 16-byte file)"
fakeSeed 0004000001234500 - # the server cannot be reached
expect "$(seeded --run)" "curl put run " "a seed the server did not give"
# shellcheck disable=SC2059 # the format is the message
expect "$(cat "$TMP/err")" "$(printf "$fetchFailed" "curl: (7) Failed to connect to kagiya-ctr.cdn.nintendo.net port 443")" "an unreachable seed server"
noCurl() (
	# shellcheck disable=SC2317 # drop.sh's curl, as a shell with none finds it
	curl() { return 127; }
	export -f curl
	seeded --run
)
expect "$(noCurl)" "put run " "a seed with no curl"
# shellcheck disable=SC2059
expect "$(cat "$TMP/err")" "$(printf "$fetchFailed" "no curl here to fetch it with")" "a seed with no curl"
fakeSeed 0004000001234500 200 "$seed"
echo "0004000001234500 ffffffffffffffffffffffffffffffff" >"$FAKE/seeds" # another seed is held
expect "$(seeded)" "curl put seed " "a seed the console refused"
{ grep -q '^drop: response .* seed rejected exists ' "$TMP/err" && grep -q '^drop: the seed was not imported, so ' "$TMP/err"; } || fail "a refused seed was not said: $(cat "$TMP/err")"
for id in 0004000e01234500 0004008c01234502; do
	cia="$TMP/$id.cia"
	expect "$(seeded)" "put " "$id"
	[ ! -s "$FAKE/seed-log" ] || fail "$id asked the seed server"
done
cia="$TMP/app.cia"
echo "0004000003d37200 4" >"$FAKE/installed"
"$ROOT/tools/drop.sh" "$TMP/self.cia" 127.0.0.1 >/dev/null
[ ! -s "$FAKE/seed-log" ] || fail "3Drop's own install asked the seed server"
rm -r "$FAKE/ext_key" "$FAKE/seeds"

# A run whose install committed but whose reply never came (here killed as it waits out the silence after
# it) leaves a note of what it sent. Sending the same CIA again then finds that version installed and says
# the install is done, sending nothing and bumping nothing. Another CIA of that title and version is a
# new build, bumped and sent as usual.
# signalled <signal> <file> <line> <command...>: signals the command's group a second after <file> has a
# line that starts with <line>.
signalled() { inGroup "$1" 1 "$2" 1 "$3" "${@:4}"; }
killedQuiet() { signalled TERM "$FAKE/down" "" "$ROOT/tools/drop.sh" "$cia" 127.0.0.1; }
installed ""
faulted gone-after killedQuiet
rm -f "$FAKE/down"
expect "$(cat "$FAKE/installed")" "$(ciaTitleID "$cia") 5" "what the killed run installed"
: >"$FAKE/log"
out=$("$ROOT/tools/drop.sh" "$cia" 127.0.0.1 2>"$TMP/err") || fail "drop.sh after a lost reply: $(cat "$TMP/err")"
expect "$out" "put committed ok 00000000 $(ciaTitleID "$cia") 5 new" "drop.sh after a lost reply that committed"
grep -q "^drop: .* is installed at v5: the last run's install of this CIA landed" "$TMP/err" || fail "the landed install was not said: $(cat "$TMP/err")"
! grep -q '^put ' "$FAKE/log" || fail "drop.sh sent again an install that had landed"
expect "$(sent 5)" "update v6" "the same CIA once its note is settled"
# A note belongs to one key: under another key, or from before the key was named, it isn't a note about
# this console. A console that now answers here under another key, with another build at that version, isn't
# taken for the one the killed run installed on, so the CIA is bumped and sent as usual.
installed ""
faulted gone-after killedQuiet
rm -f "$FAKE/down"
keyTag=$(macOf 3drop/remembered)
keyTag=${keyTag:0:16}
notes=("$XDG_CACHE_HOME"/3drop/put-*)
[ "${notes[*]}" = "$XDG_CACHE_HOME/3drop/put-$keyTag-127.0.0.1-$fakePort-$(ciaTitleID "$cia")" ] || fail "a killed run's note, named for its key: ${notes[*]}"
mv "${notes[0]}" "$TMP/note"
for was in "another key's:0123456789abcdef-127.0.0.1" "an untagged:127.0.0.1"; do
	cp "$TMP/note" "$XDG_CACHE_HOME/3drop/put-${was#*:}-$fakePort-$(ciaTitleID "$cia")"
	expect "$(sent 5)" "update v6" "${was%%:*} note over a build at its version"
	rm -f "$XDG_CACHE_HOME"/3drop/put-*
done
installed ""
faulted gone-after killedQuiet
rm -f "$FAKE/down"
cp "$cia" "$TMP/rebuilt.cia"
filePatch "$TMP/rebuilt.cia" $((0x2100)) "$(fakeDamaged "$TMP/rebuilt.cia" $((0x2100)))" # another build
cia=$TMP/rebuilt.cia
expect "$(sent 5)" "update v6" "another build at a version a lost run installed"
cia=$TMP/app.cia
# A run whose answer never came and which never landed leaves its note. Any run of the title that got an
# answer settles the note, so another build installed at that version since is never taken for the noted
# one.
installed 5
faulted ambiguous "$ROOT/tools/drop.sh" "$cia" 127.0.0.1 >/dev/null 2>&1 && fail "a put no answer settled exited 0"
fakeCia "$TMP/other6.cia" "$(ciaTitleID "$cia")" 6
"$ROOT/tools/drop.sh" --update "$TMP/other6.cia" 127.0.0.1 >/dev/null 2>&1 || fail "drop.sh --update of another build"
: >"$FAKE/log"
out=$("$ROOT/tools/drop.sh" "$cia" 127.0.0.1 2>"$TMP/err") || fail "drop.sh over another build: $(cat "$TMP/err")"
expect "$out" "put committed ok 00000000 $(ciaTitleID "$cia") 7 update" "the noted CIA over another build at its version"

# A drop.sh stopped mid-put leaves nothing behind except what a rerun reuses. TERM or INT to its group
# removes its scratch, the bumped copy, the reply's temp and the ghost watch's fifo. A KILL runs no trap,
# so the next run of a tool removes what it left once its process is gone, but nothing belonging to a
# process still running. Anything that has no process's name goes once it is a day old.
litter() { find "$TMPDIR" "$XDG_RUNTIME_DIR" -maxdepth 2 \( -name '3drop.*' -o -name '3drop-*' -o -name 'fifo.*' \) 2>/dev/null | LC_ALL=C sort; }
mkdir "$TMPDIR/3drop.$$.living" # this test's: a run still going
: >"$TMPDIR/3drop-reply.recent"
: >"$TMPDIR/3drop-reply.stale1"
# A reply's progress and error files, a bench snapshot, and a replacement's temp in the cache and run
# folders (a tree's manifest, the address book) are also removed once they are a day old.
mkdir -p "$TMPDIR/3drop-bench.stale2" "$XDG_CACHE_HOME/3drop"
cached=("$XDG_CACHE_HOME/3drop/push-x.manifest.new.stale3" "$XDG_CACHE_HOME/3drop/addresses-x.new.stale4" "$XDG_RUNTIME_DIR/3drop/y.new.stale5")
: >"$TMPDIR/3drop-reply.stale6.sent"
: >"$TMPDIR/3drop-reply.stale7.why"
for f in "${cached[@]}"; do : >"$f"; done
touch -t 202601010000 "$TMPDIR/3drop-reply.stale1" "$TMPDIR/3drop-bench.stale2" "${cached[@]}" "$TMPDIR/3drop-reply.stale6.sent" \
	"$TMPDIR/3drop-reply.stale7.why"
: >"$XDG_CACHE_HOME/3drop/push-x.manifest.new.recent"
kept=$(litter)
for sig in TERM INT KILL; do
	installed 5 # bumped, so a scratch copy is made
	: >"$FAKE/log"
	faulted mute-put signalled "$sig" "$FAKE/log" "put " "$ROOT/tools/drop.sh" "$cia" 127.0.0.1
	grep -q '^put ' "$FAKE/log" || fail "drop.sh's put never went before its $sig"
	left=$(LC_ALL=C comm -13 <(printf '%s\n' "$kept") <(litter))
	if [ "$sig" = KILL ]; then
		[ -n "$left" ] || fail "a KILL left no scratch: the sweep below proves nothing"
	else
		[ -z "$left" ] || fail "a $sig mid-put left $left"
	fi
done
"$ROOT/tools/device.sh" stats 127.0.0.1 >/dev/null || fail "device.sh stats after a killed drop.sh"
expect "$(litter)" "$TMPDIR/3drop-reply.recent"$'\n'"$TMPDIR/3drop.$$.living" "what a run swept after a KILL"
for f in "${cached[@]}"; do [ ! -e "$f" ] || fail "a day-old $f was not swept"; done
[ -e "$XDG_CACHE_HOME/3drop/push-x.manifest.new.recent" ] || fail "a recent manifest temp was swept"
rm -r "$TMPDIR/3drop.$$.living" "$TMPDIR/3drop-reply.recent" "$XDG_CACHE_HOME/3drop/push-x.manifest.new.recent"

a="$TMP/a.cia" b="$TMP/b.cia"
fakeCia "$a" 0004000001234500 3 16384
fakeCia "$b" 0004000001234600 1 20480
# batched: prints "<item> <intent>" for each PUT (a for the 16384-byte CIA, else b) and "run <title>"
# for each launch, in order.
batched() { awk '$1 == "put" { printf "%s %s ", ($5 == 16384 ? "a" : "b"), $4 } $1 == "run" { printf "run %s ", $4 }' "$FAKE/log"; }
batch() { # <installed-a or ""> [flags...] -> prints what a batch of a and b sent (batched)
	installed "$1"
	: >"$FAKE/log"
	"$ROOT/tools/dropBatch.sh" "${@:2}" 127.0.0.1 "$a" "$b" >/dev/null 2>&1 || fail "dropBatch.sh ${*:2} failed"
	batched
}
expect "$(batch 2)" "a update b new " "no intent: drop.sh routes each item"
installed 2 # a is older than its CIA, so with no intent it would go as an update
: >"$FAKE/log"
if "$ROOT/tools/dropBatch.sh" --new 127.0.0.1 "$a" "$b" >/dev/null 2>&1; then fail "a batch --new over an installed item succeeded"; fi
expect "$(batched)" "a new b new " "--new reaches every item"
expect "$(batch "" --run 1)" "a new b new run title:0004000001234500 " "--run 1 launches item 1 once every item is in"
expect "$(batch 3 --run 1)" "a update b new run title:0004000001234500 " "--run 1 sends item 1 even at its installed version"
expect "$(batch 3 --run 01)" "a update b new run title:0004000001234500 " "--run 01 sends and launches item 1"
expect "$(batch 3)" "b new " "an item installed at its version is skipped"
expect "$(batch 4)" "b new " "an item installed newer is skipped"
# Applications go first, then updates, then DLC, each in the order given, and the launch comes after them all.
fakeCia "$TMP/p.cia" 0004000e01234500 1 24576
fakeCia "$TMP/d.cia" 0004008c01234500 1 28672
: >"$FAKE/installed"
: >"$FAKE/log"
"$ROOT/tools/dropBatch.sh" --run 4 127.0.0.1 "$TMP/d.cia" "$b" "$TMP/p.cia" "$a" >/dev/null 2>&1 || fail "a batch of every category failed"
expect "$(awk '$1 == "put" { printf "%s ", $5 } $1 == "run" { printf "run %s ", $4 }' "$FAKE/log")" "20480 16384 24576 28672 run title:0004000001234500 " "a batch's order"
# --run N of a patch or DLC launches its game.
for item in 1 3; do
	: >"$FAKE/installed"
	: >"$FAKE/log"
	"$ROOT/tools/dropBatch.sh" --run "$item" 127.0.0.1 "$TMP/d.cia" "$b" "$TMP/p.cia" "$a" >/dev/null 2>&1 || fail "a batch --run $item of a patch or DLC failed"
	expect "$(awk '$1 == "run" { print $4 }' "$FAKE/log")" title:0004000001234500 "the launch of batch item $item"
	: >"$FAKE/log"
	"$ROOT/tools/dropBatch.sh" --run "$item" 127.0.0.1 "$TMP/d.cia" "$b" "$TMP/p.cia" "$a" >/dev/null 2>&1 || fail "a batch --run $item of an installed patch or DLC failed"
	expect "$(count put):$(awk '$1 == "run" { print $4 }' "$FAKE/log")" 0:title:0004000001234500 "the launch of installed batch item $item"
done
# An item that got no answer ends the batch there when the console then doesn't answer stats (each later
# item would wait it out), or when a bench rep is holding the console. If the console does answer, the rest
# go. The batch runs through a copy of the tools whose drop.sh ends the first item as told and can silence
# the console.
mkdir -p "$TMP/kit/tools"
cp "$ROOT/tools/dropBatch.sh" "$ROOT/tools/wire.sh" "$ROOT/tools/lib.sh" "$ROOT/tools/cia.sh" "$TMP/kit/tools/"
# shellcheck disable=SC2016 # the stub's own
printf '%s\n' '#!/usr/bin/env bash' 'echo "$*" >>"$KIT_LOG"' 'read -r st mode <"$KIT_PLAN"' 'echo "0 -" >"$KIT_PLAN"' \
	'[ "$mode" = - ] || echo "$mode" >"$FAKE/mode"' 'exit "$st"' >"$TMP/kit/tools/drop.sh"
chmod +x "$TMP/kit/tools/drop.sh"
while read -r first mode want words; do
	installed ""
	echo "$first $mode" >"$TMP/plan"
	: >"$TMP/kit.log"
	st=0
	THREEDROP_TIMEOUT=2 KIT_LOG=$TMP/kit.log KIT_PLAN=$TMP/plan "$TMP/kit/tools/dropBatch.sh" 127.0.0.1 "$a" "$b" >/dev/null 2>"$TMP/err" || st=$?
	rm -f "$FAKE/mode"
	expect "$st:$(grep -c . "$TMP/kit.log")" "$want" "a batch whose first item ended $first, the console then $mode (status, items sent)"
	[ "$words" = - ] || said "dropBatch: the console is $words: the rest are not sent"
done <<'EOF'
4 mute 4:1 not answering
124 mute 124:1 not answering
3 - 3:1 held by a benchmark run on this PC
4 - 4:2 -
1 - 1:2 -
EOF
# An item that AM lists with no info (`t - -`) is neither skipped nor installed over. drop.sh's `new` is
# refused with `am-info`, the batch fails, and the other item still goes.
installed -
: >"$FAKE/log"
if "$ROOT/tools/dropBatch.sh" 127.0.0.1 "$a" "$b" >/dev/null 2>"$TMP/err"; then fail "a batch over a no-info title succeeded"; fi
if grep -q 'integer' "$TMP/err"; then fail "dropBatch read '-' as a version: $(cat "$TMP/err")"; fi
expect "$(batched)" "a new b new " "puts over a no-info title"
expect "$(cat "$FAKE/installed")" "0004000001234500 -"$'\n'"0004000001234600 1" "what the no-info batch installed"
# When an item fails, the launch target is installed but not launched.
installed -
: >"$FAKE/log"
if "$ROOT/tools/dropBatch.sh" --run 2 127.0.0.1 "$a" "$b" >/dev/null 2>&1; then fail "a batch --run with a failed item succeeded"; fi
expect "$(batched)" "a new b new " "a batch --run with a failed item"

# A batch's status is that of its first failed item, as the exit table says: an install whose outcome no
# receipt could give is 4. A call it cannot take is a usage error, status 2.
installed ""
run 4 faulted ambiguous "$ROOT/tools/dropBatch.sh" 127.0.0.1 "$a"
for call in "--run 0" "--run 3" "--bogus"; do
	# shellcheck disable=SC2086 # the call's words
	run 2 "$ROOT/tools/dropBatch.sh" $call 127.0.0.1 "$a" "$b"
done
run 2 "$ROOT/tools/drop.sh" --bogus "$cia" 127.0.0.1
run 2 "$ROOT/tools/drop.sh" --batch 1 "$cia" 127.0.0.1

# PBKDF2 runs once per process, never once per MAC, because a cache inside macOf would die with each
# subshell.
opensslShim
derivations() { grep -cE '^enc( |$)' "$TMP/openssl.log" || true; }
: >"$TMP/openssl.log"
installed ""
PATH="$TMP/shim:$PATH" "$ROOT/tools/drop.sh" "$cia" 127.0.0.1 >/dev/null
expect "$(derivations)" 0 "derivations in a tool run with the key already stretched"
for i in 1 2; do
	installed ""
	PATH="$TMP/shim:$PATH" env -u WIRE_KEY_FOR -u WIRE_IPAD -u WIRE_OPAD "$ROOT/tools/drop.sh" "$cia" 127.0.0.1 >/dev/null
done
expect "$(derivations)" 2 "derivations in two tool runs that stretched the key themselves"
[ "$(grep -cE '^dgst( |$)' "$TMP/openssl.log")" -gt 8 ] || fail "the runs signed fewer requests than they sent"
# A run makes each private folder once. It reads a CIA once (od, three times: its header, its TMD's
# signature, and the TMD's title and version), and reads 3Drop's own id (sed, from the RSF) only when no
# route was given. These are counted through shims first on PATH, over the listener, so the fake's own
# processes are not counted. dropBatch reads each CIA once, and leaves drop.sh to read it. A key a parent
# stretched is its child's too (a batch's drop.sh, the bridge's device.sh): that child makes the cache
# folder where it first asks for it, once, and a child that never reads the cache makes none.
mkdir "$TMP/count"
for exe in mkdir chmod od sed; do
	: >"$TMP/count/$exe.log"
	# shellcheck disable=SC2016 # the shim's own $* and $@
	printf '#!/bin/sh\necho "$*" >>%q\nexec %q "$@"\n' "$TMP/count/$exe.log" "$(type -P "$exe")" >"$TMP/count/$exe"
	chmod +x "$TMP/count/$exe"
done
# cold: the state of a fresh terminal, with the key not stretched yet (wireKey exports these five).
# Without it, a tool has this harness's key.
cold=(env -u WIRE_KEY_FOR -u WIRE_IPAD -u WIRE_OPAD -u WIRE_BOOK -u WIRE_TAG)
counted() { # <command...>: runs it once with the shims, their logs starting empty
	local log
	for log in "$TMP"/count/*.log; do : >"$log"; done
	installed ""
	PATH="$TMP/count:${PATH//"$FAKE/bin:"/}" "$@" >/dev/null 2>"$TMP/err" || fail "$* under the counting shims: $(cat "$TMP/err")"
}
# reads: counts the od lines that name the CIA, not a seed's or a digest's.
reads() { grep -cF -- "$cia" "$TMP/count/od.log" || :; }
rsfReads() { grep -c UniqueId "$TMP/count/sed.log" || :; }
counted "${cold[@]}" "$ROOT/tools/drop.sh" --no-seed "$cia" 127.0.0.1
expect "$(sort "$TMP/count/mkdir.log" | uniq -d)" "" "a private folder a drop made twice"
expect "$(sort "$TMP/count/chmod.log" | uniq -d)" "" "a private folder a drop closed twice"
expect "$(reads)" 3 "the reads of a CIA by a drop: $(tr '\n' ';' <"$TMP/count/od.log")"
expect "$(rsfReads)" 1 "the reads of the RSF by a drop that was given no route"
# Where XDG_RUNTIME_DIR does not name a folder of its own (Git Bash, macOS), the run folder is the cache
# folder.
counted "${cold[@]}" env -u XDG_RUNTIME_DIR "$ROOT/tools/drop.sh" --new --no-seed "$cia" 127.0.0.1
expect "$(sort "$TMP/count/mkdir.log" | uniq -d)" "" "the cache folder, with no run folder of its own, made twice by a drop"
expect "$(sort "$TMP/count/chmod.log" | uniq -d)" "" "the cache folder, with no run folder of its own, closed twice by a drop"
expect "$(rsfReads)" 0 "the reads of the RSF by a drop that was given its route"
counted "${cold[@]}" "$ROOT/tools/dropBatch.sh" 127.0.0.1 "$cia"
expect "$(reads)" 6 "the reads of a CIA by a batch of one, its own and drop.sh's: $(tr '\n' ';' <"$TMP/count/od.log")"
# With separate run and cache roots, this checks the batch's own two folders and its drop.sh child's (the
# child inherits the key); then a drop that inherited its key, which asks for the cache once; then a tool
# that inherited its key and never reads the cache, which makes the run folder alone.
runFolder=$XDG_RUNTIME_DIR/3drop cacheFolder=$XDG_CACHE_HOME/3drop
# made: prints the folders made, then those closed, one call per line
made() { cat "$TMP/count/mkdir.log" "$TMP/count/chmod.log"; }
expect "$(made)" "$(printf '%s\n' "-p $runFolder" "-p $cacheFolder" "-p $runFolder" "-p $cacheFolder" "700 $runFolder" "700 $cacheFolder" "700 $runFolder" "700 $cacheFolder")" "the folders made by a batch of one and its drop.sh"
counted "$ROOT/tools/drop.sh" --no-seed "$cia" 127.0.0.1
expect "$(made)" "$(printf '%s\n' "-p $runFolder" "-p $cacheFolder" "700 $runFolder" "700 $cacheFolder")" "the folders made by a drop that inherited its key"
counted "$ROOT/tools/device.sh" stats 127.0.0.1
expect "$(made)" "$(printf '%s\n' "-p $runFolder" "700 $runFolder")" "the folders made by a tool that inherited its key and reads no cache"
# wire.sh is parsed once per run, by the child that does the work, and never by the parent that holds the
# scratch. It runs over the listener, whose device sources the tests' tree; the nc shim's device would
# source the copy.
mkdir -p "$TMP/once/tools" "$TMP/once/res"
cp "$ROOT"/tools/{lib,cia,device,drop}.sh "$TMP/once/tools/"
cp "$ROOT/res/buildCIA.rsf" "$TMP/once/res/"
{ printf 'echo x >>%q\n' "$TMP/parsed" && cat "$ROOT/tools/wire.sh"; } >"$TMP/once/tools/wire.sh"
: >"$TMP/parsed"
PATH=${PATH//"$FAKE/bin:"/} "$TMP/once/tools/device.sh" stats 127.0.0.1 >/dev/null || fail "device.sh stats from a copy of the tools"
installed ""
PATH=${PATH//"$FAKE/bin:"/} "$TMP/once/tools/drop.sh" "$cia" 127.0.0.1 >/dev/null || fail "drop.sh from a copy of the tools"
expect "$(grep -c . "$TMP/parsed")" 2 "wire.sh parses in a device.sh run and a drop.sh run"
echo "testDrop: PASS"

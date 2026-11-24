#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2034,SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# get and put in signed pieces against the fake device, and a rerun resuming them.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
# offsets <verb>: the offsets of the logged range requests of the verb.
offsets() { awk -v v="$1" '$1 == v { for (i = 1; i < NF; i++) if ($i == "range") printf "%s ", $(i + 1) }' "$FAKE/log"; }
# sizes: the sizes the logged put pieces gave the file.
sizes() { awk '$1 == "put" && $7 == "range" { printf "%s ", $5 }' "$FAKE/log"; }
# piped <file> <command...>: runs the command reading the file through a pipe, short reads and all.
piped() {
	# shellcheck disable=SC2002 # a pipe, not the file: a file's reads come whole
	cat "$1" | "${@:2}"
}
# betweenPieces <ordinals> <command> <get...>: runs the get and runs <command> before those exchanges
# (numbered from 1).
betweenPieces() (
	original _wireStreamOpen
	# shellcheck disable=SC2317 # called by wire.sh
	_wireStreamOpen() {
		echo >>"$TMP/opens"
		[[ " $ordinals " != *" $(($(wc -l <"$TMP/opens"))) "* ]] || eval "$hook"
		real__wireStreamOpen "$@"
	}
	ordinals=$1 hook=$2
	rm -f "$TMP/opens"
	"${@:3}"
)

WIRE_PIECE=2097152
size=5242885 # two whole pieces and one of a unit and 5 bytes
head -c "$size" /dev/urandom >"$TMP/a"
head -c "$size" /dev/urandom >"$TMP/b"
# Each byte a damage below lands on is one a fixed damage would write (X, ff): such a damage fails
# every run, not one in 256.
filePatch "$TMP/a" 2097152 58
filePatch "$TMP/a" 4194304 58
filePatch "$TMP/a" 1572864 ff
part=$TMP/a.out.3drop-part meta=$TMP/a.out.3drop-meta

# get: a file over one piece comes piece by piece, byte-exact, and leaves no part. A file of one piece or
# less, even an empty one, comes in one exchange.
cp "$TMP/a" "$FAKE/sd/a.bin"
: >"$FAKE/log"
landed "$TMP/a" "$TMP/a.out" "a get in pieces" wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out"
expect "$(offsets get)" "0 2097152 4194304 " "a get's pieces"
{ [ ! -e "$part" ] && [ ! -e "$meta" ]; } || fail "a whole get left its part"
head -c 1000 "$TMP/a" >"$FAKE/sd/small.bin"
: >"$FAKE/sd/empty.bin"
for f in small empty; do
	: >"$FAKE/log"
	landed "$FAKE/sd/$f.bin" "$TMP/$f.out" "a get of the $f file" wireGet 127.0.0.1 "$fakePort" "/$f.bin" "$TMP/$f.out"
	expect "$(offsets get)" "0 " "the $f file's pieces"
done

(cd "$TMP" && wireGet 127.0.0.1 "$fakePort" /small.bin -download >/dev/null)
cmp -s "$FAKE/sd/small.bin" "$TMP/-download" || fail "a get into a name starting with a dash"

# A get with no room for the file where it lands says so once its first piece names the size, and
# asks no more; a resume says so before it asks at all.
mkdir "$TMP/full"
# shellcheck disable=SC2317 # called by wire.sh
roomless() (
	original freeKiB
	freeKiB() { if [ "$1" = "$TMP/full" ]; then echo 2048; else real_freeKiB "$1"; fi; }
	"$@"
)
: >"$FAKE/log"
st=0
roomless wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/full/a.out" >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets get)" "1:0 " "a get with no room where it lands"
said "no room for $TMP/full/a.out: $size bytes to come, 2097152 free in $TMP/full"
[ -z "$(ls -A "$TMP/full")" ] || fail "a get with no room left $(ls -A "$TMP/full")"
faultedOn get "2 3 4" short-body wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/full/a.out" >/dev/null 2>&1 || :
[ -s "$TMP/full/a.out.3drop-part" ] || fail "no part kept to resume"
: >"$FAKE/log"
st=0
roomless wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/full/a.out" >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets get)" "1:" "a resume with no room where it lands"
said "no room for $TMP/full/a.out: $((size - 2097152)) bytes to come"
rm -r "$TMP/full"

# A cut or damaged piece is asked for again. A third failure stops the run with its status and keeps the
# pieces.
: >"$FAKE/log"
faultedOn get 2 short-body wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null || fail "a get with a piece cut short"
expect "$(offsets get)" "0 2097152 2097152 4194304 " "a cut piece, asked again"
: >"$FAKE/log"
faultedOn get 3 corrupt-body wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null || fail "a get with a damaged piece"
expect "$(offsets get)" "0 2097152 4194304 4194304 " "a damaged piece, asked again"
cmp -s "$TMP/a" "$TMP/a.out" || fail "a get with failed pieces was not byte-exact"
# A piece the console stopped when its own send timed out (the host went quiet) or failed is asked again too:
# the transport cut it. One it refused (`read`) stops the run with status 1.
for fault in "timed-out timeout d963f805" "send-failed send d963f816"; do
	read -r mode detail <<<"$fault"
	rm "$TMP/a.out"
	: >"$FAKE/log"
	faultedOn get 2 "$mode" wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>"$TMP/err" || fail "a get with a piece under $mode"
	expect "$(offsets get)" "0 2097152 2097152 4194304 " "a piece under $mode, asked again"
	cmp -s "$TMP/a" "$TMP/a.out" || fail "a get with a piece under $mode was not byte-exact"
	said "$TOOL: get stopped after 1048576 of the body's bytes: $detail"
done
: >"$FAKE/log"
st=0
faultedOn get 2 stopped wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets get)" "1:0 2097152 " "a piece the console refused"
said "$TOOL: get stopped after 1048576 of the body's bytes: read c8804464"
rm "$TMP/a.out" "$part" "$meta"
run 4 faultedOn get "2 3 4" short-body wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out"
said "$TOOL: the piece at byte 2097152 failed 3 times"
said "$TOOL: 2097152 bytes of it are kept in $part: get it again to resume"
{ [ ! -e "$TMP/a.out" ] && cmp -s "$part" <(head -c 2097152 "$TMP/a") && [ -s "$meta" ]; } || fail "the verified piece was not kept"
# At the default, a failed piece is retried a second later: three tries, two pauses.
t0=$(msNow)
st=0
faultedOn get "1 2 3" short-body env -u _THREEDROP_RETRY_PAUSE "$ROOT/tools/device.sh" get 127.0.0.1 /small.bin "$TMP/paused.out" >/dev/null 2>&1 || st=$?
took=$(($(msNow) - t0))
expect "$st" 4 "a get whose piece failed three times at the default pause"
[ "$took" -ge 2000 ] || fail "three tries of a piece took $took ms at the default pause"
# A rerun asks from one unit back, which must still match, and then goes on.
: >"$FAKE/log"
landed "$TMP/a" "$TMP/a.out" "a resumed get" wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out"
expect "$(offsets get)" "1048576 3145728 5242880 " "a resumed get's pieces"
{ [ ! -e "$part" ] && [ ! -e "$meta" ]; } || fail "a resumed get left its part"
rm "$TMP/a.out"
rm -f "$part" "$meta"
# The same goes for a part that is cut and then filled back to the length it should have: each append checks
# where it lands.
st=0
# shellcheck disable=SC2016 # expanded when the hook runs
betweenPieces "2 3" '[ $(($(wc -l <"$TMP/opens"))) = 2 ] && fakeCut "$part" 1048576 || head -c 1048576 /dev/zero >>"$part"' \
	wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>"$TMP/err" || st=$?
expect "$st" 1 "a get whose part was cut under it"
said "$TOOL: $part changed under this get: get it again"
[ ! -e "$TMP/a.out" ] || fail "a get whose part was cut under it wrote the file"
rm -f "$part" "$meta"
# A body that goes quiet midway fails each of its three tries at the idle bound. The get times out, says so,
# and writes nothing.
t0=$SECONDS
st=0
(
	WIRE_WORKER_SILENCE=1
	faultedOn get "1 2 3" stall-body wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/stalled.out" >/dev/null 2>"$TMP/err"
) || st=$?
expect "$st" 124 "a get whose piece went quiet three times"
saidLine "$TOOL: timed out: 127.0.0.1:$fakePort went quiet for 1 s"
said "$TOOL: the piece at byte 0 failed 3 times"
for f in "" .3drop-part .3drop-meta; do [ ! -e "$TMP/stalled.out$f" ] || fail "a get that kept nothing left stalled.out$f"; done
[ $((SECONDS - t0)) -le 45 ] || fail "a stalled get held the client $((SECONDS - t0)) s" # 3 tries at ~2 s, slow forks
# A console that goes silent for good is asked once whether it is there, not asked for the piece three times.
# A quiet piece and an unanswered stats stop the get; a console that is quiet once and then answers gets its
# retry.
until steadyRun; do
	: >"$FAKE/log"
	st=0
	(
		WIRE_WORKER_SILENCE=1 THREEDROP_TIMEOUT=2
		faulted mute wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/gone.out" >/dev/null 2>"$TMP/err"
	) || st=$?
done
expect "$st:$(count get):$(count stats)" "124:1:1" "a get from a console gone silent (status, gets, stats)"
said "the console answered no stats after it"
# The retried piece must start within the bounds of 1 s and 2 s. The fake starts one in 0.1 s on Linux and in
# 1.0 to 1.6 s on Git Bash (a stats takes it 0.6 s), where the bounds are five times as long.
slow=$((gitBash ? 5 : 1))
until steadyRun; do
	: >"$FAKE/log"
	(
		WIRE_WORKER_SILENCE=$slow THREEDROP_TIMEOUT=$((slow * 2))
		landed "$FAKE/sd/a.bin" "$TMP/back.out" "a get whose console was quiet once" faultedOn get 1 mute wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/back.out"
	)
done
expect "$(count stats)" 1 "stats asked after a quiet piece"

# A file deleted between pieces ends the get (status 1, notfound). The pieces are kept for a rerun, and
# nothing lands.
cp "$TMP/a" "$FAKE/sd/gone.bin"
st=0
# shellcheck disable=SC2016 # expanded when the hook runs
betweenPieces 2 'rm "$FAKE/sd/gone.bin"' wireGet 127.0.0.1 "$fakePort" /gone.bin "$TMP/gone.out" >/dev/null 2>"$TMP/err" || st=$?
expect "$st" 1 "a get whose file was deleted between pieces"
said " get rejected notfound "
{ [ ! -e "$TMP/gone.out" ] && cmp -s "$TMP/gone.out.3drop-part" <(head -c 2097152 "$TMP/a"); } ||
	fail "a get whose file was deleted between pieces kept other than its first piece"
rm -f "$TMP/gone.out"*
# A file that changed since the part began starts over. A size or an mtime that differs from the meta's shows
# the change; so does the unit before the part's end, for a rewrite inside FAT's 2 s that leaves both as they
# were.
keepPart() { faultedOn get "2 3 4" short-body wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>&1 || :; }
keepPart
printf 'x' >>"$FAKE/sd/a.bin"
: >"$FAKE/log"
wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>"$TMP/err" || fail "a get of a file grown since its part"
{ cmp -s "$FAKE/sd/a.bin" "$TMP/a.out" && [ ! -e "$part" ]; } || fail "a get of a grown file"
expect "$(offsets get)" "1048576 0 2097152 4194304 " "a grown file, started over"
said "$TOOL: the file changed since $part began: it starts over"
cp "$TMP/a" "$FAKE/sd/a.bin"
keepPart
touch -r "$FAKE/sd/a.bin" "$TMP/when"
filePatch "$FAKE/sd/a.bin" 1572864 "$(fakeDamaged "$FAKE/sd/a.bin" 1572864)"
touch -r "$TMP/when" "$FAKE/sd/a.bin"
: >"$FAKE/log"
{ wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>&1 && cmp -s "$FAKE/sd/a.bin" "$TMP/a.out"; } || fail "a get of a file rewritten in place"
expect "$(offsets get)" "1048576 0 2097152 4194304 " "a file rewritten in place, started over"
cp "$TMP/a" "$FAKE/sd/a.bin"
keepPart
head -c 524288 "$TMP/a" >"$FAKE/sd/a.bin"
: >"$FAKE/log"
{ wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>&1 && cmp -s "$FAKE/sd/a.bin" "$TMP/a.out"; } || fail "a get of a file shrunk below its part"
expect "$(offsets get)" "1048576 0 " "a file shrunk below its part, started over"
# The file changes again in the run that started over: the get says so and exits 1.
cp "$TMP/a" "$FAKE/sd/a.bin"
keepPart
st=0
# shellcheck disable=SC2016 # expanded when the hook runs
betweenPieces "1 3" 'printf x >>"$FAKE/sd/a.bin"' wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>"$TMP/err" || st=$?
expect "$st" 1 "a get of a file that changed twice"
said "$TOOL: the file changed again as it came: get it again"
rm -f "$TMP/a.out" "$part" "$meta"
# A part whose kept bytes changed below its last unit (a restart's zeroed extent) is checked against each
# piece's digest from when the piece landed. The part is cut back to the piece that differs, the get says so,
# and the file lands whole.
cp "$TMP/a" "$FAKE/sd/a.bin"
faultedOn get "3 4 5" short-body wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>&1 || :
expect "$(fileSize "$part")" 4194304 "the part two pieces kept"
filePatch "$part" 2098152 "$(fakeDamaged "$part" 2098152)"
: >"$FAKE/log"
run 0 wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out"
cmp -s "$TMP/a" "$TMP/a.out" || fail "a get resumed over a part damaged inside landed other bytes"
expect "$(offsets get)" "1048576 3145728 5242880 " "a get resumed over a part damaged in its second piece"
said "$TOOL: $part no longer holds what came from byte 2097152"
# A part of another file is not a part of this one.
cp "$TMP/a" "$FAKE/sd/a.bin"
keepPart
cp "$TMP/b" "$FAKE/sd/b.bin"
: >"$FAKE/log"
landed "$TMP/b" "$TMP/a.out" "a get over another file's part" wireGet 127.0.0.1 "$fakePort" /b.bin "$TMP/a.out"
expect "$(offsets get)" "0 2097152 4194304 " "a get over another file's part"
# A part belongs to one console. A part kept under another key or port, or by a get that wrote neither (a meta
# of three fields), starts over, even though the console now answers /a.bin with the same size, time and last
# MiB of a file that differs before them. Resumed, it would land as a mix of two files and be said to be
# whole.
tamper() { # /a.bin as another console has it: the same size, time and last MiB, another byte at 100
	touch -r "$FAKE/sd/a.bin" "$TMP/when"
	filePatch "$FAKE/sd/a.bin" 100 "$(fakeDamaged "$FAKE/sd/a.bin" 100)"
	touch -r "$TMP/when" "$FAKE/sd/a.bin"
}
bindMeta() { # <fields>: rewrites the meta's header, replacing the fields after its mtime (key tag and port)
	local path size mtime
	{
		read -r path size mtime _
		printf '%s %s %s%s\n' "$path" "$size" "$mtime" "${1:+ $1}"
		cat
	} <"$meta" >"$meta.new" && mv "$meta.new" "$meta"
}
keyTag=$(macOf 3drop/remembered)
keyTag=${keyTag:0:16}
rm -f "$TMP/a.out" "$part" "$meta"
cp "$TMP/a" "$FAKE/sd/a.bin"
keepPart
read -r _ _ _ binding <"$meta"
expect "$binding" "$keyTag $fakePort" "the key tag and port a part is kept under"
for kind in key port legacy; do
	case $kind in
	key) binding="0123456789abcdef $fakePort" ;;
	port) binding="$keyTag $((fakePort + 1))" ;;
	legacy) binding="" ;;
	esac
	rm -f "$TMP/a.out" "$part" "$meta"
	cp "$TMP/a" "$FAKE/sd/a.bin"
	keepPart
	bindMeta "$binding"
	tamper
	: >"$FAKE/log"
	st=0
	wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/a.out" >/dev/null 2>"$TMP/err" || st=$?
	expect "$st:$(offsets get)" "0:0 2097152 4194304 " "a part kept under another $kind, started over"
	cmp -s "$FAKE/sd/a.bin" "$TMP/a.out" || fail "a part kept under another $kind landed as a mix of two files"
	said "$TOOL: $part was not begun under this key and port: it starts over"
done
# And again with a second console that has a key and a port of its own, and whose /a.bin differs from the
# first's before its last MiB.
other=$TMP/device2 otherKey=other-key-5678
mkdir -p "$other/sd"
cp "$FAKE/verbs" "$FAKE/knobs" "$other/"
: >"$other/installed"
cp "$TMP/a" "$FAKE/sd/a.bin"
cp "$TMP/a" "$other/sd/a.bin"
filePatch "$other/sd/a.bin" 100 "$(fakeDamaged "$other/sd/a.bin" 100)"
touch -r "$FAKE/sd/a.bin" "$other/sd/a.bin"
(
	THREEDROP_KEY=$otherKey wireKey
	THREEDROP_KEY=$otherKey FAKE=$other fakeListen
	echo "$fakePort $fakeListenPid" >"$TMP/other.port"
) >/dev/null
read -r otherPort otherPid <"$TMP/other.port"
rm -f "$TMP/a.out" "$part" "$meta"
keepPart
: >"$other/log"
st=0
THREEDROP_KEY=$otherKey devtcp wireGet 127.0.0.1 "$otherPort" /a.bin "$TMP/a.out" >/dev/null 2>"$TMP/err" || st=$?
killTree "$otherPid"
expect "$st:$(FAKE=$other offsets get)" "0:0 2097152 4194304 " "a part begun on one console, got again from another"
cmp -s "$other/sd/a.bin" "$TMP/a.out" || fail "a part begun on one console landed as a mix with another's file"
said "$TOOL: $part was not begun under this key and port: it starts over"
rm -f "$TMP/a.out" "$part" "$meta"
# A link at the file, its part or its meta is never written through, and another user's file is never used
# (root's stands in) where the host tells owners (Git Bash does not). Planted in a shared directory, either
# would take the download or feed it. Nothing is asked.
if ln -s "$TMP/nowhere" "$TMP/probe" 2>/dev/null && [ -L "$TMP/probe" ]; then
	printf t >"$TMP/target"
	for f in "" .3drop-part .3drop-meta; do
		ln -s "$TMP/target" "$TMP/linked.out$f"
		: >"$FAKE/log"
		st=0
		wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/linked.out" >/dev/null 2>"$TMP/err" || st=$?
		expect "$st:$(offsets get):$(cat "$TMP/target")" "1::t" "a get with a link at linked.out$f"
		said "$TOOL: '$TMP/linked.out$f' is a link, which a get never writes through"
		[ -L "$TMP/linked.out$f" ] || fail "a get replaced the link at linked.out$f"
		rm "$TMP/linked.out$f"
	done
	# The same holds for a link planted once the get began (as its first piece comes, or before its last
	# lands).
	for f in .3drop-part .3drop-meta; do
		printf t >"$TMP/target"
		st=0
		# shellcheck disable=SC2016 # expanded when the hook runs
		betweenPieces 1 'ln -s "$TMP/target" "$TMP/raced.out$f"' wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/raced.out" >/dev/null 2>"$TMP/err" || st=$?
		expect "$st:$(cat "$TMP/target")" "1:t" "a link planted at raced.out$f as the get began"
		said "$TOOL: another process made '$TMP/raced.out.3drop-part' or its meta during this get: get it again"
		rm -f "$TMP/raced.out"*
	done
	st=0
	# shellcheck disable=SC2016 # expanded when the hook runs
	betweenPieces 3 'mkdir "$TMP/raced.out"' wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/raced.out" >/dev/null 2>"$TMP/err" || st=$?
	expect "$st:$(ls -A "$TMP/raced.out")" "1:" "a folder made at raced.out before the get landed"
	said "$TOOL: '$TMP/raced.out' is now a folder or a link, which a get never writes into"
	rm -rf "$TMP/raced.out"*
else
	skip "no links here, so a get's refusal of one is unchecked"
fi
# A FIFO planted the same way is refused at once: noclobber opens one as it is, waiting for a reader. One
# comes here after 8 s (120 s on Git Bash, where the get's own first piece, which comes before it meets the
# FIFO, took 10 to 28 s on a loaded PC). A get that waited is ended by the reader, so it fails the bound
# however slow the host.
fifoWait=$((gitBash ? 120 : 8))
for f in .3drop-part .3drop-meta; do
	st=0 t0=$(msNow)
	{ sleep "$fifoWait" && { [ ! -p "$TMP/fifo.out$f" ] || : <>"$TMP/fifo.out$f"; }; } &
	rescuer=$!
	# shellcheck disable=SC2016 # expanded when the hook runs
	betweenPieces 1 'mkfifo "$TMP/fifo.out'"$f"'"' wireGet 127.0.0.1 "$fakePort" /a.bin "$TMP/fifo.out" >/dev/null 2>"$TMP/err" || st=$?
	kill "$rescuer" 2>/dev/null || :
	[ $(($(msNow) - t0)) -lt $((fifoWait * 1000)) ] || fail "a get waited $(($(msNow) - t0)) ms on a FIFO planted at fifo.out$f"
	expect "$st" 1 "a FIFO planted at fifo.out$f as the get began"
	said "$TOOL: another process made '$TMP/fifo.out.3drop-part' or its meta during this get: get it again"
	rm -f "$TMP/fifo.out"*
done
# When the host cannot copy a piece out of its file (a read error, a full scratch), the tool says why, not
# just a bare 1.
mkdir "$TMP/ddshim"
cat >"$TMP/ddshim/dd" <<EOF
#!/usr/bin/env bash
for a; do case \$a in of=*/piece) echo "dd: \${1#if=}: Input/output error" >&2; exit 1 ;; esac; done
exec $(command -v dd) "\$@"
EOF
chmod +x "$TMP/ddshim/dd"
: >"$FAKE/log"
st=0
PATH="$TMP/ddshim:$PATH" wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /unread.bin >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(count put)" "1:0" "a put whose piece could not be read here"
saidLine "$TOOL: could not copy $TMP/a from byte 0 for its piece: dd: $TMP/a: Input/output error"
rm -r "$TMP/ddshim"
# A put's or a hash's local file must be a regular file. A FIFO (whose open waits forever for a writer) or a
# pipe's /dev/fd name (which the first measure drains) is refused at once, saying what takes a pipe. A file
# that changed between its digest and its send goes nowhere: the put says so, exits 1 and sends nothing.
mkfifo "$TMP/src.fifo"
: >"$FAKE/log"
for src in fifo pipe hash; do
	st=0
	# shellcheck disable=SC2016 # expanded by the inner shell
	case "$src" in
	fifo) withTimeout 10 bash -c '. "$1/tools/wire.sh"; wireFilePut 127.0.0.1 "$2" "$3" /x' _ "$ROOT" "$fakePort" "$TMP/src.fifo" ;;
	pipe) withTimeout 10 bash -c '. "$1/tools/wire.sh"; wireFilePut 127.0.0.1 "$2" <(printf x) /x' _ "$ROOT" "$fakePort" ;;
	hash) withTimeout 10 bash -c '. "$1/tools/wire.sh"; wireHash 127.0.0.1 "$2" /a.bin "$3"' _ "$ROOT" "$fakePort" "$TMP/src.fifo" ;;
	esac >/dev/null 2>"$TMP/err" || st=$?
	expect "$st:$(count put):$(count hash)" "1:0:0" "a $src as the local file"
	said "is not a regular file: send a pipe's bytes as put - <sd-path>, from stdin" "a $src as the local file"
done
rm "$TMP/src.fifo"
printf 'eleven bytes' >"$TMP/shrinks"
st=0
(
	original unitDigest
	# shellcheck disable=SC2317 # called by wire.sh
	unitDigest() { real_unitDigest "$@" && : >"$1"; }
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/shrinks" /shrinks >/dev/null 2>"$TMP/err"
) || st=$?
expect "$st:$(count put)" "1:0" "a put whose file was emptied after its digest"
said "$TOOL: $TMP/shrinks changed as it went: put it again"
if [ -f /etc/hosts ] && [ ! -O /etc/hosts ]; then
	: >"$FAKE/log"
	st=0
	wireGet 127.0.0.1 "$fakePort" /a.bin /etc/hosts >/dev/null 2>"$TMP/err" || st=$?
	expect "$st:$(offsets get)" "1:" "a get over another user's file"
	# A mount that maps owners shows the user's own file that way, so the refusal says where to get it
	# instead.
	said "$TOOL: '/etc/hosts' is another user's, which a get never writes or reads (a mount that maps owners, as sshfs or NFS can, shows your own files so: use a folder on this PC's own disk)"
else
	skip "no file of another user's here, so a get's refusal of one is unchecked"
fi

# put: a file over a piece goes to the console's temp in pieces, after the temp is listed; the last piece
# publishes it.
: >"$FAKE/log"
landed "$TMP/a" "$FAKE/sd/up.bin" "a put in pieces" wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /up.bin
expect "$(awk '$1 != "stats" { printf "%s ", $1 }' "$FAKE/log")$(offsets put)" "ls put put put 0 2097152 4194304 " "a put's pieces"
[ ! -e "$FAKE/sd/up.bin$WIRE_PUT_TEMP" ] || fail "a whole put left its temp"
# Each piece except the last gives `-` as the file's size; only the last names it, and publishes.
expect "$(sizes)" "- - 5242885 " "a put's sizes"
: >"$FAKE/log"
faultedOn put 2 corrupt-put wireFilePut 127.0.0.1 "$fakePort" "$TMP/b" /up.bin >/dev/null 2>"$TMP/err" || fail "a put with a damaged piece"
expect "$(offsets put)" "0 2097152 2097152 4194304 " "a damaged piece, sent again"
cmp -s "$TMP/b" "$FAKE/sd/up.bin" || fail "a put with a damaged piece was not byte-exact"
said "$TOOL: the piece at byte 2097152 did not arrive whole: response "
said " put rejected digest d963f80a"
# A piece cut on the wire, with its reply lost: the receipt says `short`, and the piece is sent again.
: >"$FAKE/log"
faultedOn put 2 cut-put wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /up.bin >/dev/null 2>"$TMP/err" || fail "a put with a piece cut on the wire"
expect "$(offsets put)" "0 2097152 2097152 4194304 " "a piece cut on the wire, sent again"
cmp -s "$TMP/a" "$FAKE/sd/up.bin" || fail "a put with a cut piece was not byte-exact"
said " put rejected short d963f802"
# A file edited in place as it goes, in a piece not sent yet, is refused that piece's digest. The put says the
# file changed and stops, never sending the piece again; run again, it goes on.
cp "$TMP/a" "$TMP/edit"
: >"$FAKE/log"
st=0
(
	original _wirePutBody
	# shellcheck disable=SC2317 # called by wire.sh
	_wirePutBody() {
		[ -e "$TMP/edited" ] || { filePatch "$TMP/edit" 3000000 "$(fakeDamaged "$TMP/edit" 3000000)" && : >"$TMP/edited"; }
		real__wirePutBody "$@"
	}
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/edit" /edit.bin
) >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets put)" "1:0 2097152 " "a file edited in place as it went"
said "$TOOL: $TMP/edit changed as it went"
: >"$FAKE/log"
landed "$TMP/edit" "$FAKE/sd/edit.bin" "the edited file put again" wireFilePut 127.0.0.1 "$fakePort" "$TMP/edit" /edit.bin
expect "$(offsets put)" "2097152 4194304 " "the edited file put again"
# A piece whose commit names other bytes stops the put, and is never sent again.
: >"$FAKE/log"
st=0
faultedOn put 2 other-bytes wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /named.bin >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets put)" "1:0 2097152 " "a piece whose commit names other bytes"
said "$TOOL: the reply names other bytes than these: response "
# Three failures stop the run, and the temp keeps the whole pieces before them. A rerun sends the rest.
run 4 faultedOn put "2 3 4" hangup wireFilePut 127.0.0.1 "$fakePort" "$TMP/b" /up.bin
said "$TOOL: the piece at byte 2097152 failed 3 times"
said "$TOOL: the console keeps 2097152 bytes of it in /up.bin$WIRE_PUT_TEMP: put it again to resume, or delete that file"
{ cmp -s "$TMP/a" "$FAKE/sd/up.bin" && cmp -s "$FAKE/sd/up.bin$WIRE_PUT_TEMP" <(head -c 2097152 "$TMP/b"); } || fail "a stopped put changed the file, or kept other than its whole pieces"
# A listing of the temp that gets no answer stops the run, because a piece from 0 would cut what is kept.
: >"$FAKE/log"
st=0
faultedOn ls 1 hangup wireFilePut 127.0.0.1 "$fakePort" "$TMP/b" /up.bin >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets put)" 4: "a put whose temp could not be listed"
cmp -s "$FAKE/sd/up.bin$WIRE_PUT_TEMP" <(head -c 2097152 "$TMP/b") || fail "a put whose temp could not be listed cut it"
: >"$FAKE/log"
landed "$TMP/b" "$FAKE/sd/up.bin" "a resumed put" wireFilePut 127.0.0.1 "$fakePort" "$TMP/b" /up.bin
expect "$(offsets put)" "2097152 4194304 " "a resumed put's pieces"
# If the temp is shorter than the listing said (`size`), the file is sent again from its start.
faultedOn put "2 3 4" hangup wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /up.bin >/dev/null 2>&1 || :
: >"$FAKE/log"
(
	original wireLs
	# shellcheck disable=SC2317 # called by wire.sh
	wireLs() { if [[ "$3" == *"$WIRE_PUT_TEMP" ]]; then echo "f 4194304 - x"; else real_wireLs "$@"; fi; }
	{ wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /up.bin >/dev/null 2>&1 && cmp -s "$TMP/a" "$FAKE/sd/up.bin"; } || fail "a put onto a temp shorter than listed"
)
expect "$(offsets put)" "4194304 0 2097152 4194304 " "a put onto a temp shorter than listed, sent again from its start"
# A killed put, run as a tool with its own scratch, resumes after the whole pieces it had sent.
: >"$FAKE/log"
# shellcheck disable=SC2016 # expanded when the job runs
faultedOn put 3 mute-put inGroup TERM 0 "$FAKE/log" 3 "put " bash -c '. "$1/tools/wire.sh"; scratch; WIRE_PIECE=2097152; wireFilePut 127.0.0.1 "$2" "$3" /killed.bin' _ "$ROOT" "$fakePort" "$TMP/a"
cmp -s "$FAKE/sd/killed.bin$WIRE_PUT_TEMP" <(head -c 4194304 "$TMP/a") || fail "a killed put kept other than its two whole pieces"
: >"$FAKE/log"
landed "$TMP/a" "$FAKE/sd/killed.bin" "a put resumed after a kill" wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /killed.bin
expect "$(offsets put)" "4194304 " "a put resumed after a kill"
# A killed get resumes too: its part keeps the whole pieces before the one it was taking, beside its
# meta, and nothing else is left; the rerun goes on from a unit before the part's end.
cp "$TMP/a" "$FAKE/sd/killed.bin"
: >"$FAKE/log"
# shellcheck disable=SC2016 # expanded when the job runs
faultedOn get 3 stall-body inGroup TERM 0.5 "$FAKE/log" 3 "get " bash -c '. "$1/tools/wire.sh"; scratch; WIRE_PIECE=2097152; wireGet 127.0.0.1 "$2" /killed.bin "$3"' _ "$ROOT" "$fakePort" "$TMP/killed.out"
{ [ ! -e "$TMP/killed.out" ] && cmp -s "$TMP/killed.out.3drop-part" <(head -c 4194304 "$TMP/a") && [ -s "$TMP/killed.out.3drop-meta" ]; } ||
	fail "a killed get kept other than its two whole pieces and its meta"
# Windows lists a deleted directory until the last handle to it closes (a scanner's handle counts too, and is
# gone within 3 s here), so what the kills left has 5 s to go. One that stays is a leak.
left() { find "$TMPDIR" -maxdepth 1 -name '3drop[.-]*'; }
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -n "$(left)" ] || break
	sleep 0.5
done
[ -z "$(left)" ] || fail "the kills left $(left)"
: >"$FAKE/log"
landed "$TMP/a" "$TMP/killed.out" "a get resumed after a kill" wireGet 127.0.0.1 "$fakePort" /killed.bin "$TMP/killed.out"
expect "$(offsets get)" "3145728 5242880 " "a get resumed after a kill"
# The temp's rule: if a temp holds another file's bytes, the file is sent again from its start; a put of one
# piece takes over a kept temp; a temp deleted by hand is gone.
faultedOn put "2 3 4" hangup wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /other.bin >/dev/null 2>&1 || :
: >"$FAKE/log"
{ wireFilePut 127.0.0.1 "$fakePort" "$TMP/b" /other.bin >/dev/null 2>"$TMP/err" && cmp -s "$TMP/b" "$FAKE/sd/other.bin"; } || fail "a put onto another file's temp"
expect "$(offsets put)" "2097152 0 2097152 4194304 " "a put onto another file's temp, sent again from its start"
said "$TOOL: the console's /other.bin$WIRE_PUT_TEMP keeps other bytes: the file goes again from its start"
# A run starts over only once: a temp that refuses the file again after that stops the run, with the console's
# word.
: >"$FAKE/log"
st=0
faultedOn put "1 2" rejected-size wireFilePut 127.0.0.1 "$fakePort" "$TMP/b" /again.bin >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets put)" "1:0 0 " "a file its temp refused again after it started over"
said "$TOOL: the console's /again.bin$WIRE_PUT_TEMP keeps other bytes: the file goes again from its start"
said "$TOOL: response "
said " put rejected size 00000000"
faultedOn put "2 3 4" hangup wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /other.bin >/dev/null 2>&1 || :
[ -e "$FAKE/sd/other.bin$WIRE_PUT_TEMP" ] || fail "no temp was kept for a put of one piece to take over"
head -c 1000 "$TMP/b" >"$TMP/small"
wireFilePut 127.0.0.1 "$fakePort" "$TMP/small" /other.bin >/dev/null || fail "a put of one piece over a kept temp"
[ ! -e "$FAKE/sd/other.bin$WIRE_PUT_TEMP" ] || fail "a put of one piece left a kept temp"
faultedOn put "2 3 4" hangup wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /other.bin >/dev/null 2>&1 || :
wireDelete 127.0.0.1 "$fakePort" "/other.bin$WIRE_PUT_TEMP" >/dev/null || fail "a kept temp deleted by hand"
: >"$FAKE/log"
wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /other.bin >/dev/null || fail "a put after its temp was deleted"
expect "$(offsets put)" "0 2097152 4194304 " "a put after its temp was deleted"
# get <path> -: sends the file to stdout in the same pieces, each written once it checks out. A bad piece is
# asked for again, and three failures stop the get (status 4), with only verified pieces written before that.
# Nothing is kept here.
cp "$TMP/a" "$FAKE/sd/a.bin"
mkdir "$TMP/here"
: >"$FAKE/log"
{ (cd "$TMP/here" && wireGetPipe 127.0.0.1 "$fakePort" /a.bin) >"$TMP/piped" && cmp -s "$TMP/a" "$TMP/piped"; } || fail "a get to stdout"
expect "$(offsets get)$(ls -A "$TMP/here")" "0 2097152 4194304 " "a get to stdout's pieces, and nothing kept"
for mode in corrupt-body bad-trailer short-body; do
	{ faultedOn get 2 "$mode" wireGetPipe 127.0.0.1 "$fakePort" /a.bin >"$TMP/piped" 2>/dev/null && cmp -s "$TMP/a" "$TMP/piped"; } ||
		fail "a get to stdout with a piece under $mode"
done
st=0
faultedOn get "2 3 4" corrupt-body wireGetPipe 127.0.0.1 "$fakePort" /a.bin >"$TMP/piped" 2>"$TMP/err" || st=$?
expect "$st" 4 "a get to stdout whose piece failed three times"
said "$TOOL: the piece at byte 2097152 failed 3 times"
cmp -s "$TMP/piped" <(head -c 2097152 "$TMP/a") || fail "a get to stdout wrote other than its verified piece"
# From a byte on: a cut one goes on from there. At the file's end nothing comes; past it the get is rejected
# as `size`, and says so.
: >"$FAKE/log"
{ wireGetPipe 127.0.0.1 "$fakePort" /a.bin 3000000 >"$TMP/piped" && cmp -s "$TMP/piped" <(tail -c +3000001 "$TMP/a"); } || fail "a get to stdout from a byte"
expect "$(offsets get)" "3000000 5097152 " "a get to stdout from a byte's pieces"
{ wireGetPipe 127.0.0.1 "$fakePort" /a.bin "$size" >"$TMP/piped" && [ ! -s "$TMP/piped" ]; } || fail "a get to stdout from the file's end"
{ wireGetPipe 127.0.0.1 "$fakePort" /empty.bin >"$TMP/piped" && [ ! -s "$TMP/piped" ]; } || fail "a get of an empty file to stdout"
st=0
wireGetPipe 127.0.0.1 "$fakePort" /a.bin $((size + 1)) >"$TMP/piped" 2>"$TMP/err" || st=$?
expect "$st:$(fileSize "$TMP/piped")" 1:0 "a get to stdout from past the file's end"
said " get rejected size 00000000"
said "$TOOL: the file ends before the byte the get starts from"
# A file that changes as it comes stops the get, because what went out cannot start over.
st=0
# shellcheck disable=SC2016 # expanded when the hook runs
betweenPieces 2 'printf x >>"$FAKE/sd/a.bin"' wireGetPipe 127.0.0.1 "$fakePort" /a.bin >"$TMP/piped" 2>"$TMP/err" || st=$?
expect "$st" 1 "a get to stdout of a file that changed as it came"
said "$TOOL: the file changed on the console as it came, with its bytes to 2097152 out: get it again"
cmp -s "$TMP/piped" <(head -c 2097152 "$TMP/a") || fail "a get to stdout of a changed file wrote past its first piece"
cp "$TMP/a" "$FAKE/sd/a.bin"

# put - <path>: sends stdin in the same pieces. Each piece is read whole however a pipe hands it out, and read
# once however often it goes. A stdin of one piece or less, an empty one too, is a put of that file.
: >"$FAKE/log"
landed "$TMP/a" "$FAKE/sd/piped.bin" "a put from stdin" piped "$TMP/a" wirePipePut 127.0.0.1 "$fakePort" /piped.bin
expect "$(offsets put)$(sizes)" "0 2097152 4194304 - - 5242885 " "a put from stdin's pieces"
: >"$FAKE/log"
{ piped "$TMP/b" faultedOn put 2 corrupt-put wirePipePut 127.0.0.1 "$fakePort" /piped.bin >/dev/null 2>&1 && cmp -s "$TMP/b" "$FAKE/sd/piped.bin"; } ||
	fail "a put from stdin with a damaged piece"
expect "$(offsets put)" "0 2097152 2097152 4194304 " "a damaged piece from stdin, sent again"
: >"$FAKE/log"
{ head -c 4194304 "$TMP/a" | wirePipePut 127.0.0.1 "$fakePort" /two.bin >/dev/null && cmp -s <(head -c 4194304 "$TMP/a") "$FAKE/sd/two.bin"; } || fail "a put of two pieces from stdin"
expect "$(offsets put)$(sizes)" "0 2097152 - 4194304 " "a put of two whole pieces from stdin"
for bytes in 0 1000 2097152; do
	{ [ "$bytes" = 0 ] || head -c "$bytes" "$TMP/a"; } >"$TMP/one" # BSD head refuses a count of 0
	: >"$FAKE/log"
	landed "$TMP/one" "$FAKE/sd/one.bin" "a put of $bytes bytes from stdin" \
		piped "$TMP/one" wirePipePut 127.0.0.1 "$fakePort" /one.bin
	landed "$TMP/one" "$FAKE/sd/one" "a put of $bytes bytes from stdin to a name with no extension" \
		piped "$TMP/one" wirePipePut 127.0.0.1 "$fakePort" /one
	expect "$(awk '$1 == "put" { printf "%s %s ", NF, $5 }' "$FAKE/log")" "6 $bytes 6 $bytes " "a put of $bytes bytes from stdin, whole"
done
# A save's, extdata's or --system file goes whole: from stdin it can be one piece at most, and the word goes
# with it.
head -c 4096 /dev/zero >"$TMP/payload"
: >"$FAKE/log"
{ piped "$TMP/payload" wirePipePut 127.0.0.1 "$fakePort" /luma/payloads/3drop-fixture.firm system >/dev/null &&
	cmp -s "$TMP/payload" "$FAKE/sd/luma/payloads/3drop-fixture.firm"; } || fail "a --system put from stdin"
grep -q '^put .* system$' "$FAKE/log" || fail "a --system put from stdin went as: $(cat "$FAKE/log")"
for into in "save:000400000f800100/x.sav" "/luma/payloads/x.firm system"; do
	st=0
	# shellcheck disable=SC2086 # a path and the word
	piped "$TMP/a" wirePipePut 127.0.0.1 "$fakePort" $into >/dev/null 2>"$TMP/err" || st=$?
	expect "$st" 1 "a put from stdin into $into"
	said "$TOOL: '${into% *}' goes whole, so stdin gives it 2097152 bytes at most"
done
# A file over FAT32's largest (lowered here) is refused before anything goes. Stdin that passes it stops there
# with a message, which never says to put it again.
: >"$FAKE/log"
st=0
(WIRE_FILE_MAX=5000000 && wireFilePut 127.0.0.1 "$fakePort" "$TMP/a" /cap.bin) >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(grep -c '^put ' "$FAKE/log")" 1:0 "a put of a file over FAT32's largest"
said "$TOOL: $TMP/a is over 5000000 bytes, the most a FAT32 file holds"
st=0
(WIRE_FILE_MAX=5000000 && piped "$TMP/a" wirePipePut 127.0.0.1 "$fakePort" /cap.pipe) >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(offsets put)" "1:0 2097152 " "a put from stdin past FAT32's largest file"
said "$TOOL: stdin went past 5000000 bytes, the most a FAT32 file holds: delete /cap.pipe$WIRE_PUT_TEMP"
! grep -q "put it again" "$TMP/err" || fail "a put from stdin past FAT32's largest file said to put it again: $(cat "$TMP/err")"
# Three failures stop the put, and the console keeps the whole pieces before them; so does a put killed
# mid-piece. Run again on the same stdin, it reads past them; on other bytes it stops, and the temp is kept.
run 4 piped "$TMP/b" faultedOn put "2 3 4" hangup wirePipePut 127.0.0.1 "$fakePort" /piped.bin
said "$TOOL: the console keeps 2097152 bytes of it in /piped.bin$WIRE_PUT_TEMP: put it again to resume, or delete that file"
# A stdin that ends inside what the console keeps still sends its last piece, which publishes.
piped "$TMP/b" faultedOn put "3 4 5" hangup wirePipePut 127.0.0.1 "$fakePort" /short.pipe >/dev/null 2>&1 || :
head -c 4194304 "$TMP/b" >"$TMP/short"
: >"$FAKE/log"
landed "$TMP/short" "$FAKE/sd/short.pipe" "a put from a stdin that ends inside the kept pieces" \
	piped "$TMP/short" wirePipePut 127.0.0.1 "$fakePort" /short.pipe
expect "$(offsets put)$(sizes)" "2097152 4194304 " "a put from a stdin that ends inside the kept pieces"
: >"$FAKE/log"
# shellcheck disable=SC2016 # expanded when the job runs
faultedOn put 3 mute-put inGroup TERM 0 "$FAKE/log" 3 "put " bash -c '. "$1/tools/wire.sh"; WIRE_PIECE=2097152; cat "$3" | wirePipePut 127.0.0.1 "$2" /killed.pipe' _ "$ROOT" "$fakePort" "$TMP/a"
cmp -s "$FAKE/sd/killed.pipe$WIRE_PUT_TEMP" <(head -c 4194304 "$TMP/a") || fail "a killed put from stdin kept other than its two whole pieces"
run 1 piped "$TMP/b" wirePipePut 127.0.0.1 "$fakePort" /killed.pipe
said "$TOOL: the console's /killed.pipe$WIRE_PUT_TEMP keeps other bytes than this stdin's: delete that file, then put again"
! grep -q "put it again to resume" "$TMP/err" || fail "a put from other stdin said to resume: $(cat "$TMP/err")"
# If the temp is gone since its listing (`size`), the same stdin is sent again rather than resumed.
(
	original wireLs
	# shellcheck disable=SC2317 # called by wire.sh
	wireLs() { if [[ "$3" == *"$WIRE_PUT_TEMP" ]]; then echo "f 4194304 - x"; else real_wireLs "$@"; fi; }
	run 1 piped "$TMP/a" wirePipePut 127.0.0.1 "$fakePort" /gone.pipe
)
said "$TOOL: the console's /gone.pipe$WIRE_PUT_TEMP holds less than its listing said: put it again"
! grep -q "keeps 4194304 bytes" "$TMP/err" || fail "a put from stdin over a temp gone said it keeps bytes: $(cat "$TMP/err")"
cmp -s "$FAKE/sd/killed.pipe$WIRE_PUT_TEMP" <(head -c 4194304 "$TMP/a") || fail "a put from other stdin changed the kept temp"
: >"$FAKE/log"
landed "$TMP/a" "$FAKE/sd/killed.pipe" "a put from stdin resumed" piped "$TMP/a" wirePipePut 127.0.0.1 "$fakePort" /killed.pipe
expect "$(offsets put)$(sizes)" "4194304 5242885 " "a put from stdin resumed after a kill"
# device.sh's words for them: `-` for stdout or stdin, and `from <n>` for a byte count.
{ "$ROOT/tools/device.sh" get 127.0.0.1 /a.bin - from 3000000 | cmp -s - <(tail -c +3000001 "$TMP/a"); } || fail "device.sh get - from"
# shellcheck disable=SC2094 # landed only reads the file it is fed
landed "$TMP/b" "$FAKE/sd/dev.bin" "device.sh put -" "$ROOT/tools/device.sh" put 127.0.0.1 - /dev.bin <"$TMP/b"
for from in x -1 0x10 1234567890123456789; do
	run 2 "$ROOT/tools/device.sh" get 127.0.0.1 /a.bin - from "$from"
done
echo "testPieces: PASS"

#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# THREEDROP_COMPRESSION=auto against the fake device: which bodies go gzip and the bytes each puts on
# the wire, at the size and name boundaries, in a tree, from stdin, in an install and a resumed get;
# no gzip installed, a damaged body, and a gzip answer past one piece.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
export THREEDROP_COMPRESSION=auto
# Every size rule compares with the piece, so the boundaries sit at a smaller one; the default is pinned.
expect "$WIRE_PIECE" 33554432 "the default piece"
WIRE_PIECE=1048576
# sent <file> <gzip|none> <what> checks the body of the last whole put. It must be the file in gzip, in fewer
# bytes, or as is.
sent() {
	local raw wire
	raw=$(fileSize "$1") wire=$(fileSize "$FAKE/wire")
	expect "$(cat "$FAKE/compression")" "$2" "$3"
	if [ "$2" = none ]; then
		expect "$wire" "$raw" "$3: bytes on the wire"
	elif [ "$wire" -ge "$raw" ]; then
		fail "$3: $wire bytes on the wire for $raw"
	fi
}
# asked prints the logged requests, each as its verb and last word.
asked() { awk '{ printf "%s %s,", $1, $NF }' "$FAKE/log"; }
# gzips <verb> counts the logged requests of that verb that ask for gzip.
gzips() { awk -v v="$1" '$1 == v && / gzip( |$)/ { n++ } END { print n + 0 }' "$FAKE/log"; }

# A put goes gzip from 1024 bytes up to one piece. Past one piece it goes raw, in every piece.
for n in 1023 1024 1048576 1048577; do
	head -c "$n" /dev/zero >"$TMP/z.bin"
	: >"$FAKE/log"
	landed "$TMP/z.bin" "$FAKE/sd/z.bin" "an auto put of $n bytes" wireFilePut 127.0.0.1 "$fakePort" "$TMP/z.bin" /z.bin
	case "$n" in
	1023) sent "$TMP/z.bin" none "a put of $n bytes" ;;
	1048577) expect "$(gzips put):$(count put)" 0:2 "a put of $n bytes, in pieces" ;;
	*) sent "$TMP/z.bin" gzip "a put of $n bytes" ;;
	esac
done
# A get asks for gzip up to one piece (past that it uses ranges, tested below), and the answer is raw under
# 1024 bytes.
for n in 1023 1048576; do
	head -c "$n" /dev/zero >"$FAKE/sd/g.bin"
	: >"$FAKE/log"
	landed "$FAKE/sd/g.bin" "$TMP/g.out" "an auto get of $n bytes" wireGet 127.0.0.1 "$fakePort" /g.bin "$TMP/g.out"
	case "$n" in
	1023) expect "$(asked)$(fileSize "$FAKE/send")" "ls d,get gzip,1023" "a get of $n bytes" ;;
	*) expect "$(asked)$(($(fileSize "$FAKE/send") < n))" "ls d,get gzip,1" "a get of $n bytes" ;;
	esac
done
# A get lands beside its file first and is then renamed into place. A move from the scratch on another
# filesystem is a copy, and a copy cut short (ENOSPC, a Ctrl-C) never leaves the file there half replaced.
mkdir "$TMP/mvshim"
cat >"$TMP/mvshim/mv" <<EOF
#!/usr/bin/env bash
# Across folders, half the file and a failure, as a copy cut short. The paths are the last two
# arguments, after any options and --.
for a; do src=\$dst dst=\$a; done
if [ "\${src%/*}" != "\${dst%/*}" ]; then head -c \$((\$(wc -c <"\$src") / 2)) "\$src" >"\$dst"; exit 1; fi
exec $(command -v mv) "\$@"
EOF
chmod +x "$TMP/mvshim/mv"
head -c 65536 /dev/zero >"$FAKE/sd/g.bin"
printf 'the last good copy\n' >"$TMP/g.out"
st=0
PATH="$TMP/mvshim:$PATH" wireGet 127.0.0.1 "$fakePort" /g.bin "$TMP/g.out" >/dev/null 2>&1 || st=$?
[ "$st" != 0 ] || fail "an auto get whose landing was cut short succeeded"
expect "$(cat "$TMP/g.out")" "the last good copy" "the file an auto get's cut landing would have replaced"
rm -f "$TMP/g.out.3drop-part"
# A gzip body cut short under a true trailer inflates short. It is refused after three tries and nothing
# lands.
: >"$FAKE/log"
faulted gz-truncated fails wireGet 127.0.0.1 "$fakePort" /g.bin "$TMP/gz.out"
{ [ ! -e "$TMP/gz.out" ] && [ ! -e "$TMP/gz.out.3drop-part" ]; } || fail "an auto get of a cut gzip body left $(ls "$TMP"/gz.out*)"
expect "$(grep -c '^get .* gzip$' "$FAKE/log")" 3 "the tries of an auto get whose gzip body was cut"
# Random bytes of one piece come back larger in gzip than raw, yet still within what a get reads. At the
# default piece the excess is more than the reader's other slack, so only the cap's piece / 1000 lets them in.
head -c 33554432 /dev/urandom >"$FAKE/sd/r.bin"
: >"$FAKE/log"
(
	WIRE_PIECE=33554432
	landed "$FAKE/sd/r.bin" "$TMP/r.out" "an auto get of a random piece" wireGet 127.0.0.1 "$fakePort" /r.bin "$TMP/r.out"
)
slack=$((1024 + WIRE_REPLY_MAX + 2 + 128 + WIRE_TRAILER_WINDOW))
expect "$(asked)$(($(fileSize "$FAKE/send") > 33554432 + slack))" "ls d,get gzip,1" "an auto get of a random piece"
rm "$FAKE/sd/g.bin" "$FAKE/sd/r.bin" "$TMP/g.out" "$TMP/r.out"
# Only auto sends gzip; unset or any other word sends raw. Even under auto, a .cia or a name on the list never
# goes gzip, in any case (Z.ZIP). Each comes back whole under its own setting, because the digest covers the
# raw bytes.
head -c 131072 /dev/zero >"$TMP/z.bin"
cp "$TMP/z.bin" "$TMP/z.cia"
cp "$TMP/z.bin" "$TMP/Z.ZIP"
for c in "z.bin unset" "z.bin -" "z.cia auto" "Z.ZIP auto"; do
	read -r file mode <<<"$c"
	(
		if [ "$mode" = unset ]; then unset THREEDROP_COMPRESSION; else THREEDROP_COMPRESSION=$mode; fi
		landed "$TMP/$file" "$FAKE/sd/x" "a put of $file under $mode" wireFilePut 127.0.0.1 "$fakePort" "$TMP/$file" /x
		sent "$TMP/$file" none "a put of $file under $mode"
		: >"$FAKE/log"
		landed "$TMP/$file" "$TMP/x" "a get of $file under $mode" wireGet 127.0.0.1 "$fakePort" /x "$TMP/x"
		[ "$mode" = auto ] || expect "$(gzips get)" 0 "gzip gets of $file under $mode"
	)
done
rm "$FAKE/sd/x" "$TMP/x"

# Text goes in fewer bytes; random bytes, which gzip cannot shrink, go as they are.
head -c 200000 < <(seq 1 99999) >"$TMP/t.txt"
head -c 65536 /dev/urandom >"$TMP/r.bin"
for c in "r.bin none" "t.txt gzip"; do
	read -r file want <<<"$c"
	landed "$TMP/$file" "$FAKE/sd/$file" "an auto put of $file" wireFilePut 127.0.0.1 "$fakePort" "$TMP/$file" "/$file"
	sent "$TMP/$file" "$want" "an auto put of $file"
done
# A gzip no smaller than the file, or a gzip that fails, leaves the put raw.
for body in 'head -c 200000 /dev/zero' 'return 1'; do
	(
		eval "gzip() { $body; }"
		landed "$TMP/t.txt" "$FAKE/sd/t.txt" "a put whose gzip is '$body'" wireFilePut 127.0.0.1 "$fakePort" "$TMP/t.txt" /t.txt
	)
	sent "$TMP/t.txt" none "a put whose gzip is '$body'"
done
landed "$TMP/t.txt" "$FAKE/sd/t.txt" "an auto put of t.txt" wireFilePut 127.0.0.1 "$fakePort" "$TMP/t.txt" /t.txt
# The same bytes go as the same gzip under another name and time, because gzip -n keeps neither in it.
cp "$FAKE/wire" "$TMP/wire1"
cp "$TMP/t.txt" "$TMP/u.txt"
touch -t 200001010000 "$TMP/u.txt"
landed "$TMP/u.txt" "$FAKE/sd/t.txt" "an auto put of u.txt" wireFilePut 127.0.0.1 "$fakePort" "$TMP/u.txt" /t.txt
cmp -s "$TMP/wire1" "$FAKE/wire" || fail "the same bytes went as other gzip under another name and time"
# A put goes by the local name and a get by the card's name. So a.bin put as x.zip goes gzip, a.zip put as
# x.bin goes raw, and the card's x.zip comes back raw even though the get asked for gzip.
cp "$TMP/t.txt" "$TMP/a.bin"
cp "$TMP/t.txt" "$TMP/a.zip"
landed "$TMP/a.bin" "$FAKE/sd/x.zip" "a.bin put as x.zip" wireFilePut 127.0.0.1 "$fakePort" "$TMP/a.bin" /x.zip
sent "$TMP/a.bin" gzip "a.bin put as x.zip"
landed "$TMP/a.zip" "$FAKE/sd/x.bin" "a.zip put as x.bin" wireFilePut 127.0.0.1 "$fakePort" "$TMP/a.zip" /x.bin
sent "$TMP/a.zip" none "a.zip put as x.bin"
: >"$FAKE/log"
landed "$TMP/t.txt" "$TMP/x.out" "a get of x.zip" wireGet 127.0.0.1 "$fakePort" /x.zip "$TMP/x.out"
expect "$(asked)$(fileSize "$FAKE/send")" "ls d,get gzip,200000" "a get of x.zip"

# stdin of a piece or less (or none) goes the way a file with the card's name would; more than that goes in
# raw pieces.
: >"$TMP/empty"
for c in "p.txt t.txt gzip" "p.zip t.txt none" "p t.txt gzip" "e.txt empty none"; do
	read -r name file want <<<"$c"
	cp "$TMP/$file" "$TMP/stdin"
	landed "$TMP/$file" "$FAKE/sd/$name" "an auto put - as $name" wirePipePut 127.0.0.1 "$fakePort" "/$name" <"$TMP/stdin"
	sent "$TMP/$file" "$want" "an auto put - as $name"
done
head -c 1048577 /dev/zero >"$TMP/z.bin"
cp "$TMP/z.bin" "$TMP/stdin"
: >"$FAKE/log"
landed "$TMP/z.bin" "$FAKE/sd/big.pipe" "an auto put - past one piece" wirePipePut 127.0.0.1 "$fakePort" /big.pipe <"$TMP/stdin"
expect "$(gzips put):$(count put)" 0:2 "an auto put - past one piece"
rm "$FAKE/sd/big.pipe" "$FAKE/sd/z.bin" "$TMP/z.bin"

# An install never goes gzip, under any name, so every byte of it is on the wire.
fakeCia "$TMP/app.bin" 0004000001234500 5 1048576
dd if=/dev/zero of="$TMP/app.bin" bs=65536 seek=1 count=15 conv=notrunc 2>/dev/null
: >"$FAKE/log"
: >"$FAKE/bytes"
"$ROOT/tools/drop.sh" "$TMP/app.bin" 127.0.0.1 >/dev/null 2>"$TMP/err" || fail "an auto install: $(cat "$TMP/err")"
expect "$(gzips put):$(tail -n 1 "$FAKE/bytes")" 0:1048576 "an auto install"

# A tree put sends each file the way a put of that file alone would, a rerun finds nothing changed, and the
# tree comes back whole.
mkdir -p "$TMP/tree/sub"
cp "$TMP/t.txt" "$TMP/tree/t.txt"
cp "$TMP/r.bin" "$TMP/tree/r.bin"
cp "$TMP/t.txt" "$TMP/tree/z.zip"
head -c 4096 /dev/zero >"$TMP/tree/sub/z.bin"
head -c 1023 /dev/zero >"$TMP/tree/small"
: >"$FAKE/log"
"$ROOT/tools/device.sh" put 127.0.0.1 "$TMP/tree" /tree r >/dev/null 2>"$TMP/err" || fail "an auto tree put: $(cat "$TMP/err")"
expect "$(awk '$1 == "put" { printf "%s %s,", $4, ($7 == "gzip" ? "gzip" : "raw") }' "$FAKE/log")" \
	"/tree/r.bin raw,/tree/small raw,/tree/sub/z.bin gzip,/tree/t.txt gzip,/tree/z.zip raw," "an auto tree put"
"$ROOT/tools/device.sh" put 127.0.0.1 "$TMP/tree" /tree r >/dev/null 2>"$TMP/err" || fail "an auto tree put again: $(cat "$TMP/err")"
said "device: nothing changed -- 5 file(s) already current"
: >"$FAKE/log"
"$ROOT/tools/device.sh" get 127.0.0.1 /tree "$TMP/back" r >/dev/null 2>"$TMP/err" || fail "an auto tree get: $(cat "$TMP/err")"
for f in r.bin small sub/z.bin t.txt z.zip; do cmp -s "$TMP/tree/$f" "$TMP/back/$f" || fail "an auto tree get's $f"; done
# Its listing gave each file's size, so no `ls d` asks it again.
expect "$(gzips get):$(count ls)" 5:1 "an auto tree get"

# A file past a piece comes in raw ranges, after its listing.
head -c 3145728 < <(yes 3drop) >"$FAKE/sd/k.txt"
: >"$FAKE/log"
landed "$FAKE/sd/k.txt" "$TMP/k3.out" "an auto get of three pieces" wireGet 127.0.0.1 "$fakePort" /k.txt "$TMP/k3.out"
expect "$(awk '{ printf "%s ", $1 } $1 == "get" { for (i = 1; i < NF; i++) if ($i == "range") r = r $(i + 1) " " } END { printf "%s", r }' "$FAKE/log")" \
	"ls get get get 0 1048576 2097152 " "an auto get of three pieces"
# A get that resumes a kept part goes on in raw ranges. A part with no meta is removed, and the get is gzip.
faultedOn get "2 3 4" short-body wireGet 127.0.0.1 "$fakePort" /k.txt "$TMP/k.out" >/dev/null 2>&1 &&
	fail "a get whose second piece failed three times"
[ -s "$TMP/k.out.3drop-meta" ] || fail "a get that kept a piece kept no meta"
: >"$FAKE/log"
landed "$FAKE/sd/k.txt" "$TMP/k.out" "an auto get resumed" wireGet 127.0.0.1 "$fakePort" /k.txt "$TMP/k.out"
expect "$(gzips get):$(count ls)" 0:0 "an auto get resumed"
printf junk >"$TMP/k2.out.3drop-part"
: >"$FAKE/log"
landed "$FAKE/sd/t.txt" "$TMP/k2.out" "an auto get over a part with no meta" wireGet 127.0.0.1 "$fakePort" /t.txt "$TMP/k2.out"
expect "$(asked)" "ls d,get gzip," "an auto get over a part with no meta"
[ ! -e "$TMP/k2.out.3drop-part" ] || fail "an auto get left a part with no meta"
# get - from n uses ranges and never gzip.
: >"$FAKE/log"
wireGetPipe 127.0.0.1 "$fakePort" /k.txt 5 >"$TMP/from.out" || fail "an auto get - from 5"
cmp -s "$TMP/from.out" <(tail -c +6 "$FAKE/sd/k.txt") || fail "an auto get - from 5's bytes"
expect "$(gzips get)" 0 "an auto get - from 5"

# A file changed after its digest was taken is refused as `digest`, and a damaged gzip body as `gzip`; nothing
# is written.
cp "$TMP/t.txt" "$TMP/c.txt"
st=0
(
	# shellcheck disable=SC2317 # called by wire.sh
	gzip() { filePatch "$TMP/c.txt" 0 58 && command gzip "$@"; }
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/c.txt" /c.txt
) >/dev/null 2>"$TMP/err" || st=$?
expect "$st" 1 "a put whose file changed before its gzip"
said " put rejected digest d963f80a"
said "$TOOL: the bytes that came are not the ones this PC hashed: the file changed as it went, or was damaged on the way; send it again"
st=0
(
	# shellcheck disable=SC2317 # called by wire.sh
	gzip() { command gzip "$@" | perl -0777 -pe 'substr($_, 20, 1) ^= "\xff"'; }
	wireFilePut 127.0.0.1 "$fakePort" "$TMP/t.txt" /c.txt
) >/dev/null 2>"$TMP/err" || st=$?
expect "$st" 1 "a damaged gzip put"
said " put rejected gzip 00000000"
said "$TOOL: the gzip body was damaged, or did not inflate to exactly the size it declared, so nothing was written"
[ ! -e "$FAKE/sd/c.txt" ] || fail "a refused put wrote the file"
expect "$(_wireWhy put rejected gzip d963f811)" "the console had no memory for gzip: send it again, or with THREEDROP_COMPRESSION unset" "a gzip refusal for memory"
# A gzip get that the console timed out is asked again, as a piece is.
head -c 65536 /dev/zero >"$FAKE/sd/zeros.bin"
: >"$FAKE/log"
{ faultedOn get 1 timed-out wireGet 127.0.0.1 "$fakePort" /zeros.bin "$TMP/zeros.out" >/dev/null 2>&1 && cmp -s "$FAKE/sd/zeros.bin" "$TMP/zeros.out"; } || fail "an auto get the console timed out"
expect "$(asked)" "ls d,get gzip,get gzip," "an auto get the console timed out"
# A gzip body is inflated no more than one byte past the size its header declares before its digest is
# checked, because a same-length bomb would fill the disk first. The exit status is 4 and nothing is written.
(
	original _unitDigest
	# shellcheck disable=SC2317 # called by wire.sh
	_unitDigest() {
		[[ "$2" != */raw ]] || fileSize "$2" >"$TMP/inflated"
		real__unitDigest "$@"
	}
	st=0
	faulted gzip-bomb wireGet 127.0.0.1 "$fakePort" /zeros.bin "$TMP/bomb.out" >/dev/null 2>"$TMP/err" || st=$?
	expect "$st:$(cat "$TMP/inflated")" 4:65537 "a gzip body that inflates past its size"
	said "$TOOL: get body did not inflate to the authenticated size and trailer digest"
	[ ! -e "$TMP/bomb.out" ] || fail "a gzip bomb wrote the file"
	# A damaged gzip get is retried the way a failed piece is, then reported for what it is.
	run 4 faultedOn get "1 2 3" corrupt-body wireGet 127.0.0.1 "$fakePort" /t.txt "$TMP/bad.out"
	said "$TOOL: get body did not inflate to the authenticated size and trailer digest (gzip: "
	[ ! -e "$TMP/bad.out" ] || fail "a damaged gzip get wrote the file"
	# A gzip answer past one piece is refused before any of it is inflated, and is not asked for again (the
	# bomb case showed that an inflate sets the marker, so its absence here proves there was none).
	rm "$TMP/inflated"
	st=0
	: >"$FAKE/log"
	faulted gzip-huge wireGet 127.0.0.1 "$fakePort" /t.txt "$TMP/huge.out" >/dev/null 2>"$TMP/err" || st=$?
	expect "$st:$(count get)" 1:1 "a gzip answer past one piece"
)
said "$TOOL: get answered 1099511627776 bytes of gzip"
if [ -e "$TMP/inflated" ] || [ -e "$TMP/huge.out" ]; then fail "a gzip answer past one piece was inflated"; fi

# With no gzip installed, auto sends raw and says so once a run. A get then asks for no gzip, so none fails
# for lack of it.
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) skip "no PATH without gzip here, which takes symlinks" ;;
*)
	mkdir "$TMP/nogzip"
	perl -e 'my $to = shift; for my $d (split /:/, $ENV{PATH}) {
		opendir(my $h, $d) or next;
		symlink("$d/$_", "$to/$_") for grep { $_ ne "gzip" && !/^\.\.?$/ && !-e "$to/$_" } readdir $h;
	}' "$TMP/nogzip"
	! PATH=$TMP/nogzip "$BASH" -c 'command -v gzip' >/dev/null || fail "gzip is still on the PATH without it"
	: >"$FAKE/log"
	PATH=$TMP/nogzip "$ROOT/tools/device.sh" put 127.0.0.1 "$TMP/tree" /nogzip r >/dev/null 2>"$TMP/err" || fail "a tree put with no gzip: $(cat "$TMP/err")"
	expect "$(grep -c "no gzip here" "$TMP/err"):$(gzips put)" 1:0 "a tree put with no gzip"
	: >"$FAKE/log"
	PATH=$TMP/nogzip "$ROOT/tools/device.sh" get 127.0.0.1 /k.txt "$TMP/n.out" >/dev/null 2>"$TMP/err" || fail "a get with no gzip: $(cat "$TMP/err")"
	cmp -s "$FAKE/sd/k.txt" "$TMP/n.out" || fail "a get with no gzip's bytes"
	expect "$(grep -c "no gzip here" "$TMP/err"):$(gzips get):$(count get)" 1:0:1 "a get with no gzip"
	;;
esac
echo "$TEST_NAME: PASS"

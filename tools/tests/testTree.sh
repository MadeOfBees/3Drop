#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2034,SC2153,SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# put and get <dir> r against the fake device.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
# A failure says what the copy said, because a load-borne one on Git Bash may not come again to be asked.
itSaid() { [ ! -s "$TMP/err" ] || printf '; it said:\n%s' "$(cat "$TMP/err")"; }
# A manifest is compacted through a temp of its own, so another copy of the folder compacting it at
# once never has its temp overwritten or moved.
printf 'f 1 - a\nf 2 - a\n' >"$TMP/m.manifest"
printf 'theirs\n' >"$TMP/m.manifest.new"
_wireTreeKeep "$TMP/m.manifest"
[ "$(cat "$TMP/m.manifest"):$(cat "$TMP/m.manifest.new")" = "f 2 - a:theirs" ] || fail "a compaction beside another's took its temp"
expect() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'$(itSaid)"; }
wireFailed=("a b" c) wireSkipped=0
run 4 _wireTreeDone 0 2 "fetched here"
saidLine "$TOOL: failed: 'a b'" "the first failed name"
saidLine "$TOOL: failed: 'c'" "the second failed name"
device=$ROOT/tools/device.sh
os=$OSTYPE # names() plays other hosts
for name in COM0.txt LPT0; do OSTYPE=msys _wireHostName "$name" || fail "Windows does not reserve $name"; done
tree=$TMP/tree card=$FAKE/sd/tree
mkdir -p "$tree/sub dir/深い"
printf 'a\n' >"$tree/a.txt"
printf 'b\000\377' >"$tree/sub dir/b.bin"
printf 'c' >"$tree/sub dir/深い/c.bin"
: >"$tree/empty.bin"
printf 'z' >"$tree/z.txt"
for d in ._folder x.3drop-part; do
	mkdir "$tree/$d"
	printf x >"$tree/$d/inside"
done
for f in ._a.txt .DS_Store "sub dir/x.bin.3drop-part" "sub dir/x.bin.3drop-meta" "sub dir/x.bin.3drop.tmp" \
	"sub dir/x.bin.3drop.bak"; do printf x >"$tree/$f"; done

# moved <verb> <device.sh args...> sets movedList to each file the copy moved, as the fake logged them,
# with `|` after each, and returns the copy's status. Never call it inside $(...), where its status
# and its own fail would be lost.
moved() {
	local st=0 path
	: >"$FAKE/log"
	"$device" "$@" >"$TMP/out" 2>"$TMP/err" || st=$?
	[ ! -s "$TMP/out" ] || fail "$* printed on stdout: $(cat "$TMP/out")"
	movedList=""
	while IFS= read -r path; do
		[[ "$path" != hex:* ]] || path=$(hexDecode "${path#hex:}")
		movedList+="${path#/tree/}|"
	done < <(awk -v v="$1" '$1 == v { print ($1 == "put" ? $4 : $3) }' "$FAKE/log")
	return "$st"
}
# movedExits <status>:<files> <what> <verb> <device.sh args...> checks that the copy exits with
# that status, having moved those files.
movedExits() {
	local st=0
	moved "${@:3}" || st=$?
	expect "$st:$movedList" "$1" "$2"
}
movedAs() { movedExits "0:$1" "${@:2}"; }
same() { cmp -s "$1/$3" "$2/$3" || fail "$3 did not land byte-exact$(itSaid)"; }
# own <what> checks that every line the copy said is its own, never a raw error from a command it ran.
own() { ! grep -v '^device: ' "$TMP/err" || fail "$1 said the lines above"; }

# put: every file, in byte order, whichever way the directories end; the names never carried stay.
movedAs "a.txt|empty.bin|sub dir/b.bin|sub dir/深い/c.bin|z.txt|" "the first put" put 127.0.0.1 "$tree/" /tree/ r
for f in a.txt empty.bin "sub dir/b.bin" "sub dir/深い/c.bin" z.txt; do same "$tree" "$card" "$f"; done
for f in ._a.txt .DS_Store "sub dir/x.bin.3drop-part" "sub dir/x.bin.3drop-meta" "sub dir/x.bin.3drop.tmp" \
	"sub dir/x.bin.3drop.bak"; do
	[ ! -e "$card/$f" ] || fail "$f was put"
done
movedAs "" "a second put" put 127.0.0.1 "$tree" /tree r
cp -R "$tree" "$TMP/another"
printf other >"$TMP/another/a.txt"
movedExits 1: "a put from another folder" put 127.0.0.1 "$TMP/another" /tree r
same "$tree" "$card" a.txt
# A folder with nothing to put under it is made on the card, as a get makes one here. For a nested
# pair, making the deepest makes the one above it. A folder holding only a name no copy carries is
# made too. A FIFO, socket or device is named and left out, never read. A second put makes none of
# them again.
mkdir -p "$TMP/hollow/none" "$TMP/hollow/deep/er" "$TMP/hollow/finder"
: >"$TMP/hollow/finder/.DS_Store"
printf 'x' >"$TMP/hollow/f.txt"
mkfifo "$TMP/hollow/pipe"
movedAs "/hollow/f.txt|" "a put of empty folders" put 127.0.0.1 "$TMP/hollow" /hollow r
expect "$(grep -c '^mkdir ' "$FAKE/log")" 3 "mkdirs on a put of empty folders"
for d in none deep/er finder; do [ -d "$FAKE/sd/hollow/$d" ] || fail "the empty folder $d was not made"; done
saidLine "device: left out 'pipe': no file or folder (a FIFO, socket or device), which a put never reads" "a FIFO in a put"
saidLine "device: made 'deep/er/': a folder with no file to put" "a nested empty folder"
moved put 127.0.0.1 "$TMP/hollow" /hollow r || fail "a second put of empty folders: $(cat "$TMP/err")"
expect "$(grep -c '^mkdir ' "$FAKE/log")" 0 "mkdirs on a second put of empty folders"
mkdir -p "$TMP/hollow/only/e"
faultedOn mkdir 1 hangup movedExits 4: "an empty folder with no answer" put 127.0.0.1 "$TMP/hollow/only" /only r
movedAs "" "a put of a folder holding only an empty folder" put 127.0.0.1 "$TMP/hollow/only" /only r
expect "$(grep -c '^mkdir ' "$FAKE/log")" 1 "mkdirs on a put of a folder holding only an empty folder"
[ -d "$FAKE/sd/only/e" ] || fail "a put of a folder holding only an empty folder made nothing"
rm -r "$TMP/hollow" "$FAKE/sd/hollow" "$FAKE/sd/only"
# A name with a line break (no FAT name holds one) is skipped and said once, with its tree, never
# split into names this host does not have; the rest go, and the put ends 1.
mkdir -p "$TMP/nl/a"$'\n'"dir"
printf 'k' >"$TMP/nl/ok.txt"
printf 'n' >"$TMP/nl/a"$'\n'"b.txt"
printf 'd' >"$TMP/nl/a"$'\n'"dir/in.txt"
movedExits "1:/nl/ok.txt|" "a put of names with a line break" put 127.0.0.1 "$TMP/nl" /nl r
saidLine "device: skipped 'a?b.txt': a line break in its name, which FAT never holds" "a name with a line break"
saidLine "device: skipped 'a?dir': a line break in its name, which FAT never holds" "a folder name with a line break"
! grep -q 'cannot read' "$TMP/err" || fail "a name with a line break was split: $(cat "$TMP/err")"
rm -r "$TMP/nl" "$FAKE/sd/nl"
# A changed file goes alone; one deleted here stays on the console.
printf 'a2\n' >"$tree/a.txt"
rm "$tree/z.txt"
movedAs "a.txt|" "a changed file" put 127.0.0.1 "$tree" /tree r
same "$tree" "$card" a.txt
{ [ -f "$card/z.txt" ] && ! grep -q '^delete ' "$FAKE/log"; } || fail "a put deleted a file the PC no longer has"
# The card is checked, not trusted: a file it lost goes again.
rm "$card/sub dir/b.bin"
movedAs "sub dir/b.bin|" "a file the card lost" put 127.0.0.1 "$tree" /tree r
# A card file changed since the last put, at its size (a later mtime) or another, stays as it
# is and is said, whether or not the file here changed too, and the put ends 1. So does one the
# console had before any put of the tree from here. --overwrite, and only it, replaces each.
mkdir -p "$TMP/push" "$FAKE/sd/push"
for f in a b c d; do printf '%s%s' "$f" "$f" >"$TMP/push/$f.txt"; done
printf other >"$FAKE/sd/push/d.txt"
movedExits "1:/push/a.txt|/push/b.txt|/push/c.txt|" "a first put into a folder with files" put 127.0.0.1 "$TMP/push" /push r
expect "$(cat "$FAKE/sd/push/d.txt")" other "the card's file a first put found"
printf AA >"$FAKE/sd/push/a.txt"
perl -e 'utime(undef, 1700000400, $ARGV[0]) or die' "$FAKE/sd/push/a.txt"
printf BBB >"$FAKE/sd/push/b.txt"
printf CC >"$FAKE/sd/push/c.txt"
perl -e 'utime(undef, 1700000500, $ARGV[0]) or die' "$FAKE/sd/push/c.txt"
printf c2 >"$TMP/push/c.txt"
movedExits 1: "a put over card files changed since" put 127.0.0.1 "$TMP/push" /push r
expect "$(cat "$FAKE/sd/push/a.txt" "$FAKE/sd/push/b.txt" "$FAKE/sd/push/c.txt" "$FAKE/sd/push/d.txt")" \
	AABBBCCother "card files changed since"
for f in a b c; do
	saidLine "device: skipped '$f.txt': it changed on the console since the last put; --overwrite replaces it" "a card file changed since"
done
saidLine "device: skipped 'd.txt': the console has one this host never put; --overwrite replaces it" "a card file no put from here left"
movedAs "/push/a.txt|/push/b.txt|/push/c.txt|/push/d.txt|" "--overwrite over card files" put 127.0.0.1 --overwrite "$TMP/push" /push r
for f in a b c d; do same "$TMP/push" "$FAKE/sd/push" "$f.txt"; done
movedAs "" "a put after --overwrite" put 127.0.0.1 "$TMP/push" /push r
# FAT takes a name in any ASCII case for the one it holds, so the console's README.txt is not replaced
# by a readme.txt here, nor its RomFS/ by a romfs/, as if it held neither.
mkdir -p "$TMP/case/romfs" "$FAKE/sd/case/RomFS"
printf mine >"$TMP/case/readme.txt"
printf mine >"$TMP/case/romfs/data.bin"
printf theirs >"$FAKE/sd/case/README.txt"
printf theirs >"$FAKE/sd/case/RomFS/data.bin"
movedExits 1: "a put of names the card holds in another case" put 127.0.0.1 "$TMP/case" /case r
expect "$(cat "$FAKE/sd/case/README.txt" "$FAKE/sd/case/RomFS/data.bin")" theirstheirs "the card's names in another case"
for f in readme.txt romfs/data.bin; do
	saidLine "device: skipped '$f': the console has one this host never put; --overwrite replaces it" "a name the card holds in another case"
done
# A non-ASCII name the card's listing does not hold is asked for alone, so the card's own folding says
# whether it holds one in another case (the 3DS's FAT is the device's to say; the fake's card is this
# host's folder).
mkdir -p "$TMP/wide" "$FAKE/sd/wide"
printf mine >"$TMP/wide/É.txt"
printf theirs >"$FAKE/sd/wide/é.txt"
st=0
moved put 127.0.0.1 "$TMP/wide" /wide r >/dev/null || st=$?
expect "$(cat "$FAKE/sd/wide/é.txt")" theirs "the card's é.txt beside an É.txt here"
if [ "$FAKE/sd/wide/É.txt" -ef "$FAKE/sd/wide/é.txt" ]; then
	expect "$st" 1 "a put of a name the card holds in another non-ASCII case"
	saidLine "device: skipped 'É.txt': the console has one this host never put; --overwrite replaces it" "a name the card holds in another non-ASCII case"
fi
# Under --overwrite nothing is asked for alone, so the file goes whatever the answer.
printf mine2 >"$TMP/wide/É.txt"
movedAs "/wide/É.txt|" "--overwrite over a non-ASCII name no listing holds" put 127.0.0.1 --overwrite "$TMP/wide" /wide r
expect "$(grep -c '^ls ' "$FAKE/log")" 2 "listings under --overwrite over a non-ASCII name"
# Where no mtime says the card's file is the one the last put left (a put whose listing after it
# never came, or a save's), its bytes do. If unchanged, the file here replaces it; if rewritten at
# its size, it stays and is said.
mkdir "$TMP/unseen"
printf aa >"$TMP/unseen/x.txt"
printf bb >"$TMP/unseen/y.txt"
faultedOn ls 2 hangup movedAs "/unseen/x.txt|/unseen/y.txt|" "a put whose listing after it never came" put 127.0.0.1 "$TMP/unseen" /unseen r
printf XX >"$FAKE/sd/unseen/x.txt"
printf x2 >"$TMP/unseen/x.txt"
printf y2 >"$TMP/unseen/y.txt"
movedExits "1:/unseen/y.txt|" "a put over files no mtime vouches for" put 127.0.0.1 "$TMP/unseen" /unseen r
expect "$(cat "$FAKE/sd/unseen/x.txt" "$FAKE/sd/unseen/y.txt")" XXy2 "files no mtime vouches for"
saidLine "device: skipped 'x.txt': it changed on the console since the last put; --overwrite replaces it" "a rewritten file no mtime vouches for"
# A card copy that cannot be read to compare stays too, and is said; --overwrite replaces it unread.
printf x3 >"$TMP/unseen/x.txt"
faultedOn get 1 stopped movedExits 1: "a put over a card file that could not be read" put 127.0.0.1 "$TMP/unseen" /unseen r
expect "$(cat "$FAKE/sd/unseen/x.txt")" XX "a card file that could not be read"
saidLine "device: skipped 'x.txt': the console's could not be read to tell whether it changed since the last put; --overwrite replaces it" "a card file that could not be read"
movedAs "/unseen/x.txt|" "--overwrite over a file no mtime vouches for" put 127.0.0.1 --overwrite "$TMP/unseen" /unseen r
expect "$(count get)" 0 "gets under --overwrite over a file no mtime vouches for"
# When the card's listing ends early, a file it did not reach is asked for alone. One there that no
# put from here left stays, and is said.
mkdir -p "$TMP/reach" "$FAKE/sd/reach"
printf a >"$TMP/reach/a.txt"
printf f >"$TMP/reach/f.txt"
printf theirs >"$FAKE/sd/reach/f.txt"
reach() ( # wirePutTree whose card listing ended before naming f.txt
	original wireLs
	# shellcheck disable=SC2317 # called by wirePutTree
	wireLs() {
		if [ "${4:-}" != rt ]; then
			real_wireLs "$@"
			return
		fi
		real_wireLs "$@" | grep -v ' f\.txt$'
		echo "$TOOL: $WIRE_ENDED_EARLY (deadline 00000000); what came is above" >&2
		return 1
	}
	: >"$FAKE/log"
	wirePutTree 127.0.0.1 "$fakePort" "$TMP/reach" /reach
)
st=0
reach 2>"$TMP/err" || st=$?
expect "$st:$(cat "$FAKE/sd/reach/f.txt")" 1:theirs "a put whose listing did not reach a file the console holds"
saidLine "$TOOL: skipped 'f.txt': the console has one this host never put; --overwrite replaces it" "a file the listing did not reach"
same "$TMP/reach" "$FAKE/sd/reach" a.txt
# A card listing that ended early (a deadline, a walk too deep) still lets the put go, with the
# early end said. A file it did not name is asked for alone.
printf 'z2' >"$tree/z.txt"
faultedOn ls 1 ended-early movedAs "z.txt|" "a put whose card listing ended early" put 127.0.0.1 "$tree" /tree r
saidLine "device: the listing of '/tree' ended early: each file it did not name is asked for alone" "a put whose card listing ended early"
# A card that is full (`space`) or a console stranded stops the put at once, with 1: no file after
# it is tried, since each would be refused the same way. A file no answer came for (4), from a console
# that answers a stats after it, is said and left, and the rest go: the put ends 4. Run again, it goes
# on from there. b.bin keeps its size, so only the manifest says it has not gone.
printf 'B\000\377' >"$tree/sub dir/b.bin"
printf 'c2' >"$tree/sub dir/深い/c.bin"
printf 'e2' >"$tree/empty.bin"
for mode in refuse-early stranded; do
	[ "$mode" = refuse-early ] && at=2 want="1:empty.bin|sub dir/b.bin|" || at=1 want="1:sub dir/b.bin|"
	faultedOn put "$at" "$mode" movedExits "$want" "a put mid-tree, $mode" put 127.0.0.1 "$tree" /tree r
	saidLine "device: 'sub dir/b.bin' was not put; stopping: the same command again goes on from there" "a put stopped by $mode"
done
faultedOn put 1 hangup movedExits "4:sub dir/b.bin|sub dir/深い/c.bin|" "a put with no answer mid-tree" put 127.0.0.1 "$tree" /tree r
saidLine "device: 'sub dir/b.bin' was not put, and the console answers after it: going on without it" "a put with no answer"
said "device: 1 file(s) failed with no answer, the console answering after each" "a put with no answer"
movedAs "sub dir/b.bin|" "the put run again" put 127.0.0.1 "$tree" /tree r
for f in empty.bin "sub dir/b.bin" "sub dir/深い/c.bin"; do same "$tree" "$card" "$f"; done
# A name the console always refuses (a dot beside a non-ASCII character: `path`) is skipped on every
# run, never stopping the files after it.
mkdir "$TMP/dot"
printf a >"$TMP/dot/a.txt"
printf b >"$TMP/dot/.é"
printf z >"$TMP/dot/z.txt"
for run in first again; do
	[ "$run" = first ] && want="/dot/a.txt|/dot/z.txt|" || want="" # its listing alone is refused
	movedExits "1:$want" "a name the console refuses, $run" put 127.0.0.1 "$TMP/dot" /dot r
done
same "$TMP/dot" "$FAKE/sd/dot" z.txt
# Only a directory, never the root for an empty one, never with --system, and --overwrite only for a tree.
movedExits 1: "a put of a file as a tree" put 127.0.0.1 "$tree/a.txt" /tree r
for call in "put $tree '' r" "put '' /tree r" "put --system $tree /tree r" "put $tree /tree x" \
	"put --overwrite $tree '' r" "put --overwrite $tree /tree" "put --overwrite --system $tree /tree r" \
	"get /tree '' r" "get '' $TMP/back r" "get /tree $TMP/back x" "get --overwrite /tree '' r" \
	"get --overwrite /tree $TMP/back" "get --overwrite /tree $TMP/back x"; do
	eval "movedExits 2: \"\$call\" ${call%% *} 127.0.0.1 ${call#* }"
done

# get: every file and directory, byte-exact, whichever way the directories end; the names never
# carried stay on the console.
back=$TMP/back
mkdir "$card/empty dir"
for d in ._folder x.3drop-part; do
	mkdir "$card/$d"
	printf x >"$card/$d/inside"
done
for f in ._b .DS_Store "sub dir/y.3drop-part" "sub dir/y.3drop-meta" "sub dir/y.3drop.tmp" "sub dir/y.3drop.bak"; do printf x >"$card/$f"; done
movedAs "a.txt|empty.bin|sub dir/b.bin|sub dir/深い/c.bin|z.txt|" "the first get" get 127.0.0.1 /tree/ "$back/" r
for f in a.txt empty.bin "sub dir/b.bin" "sub dir/深い/c.bin" z.txt; do same "$card" "$back" "$f"; done
[ -d "$back/empty dir" ] || fail "a get made no empty directory"
for f in ._b .DS_Store "sub dir/y.3drop-part" "sub dir/y.3drop-meta" "sub dir/y.3drop.tmp" "sub dir/y.3drop.bak"; do
	[ ! -e "$back/$f" ] || fail "$f was fetched"
done
movedAs "" "a second get" get 127.0.0.1 /tree "$back" r
# A file changed on the console comes alone, by its size or by its mtime at the same size; one
# deleted there stays here, and one this folder lost comes again. One changed here since the last
# get stays as it is, said, and the get ends 1: --overwrite, and only it, replaces it.
printf 'longer\n' >"$card/a.txt"
printf 'C2' >"$card/sub dir/深い/c.bin"
perl -e 'utime(undef, 1700000000, $ARGV[0]) or die' "$card/sub dir/深い/c.bin"
rm "$card/z.txt" "$back/empty.bin"
printf 'more' >>"$back/sub dir/b.bin"
cp "$back/sub dir/b.bin" "$TMP/b-here"
movedExits "1:a.txt|empty.bin|sub dir/深い/c.bin|" "changed files" get 127.0.0.1 /tree "$back" r
saidLine "device: skipped 'sub dir/b.bin': it changed here since the last get; --overwrite replaces it" "a file changed here"
cmp -s "$TMP/b-here" "$back/sub dir/b.bin" || fail "a get replaced a file changed here"
movedAs "sub dir/b.bin|" "--overwrite over a file changed here" get 127.0.0.1 --overwrite /tree "$back" r
for f in a.txt empty.bin "sub dir/b.bin" "sub dir/深い/c.bin"; do same "$card" "$back" "$f"; done
[ -f "$back/z.txt" ] || fail "a get deleted a file the console no longer has"
# A get cut short three times (4), the console answering after it, is said and left, the rest come,
# and the get ends 4; a refusal (1) is said and skipped, the rest come, and the get ends 1. Run again,
# it goes on from there. b.bin keeps its size, so only the manifest says it has not landed.
printf 'a3' >"$card/a.txt"
printf 'b33' >"$card/sub dir/b.bin"
perl -e 'utime(undef, 1700000100, $ARGV[0]) or die' "$card/sub dir/b.bin"
printf 'c3' >"$card/sub dir/深い/c.bin"
faultedOn get "2 3 4" short-body movedExits "4:a.txt|sub dir/b.bin|sub dir/b.bin|sub dir/b.bin|sub dir/深い/c.bin|" "a get cut mid-tree" \
	get 127.0.0.1 /tree "$back" r
saidLine "device: 'sub dir/b.bin' was not fetched, and the console answers after it: going on without it" "a get cut mid-tree"
said "device: 1 file(s) failed with no answer, the console answering after each" "a get cut mid-tree"
saidLine "device: failed: 'sub dir/b.bin'" "a get cut mid-tree"
same "$card" "$back" a.txt
same "$card" "$back" "sub dir/深い/c.bin"
printf 'c4' >"$card/sub dir/深い/c.bin"
perl -e 'utime(undef, 1700000200, $ARGV[0]) or die' "$card/sub dir/深い/c.bin" # c3's may be this second's
faultedOn get 1 stopped movedExits "1:sub dir/b.bin|sub dir/深い/c.bin|" "a get refused mid-tree" get 127.0.0.1 /tree "$back" r
saidLine "device: skipped 'sub dir/b.bin': it was not fetched" "a refused get"
same "$card" "$back" "sub dir/深い/c.bin"
printf 'c4' >"$card/sub dir/深い/c.bin"
perl -e 'utime(undef, 1700000300, $ARGV[0]) or die' "$card/sub dir/深い/c.bin" # c3's may be this second's
faultedOn get 1 stranded movedExits "1:sub dir/b.bin|" "a get the console refused as stranded" get 127.0.0.1 /tree "$back" r
saidLine "device: 'sub dir/b.bin' was not fetched; stopping: the same command again goes on from there" "a get stopped by a stranded console"
# A file that failed with no room left in the folder for a piece of it stops the get at once too.
full() ( # <folder>: runs wireGetTree with the folder's filesystem full and its first get failing
	original freeKiB
	# shellcheck disable=SC2317 # called by wireGetTree
	freeKiB() { if [ "$1" = "$back" ]; then echo 0; else real_freeKiB "$1"; fi; }
	: >"$FAKE/log"
	faultedOn get 1 stopped wireGetTree 127.0.0.1 "$fakePort" /tree "$1"
)
st=0
full "$back" 2>"$TMP/err" || st=$?
expect "$st:$(grep -c '^get ' "$FAKE/log")" 1:1 "a get with no room here"
saidLine "$TOOL: 'sub dir/b.bin' was not fetched; stopping: the same command again goes on from there" "a get with no room here"
# A file no answer came for, from a console that answers no stats after it either, stops the get there.
gone() (
	# shellcheck disable=SC2317 # called by wireGetTree
	wireAnswers() { return 1; }
	: >"$FAKE/log"
	faultedOn get "1 2 3" short-body wireGetTree 127.0.0.1 "$fakePort" /tree "$back"
)
st=0
gone 2>"$TMP/err" || st=$?
expect "$st:$(grep -c '^get ' "$FAKE/log")" 4:3 "a get mid-tree from a console gone"
saidLine "$TOOL: 'sub dir/b.bin' was not fetched; stopping: the same command again goes on from there" "a get mid-tree from a console gone"
# A file whose piece went quiet, from a console that then answered no stats, stops the get there,
# the console asked once: the copy reads the piece's words (WIRE_NO_STATS), never asking again.
quietGone() (
	# shellcheck disable=SC2317 # called by wireGetTree
	wireAnswers() {
		echo asked >>"$TMP/asked"
		return 1
	}
	WIRE_WORKER_SILENCE=1
	rm -f "$TMP/asked"
	faultedOn get 1 stall-body wireGetTree 127.0.0.1 "$fakePort" /tree "$back"
)
st=0
quietGone 2>"$TMP/err" || st=$?
expect "$st:$(grep -c . "$TMP/asked")" 124:1 "a get whose file went quiet, the console answering no stats after it"
saidLine "$TOOL: 'sub dir/b.bin' was not fetched; stopping: the same command again goes on from there" "a get from a console gone quiet"
movedAs "sub dir/b.bin|sub dir/深い/c.bin|" "the get run again" get 127.0.0.1 /tree "$back" r
# A file's words come as they are said, not once it is done, so a wait says why while it waits.
printf 'b5' >"$card/sub dir/b.bin"
(
	WIRE_WORKER_SILENCE=2
	: >"$FAKE/log"
	faultedOn get "1 2" stall-body wireGetTree 127.0.0.1 "$fakePort" /tree "$back"
) 2>"$TMP/live" &
copier=$!
tryFor 600 grep -q 'went quiet' "$TMP/live" || :
seen=$(grep -c '^get ' "$FAKE/log")
wait "$copier" || fail "a get whose file went quiet twice: $(cat "$TMP/live")"
[ "$seen" -lt 3 ] || fail "a file's timeout was said only after its third try ($seen gets by then)"
same "$card" "$back" "sub dir/b.bin"
# A file the console always refuses (a key file: `protected`), or one whose part this host cannot
# make (a name of 246 bytes, its .3drop-part past NAME_MAX), is skipped on every run, never
# stopping the files after it.
long=$(printf 'n%.0s' {1..246})
mkdir -p "$FAKE/sd/3ds/3drop" "$FAKE/sd/3ds/$long"
printf k >"$FAKE/sd/3ds/3drop/key.cache"
printf l >"$FAKE/sd/3ds/3drop/log.txt"
printf n >"$FAKE/sd/3ds/$long/x"
printf m >"$FAKE/sd/3ds/m$long"
printf z >"$FAKE/sd/3ds/z.txt"
for run in first again; do
	st=0
	moved get 127.0.0.1 /3ds "$TMP/3ds" r >/dev/null || st=$?
	expect "$st:$(grep -c "^device: skipped '" "$TMP/err")" 1:2 "files that always fail, $run"
	saidLine "device: skipped '3drop/key.cache': it was not fetched" "a key file, $run"
	saidLine "device: skipped 'm$long': it was not fetched" "a long name, $run"
	grep -q "^device: could not make '.*/m$long\.3drop-part' here: " "$TMP/err" || fail "a long name's part, $run: $(cat "$TMP/err")"
	own "files that always fail, $run"
done
for f in 3drop/log.txt "$long/x" z.txt; do same "$FAKE/sd/3ds" "$TMP/3ds" "$f"; done
# The manifest is the folder's, so a first get into a folder that holds files replaces none of them:
# each is said, the rest come, and the get ends 1. A listing that ended early still brings what it
# named, then says so, and the get ends 1.
mkdir "$TMP/other"
printf 'xx' >"$TMP/other/a.txt"
printf 'yy' >"$TMP/other/empty.bin"
faultedOn ls 1 ended-early movedExits "1:sub dir/b.bin|sub dir/深い/c.bin|" "a first get into a folder that holds files, its listing ended early" \
	get 127.0.0.1 /tree "$TMP/other" r
said "device: the listing ended early (read c8804464): only what it named was fetched" "a get whose listing ended early"
for f in a.txt empty.bin; do
	saidLine "device: skipped '$f': this folder has one no get here landed; --overwrite replaces it" "a file a first get found"
done
expect "$(cat "$TMP/other/a.txt" "$TMP/other/empty.bin")" xxyy "files a first get found"
movedAs "a.txt|empty.bin|" "--overwrite over files a first get found" get 127.0.0.1 --overwrite /tree "$TMP/other" r
for f in a.txt empty.bin; do same "$card" "$TMP/other" "$f"; done
# A file changed here since the last get, at its size or another, stays as it is and is said,
# whether or not the console's changed too, and the get ends 1; --overwrite replaces each.
mkdir -p "$FAKE/sd/keep"
for f in a b c; do printf '%s%s' "$f" "$f" >"$FAKE/sd/keep/$f.txt"; done
moved get 127.0.0.1 /keep "$TMP/keep" r >/dev/null
printf A1 >"$TMP/keep/a.txt"
printf B22 >"$TMP/keep/b.txt"
printf C1 >"$TMP/keep/c.txt"
printf cc2 >"$FAKE/sd/keep/c.txt"
movedExits 1: "a get over files changed here" get 127.0.0.1 /keep "$TMP/keep" r
expect "$(cat "$TMP/keep/a.txt" "$TMP/keep/b.txt" "$TMP/keep/c.txt")" A1B22C1 "files changed here"
for f in a b c; do
	saidLine "device: skipped '$f.txt': it changed here since the last get; --overwrite replaces it" "a file changed here"
done
movedAs "/keep/a.txt|/keep/b.txt|/keep/c.txt|" "--overwrite over files changed here" get 127.0.0.1 --overwrite /keep "$TMP/keep" r
for f in a b c; do same "$FAKE/sd/keep" "$TMP/keep" "$f.txt"; done
# A directory where a file would land is never written into, even with --overwrite, because mv would
# land the file inside it, under another name.
printf 'n' >"$card/new.txt"
mkdir "$back/new.txt"
for word in "" --overwrite; do
	movedExits 1: "a get onto a directory${word:+ with $word}" get 127.0.0.1 ${word:+"$word"} /tree "$back" r
	expect "$(ls -A "$back/new.txt")" "" "what a get${word:+ with $word} put in a directory"
done
said "device: '$back/new.txt' is a directory" "a get onto a directory"
[ ! -e "$back/new.txt.3drop-part" ] || fail "a get onto a directory left its part"
# A directory the console does not have is said, and nothing is made here.
movedExits 1: "a get of no directory" get 127.0.0.1 /none "$TMP/none" r
[ ! -e "$TMP/none" ] || fail "a get of no directory made the folder"
# The host's own failures are said in this tool's words: a file here where the console has a folder,
# or in place of the folder asked for. The console's file asked for as a tree is named as a file.
mkdir -p "$FAKE/sd/q2/sub" "$TMP/q2"
printf x >"$FAKE/sd/q2/sub/x"
printf f >"$TMP/q2/sub"
movedExits 1: "a get where this host has a file for a folder" get 127.0.0.1 /q2 "$TMP/q2" r
expect "$(grep -c "^device: skipped '" "$TMP/err")" 2 "skips where this host has a file for a folder"
grep -q "^device: skipped 'sub': no folder could be made for it here: " "$TMP/err" || fail "a folder over a file: $(cat "$TMP/err")"
own "a get where this host has a file for a folder"
movedExits 1: "a get into a file" get 127.0.0.1 /q2 "$TMP/q2/sub" r
grep -q "^device: could not make '$TMP/q2/sub' here: " "$TMP/err" || fail "a get into a file said: $(cat "$TMP/err")"
own "a get into a file"
movedExits 1: "a get of a file as a tree" get 127.0.0.1 /q2/sub/x "$TMP/q1" r
[ ! -e "$TMP/q1" ] || fail "a get of a file as a tree made the folder"
saidLine "device: '/q2/sub/x' is a file, not a directory: get it without r" "a get of a file as a tree"
# A file or a folder this host cannot read is skipped, said with the host's reason, and the rest go.
# Root reads anything, and Git Bash's chmod changes nothing: then there is nothing to try.
mkdir -p "$TMP/unread/sub"
for f in a.txt u.txt z.txt sub/s.txt; do printf x >"$TMP/unread/$f"; done
chmod 000 "$TMP/unread/u.txt" "$TMP/unread/sub"
if [ ! -r "$TMP/unread/u.txt" ]; then
	st=0
	moved put 127.0.0.1 "$TMP/unread" /unread r || st=$?
	chmod 700 "$TMP/unread/sub" # so the test's own cleanup can remove it, pass or fail
	expect "$st:$movedList:$(grep -c "^device: skipped '" "$TMP/err")" "1:/unread/a.txt|/unread/z.txt|:2" "a put past what this host cannot read"
	for f in u.txt sub; do
		grep -q "^device: skipped '$f': this host cannot read it: " "$TMP/err" || fail "an unreadable '$f' said: $(cat "$TMP/err")"
	done
	own "a put past what this host cannot read"
fi
chmod 700 "$TMP/unread/sub"
# A warning the hashing says on stderr as it succeeds is never taken for part of a digest, which would
# make every file go again.
mkdir "$TMP/warned"
printf w >"$TMP/warned/w.txt"
warned() ( # wirePutTree with a hashing that warns
	original sha256File
	# shellcheck disable=SC2317 # called by wirePutTree
	sha256File() { echo "a warning" >&2 && real_sha256File "$@"; }
	: >"$FAKE/log"
	wirePutTree 127.0.0.1 "$fakePort" "$TMP/warned" /warned 2>/dev/null
	grep -c '^put ' "$FAKE/log"
)
expect "$(warned):$(warned)" 1:0 "a put whose hashing warns, then again"
# Each file's bytes go through openssl once per copy, a fresh or changed file too. The files a put
# has to hash go in one openssl, and the fresh ones it has to size in one wc, never one process per
# file.
opensslShim
printf '#!/usr/bin/env bash\necho "$*" >>%q\nexec %q "$@"\n' "$TMP/wc.log" "$(type -P wc)" >"$TMP/shim/wc"
chmod +x "$TMP/shim/wc"
mkdir -p "$TMP/h/sub" "$TMP/n" "$TMP/tw"
for f in h/a h/b h/sub/c n/1 n/2 n/3 n/4 n/5 n/6 tw/A tw/a; do head -c 77777 /dev/urandom >"$TMP/$f.bin"; done
# hashed <status> <command...> runs the command over /dev/tcp, so the fake's own openssl goes unlogged,
# and prints `<calls>:<files> <wc>:<alone>`: the openssl calls that read a file here and the files
# they read; the wc calls over files; and the files wirePutTree itself asks fileSize of.
hashed() (
	PATH=$TMP/shim:$PATH
	: >"$TMP/openssl.log"
	: >"$TMP/wc.log"
	: >"$TMP/alone.log"
	original fileSize
	# shellcheck disable=SC2317 # called by wirePutTree
	fileSize() {
		[ "${FUNCNAME[1]}" != wirePutTree ] || echo >>"$TMP/alone.log"
		real_fileSize "$@"
	}
	st=0
	devtcp "${@:2}" >/dev/null 2>&1 || st=$?
	[ "$st" = "$1" ] || fail "${*:2}: exited $st, want $1"
	awk -F'[|]' '$2 ~ /(^| )77777( |$)/ { calls++; files += gsub(/77777/, "", $2) } END { printf "%d:%d ", calls, files }' "$TMP/openssl.log"
	echo "$(grep -c '^-c .' "$TMP/wc.log" || :):$(($(wc -l <"$TMP/alone.log")))"
)
expect "$(hashed 0 wirePutTree 127.0.0.1 "$fakePort" "$TMP/h" /h)" "1:3 1:0" "a first put of 3 files"
expect "$(hashed 0 wirePutTree 127.0.0.1 "$fakePort" "$TMP/n" /n)" "1:6 1:0" "a first put of 6 files"
expect "$(hashed 0 wirePutTree 127.0.0.1 "$fakePort" "$TMP/h" /h)" "1:3 0:0" "a put of files unchanged"
# A file changed at its size, found changed by the openssl that hashed the files unchanged, goes as
# that found it: hashed once.
head -c 77777 /dev/urandom >"$TMP/h/a.bin"
: >"$FAKE/log"
expect "$(hashed 0 wirePutTree 127.0.0.1 "$fakePort" "$TMP/h" /h)" "1:3 0:1" "a put of a file changed"
expect "$(count put)" 1 "files a put of a file changed sent"
same "$TMP/h" "$FAKE/sd/h" a.bin
# A name the console takes for an earlier one is never read.
expect "$(hashed 1 wirePutTree 127.0.0.1 "$fakePort" "$TMP/tw" /tw)" "1:1 1:0" "a put of two names the console takes for one"
hashed 0 wireGetTree 127.0.0.1 "$fakePort" /h "$TMP/h-back" >/dev/null
expect "$(hashed 0 wireGetTree 127.0.0.1 "$fakePort" /h "$TMP/h-back")" "1:3 0:0" "a get of files unchanged"
# A batch takes a command line's worth of files per openssl (Windows caps one at 32,767 characters),
# so 120 files with 200-character names take two, each digest in its file's place.
mkdir "$TMP/many"
printf -v long '%0200d' 0
for i in $(seq 100 219); do printf '%s' "$i" >"$TMP/many/$i${long:3}"; done
many=("$TMP/many"/*)
: >"$TMP/openssl.log"
# shellcheck disable=SC2317 # called by _wireHashWords
openssl() { "$TMP/shim/openssl" "$@"; }
_wireHashes "${many[@]}" || fail "a batch of ${#many[@]} files"
unset -f openssl
expect "$(grep -c '^dgst ' "$TMP/openssl.log")" 2 "openssl calls over 120 long names"
for i in "${!many[@]}"; do [ "${wireHashes[i]}" = "$(sha256File "${many[i]}")" ] || fail "a batch's digest of ${many[i]##*/}"; done
_wireSizes "${many[@]}" || fail "the sizes of ${#many[@]} files"
expect "${#wireSizes[@]}:${wireSizes[0]}:${wireSizes[119]}" 120:3:3 "the sizes of 120 long names"
# A batch that cannot read a file leaves every file to be judged at its turn: the unreadable one is
# said, never taken for unchanged (its twin, the same bytes, comes next in the batch).
mkdir "$TMP/fb"
for f in a u v; do printf '%s' "${f/v/u}" >"$TMP/fb/$f.txt"; done
movedAs "/fb/a.txt|/fb/u.txt|/fb/v.txt|" "the batch's tree, put" put 127.0.0.1 "$TMP/fb" /fb r
movedAs "/fb/a.txt|/fb/u.txt|/fb/v.txt|" "the batch's tree, got" get 127.0.0.1 /fb "$TMP/fb-back" r
chmod 000 "$TMP/fb/u.txt" "$TMP/fb-back/u.txt"
if [ ! -r "$TMP/fb/u.txt" ]; then
	movedExits 1: "a put whose batch could not read a file" put 127.0.0.1 "$TMP/fb" /fb r
	expect "$(grep -c "^device: skipped 'u.txt': this host cannot read it: " "$TMP/err")" 1 "its unreadable file, said"
	movedExits 1: "a get whose batch could not read a file" get 127.0.0.1 /fb "$TMP/fb-back" r
	expect "$(grep -c "^device: skipped 'u.txt': it changed here since the last get; " "$TMP/err")" 1 "its unreadable file, said"
else
	skip "this host reads a file chmod 000, so the batch's fallback is unchecked"
fi
chmod 600 "$TMP/fb/u.txt" "$TMP/fb-back/u.txt"
# A file edited once the batch hashed it goes on the next put, never taken for unchanged again.
edited() ( # <command...>: runs it with $TMP/e/a.txt edited once a batch hashed it
	original _wireHashes
	# shellcheck disable=SC2317 # called by wirePutTree
	_wireHashes() { real__wireHashes "$@" && printf edited >"$TMP/e/a.txt"; }
	"$@"
)
mkdir "$TMP/e"
printf a >"$TMP/e/a.txt"
movedAs "/e/a.txt|" "the first put of a file then edited mid-copy" put 127.0.0.1 "$TMP/e" /e r
: >"$FAKE/log"
edited wirePutTree 127.0.0.1 "$fakePort" "$TMP/e" /e 2>/dev/null || fail "a put whose file was edited once its batch hashed it"
expect "$(count put):$(cat "$TMP/e/a.txt")" 0:edited "a put whose file was edited once its batch hashed it"
movedAs "/e/a.txt|" "the next put of a file edited mid-copy" put 127.0.0.1 "$TMP/e" /e r
same "$TMP/e" "$FAKE/sd/e" a.txt
# On Windows a name it reserves, which FAT holds, is skipped and said, never renamed, and the get
# ends 1 once the rest landed. On any host the same goes for a control character, which the listing
# shows as '?', and for a step out of the folder. Names like the reserved ones still come.
names() ( # <ostype> <folder> <listing lines...>: runs wireGetTree of $from (/tree) with the lines after
	# the fake's own
	OSTYPE=$1
	original _wireLsLines
	# shellcheck disable=SC2317 # called by wireGetTree
	_wireLsLines() { real__wireLsLines "$@" && { [ "${#lines[@]}" = 0 ] || printf '%s\n' "${lines[@]}"; }; }
	lines=("${@:3}")
	: >"$FAKE/log"
	wireGetTree 127.0.0.1 "$fakePort" "${from:-/tree}" "$2"
)
# The look-alikes get a folder of their own, beside a nested, an empty and a binary file, because the
# rest of /tree would come again for nothing.
mkdir -p "$FAKE/sd/names/sub dir/深い"
for f in CONSOLE.txt com10.txt nul2 'x.y z'; do printf r >"$FAKE/sd/names/$f"; done
: >"$FAKE/sd/names/empty.bin"
printf 'b\000\377' >"$FAKE/sd/names/sub dir/b.bin"
printf 'c' >"$FAKE/sd/names/sub dir/深い/c.bin"
st=0
from=/names names cygwin "$TMP/win" 'f 1 1700000000 a:b.txt' 'f 1 1700000000 q"x' 'f 1 1700000000 lt<x' \
	'f 1 1700000000 gt>x' 'f 1 1700000000 star*' 'f 1 1700000000 pipe|x' 'f 1 1700000000 back\x' \
	'f 1 1700000000 trail.' 'f 1 1700000000 space ' 'f 1 1700000000 CON' 'f 1 1700000000 nul.txt' \
	'f 1 1700000000 Com1.log' 'f 1 1700000000 lpt9' 'f 1 1700000000 COM¹.x' 'd - 1700000000 PRN' \
	'f 1 1700000000 PRN/x' 'f 1 1700000000 sub dir/AUX.b' 'f 1 1700000000 ctl?x' \
	'f 1 1700000000 ../escaped' 2>"$TMP/err" || st=$?
expect "$st:$(grep -c "skipped '" "$TMP/err")" 1:19 "a get of names Windows reserves"
saidLine "$TOOL: 19 name(s) skipped" "the skipped names' count"
expect "$(grep -c '^get ' "$FAKE/log")" 7 "the files fetched beside names Windows reserves"
for f in CONSOLE.txt com10.txt empty.bin nul2 "sub dir/b.bin" "sub dir/深い/c.bin" 'x.y z'; do
	same "$FAKE/sd/names" "$TMP/win" "$f"
done
{ [ ! -e "$TMP/escaped" ] && [ ! -e "$TMP/win/PRN" ]; } || fail "a skipped name was made"
st=0
from=/names names linux-gnu "$TMP/posix" 'f 1 1700000000 ctl?x' 'f 1 1700000000 ../escaped' 2>"$TMP/err" || st=$?
expect "$st:$(grep -c "skipped '" "$TMP/err"):$(grep -c '^get ' "$FAKE/log")" 1:2:7 "a get of names no host can make"
# Elsewhere a name only Windows reserves comes, as named.
case "$os" in msys* | cygwin*) ;; *)
	printf w >"$FAKE/sd/names/Aux.txt"
	movedAs "/names/Aux.txt|" "a name only Windows reserves, elsewhere" get 127.0.0.1 /names "$TMP/posix" r
	same "$FAKE/sd/names" "$TMP/posix" Aux.txt
	;;
esac
# A save's or extdata's root keeps its slash, so its tree comes and goes as any other's; an archive's
# listing has no mtime, so every get fetches it all.
echo "000400000f800100 1" >>"$FAKE/installed"
n=0
for root in save:000400000f800100 extdata:000f8001; do
	n=$((n + 1))
	mkdir -p "$FAKE/$root/sub"
	printf s >"$FAKE/$root/sub/s.bin"
	for run in first again; do
		movedAs "$root/sub/s.bin|" "a get of $root/, $run" get 127.0.0.1 "$root/" "$TMP/archive$n" r
	done
	same "$FAKE/$root" "$TMP/archive$n" sub/s.bin
	printf t >"$TMP/archive$n/sub/s.bin" # its size: an extdata file keeps its own
	movedAs "$root/sub/s.bin|" "a put of $root/" put 127.0.0.1 --overwrite "$TMP/archive$n" "$root/" r
	same "$FAKE/$root" "$TMP/archive$n" sub/s.bin
	movedAs "" "a put of $root/ again" put 127.0.0.1 "$TMP/archive$n" "$root" r
	printf u >"$TMP/archive$n/sub/s.bin"
	movedAs "$root/sub/s.bin|" "a put of a change to $root/" put 127.0.0.1 "$TMP/archive$n" "$root" r
	printf G >"$FAKE/$root/sub/s.bin" # the game's, at its size
	printf v >"$TMP/archive$n/sub/s.bin"
	movedExits 1: "a put over $root/ the game rewrote" put 127.0.0.1 "$TMP/archive$n" "$root" r
	expect "$(cat "$FAKE/$root/sub/s.bin")" G "the file in $root/ the game rewrote"
done
# macOS hands out a name decomposed (NFD), which goes composed, as the card names it, and is found there.
if [[ "$os" == darwin* ]]; then
	mkdir "$TMP/nfd"
	printf x >"$TMP/nfd/"$'\xe3\x81\x8b\xe3\x82\x99' # か and a combining voiced mark
	movedAs "/nfd/"$'\xe3\x81\x8c'"|" "a decomposed name" put 127.0.0.1 "$TMP/nfd" /nfd r
	movedAs "" "a decomposed name put again" put 127.0.0.1 "$TMP/nfd" /nfd r
fi
# A link is never followed: a put says each one it leaves out, and a get never writes through one
# here (a folder, a file, or a file's part), each said and skipped, ending 1. Git Bash makes copies
# for links unless told otherwise: then there is nothing to try.
if ln -s "$TMP/nowhere" "$TMP/probe" 2>/dev/null && [ -L "$TMP/probe" ]; then
	mkdir -p "$TMP/links/real" "$TMP/outside"
	printf r >"$TMP/links/real/r.txt"
	printf t >"$TMP/outside/t.txt"
	ln -s "$TMP/outside/t.txt" "$TMP/links/file"
	ln -s "$TMP/outside" "$TMP/links/folder"
	movedAs "/links/real/r.txt|" "a put of a tree with links" put 127.0.0.1 "$TMP/links" /links r
	for f in file folder; do
		saidLine "device: left out '$f': a link, which a put never follows" "a put left out '$f'"
	done
	mkdir -p "$FAKE/sd/pull/sub" "$TMP/pull"
	for f in a.txt f.txt g.txt sub/v.txt; do printf p >"$FAKE/sd/pull/$f"; done
	ln -s "$TMP/outside" "$TMP/pull/sub"
	ln -s "$TMP/outside/t.txt" "$TMP/pull/f.txt"
	ln -s "$TMP/outside/part" "$TMP/pull/g.txt.3drop-part"
	movedExits "1:/pull/a.txt|" "a get through links" get 127.0.0.1 /pull "$TMP/pull" r
	expect "$(grep -c "^device: skipped '" "$TMP/err")" 4 "skips on a get through links"
	for f in sub sub/v.txt f.txt; do
		saidLine "device: skipped '$f': a link here, which a get never writes through" "a get through the link at '$f'"
	done
	saidLine "device: '$TMP/pull/g.txt.3drop-part' is a link, which a get never writes through" "a get through a link as its part"
	expect "$(ls "$TMP/outside"):$(cat "$TMP/outside/t.txt")" "t.txt:t" "what the links lead to"
	{ [ -L "$TMP/pull/f.txt" ] && [ -L "$TMP/pull/g.txt.3drop-part" ]; } || fail "a get replaced a link"
else
	skip "no links here, so a tree copy's refusal of one is unchecked"
fi
# Two names the console keeps apart which this host takes for one, by case, or composed and
# decomposed on macOS: the second is skipped and said, ending 1, and never lands over the first. A
# host that keeps them apart gets both. Where the fake's card, a folder here, cannot hold the second
# either, it comes as a listing line after the first.
nfc=$'\xc3\xa9' nfd=$'e\xcc\x81'
mkdir -p "$FAKE/sd/twins" "$TMP/fold"
printf x >"$TMP/fold/A"
printf x >"$TMP/fold/$nfc"
extra=() firsts=() seconds=()
for pair in "A.txt a.txt" "$nfc.txt $nfd.txt"; do
	one=${pair%% *} two=${pair#* }
	printf first >"$FAKE/sd/twins/$one"
	firsts+=("$one")
	if [ -e "$TMP/fold/${two%.txt}" ]; then
		extra+=("f 6 1700000000 $two")
	else
		printf second >"$FAKE/sd/twins/$two"
		seconds+=("$two")
	fi
done
st=0
from=/twins names "$os" "$TMP/twins" ${extra[@]+"${extra[@]}"} 2>"$TMP/err" || st=$?
expect "$st:$(grep -c '^get ' "$FAKE/log"):$(grep -c "^testTree: skipped '" "$TMP/err")" \
	"$((${#extra[@]} != 0)):$((4 - ${#extra[@]})):${#extra[@]}" "a get of names this host may take for one"
for f in "${firsts[@]}"; do expect "$(cat "$TMP/twins/$f")" first "the first of two names, $f"; done
for f in ${seconds[@]+"${seconds[@]}"}; do expect "$(cat "$TMP/twins/$f")" second "the second of two names kept apart, $f"; done
for line in ${extra[@]+"${extra[@]}"}; do
	said "testTree: skipped '${line#f 6 1700000000 }': this host takes it for '" "the second of two names"
done
# A name that is not UTF-8 (a lone low surrogate in FAT comes as ED Bx xx) still folds, bytewise: it
# is skipped, and the rest come.
st=0
from=/twins names "$os" "$TMP/twins-bad" $'f 3 1700000000 \xed\xb0\x80bad' 2>"$TMP/err" || st=$?
expect "$st:$(LC_ALL=C grep -c "^testTree: skipped '" "$TMP/err")" 1:1 "a get of a name that is no UTF-8"
for f in "${firsts[@]}"; do same "$FAKE/sd/twins" "$TMP/twins-bad" "$f"; done
! LC_ALL=C grep -v '^testTree: ' "$TMP/err" || fail "a get of a name that is no UTF-8 said the lines above"
# What the last copy left is looked up by the whole name, never by its last word: a file given the
# bytes, size and mtime of one whose name ends its own still goes.
mkdir "$TMP/ends"
printf same >"$TMP/ends/b.txt"
printf diff >"$TMP/ends/a b.txt"
moved put 127.0.0.1 "$TMP/ends" /ends r >/dev/null
printf same >"$TMP/ends/a b.txt"
movedAs "/ends/a b.txt|" "a file given the bytes of one whose name ends its own" put 127.0.0.1 "$TMP/ends" /ends r
printf diff >"$FAKE/sd/ends/a b.txt"
perl -e 'utime(undef, 1700000000, $ARGV[0]) or die' "$FAKE/sd/ends/a b.txt"
perl -e 'utime(undef, 1700000200, $ARGV[0]) or die' "$FAKE/sd/ends/b.txt"
moved get 127.0.0.1 /ends "$TMP/ends-back" r >/dev/null
printf same >"$FAKE/sd/ends/a b.txt"
perl -e 'utime(undef, 1700000200, $ARGV[0]) or die' "$FAKE/sd/ends/a b.txt"
movedAs "/ends/a b.txt|" "a card file given the size and mtime of one whose name ends its own" get 127.0.0.1 /ends "$TMP/ends-back" r
same "$FAKE/sd/ends" "$TMP/ends-back" "a b.txt"
# Into a relative folder under an exported CDPATH (cd searches it before the current folder), a get
# keys what it landed by that folder, not one the search finds: again without CDPATH, nothing goes.
mkdir -p "$FAKE/sd/cdpath" "$TMP/decoy/rel"
printf x >"$FAKE/sd/cdpath/x.bin"
(cd "$TMP" && CDPATH="$TMP/decoy" run 0 "$device" get 127.0.0.1 /cdpath rel r)
: >"$FAKE/log"
(cd "$TMP" && run 0 "$device" get 127.0.0.1 /cdpath rel r)
expect "$(count get)" 0 "the same get again without CDPATH"
# A file the console cannot write (its folder's name a file on the card) is refused, said and
# skipped. The rest go, the put ends 1, and the card's file stays.
mkdir -p "$TMP/blocked/d" "$FAKE/sd/blocked"
printf x >"$TMP/blocked/d/x.txt"
printf y >"$TMP/blocked/y.txt"
printf card >"$FAKE/sd/blocked/d"
movedExits "1:/blocked/d/x.txt|/blocked/y.txt|" "a put of a file the console cannot write" put 127.0.0.1 "$TMP/blocked" /blocked r
said "device: skipped 'd/x.txt': it was not put" "a put of a file the console cannot write"
{ [ "$(cat "$FAKE/sd/blocked/d")" = card ] && cmp -s "$TMP/blocked/y.txt" "$FAKE/sd/blocked/y.txt"; } ||
	fail "a put of a file the console cannot write changed the card, or the rest did not go"
# Two names FAT takes for one (ASCII case), as a case-sensitive host holds them: the second is skipped,
# said, and never put over the first. Where this host folds case too, find is made to list both.
mkdir "$TMP/twins-put"
printf first >"$TMP/twins-put/A.bin"
printf second >"$TMP/twins-put/a.bin"
if [ "$TMP/twins-put/A.bin" -ef "$TMP/twins-put/a.bin" ]; then
	# The paths written in (device.sh, where it runs, has no TMP); the tree's own listing alone.
	eval "find() { command find \"\$@\"; [ \"\$2\" != $(printf %q "$TMP/twins-put") ] || echo $(printf %q "$TMP/twins-put/a.bin"); }"
	export -f find
fi
movedExits "1:/twins-put/A.bin|" "a put of two names FAT takes for one" put 127.0.0.1 "$TMP/twins-put" /twins-put r
unset -f find
saidLine "device: skipped 'a.bin': the console takes it for 'A.bin'" "a put of two names FAT takes for one"
# A put keeps its state to itself: a caller's w stays, and no array of what the card holds is left.
w=caller
wirePutTree 127.0.0.1 "$fakePort" "$TMP/twins-put" /twins-put >/dev/null 2>&1 || :
expect "$w:${wireCardSize[*]-none}" caller:none "a caller's state after a put"
# A copy's state is the console's, whatever address reaches it (another network, a new lease): the
# same copies through another address of it move nothing and skip nothing.
mkdir -p "$TMP/lease"
printf l >"$TMP/lease/l.txt"
"$device" put 127.0.0.1 "$TMP/lease" /lease r >/dev/null 2>"$TMP/err" || fail "the put another address follows$(itSaid)"
"$device" get 127.0.0.1 /lease "$TMP/lease-back" r >/dev/null 2>"$TMP/err" || fail "the get another address follows$(itSaid)"
: >"$FAKE/log"
st=0
{ "$device" put localhost "$TMP/lease" /lease r && "$device" get localhost /lease "$TMP/lease-back" r; } >/dev/null 2>"$TMP/err" || st=$?
expect "$st:$(count put):$(count get)" 0:0:0 "a put and a get again through another address of the console"
echo "testTree: PASS"

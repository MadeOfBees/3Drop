#!/usr/bin/env bash
# shellcheck source=/dev/null # fakeDevice.sh and wire.sh are linted alone, not again in each test
# sync.sh against the fake device.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
stub="$TMP/stub" tid=000400000ff4e500
echo "$tid 1" >"$FAKE/installed"
# There is no top-level romfs/ until later, because the console launches no title that has one unless it is
# a game.
mkdir -p "$stub/data/a"
printf 'code\000\377' >"$stub/code.bin"
printf 'level\n' >"$stub/data/a/b.bin"
printf 'ips' >"$stub/code.ips"
: >"$stub/data/empty.bin"
printf 'x' >"$stub/data/a b.txt"
: >"$stub/.DS_Store"

runSync() { # [flags...] -> prints "<relpath>" for each put and "run" with its word for each launch, in order
	: >"$FAKE/log"
	"$ROOT/tools/sync.sh" "$@" "$tid" "$stub" 127.0.0.1 >/dev/null 2>&1 || return 1
	while read -r verb _ _ path word; do
		case "$verb" in
		put)
			[[ "$path" != hex:* ]] || path=$(hexDecode "${path#hex:}")
			printf '%s ' "${path#/luma/titles/"$tid"/}"
			;;
		run) printf 'run %s' "${word:+$word }" ;;
		esac
	done <"$FAKE/log"
}
synced() { cmp -s "$stub/$1" "$FAKE/sd/luma/titles/$tid/$1" || fail "$1 was not synced byte-exact"; }

expect "$(runSync)" "code.bin code.ips data/a b.txt data/a/b.bin data/empty.bin run " "the first sync"
for f in code.bin code.ips "data/a b.txt" data/a/b.bin data/empty.bin; do synced "$f"; done
out=$(runSync) || fail "a sync of an unchanged tree failed"
expect "$out" "" "an unchanged tree"
# The romfs check looks at the stub with the shell's own glob, so a sync runs the same finds (the tree's
# walk) with the check as without it (--game).
FINDS=$TMP/finds
export FINDS
# shellcheck disable=SC2317 # called by the sync's own processes, through export -f
find() {
	echo x >>"$FINDS"
	command find "$@"
}
export -f find
finds() { : >"$FINDS" && runSync "$@" >/dev/null && awk 'END { print NR }' "$FINDS"; }
walk=$(finds) || fail "a sync of an unchanged tree failed"
[ "$walk" -gt 0 ] || fail "no find was counted: the walk's are not seen"
expect "$(finds --game)" "$walk" "the finds of a sync, with the romfs check and without it"
unset -f find finds
printf 'level 2\n' >"$stub/data/a/b.bin"
expect "$(runSync)" "data/a/b.bin run " "one changed file"
synced data/a/b.bin
printf 'level\n' >"$stub/data/a/b.bin" # back to bytes an earlier sync sent, while the console has others
expect "$(runSync)" "data/a/b.bin run " "a file reverted to its earlier bytes"
synced data/a/b.bin
printf 'code 2' >"$stub/code.bin"
if faulted bad-mac runSync >/dev/null; then fail "sync.sh accepted an unauthenticated reply"; fi
expect "$(runSync)" "code.bin run " "a failed file is resent"
synced code.bin
# A name the console refuses (a dot beside a non-ASCII character) is skipped, the rest are sent, and
# nothing launches. The exit status is 1.
printf x >"$stub/data/.é"
printf 'level 3\n' >"$stub/data/a/b.bin"
: >"$FAKE/log"
if "$ROOT/tools/sync.sh" "$tid" "$stub" 127.0.0.1 >/dev/null 2>"$TMP/err"; then fail "a sync with a refused name passed"; fi
! grep -q '^run ' "$FAKE/log" || fail "a sync with a skipped name launched"
saidLine "sync: title $tid was not launched: not every file went" "a sync that did not launch"
synced data/a/b.bin
rm "$stub/data/.é"
expect "$(runSync --force-full-sync)" "code.bin code.ips data/a b.txt data/a/b.bin data/empty.bin run " "--force-full-sync"
# A card file that changed since the last sync, even at the same size (a later mtime), is replaced, because
# the stub is the truth (a put's --overwrite, whose other cases testTree holds).
printf 'IPS' >"$FAKE/sd/luma/titles/$tid/code.ips"
perl -e 'utime(undef, 1700000600, $ARGV[0]) or die' "$FAKE/sd/luma/titles/$tid/code.ips"
expect "$(runSync)" "code.ips run " "a card file changed at its size"
synced code.ips
# A folder named romfs below the top isn't a romfs folder as Luma sees it, so it is sent and the title
# launches.
mkdir "$stub/data/romfs"
printf 'level\n' >"$stub/data/romfs/b.bin"
expect "$(runSync)" "data/romfs/b.bin run " "a romfs folder below the top"
synced data/romfs/b.bin
# A top-level romfs folder, in any letter case, crashes the console in Luma's loader unless the title is a
# game. Without --game nothing is sent, with or without --force-full-sync, and no request is made.
refusal="sync: nothing was sent: the stub has a romfs folder, which crashes the console in Luma's loader unless the title is a game built with Nintendo's SDK; for such a game, add --game; for your own app, take the folder out of the stub"
for name in romfs ROMFS RoMfS; do
	mkdir "$stub/$name"
	printf 'level\n' >"$stub/$name/a.bin"
	for flags in "" --force-full-sync; do
		: >"$FAKE/log"
		if "$ROOT/tools/sync.sh" ${flags:+"$flags"} "$tid" "$stub" 127.0.0.1 >/dev/null 2>"$TMP/err"; then fail "a sync of a $name folder $flags went"; fi
		[ ! -s "$FAKE/log" ] || fail "a sync of a $name folder $flags sent: $(cat "$FAKE/log")"
		saidLine "$refusal" "a sync of a $name folder $flags"
	done
	rm -r "${stub:?}/${name:?}"
done
# A link named romfs isn't a folder, and a put never follows one, so the sync goes ahead.
ln -s data "$stub/romfs"
printf 'level 4\n' >"$stub/data/a/b.bin"
expect "$(runSync)" "data/a/b.bin run " "a link named romfs"
rm "$stub/romfs"
# --game says the title is a game. The files are sent, and the launch carries the word, whichever order
# the flags come in.
mkdir "$stub/romfs"
printf 'level\n' >"$stub/romfs/a.bin"
expect "$(runSync --game)" "romfs/a.bin run game " "--game with a romfs folder"
synced romfs/a.bin
all="code.bin code.ips data/a b.txt data/a/b.bin data/empty.bin data/romfs/b.bin romfs/a.bin run game "
expect "$(runSync --game --force-full-sync)" "$all" "--game, then --force-full-sync"
expect "$(runSync --force-full-sync --game)" "$all" "--force-full-sync, then --game"
# The card keeps the folder (sync never deletes), so a launch without the word is refused by the console
# once the files have been sent. With the word, and no folder in the stub, the title launches.
rm -r "$stub/romfs"
printf 'code 3' >"$stub/code.bin"
if "$ROOT/tools/sync.sh" "$tid" "$stub" 127.0.0.1 >/dev/null 2>"$TMP/err"; then fail "a sync launched a title whose card holds a romfs folder"; fi
synced code.bin
said "run rejected layeredfs" "the console's refusal"
said "tools/device.sh run <ip> --game <title-id>" "the way to launch it"
printf 'code 4' >"$stub/code.bin"
expect "$(runSync --game)" "code.bin run game " "--game with no romfs folder in the stub"
tid=0004000003d37200
if runSync >/dev/null; then fail "3Drop's own title was synced"; fi
[ ! -e "$FAKE/sd/luma/titles/$tid" ] || fail "a file landed in 3Drop's own LayeredFS directory"
echo "testSync: PASS"

#!/usr/bin/env bash
# sync.sh [--force-full-sync] [--game] <title-id> <stub-dir> <ip> [port]
# Sends the stub's changed files to sdmc:/luma/titles/<title-id>/, then launches the title if anything
# went and nothing was skipped. A file goes when its SHA-256 changed since the last sync, or the card
# no longer holds it as that sync left it; --force-full-sync sends every file. This PC keeps the record
# of the last sync, so another PC starts with a full one. It never deletes: a file you remove from the
# stub stays on the card until device.sh delete removes it there. Needs Luma3DS "Enable game patching".
# A romfs folder crashes the console in Luma's loader unless the title is a game built with
# Nintendo's SDK: a stub with one is refused unless you add --game, which says the title is one.
set -euo pipefail

. "$(dirname "$0")/lib.sh"
scratchTool
. "$ROOT/tools/wire.sh"

# A card file changed since the last sync is replaced too, as put --overwrite does.
overwrite=overwrite game=""
while :; do
	case "${1:-}" in
	--force-full-sync) overwrite=all ;;
	--game) game=game ;;
	*) break ;;
	esac
	shift
done
stubDir="${2:-}"
if [ -z "${1:-}" ] || [ -z "$stubDir" ] || [ -z "${3:-}" ]; then usage; fi
titleID=$(wireTitleID "$1") || exit 2
wireConsole "$3" "${4:-}"
ip=$wireIP port=$wirePort

# This looks for a top-level folder named romfs in any case, as FAT sees it, because Luma reads
# /luma/titles/<id>/romfs. A link doesn't count, because a put never follows one. It uses the shell's glob, so
# a sync runs no program for it.
if [ -z "$game" ]; then
	for romfs in "$stubDir"/[Rr][Oo][Mm][Ff][Ss]; do
		if [ -d "$romfs" ] && [ ! -L "$romfs" ]; then
			die "nothing was sent: the stub has a romfs folder, which crashes the console in Luma's loader unless the title is a game built with Nintendo's SDK; for such a game, add --game; for your own app, take the folder out of the stub"
		fi
	done
fi

st=0
wirePutTree "$ip" "$port" "$stubDir" "/luma/titles/$titleID" "$overwrite" || st=$?
if [ "$st" != 0 ]; then
	[ "$wireSent" = 0 ] || echo "sync: title $titleID was not launched: not every file went" >&2
	exit "$st"
fi
[ "$wireSent" != 0 ] || exit 0
echo "sync: launching title $titleID" >&2
wireRun "$ip" "$port" "$titleID" ${game:+"$game"}

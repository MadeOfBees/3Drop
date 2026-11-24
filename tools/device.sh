#!/usr/bin/env bash
# device.sh <command> <ip> [args]: authenticated device commands. The key comes from THREEDROP_KEY
# or the gitignored .env; the port from THREEDROP_PORT (default 1337), or from an <ip> pasted as the
# console's top screen shows it, ip:port. SD paths may contain spaces.
# Exit 0 ok, 1 refused, 2 usage, 3 a benchmark run on this PC holds the console, 4 no answer,
# 124 timed out, 42 a send stopped under THREEDROP_GHOST_FLOOR,
# 141 a `get ... -` whose reader stopped early; a refusal goes to stderr.
#   stats [xfer|self|op]            the last transfer's measurements, this build's and AM's state,
#                                   or the last mutation's reply
#   ls <sd-path|title:[title-id]> [d|t|dt|r|rt]
#                                   entries; d the path itself, r the tree, t mtimes
#   ls <sd-dir>/<pattern> [t]       the names in it that match (quoted): `*` any, `?` one character,
#                                   in either ASCII case; get <sd-dir>/<pattern> <local-dir> r takes
#                                   what it names, each folder with its tree (never delete)
#   titles                          every installed SD application, patch and DLC (ls title:)
#   get <sd-path> <local-file>      copy a file from the SD card
#   put <local-file> <sd-path>      copy a file to the SD card
#                                   (a file over 32 MiB, or THREEDROP_PIECE's size, goes in pieces:
#                                   run it again to resume)
#                                   (ls, get, put and delete also take save:<title-id>/<path> and
#                                   extdata:<extdata-id>/<path>)
#   hash <sd-path> [local-file]     the console's digest of its file and its size, u:<hex> <size>,
#                                   read on the card; with a local file, 1 when that differs
#   get <sd-path> - [from <n>]      a file to stdout, from byte n, each piece once it checks out
#   put - <sd-path>                 stdin to a file, in pieces (again on the same stdin, it goes on
#                                   where it stopped)
#   get [--overwrite] <sd-dir> <local-dir> r
#   put [--overwrite] <local-dir> <sd-dir> r
#                                   a tree: each file changed since the last copy of it goes (the
#                                   console's by size and mtime, the PC's by SHA-256), replacing only
#                                   one the last copy left as it is, but with --overwrite; nothing is
#                                   deleted on either side, and run again it goes on where it stopped
#   delete <sd-path> [r]            delete a file; with r, a folder's whole tree
#   mkdir <sd-path>                 make a folder
#   rename <from> <to>              rename a file or a folder
#   put --system <local-file> <sd-path>
#                                   a file the console boots from, sent whole: one directly in
#                                   /luma/payloads or /luma/sysmodules, or /boot.firm, which the
#                                   console checks before it replaces the old one: some of
#                                   boot9strap's checks, not all (docs/safety.md says which)
#   delete --system <sd-path>       delete one in /luma/payloads or /luma/sysmodules
#   uninstall [--ticket] <title-id> delete a title (an application with its save data); --ticket
#                                   its ticket too, only one you created: the console cannot tell
#   seed <title-id> [seed-file]     give an installed application its seed, which a seeded game needs
#                                   to boot: from Nintendo's server, or a 16-byte file
#   screenshot <out-prefix>         both screens as BMP (and PNG where sips exists)
#   run <self|[title:]title-id>     relaunch 3Drop, or launch an installed application
#   run --game <title-id>           launch a game built with Nintendo's SDK that has a romfs folder
#   run <sd-path.3dsx> [stdio]      experimental: a 3dsx through Luma's loader; remote access ends
#                                   until 3Drop is relaunched; stdio: run `nc -l 17491` first, at an
#                                   address the console can reach: it dials the request's source,
#                                   which under WSL 2's default network is Windows, not the VM
#   restart                         run self, then wait for 3Drop to answer again
#   discover [seconds]              no <ip>: this key's console at each address that answered it
#                                   here before, then at its name 3drop-<tag>.local, then at each
#                                   console that answers a probe within the seconds (2; the name
#                                   and the probe need perl), as <ip> <port> <model> (- where no
#                                   probe said it), each proved by a signed stats
#   -h, --help                      this text
set -euo pipefail
. "$(dirname "$0")/lib.sh"
scratchTool
. "$ROOT/tools/wire.sh"

# screenshot <ip> <port> <out-prefix>: also makes PNGs where sips exists. They are made in the
# scratch directory and moved into place the way a get lands its file, since sips writes through a
# link. A link at any output is refused before the console is asked.
screenshot() {
	local shot
	for shot in "$3"-{top,bottom}.{bmp,png}; do wireMine "$shot" "a screenshot" || return; done
	wireScreenshot "$@" || return
	command -v sips >/dev/null 2>&1 || return 0
	for shot in top bottom; do
		if sips -s format png "$3-$shot.bmp" --out "$SCRATCH/$shot.png" >/dev/null 2>&1; then
			wirePlace "$SCRATCH/$shot.png" "$3-$shot.png" || return
		fi
	done
}

# restart <ip> <port>
restart() {
	wireRun "$1" "$2" self && wireWaitUp "$1" "$2" >/dev/null
}

# get <ip> <port> <sd-path> <local-file|-> [from <n>] and get <ip> <port> [--overwrite] <sd-dir>
# <local-dir> r: `-` is stdout.
get() {
	case "$3:$#:${5:-}:${6:-}" in
	--overwrite:6:*:r) wireGetTree "$1" "$2" "$4" "$5" overwrite ;;
	--overwrite:*) usage ;;
	*:4::) if [ "$4" = - ]; then wireGetPipe "$1" "$2" "$3"; else wireGet "$@"; fi ;;
	*:6:from:*)
		if [ "$4" != - ] || ! isUint "$6" || [ "${#6}" -gt 18 ]; then usage; fi
		wireGetPipe "$1" "$2" "$3" "$((10#$6))" # read as decimal, so a leading 0 isn't octal
		;;
	*:5:r:) wireGetTree "$1" "$2" "$3" "$4" ;;
	*) usage ;;
	esac
}

# put <ip> <port> [--system] <local-file|-> <sd-path>, put <ip> <port> [--overwrite] <local-dir> <sd-dir>
# r, delete <ip> <port> [--system] <sd-path> and delete <ip> <port> <sd-path> [r]: --system goes as
# the request's last word, and `-` is stdin.
put() {
	case "$3:$#:${5:-}:${6:-}" in
	--system:5:*) if [ "$4" = - ]; then wirePipePut "$1" "$2" "$5" system; else wireFilePut "$1" "$2" "$4" "$5" system; fi ;;
	--system:*) usage ;;
	--overwrite:6:*:r) wirePutTree "$1" "$2" "$4" "$5" overwrite ;;
	--overwrite:*) usage ;;
	*:4::) if [ "$3" = - ]; then wirePipePut "$1" "$2" "$4"; else wireFilePut "$@"; fi ;;
	*:5:r:) wirePutTree "$1" "$2" "$3" "$4" ;;
	*) usage ;;
	esac
}
delete() {
	case "$3:$#:${4:-}" in
	--system:4:title:*) usage ;; # the word belongs to paths; a title uses uninstall
	--system:4:*) wireDelete "$1" "$2" "$4" system ;;
	--system:*) usage ;;
	*:3:* | *:4:r) wireDelete "$@" ;;
	*) usage ;;
	esac
}

# uninstall <ip> <port> [--ticket] <title-id>
uninstall() {
	case "$3:$#" in
	--ticket:4) wireUninstall "$1" "$2" "$4" ticket ;;
	*:3) wireUninstall "$1" "$2" "$3" ;;
	*) usage ;;
	esac
}

# seed <ip> <port> <title-id> [seed-file]
seed() {
	local reply
	reply=$(wireSeed "$@") || return
	[ -n "$reply" ] || reply="$3 has no seed on Nintendo's server: nothing to import"
	echo "$reply"
}

# run <ip> <port> [--game] <self|[title:]title-id|sd-path.3dsx> [stdio]: --game goes as the request's
# last word, `game`.
run() {
	case "$3:$#" in
	--game:4) wireRun "$1" "$2" "$4" game ;;
	--game:*) usage ;;
	*) wireRun "$@" ;;
	esac
}

# <command> <fewest args> <most args> <function> [leading arguments]
COMMANDS='stats 0 1 wireRead stats
ls 1 2 wireLsMatch
titles 0 0 wireLs title:
get 2 4 get
hash 1 2 wireHash
put 2 4 put
delete 1 2 delete
mkdir 1 1 wireMkdir
rename 2 2 wireRename
uninstall 1 2 uninstall
seed 1 2 seed
screenshot 1 1 screenshot
run 1 2 run
restart 0 0 restart'

action="${1:-}"
if [ "$action" = discover ]; then
	if [ "$#" -gt 2 ] || { [ "$#" = 2 ] && ! isUint "$2"; }; then usage; fi
	requireKey
	wireDiscover "$THREEDROP_PORT" "${2:-}"
	exit
fi
[ -n "$action" ] || usage
# The loop stops on the action's row; a verb that has no row leaves every field empty.
while read -r verb fewest most fn leading && [ "$verb" != "$action" ]; do :; done <<<"$COMMANDS"
# A first word that is not a command, most often the <ip> put first, is named by itself, not under
# the usage.
[ -n "${fn:-}" ] || dieWith 2 "'$action' is not a command: the command comes first, as in device.sh stats <ip> self; device.sh --help lists them"
ip="${2:-}"
[ -n "$ip" ] || usage
shift 2
if [ "$#" -lt "$fewest" ] || [ "$#" -gt "$most" ]; then usage; fi
for arg in "$@"; do [ -n "$arg" ] || usage; done # an unset variable names nothing and is never a path
wireConsole "$ip"
# shellcheck disable=SC2086 # the leading arguments are one word or none
"$fn" "$wireIP" "$wirePort" $leading "$@"

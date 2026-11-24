#!/usr/bin/env bash
# drop.sh [--new|--update|--self] [--run] [--no-bump] [--no-seed] <file.cia> <ip> [port]
#   --new|--update|--self   the route; with none, what the console has installed picks it
#   --run                   launch the title once it is installed
#   --no-bump               refuse a CIA at or below the installed version; do not send it as installed + 1
#   --no-seed               do not ask Nintendo's server for the application's seed
#   -h, --help              this text

# A rejected or lost install is never retried as another route. A ghost (an install crawling below
# THREEDROP_GHOST_FLOOR bytes a second) is stopped. Once the console's receipt says it stopped that
# install, the install is sent again, once, with a new request id and nonce (THREEDROP_GHOST_RETRY=0
# exits 42 instead). A put refused on a spent nonce ran nothing, and is sent once more with the live
# nonce.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
scratchTool
. "$ROOT/tools/wire.sh"
: "${THREEDROP_GHOST_FLOOR:=800000}"
export THREEDROP_GHOST_FLOOR

intent=""
run=0
noBump=0
seed=1
batch=()
while :; do
	case "${1:-}" in
	-h | --help) usage 0 ;;
	--new | --update | --self) intent=${1#--} ;;
	--run) run=1 ;;
	--no-bump) noBump=1 ;;
	--no-seed) seed=0 ;;
	--batch) # <index> <total> <installed>: this item's position, which the MAC covers and the console checks
		[ "$#" -ge 4 ] || usage
		batch=("$2" "$3" "$4")
		shift 3
		;;
	-*) usage ;;
	*) break ;;
	esac
	shift
done
cia="${1:-}"
if [ -z "$cia" ] || [ -z "${2:-}" ]; then usage; fi
wireConsole "$2" "${3:-}"
ip=$wireIP port=$wirePort
[ -s "$cia" ] || die "'$cia' is missing or empty"

send="$cia"
ciaRead "$cia" || exit 1
titleID=$ciaTitle version=$ciaVer sendVersion=$ciaVer
if [ -z "$intent" ]; then
	self=$(selfTitleID)
	[ "$titleID" != "$self" ] || intent=self
fi
# A routed put that no answer settled (a lost reply, a kill) leaves a note of what it sent. If that
# version is installed now, that put landed, and sending it again would cost the whole body and bump
# the version past it. Any answered put of the title settles the note. With no cache dir, no note is
# kept. The key's tag is in the note's name, so another console answering at this address under
# another key is not a console that put went to, and a note from before the tag is never read. With
# no tag, no note is kept.
note="" installed="" landed="" routed=""
if [ -n "${WIRE_TAG:-}" ] && cache=$(cacheDir 2>/dev/null); then
	note=$cache/put-$WIRE_TAG-${ip//[^0-9A-Za-z.-]/_}-$port-$titleID
fi
# The installed version routes a CIA sent with no intent, and lets --no-bump refuse an update here,
# before its body is sent.
if [ -z "$intent" ] || [ "$intent:$noBump" = update:1 ]; then
	installed=$(wireTitleVersion "$ip" "$port" "$titleID") || dieWith $? "not sending $titleID"
fi
if [ -z "$intent" ] && [ -n "$note" ]; then
	routed=1
	build=$(unitDigest "$cia" "$ciaPrefix") || die "could not read '$cia'"
	if [ -f "$note" ] && read -r was wasVersion wasIntent <"$note" && [ "$was $wasVersion" = "$build $installed" ]; then
		landed=$wasIntent
		echo "drop: $titleID is installed at v$installed: the last run's install of this CIA landed, though no answer said so" >&2
	fi
	rm -f "$note"
fi
# An installed version of `-` means AM gives no version for the title, and the console refuses to
# install over it.
if [ -z "$landed" ] && [ -n "$installed" ] && [ "$installed" != - ]; then
	: "${intent:=update}"
	if [ "$version" -le "$installed" ]; then
		# A bumped patch or DLC would shadow the real release that carries that version.
		[ "${titleID:0:8}" = 00040000 ] || die "$titleID v$installed is installed and this CIA is v$version; a patch or DLC is never bumped, so uninstall it first to send this one"
		[ "$noBump" = 0 ] || die "$titleID v$installed is installed and this CIA is v$version; --no-bump leaves no route that the console accepts"
		sendVersion=$((installed + 1))
		# An uninstall takes an application's save with it.
		ciaValidVersion "$sendVersion" ||
			die "$titleID is installed at version $installed, so no newer update exists; uninstalling it deletes its save data, so first copy the save off (device.sh ls $ip:$port save:$titleID/ r, then device.sh get each file), then uninstall it with device.sh uninstall, then send it with drop.sh --new"
		send="$SCRATCH/bumped.cia"
		cp -- "$cia" "$send"
		chmod u+w "$send"
		ciaSetVersion "$send" "$sendVersion"
		echo "drop: $titleID v$installed is installed; sending v$sendVersion as an update" >&2
	fi
fi
: "${intent:=new}"

# A seeded application will not boot without its seed. The seed is fetched alongside the install,
# which holds AM for seconds, and read once the install commits. The fetch is stopped with this
# process, however it ends.
seedJob=""
if [ "$seed" = 1 ] && [ "$intent" != self ] && [ "${titleID:0:8}" = 00040000 ]; then
	seedFetch "$titleID" >"$SCRATCH/seed" 2>"$SCRATCH/seedSaid" &
	seedJob=$!
	watchOwner "$seedJob" : killTree "$seedJob"
fi

if [ -n "$landed" ]; then
	response="put committed ok 00000000 $titleID $installed $landed"
else
	[ -z "$routed" ] || { printf '%s %s %s\n' "$build" "$sendVersion" "$intent" >"$note"; } 2>/dev/null || :
	for attempt in 1 2; do
		rc=0
		response=$(wirePut "$ip" "$port" "$intent" "$send" "$ciaPrefix" ${batch[@]+"${batch[@]}"}) || rc=$?
		if [ "$rc" = 42 ] && [ "${THREEDROP_GHOST_RETRY:-1}" != 0 ] && [ "$attempt" = 1 ]; then
			echo "drop: ghost detected, so the install is sent again, once" >&2
			continue
		fi
		break
	done
	case "$rc" in 4 | 124) ;; *) [ -z "$note" ] || rm -f "$note" ;; esac
	[ "$rc" = 0 ] || exit "$rc"
fi
wireReport "$response" || exit
# A failure costs the boot, never the install. A fetch that fails (a missing curl too) says so
# itself, once.
if [ -n "$seedJob" ]; then
	st=0
	wait "$seedJob" || st=$?
	cat "$SCRATCH/seedSaid" >&2
	if [ "$st" = 0 ] && read -r seedHex <"$SCRATCH/seed" && [ -n "$seedHex" ]; then
		reply=$(wireSeedHex "$ip" "$port" "$titleID" "$seedHex") ||
			echo "drop: the seed was not imported, so $titleID may not boot; retry with tools/device.sh seed $ip:$port $titleID [seed-file]" >&2
		[ -z "$reply" ] || echo "${reply#"response "* }"
	fi
fi
# A self-update relaunches by itself; a patch or DLC launches its game.
if [ "$run" = 1 ] && [ "$intent" != self ]; then
	wireRun "$ip" "$port" "$(wireApplication "$titleID")" >/dev/null || dieWith $? "installed, but not launched"
fi

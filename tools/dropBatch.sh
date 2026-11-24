#!/usr/bin/env bash
# dropBatch.sh [--new|--update] [--run N] <ip> <cia...>
#   --new|--update   the route of every CIA; with none, what the console has installed picks each route
#   --run N          launch item N once every item has gone in
#   -h, --help       this text

# A title already installed at the same or a higher version is skipped, so re-running an interrupted batch
# sends only what is missing. --run N always sends item N, except a patch or DLC already installed at its
# version or newer. A console that a benchmark run holds, or one that answers no stats after an item it did
# not answer, ends the batch there. Item N launches only if every item succeeded, because launching leaves
# 3Drop and a failure must stay on its screen. The exit status is that of the first failure.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
scratchTool
. "$ROOT/tools/wire.sh"

intent=""
runIndex=0
while :; do
	case "${1:-}" in
	-h | --help) usage 0 ;;
	--new | --update) intent=$1 ;;
	--run)
		if ! isPositive "${2:-}"; then dieWith 2 "--run needs a 1-based item number"; fi
		runIndex=$((10#$2))
		shift
		;;
	-*) usage ;;
	*) break ;;
	esac
	shift
done
ip="${1:-}"
if [ -z "$ip" ] || [ "$#" -lt 2 ]; then usage; fi
shift
wireConsole "$ip"
ip=$wireIP port=$wirePort
files=("$@")
total=$#
[ "$runIndex" -le "$total" ] || dieWith 2 "--run $runIndex is outside 1..$total"
for f in "${files[@]}"; do
	[ -s "$f" ] || die "'$f' is missing or empty"
	ciaRead "$f" || exit 1
	title+=("$ciaTitle")
	ver+=("$ciaVer")
done
titles=$(wireLs "$ip" "$port" title:) || dieWith $? "could not list the installed titles"

failed=0
installed=0
position=0
# install <item>
install() {
	local file=${files[$(($1 - 1))]} itemTitle=${title[$1 - 1]} itemVer=${ver[$1 - 1]} have st
	position=$((position + 1))
	have=$(awk -v t="$itemTitle" '$4 == t { print $2 }' <<<"$titles")
	# `-` means AM gives no version for it, so it is not skipped (the console refuses `am-info` itself).
	if { [ "$1" != "$runIndex" ] || [[ "$itemTitle" == 0004000e* || "$itemTitle" == 0004008c* ]]; } && isUint "$have" && [ "$have" -ge "$itemVer" ]; then
		echo "[$position/$total] skip $file (v$have installed)"
		installed=$((installed + 1))
		return
	fi
	echo "[$position/$total] ${intent:-auto} $file"
	if "$ROOT/tools/drop.sh" ${intent:+"$intent"} --batch "$position" "$total" "$installed" "$file" "$ip" "$port"; then
		installed=$((installed + 1))
	else
		st=$?
		[ "$failed" != 0 ] || failed=$st
		# The rest would each wait out a console a benchmark run holds, or one that answers no stats now.
		if [ "$st" = 3 ] || { { [ "$st" = 4 ] || [ "$st" = 124 ]; } && ! wireAnswers "$ip" "$port"; }; then
			echo "dropBatch: the console is $([ "$st" = 3 ] && echo "held by a benchmark run on this PC" || echo "not answering"): the rest are not sent; run this again once it answers" >&2
			exit "$failed"
		fi
	fi
}
# Applications go first, then updates, then DLC, because their high words sort that way
# (00040000 < 0004000e < 0004008c).
order=$(for ((i = 1; i <= total; i++)); do
	echo "${title[i - 1]:0:8} $i"
done | sort -k1,1 -k2,2n | cut -d' ' -f2)
for i in $order; do
	install "$i"
done
if [ "$runIndex" != 0 ]; then
	if [ "$failed" != 0 ]; then
		echo "dropBatch: an item failed, so item $runIndex is not launched" >&2
	elif wireRun "$ip" "$port" "$(wireApplication "${title[runIndex - 1]}")" >/dev/null; then
		echo "dropBatch: launched item $runIndex; 3Drop is no longer listening."
	else
		failed=$? # the launch's refusal has already been reported
	fi
fi
exit "$failed"

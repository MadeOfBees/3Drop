#!/usr/bin/env bash
# reclaimTestTitles.sh <results.tsv> <ip> [--delete]: lists, or uninstalls with their tickets, the
# titles bench reps left. Only rows whose rep reached AM count: a rep refused before AM started
# installed nothing, so a title at its id is not ours, and deleting the wrong title's ticket can
# brick a console. Of those, only a title the console sizes as its row's fixture is ours: another
# client may have installed at the id since (a rep that never learned its outcome may have put
# nothing there; one that did, then uninstalled, left it empty), and such a title is left, listed
# with the command that removes it.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

results="${1:-}"
ip="${2:-}"
mode="${3:-list}"
if [ ! -f "$results" ] || [ -z "$ip" ]; then usage; fi
# Any other word would list, and summarize itself as if it had deleted.
case "$mode" in list | --delete) ;; *) usage ;; esac
# A file of another layout would be misread by column name, and list nothing as if nothing were left.
benchLayout "$results" || exit 1
benchConsole "$ip"
listing=$(wireLs "$ip" "$port" title:) || dieWith $? "could not list titles"
# Each installed title a row may have left (holdsTitle) is reported as `ours <title>` when the
# console's size for it (`t <version> <size> <title-id>`) is within a unit of every such row's
# size_bytes. That is the size of the CIA makeFixture.sh built: its contents and the CIA's own few
# KB around them. Fixtures of two sizes are a unit apart. Otherwise it is reported as `untied
# <title> <size> <size_bytes>`.
held=$(BENCH_LISTING=$listing awk -F'\t' -v unit="$WIRE_UNIT" "$BENCH_AWK"'
	BEGIN {
		n = split(ENVIRON["BENCH_LISTING"], line, "\n")
		for (i = 1; i <= n; i++) if (split(line[i], t, " ") == 4 && t[1] == "t") size[t[4]] = t[3]
	}
	NR == 1 { columns(); next }
	holdsTitle($col["outcome"]) && ($col["title"] in size) {
		id = $col["title"]
		b = fixture[id] = $col["size_bytes"]
		if (size[id] !~ /^[0-9]+$/ || b !~ /^[0-9]+$/ || size[id] - b >= unit || b - size[id] >= unit) untied[id] = 1
	}
	END { for (id in fixture) print (id in untied ? "untied" : "ours"), id, size[id], fixture[id] }' "$results" | sort -k2,2)
count=0 left=0 failed=0
while read -r kind title size bytes <&3; do
	case "$kind" in
	untied)
		count=$((count + 1)) left=$((left + 1))
		echo "untied: $title: the console's size for it, $size, is not its row's fixture's ($bytes bytes): left in place; if it is the bench's, tools/device.sh uninstall $ip:$port --ticket $title removes it"
		;;
	ours)
		count=$((count + 1))
		if [ "$mode" != --delete ]; then
			echo "ours: $title"
		elif ! wireUninstall "$ip" "$port" "$title" ticket; then
			# The uninstall said why above. It was refused, or its outcome is unknown, so the title
			# or its ticket may still be there, and a listing of titles alone would not show that
			# for the ticket.
			echo "reclaimTestTitles: $title: its removal is unconfirmed (see above): the title or its ticket may remain" >&2
			failed=$((failed + 1))
		fi
		;;
	esac
done 3<<<"$held"
echo "reclaimTestTitles: $count installed title(s) from $results ($mode), $left untied, $failed unconfirmed"
# A deletion that left a title behind, ours or untied, isn't a clean reclaim.
[ "$failed" = 0 ] && { [ "$mode" != --delete ] || [ "$left" = 0 ]; }

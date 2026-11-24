#!/usr/bin/env bash
# capture.sh <on|off|fetch> <ip> [outdir]: a bench build's capture-on-change (src/bench/capture.c).
#   on <ip>               arm it now and from every later boot (`bench capture on`, a set: it spends a nonce);
#                         refused when the arm file a later boot arms from is not on the card
#   fetch <ip> <outdir>   download every capture (PNG too where sips exists), deleting it on the device;
#                         refused, with nothing downloaded or deleted, when outdir already holds a file
#                         of any capture's name (a name is the boot's count and the screen's: the next
#                         boot makes the same ones)
#   off <ip>              disarm it (`bench capture off`); what its RAM slots hold is written first;
#                         refused when the arm file is still on the card
# Captures perturb throughput: a series run with them armed records capture=1 on every row.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
dir=/3ds/3drop/capture # src/bench/capture.c's CAPTURE_DIR, copied here because cDefine reads integers only
ip="${2:-}"

# captureNames <regex>: prints the names of the files in the capture folder that match, one per line,
# and nothing when the folder was never made.
captureNames() {
	local listing st=0
	listing=$(wireLs "$ip" "$port" "$dir" 2>&1) || {
		st=$?
		case "$listing" in
		*" ls rejected notfound "*) return 0 ;; # never armed, so nothing was captured
		*) dieWith "$st" "could not list $dir: $listing" ;;
		esac
	}
	printf '%s\n' "$listing" | re=$1 awk '$1 == "f" && $4 ~ ENVIRON["re"] { print $4 }'
}

case "${1:-}:${ip:+ip}" in
on:ip | off:ip) ;;
fetch:ip) [ -n "${3:-}" ] || usage ;;
*) usage ;;
esac
benchConsole "$ip" # sets ip and port
case "$1" in
on | off)
	st=0
	reply=$(wireMutation "$ip" "$port" "$(wireTimeout)" bench "capture $1" 2>&1) || st=$?
	if [ "$st" != 0 ]; then # a refusal is reported, but as not being a bench build where that is the cause
		benchBuild "$ip" "$port" "$reply"
		echo "$reply" >&2
		exit "$st"
	fi
	echo "$reply"
	benchOk "$reply" "capture capture=$([ "$1" = on ] && echo 1 || echo 0) " ||
		die "the console did not take capture $1"
	# The reply reports the console's RAM. A later boot arms from the arm file (capture.c). A card that
	# refused the write leaves that file unmade (on) or in place (off), so the capture then holds for
	# this boot only.
	armed=$(captureNames '^arm$')
	if [ -n "$armed" ]; then have=on; else have=off; fi
	[ "$have" = "$1" ] || die "capture $1 holds for this boot only: $dir/arm is $([ "$1" = on ] && echo not || echo still) on the card"
	;;
fetch)
	out=$3
	mkdir -p "$out"
	names=$(captureNames '\.bmp$')
	# sips (macOS) writes a PNG beside each capture, and that PNG is a file named after the capture too.
	png=0
	if command -v sips >/dev/null 2>&1; then png=1; fi
	# This runs before the first download because the get replaces a file of the same name, and the delete
	# after it leaves no other copy. Every conflict is named, so one move or delete clears them all.
	taken=""
	for name in $names; do
		files=("$out/$name")
		if [ "$png" = 1 ]; then files+=("$out/${name%.bmp}.png"); fi
		for file in "${files[@]}"; do
			if [ -e "$file" ] || [ -L "$file" ]; then taken="$taken ${file##*/}"; fi
		done
	done
	[ -z "$taken" ] || die "$out already holds$taken: move or delete them, or fetch into another folder; nothing was downloaded or deleted"
	fetched=0
	for name in $names; do
		wireGet "$ip" "$port" "$dir/$name" "$out/$name" >/dev/null
		if [ "$png" = 1 ]; then sips -s format png "$out/$name" --out "$out/${name%.bmp}.png" >/dev/null 2>&1 || true; fi
		wireDelete "$ip" "$port" "$dir/$name" >/dev/null
		fetched=$((fetched + 1))
	done
	echo "$TOOL: $fetched files in $out"
	;;
esac

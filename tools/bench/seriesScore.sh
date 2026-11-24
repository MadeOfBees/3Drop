#!/usr/bin/env bash
# seriesScore.sh <label> [results.tsv] [expected-reps] [ticket-gap-after]
# A failed rep ranks as the slowest, never as a missing one; void and skipped reps are counted,
# never ranked. The device's own rate leaves out the sender's work the host wall holds, and its median
# is over the committed reps alone, while the median beside it ranks the failures too. The ticket gap
# (am_tickets - am_titles) must hold still: any change is a ticket kept or lost (TICKET-GAP); the
# fourth argument, the gap after the last rep's uninstall (bench.sh's control read), checks that
# uninstall too. Each card floor prints as a rate; `after card floor` marks a rep whose first write a
# card floor lengthened, `after relaunch` rep 1, which follows the block's relaunch. Rows under
# different code, instrument, knobs or capture never pool: refused on stderr, exit 1. A block whose
# median is FAILED (half its reps or more failed), or that is short or pooled, exits 1 too.
# A label named like an option (--help, -h, --version) goes after `--`: seriesScore.sh -- --help.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
label="${1:-}"
[ -n "$label" ] || usage
requireLabel "$label" || exit
results="${2:-$BENCH_RESULTS}"
expected="${3:-0}"
after="${4:--}"
isUint "$expected" || dieWith 2 "expected-reps counts reps: not '$expected'"
[[ "$after" == - || "$after" =~ ^-?[0-9]+$ ]] || dieWith 2 "ticket-gap-after is a whole number: not '$after'"
[ -f "$results" ] || die "no $results"
benchLayout "$results" || exit 1

BENCH_LABEL=$label awk -F'\t' -v expected="$expected" -v after="$after" "$BENCH_AWK"'
BEGIN { label = ENVIRON["BENCH_LABEL"] } # never use -v, since awk reads a backslash in that as an escape
NR == 1 {
	columns()
	next
}
$col["series"] "" != label "" { next } # compared as text, so a label 1.10 is not 1.1
{
	n++
	lastRep = $col["rep"]
	stats = $col["stats"]
	out = $col["outcome"]
	class = rowClass(out, $col["wall_s"])
	# A committed rep with no wall time (a clock that stepped) is not a result, so it is counted as
	# void and never ranked.
	if (class == "void" && out ~ /^committed:/) out = "void:clock"
	ok = class == "ok"
	aside = class == "void" || class == "skip"
	bind = $col["ver"] " | " $col["inst"] " | " $col["knobs"] " | " $col["capture"]
	if (!aside && first == "") { first = bind; firstRep = $col["rep"] }
	else if (!aside && bind != first) mixed = mixed sprintf("  r%s  %s\n", $col["rep"], bind)
	# Record the ticket gap before this rep where its row has AM counts. A row without them is
	# passed over.
	if ($col["am_tickets"] ~ /^[0-9]+$/ && $col["am_titles"] ~ /^[0-9]+$/) {
		gap[++gaps] = $col["am_tickets"] - $col["am_titles"]
		gapRep[gaps] = $col["rep"]
		gapAt[gaps] = "rep " $col["rep"]
	}
	floorLine("before", $col["rep"], $col["card_before"])
	printf "r%-4s %-18s wall %9s  %8s MB/s  first_write_ms %6s  slow_mb %4s  worst_mb_ms %6s  stalls %3s  air %s%s\n",
		$col["rep"], out, $col["wall_s"], $col["rate_mbs"], kv(stats, "first_write_ms"), kv(stats, "slow_mb"),
		kv(stats, "worst_mb_ms"), kv(stats, "stall_n"), airText($col["air"]),
		($col["rep"] == 1 ? "  after relaunch" : $col["card_before"] != "-" ? "  after card floor" : "")
	floorLine("after", $col["rep"], $col["card_after"])
	if (class == "void") { voids++; next }
	if (class == "skip") { skips++; next }
	m++
	key[m] = ok ? $col["wall_s"] + 0 : FAILED
	wall[m] = $col["wall_s"]; rate[m] = $col["rate_mbs"]; bytes[m] = $col["size_bytes"]
	if (ok) {
		good++
		if (lo == "" || rate[m] + 0 < lo) lo = rate[m] + 0
		if (hi == "" || rate[m] + 0 > hi) hi = rate[m] + 0
		if (kv(stats, "total_ms") + 0 > 0) device[++devices] = mbps(bytes[m], kv(stats, "total_ms")) + 0
	} else {
		fails++
	}
}
function at(i) { return key[idx[i]] >= FAILED ? "FAILED" : sprintf("%s s (%s MB/s)", wall[idx[i]], rate[idx[i]]) }
# airText(air): returns the air probe as a rate only when it moved all its bytes (`ok ok`). A
# refused or short receive still names bytes and ms, but those are not an air rate, so it prints `-`
# and its words.
function airText(air,   w) {
	if (air ~ /^ok ok /) return mbps(kv(air, "bytes"), kv(air, "ms"))
	if (air == "-") return "-"
	split(air, w, " ")
	return "- (" w[1] " " w[2] ")"
}
# floorLine(when, rep, cell): prints a card floor as a rate over its whole write (preallocation,
# first write, steady writes, flush), only for a floor the console performed (`ok ok`). An older row
# may hold a refusal, which prints `-` and its words.
function floorLine(when, rep, cell,   w, ms) {
	if (cell == "-") return
	if (cell !~ /^ok ok /) {
		split(cell, w, " ")
		printf "card floor %s r%s  -  (%s %s)\n", when, rep, w[1], w[2]
		return
	}
	ms = kv(cell, "prealloc_ms") + kv(cell, "first_ms") + kv(cell, "steady_ms") + kv(cell, "flush_ms")
	printf "card floor %s r%s  %s MB/s\n", when, rep, mbps(kv(cell, "bytes"), ms)
}
END {
	if (mixed) {
		printf "%s: REFUSED to pool rows measured under another binding (ver | inst | knobs | capture):\n  r%s  %s\n%s", label, firstRep, first, mixed > "/dev/stderr"
		exit 1
	}
	if (expected > 0 && n != expected)
		printf "%s: scored %d of %d launched reps\n", (n < expected ? "MISSING" : "POOLED"), n, expected
	if (n == 0) { print "seriesScore: no rows for " label > "/dev/stderr"; exit 1 }
	if (m == 0) { printf "%s n=%d  no rep ran and counted (void %d, skipped %d)\n", label, n, voids, skips > "/dev/stderr"; exit 1 }
	order(key, m, idx)
	med = median(key, m, idx)
	h = int((m + 1) / 2)
	# The median is the middle wall time, or for an even count the mean of the middle pair, shown
	# with the rate at that wall time.
	medianText = med >= FAILED ? "FAILED" : med == key[idx[h]] ? at(h) : sprintf("%.2f s (%s MB/s)", med, mbps(bytes[idx[h]], med * 1000))
	for (i = 1; i <= m; i++) badRuns += bad(key[i], med)
	deviceText = "-"
	if (devices) { order(device, devices, deviceIdx); deviceText = sprintf("%.4f", median(device, devices, deviceIdx)) }
	printf "%s n=%d  median %s  p90 %s  fastest %s  device median %s MB/s  spread %s  bad runs %d  failed %d  void %d  skipped %d\n",
		label, n, medianText, at(int(m * 0.9 + 0.999)), at(1), deviceText,
		(good > 1 && lo > 0 ? sprintf("%.1f%%", (hi - lo) / lo * 100) : "-"), badRuns, fails + 0, voids + 0, skips + 0
	# The ticket gap runs from the first row with AM counts to the last, and on to the gap after the
	# uninstall of the last rep when bench.sh read one. A rep between two readings kept or lost a
	# ticket if the gap moved across it.
	if (after != "-") {
		gap[++gaps] = after + 0
		gapRep[gaps] = lastRep + 1
		gapAt[gaps] = "the block end"
	}
	if (gaps > 1) {
		last = gaps
		printf "ticket gap %+d over reps %d-%d", gap[last] - gap[1], gapRep[1], gapRep[last] - 1
		print (gap[last] == gap[1] ? "" : gap[last] > gap[1] ? "  TICKET-GAP: a rep left a ticket (the device log says ticket-kept)" : "  TICKET-GAP: a ticket went that no rep brought")
		# A net of zero can hide a ticket kept by one rep and lost by a later one, so this reports
		# the first step where the gap moved.
		for (i = 2; gap[last] == gap[1] && i <= last; i++) {
			prev = i - 1
			if (gap[i] != gap[prev]) {
				print "ticket gap moved " (gap[i] > gap[prev] ? "up" : "down") " between " gapAt[prev] " and " gapAt[i] " and was back by " gapAt[last] "  TICKET-GAP: the net of zero hides it"
				break
			}
		}
	}
	if (expected > 0 && n != expected) exit 1 # MISSING or POOLED means no clean block, whatever its score
	if (med >= FAILED) exit 1                 # half the reps or more failed, so there is nothing to compare
}' "$results"

#!/usr/bin/env bash
# The bench kit's rows and score: failures ranked slowest, void reps counted, never pooled across
# code, instrument, knobs or capture, the ticket gap, and uid prefixes within makerom's 20 bits.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
THREEDROP_KEY='test'
# shellcheck source=tools/bench/lib.sh
. "$ROOT/tools/bench/lib.sh"
results="$TMP/results.tsv"

# kv and the scorers' awk kv read a cell the same way: an unmeasured, missing or empty value reads as `-`,
# and a key never matches the end of another key (write_ms in first_write_ms).
stats="bytes=528579072 total_ms=330000 first_write_ms=2100 gate_ms=- ring_cap=41943040 stall_n=2 am_write_n=504 req=0011 empty= last=9"
for want in first_write_ms=2100 gate_ms=- am_write_n=504 req=0011 write_ms=- nosuch=- empty=- last=9; do
	key=${want%%=*}
	awkKv=$(awk -v t="$stats" -v k="$key" "$BENCH_AWK"'BEGIN { print kv(t, k) }')
	[ "$(kv "$stats" "$key"):$awkKv" = "${want#*=}:${want#*=}" ] || fail "kv read $key as '$(kv "$stats" "$key")', the awk kv as '$awkKv'"
done

# row <label> <rep> <outcome> <wall> [gap] [knobs] [air] [card_before] writes a row as bench.sh writes it,
# through benchRow. The device's total is the wall less 10 s of sender work, and gap is am_tickets -
# am_titles before the rep (`-` means no AM counts).
row() {
	local titles=- tickets=-
	# Check for `-` before the arithmetic, which cannot take it, or the row would go unwritten.
	if [ "${5:-}" != - ]; then
		titles=200 tickets=$((200 + ${5:-40}))
	fi
	benchRow "$results" series="$1" rep="$2" time="2026-09-25 00:00:00" uid="c000$2" title="$(uidTitleID "c000$2")" \
		size_bytes=528579072 wall_s="$4" rate_mbs="$(awk -v w="$4" 'BEGIN { printf "%.4f", 528.579072 / w }')" \
		outcome="$3" ver=64400 inst=0123456789abcdef capture=0 \
		knobs="${6:-rcvbuf=49152 sndbuf=65536 sendsize=65536 nohash=0 nodisc=0 capture=0 mb_rows=4096}" \
		am_titles="$titles" am_tickets="$tickets" free_bytes=1000000000 air="${7:-ok ok 00000000 rx bytes=16777216 ms=10000}" \
		card_before="${8:--}" card_after=- mb=- stats="bytes=528579072 total_ms=$(awk -v w="$4" 'BEGIN { printf "%d", (w - 10) * 1000 }') first_write_ms=2100 slow_mb=3 worst_mb_ms=2400 stall_n=2 am_write_n=504 req=00"
}
row a 1 committed:ok 330.00
row a 2 committed:ok 340.00
row a 3 committed:ok 331.00
row a 4 committed:ok 450.00
row a 5 noreply 900.00
row a 6 committed:ok 335.00 # the failed rep 5 dropped its ticket
row a 7 void:req 300.00     # fastest, but not what its row measured
row other 1 committed:ok 10.00

printf 'other\theader\n' >"$TMP/foreign.tsv"
tsvAppend "$TMP/foreign.tsv" x 2>/dev/null && fail "appended to a file with a different header"
# benchRow places each cell by its column's name, in the header's order, writing an empty one as `-`.
# A name that is not a column, a column named twice, or a column left out is refused, and nothing is
# written.
cells=()
for name in $BENCH_COLUMNS; do
	value=v-$name
	[ "$name" != mb ] || value=""
	cells=("$name=$value" ${cells[@]+"${cells[@]}"}) # the header's order reversed
done
benchRow "$TMP/byname.tsv" "${cells[@]}" || fail "benchRow refused a whole row"
for name in $BENCH_COLUMNS; do
	want=v-$name
	[ "$name" != mb ] || want=-
	expect "$(awk -F'\t' -v c="$name" "$BENCH_AWK"'NR == 1 { columns(); next } { print $col[c] }' "$TMP/byname.tsv")" "$want" "benchRow's $name cell"
done
run 1 benchRow "$TMP/refused.tsv" "${cells[@]}" nosuch=1
saidLine "$TOOL: a row has no column nosuch" "a cell of no column"
run 1 benchRow "$TMP/refused.tsv" "${cells[@]}" rep=2
saidLine "$TOOL: a row names twice the column rep" "a column named twice"
run 1 benchRow "$TMP/refused.tsv" "${cells[@]:1}"
saidLine "$TOOL: a row has no cell for mb" "a column left out"
[ ! -e "$TMP/refused.tsv" ] || fail "a refused row was written: $(cat "$TMP/refused.tsv")"

out=$("$ROOT/tools/bench/seriesScore.sh" a "$results" 7) || fail "scorer failed: $out"
grep -q '^r5 *noreply' <<<"$out" || fail "the failed rep has no row: $out"
grep -q '^r7 *void:req' <<<"$out" || fail "the void rep has no row: $out"
grep -q '^r1 .* first_write_ms   2100  slow_mb    3  worst_mb_ms   2400  stalls   2  air 1.6777  after relaunch$' <<<"$out" ||
	fail "a rep's row lost its first write, slow MBs, worst MB, stall count, air or relaunch mark: $out"
# Sorted walls of the six ranked: 330 331 335 340 450 FAILED -> median (335 + 340) / 2 = 337.5.
grep -q 'a n=7  median 337.50 s (1.5662 MB/s)' <<<"$out" || fail "wrong median: $out"
# p90 of six is the sixth: the failure, ranked slowest.
grep -q 'p90 FAILED' <<<"$out" || fail "a failure must rank as the slowest rep: $out"
grep -q 'fastest 330.00 s' <<<"$out" || fail "the void rep was ranked: $out"
# Device rates from total_ms 320 321 325 330 440 s: the median is 528.579072 / 325.
grep -q 'device median 1.6264 MB/s' <<<"$out" || fail "wrong device median: $out"
# The spread of the committed rates, 1.1746 (450 s) to 1.6018 (330 s): 36.4% of the lowest.
grep -q ' spread 36.4% ' <<<"$out" || fail "wrong spread: $out"
# 450 > 1.25 x 337.5 = 421.9, plus the failure.
grep -q 'bad runs 2  failed 1  void 1' <<<"$out" || fail "wrong bad-run, failed or void count: $out"
grep -q '^ticket gap +0 over reps 1-6$' <<<"$out" || fail "ticket gap: $out"
grep -q 'MISSING\|POOLED' <<<"$out" && fail "MISSING or POOLED with every rep present: $out"
# Neither is a clean block, so the score exits 1 (bench.sh's own status, which a series driver reads).
st=0
out=$("$ROOT/tools/bench/seriesScore.sh" a "$results" 8) || st=$?
expect "$st" 1 "the status of a block with a dropped rep"
grep -q 'MISSING: scored 7 of 8' <<<"$out" || fail "a dropped rep was not reported: $out"
st=0
out=$("$ROOT/tools/bench/seriesScore.sh" a "$results" 4) || st=$?
expect "$st" 1 "the status of a block with extra rows"
grep -q 'POOLED: scored 7 of 4' <<<"$out" || fail "extra rows were not reported: $out"

row gap 1 committed:ok 330.00 40
row gap 2 committed:ok 331.00 41 # one more ticket before rep 2, because rep 1 left one
out=$("$ROOT/tools/bench/seriesScore.sh" gap "$results")
grep -q 'ticket gap +1 over reps 1-1  TICKET-GAP: a rep left a ticket' <<<"$out" || fail "a ticket left behind: $out"
# Device rates 1.6518 and 1.6467 (total_ms 320 and 321 s): an even count's median is their mean.
grep -q 'gap n=2 .* device median 1.6492 MB/s ' <<<"$out" || fail "wrong even device median: $out"
# The air probe is a rate only for a receive that moved all its bytes. A short receive still names bytes
# and ms, which are not an air rate, so it prints `-` and its words.
row air 1 committed:ok 330.00 40 "" "rejected short d963f802 rx bytes=1000000 ms=1000 busy_ms=900 idle_ms=100 calls=15 eagain=0"
out=$("$ROOT/tools/bench/seriesScore.sh" air "$results") || fail "an arm with a short air receive: $out"
grep -q '^r1 .*  air - (rejected short)  after relaunch$' <<<"$out" || fail "a short air receive printed as a rate: $out"
# Each card floor prints as a rate over its whole write: 16777216 bytes in 0 + 40 + 900 + 1 ms (the
# fake's reply) is 17.8291 MB/s. The rep after a floor is marked, and rep 1 is marked after relaunch.
floor="ok ok 00000000 file bytes=16777216 chunk=1048576 writes=16 prealloc_ms=0 first_ms=40 steady_ms=900 steady_max_ms=70 flush_ms=1 bytes_done=16777216"
row floor 1 committed:ok 330.00 40 "" "" "$floor"
row floor 2 committed:ok 331.00 40 "" "" "$floor"
out=$("$ROOT/tools/bench/seriesScore.sh" floor "$results") || fail "an arm with card floors: $out"
[[ "$(grep -A1 '^card floor before r2 ' <<<"$out")" == "card floor before r2  17.8291 MB/s"$'\n'"r2 "*"  after card floor" ]] ||
	fail "a card floor was not printed as a rate before the rep it marks: $out"
grep -q '^card floor before r1  17.8291 MB/s$' <<<"$out" || fail "the block's first floor: $out"
grep -q '^r1 .*  after relaunch$' <<<"$out" || fail "rep 1 was not marked after relaunch: $out"
# p90 is by nearest rank: of 12 reps the 11th (311 s), not the slowest.
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do row p90 "$i" committed:ok "$((300 + i)).00"; done
out=$("$ROOT/tools/bench/seriesScore.sh" p90 "$results") || fail "a 12-rep arm: $out"
grep -q '^p90 n=12  median 306.50 s .*  p90 311.00 s ' <<<"$out" || fail "a 12-rep arm's p90: $out"
# The device median is a median of the device's rates (528.579072 MB over 100 s and over 400 s: 5.2858
# and 1.3214, so 3.3036), never the rate at the median time (250 s: 2.1143).
row dev 1 committed:ok 110.00
row dev 2 committed:ok 410.00
out=$("$ROOT/tools/bench/seriesScore.sh" dev "$results") || fail "an arm of two device rates: $out"
grep -q 'dev n=2 .* device median 3.3036 MB/s ' <<<"$out" || fail "the device median of 100 s and 400 s: $out"
# A gap that fell means a ticket is gone. A gap that fell and then rose is named at its first step. A
# first or last row with no AM counts is passed over and does not end the check. The gap after the last
# rep (bench.sh's fourth argument) checks that rep's uninstall.
row fell 1 committed:ok 330.00 41
row fell 2 committed:ok 331.00 40
row dip 1 committed:ok 330.00 40
row dip 2 committed:ok 331.00 39
row dip 3 committed:ok 332.00 40
row nocount 1 committed:ok 330.00 -
row nocount 2 committed:ok 331.00 40
row nocount 3 committed:ok 332.00 41
row nolast 1 committed:ok 330.00 40
row nolast 2 committed:ok 331.00 41
row nolast 3 committed:ok 332.00 -
amCounts() { # <label>: prints each row's rep and AM counts, as the score reads them
	awk -F'\t' -v l="$1" "$BENCH_AWK"'NR == 1 { columns(); next }
		$col["series"] == l { printf "%s%s:%s:%s", (n++ ? " " : ""), $col["rep"], $col["am_titles"], $col["am_tickets"] }' "$results"
}
expect "$(amCounts nocount):$(amCounts nolast)" "1:-:- 2:200:240 3:200:241:1:200:240 2:200:241 3:-:-" "the rows of the arms with a row of no AM counts"
for want in "fell:ticket gap -1 over reps 1-1  TICKET-GAP: a ticket went that no rep brought" \
	"dip:ticket gap moved down between rep 1 and rep 2 and was back by rep 3  TICKET-GAP: the net of zero hides it" \
	"nocount:ticket gap +1 over reps 2-2  TICKET-GAP: a rep left a ticket (the device log says ticket-kept)" \
	"nolast:ticket gap +1 over reps 1-1  TICKET-GAP: a rep left a ticket (the device log says ticket-kept)"; do
	out=$("$ROOT/tools/bench/seriesScore.sh" "${want%%:*}" "$results") || fail "the ${want%%:*} arm: $out"
	grep -qxF "${want#*:}" <<<"$out" || fail "the ${want%%:*} arm's ticket gap: $out"
done
out=$("$ROOT/tools/bench/seriesScore.sh" dip "$results" 3 41) || fail "the dip arm with the gap after it: $out"
grep -qxF "ticket gap +1 over reps 1-3  TICKET-GAP: a rep left a ticket (the device log says ticket-kept)" <<<"$out" ||
	fail "the gap after the last rep was not checked: $out"
# A block whose every rep failed still scores, and exits 1. A spread over a rate of 0 is reported as none,
# not as a division by zero. An expected count that is not a number is refused.
row allfail 1 noreply 900.00
row allfail 2 deadline 900.00
st=0
out=$("$ROOT/tools/bench/seriesScore.sh" allfail "$results" 2) || st=$?
expect "$st:$(grep -c '^allfail n=2  median FAILED ' <<<"$out")" "1:1" "the status of a block whose every rep failed"
row zero 1 committed:ok 330.00
row zero 2 committed:ok 9999999999.00 # 0.0000 MB/s
out=$("$ROOT/tools/bench/seriesScore.sh" zero "$results" 2>&1) || fail "an arm with a rate of 0: $out"
grep -q '^zero n=2 .*  spread -  ' <<<"$out" || fail "a spread over a rate of 0: $out"
run 2 "$ROOT/tools/bench/seriesScore.sh" zero "$results" abc
said "expected-reps counts reps: not 'abc'" "an expected count that is no number"
# With no file named, a score reads the file bench.sh writes: BENCH_RESULTS, else bench-results.tsv in the
# current directory.
mkdir "$TMP/here"
cp "$results" "$TMP/here/bench-results.tsv"
out=$(cd "$TMP/here" && env -u BENCH_RESULTS "$ROOT/tools/bench/seriesScore.sh" gap) || fail "a score of ./bench-results.tsv: $out"
grep -q 'gap n=2 ' <<<"$out" || fail "a score of ./bench-results.tsv: $out"
out=$(cd "$TMP" && BENCH_RESULTS="$results" "$ROOT/tools/bench/seriesScore.sh" gap) || fail "a score of BENCH_RESULTS: $out"
grep -q 'gap n=2 ' <<<"$out" || fail "a score of BENCH_RESULTS: $out"

# A skipped rep and a void one are counted, not ranked, and do not stop the arm pooling (the void
# row's knobs could not be read).
row aside 1 committed:ok 330.00
row aside 2 skipped:exists 1.00
row aside 3 void:knobs 300.00 40 "?"
row aside 4 rejected:space 2.00
row aside 5 committed:ok 331.00
out=$("$ROOT/tools/bench/seriesScore.sh" aside "$results") || fail "an arm with skipped and void reps: $out"
grep -q 'aside n=5 .* fastest 330.00 s .* failed 1  void 1  skipped 1$' <<<"$out" || fail "skipped and void counts: $out"
# Ranked: 330, 331 and the failure (the refusal before AM counts as failed). With an odd count, the
# median is the middle one.
grep -q 'aside n=5  median 331.00 s (1.5969 MB/s) ' <<<"$out" || fail "wrong odd median: $out"
grep -q '^ticket gap +0 over reps 1-4$' <<<"$out" || fail "ticket gap: $out"
# In an even count whose upper middle rep failed, half the reps or more failed. The median is then
# FAILED, never the lower middle rep's wall, and only the failed reps count as bad.
row half 1 committed:ok 330.00
row half 2 committed:ok 331.00
row half 3 noreply 900.00
row half 4 deadline 900.00
row pair 1 committed:ok 330.00
row pair 2 noreply 900.00
for arm in "half n=4  median FAILED  p90 FAILED  fastest 330.00 s .* bad runs 2  failed 2 " \
	"pair n=2  median FAILED  p90 FAILED  fastest 330.00 s .* bad runs 1  failed 1 "; do
	st=0
	out=$("$ROOT/tools/bench/seriesScore.sh" "${arm%% *}" "$results") || st=$?
	expect "$st" 1 "the status of an arm whose median FAILED" # no result to compare
	grep -q "^$arm" <<<"$out" || fail "an arm half failed: $out"
done

# Rows measured under another build (ver), host instrument (inst), knob setting or capture state are
# never pooled: an arm that holds rows under two builds, two instruments, two knob settings or two
# capture states refuses to score.
for binding in ver:64401 inst:fedcba9876543210 capture:1 \
	"knobs:rcvbuf=32768 sndbuf=65536 sendsize=65536 nohash=0 nodisc=0 capture=0 mb_rows=4096"; do
	column=${binding%%:*}
	row "mix-$column" 1 committed:ok 330.00
	row "mix-$column" 2 committed:ok 331.00
	awk -F'\t' -v OFS='\t' -v l="mix-$column" -v c="$column" -v v="${binding#*:}" "$BENCH_AWK"'
		NR == 1 { columns() }
		NR > 1 && $col["series"] == l && $col["rep"] == 2 { $col[c] = v }
		{ print }' "$results" >"$results.new"
	mv "$results.new" "$results"
	st=0
	out=$("$ROOT/tools/bench/seriesScore.sh" "mix-$column" "$results" 2>&1 >/dev/null) || st=$?
	if [ "$st" != 1 ] || ! grep -q 'REFUSED to pool' <<<"$out"; then fail "rows under two $column values were pooled (exit $st): $out"; fi
done
# A file of another column layout is refused: its rows would be misread.
printf 'series\trep\toutcome\twall_s\na\t1\tcommitted:ok\t330.00\n' >"$TMP/old.tsv"
st=0
out=$("$ROOT/tools/bench/seriesScore.sh" a "$TMP/old.tsv" 2>&1 >/dev/null) || st=$?
[[ "$st" = 1 && "$out" == *"has another column layout"* ]] || fail "scored a file of another column layout (exit $st): $out"

bench="$ROOT/tools/bench/bench.sh"
# bench.sh asks macQuiet (bench/lib.sh) whether this PC is quiet before it asks for a console. A
# container, or the tools/check.sh that runs this test (which CI runs), would make bench.sh refuse the
# calls below, so docker and ps are stand-ins that see nothing, put ahead of the host's own.
mkdir "$TMP/quiet"
for tool in docker ps; do printf '#!/bin/sh\n' >"$TMP/quiet/$tool" && chmod +x "$TMP/quiet/$tool"; done
PATH="$TMP/quiet:$PATH"
[ "$("$bench" --prefix-only "$TMP/none.tsv")" = c000 ] || fail "an empty results file did not yield the seed"
printf 'series\tuid\n' >"$TMP/p.tsv"
for uid in c0001 c0002 c001f c0021; do
	printf 'precln\t%s\n' "$uid" >>"$TMP/p.tsv"
done
[ "$("$bench" --prefix-only "$TMP/p.tsv")" = c003 ] || fail "expected the first unused prefix c003"
# A label with rows is refused before anything else, so two series never pool.
inUse() { # <label> <results>: returns 0 when bench.sh refused the label as one with rows
	local out
	out=$(BENCH_RESULTS=$2 "$bench" "$1" 2>&1) && fail "bench.sh $1 ran"
	[[ "$out" == *"label '$1' already has rows"* ]]
}
inUse precln "$TMP/p.tsv" || fail "a label with rows was not refused"
! inUse postcln "$TMP/p.tsv" || fail "an unused label was refused"
# A label is text: 1.10 is not 1.1, in the label check, the score and the in-run slow test alike.
row 1.1 1 committed:ok 330.00
row 1.1 2 committed:ok 331.00
row 1.10 1 committed:ok 900.00
! inUse 1.100 "$results" || fail "a label equal to 1.1 only as a number was refused"
out=$("$ROOT/tools/bench/seriesScore.sh" 1.10 "$results") || fail "scoring the label 1.10: $out"
grep -q '^1.10 n=1  median 900.00 s ' <<<"$out" || fail "the label 1.10 scored 1.1's rows: $out"
! repSlow "$results" 1.10 2000.00 || fail "repSlow read 1.1's rows for the label 1.10"
# A long ledger's first row is still seen: a grep -q in a pipeline SIGPIPEs under pipefail.
awk 'BEGIN { printf "series\trep\ttime\tuid\n"; for (i = 0; i < 8000; i++) printf "lbl%05d\t1\tx\t%04x1\n", i + 1, 49152 + i }' >"$TMP/long.tsv"
[ "$("$bench" --prefix-only "$TMP/long.tsv")" = df40 ] || fail "a long ledger's used prefixes were missed"
inUse lbl00001 "$TMP/long.tsv" || fail "a long ledger's first label was taken for unused"
# Every rep's uid fits makerom's 20 bits, so an arm of 16 reps is a call bench.sh cannot take (2),
# refused before anything runs, and one of 15 goes on to the address check. A size is a decimal,
# checked before the arithmetic, which would evaluate a name. No address is given, and none is remembered
# for this key (no answer, 4), so a check that let either through still reaches no console. These are
# stand-ins for makeFixture.sh's tools, which bench.sh finds before the console and never runs here.
anyTool=$(type -P true)
wide() {
	XDG_CACHE_HOME=$TMP/cache THREEDROP_KEY=test THREEDROP_PROBE='' BENCH_RESULTS="$TMP/wide.tsv" \
		MAKEROM=$anyTool BANNERTOOL=$anyTool THREEDSTOOL=$anyTool "$bench" wide "$@" 2>&1
}
for reps in 16 15; do
	st=0
	out=$(wide "$reps") || st=$?
	case "$reps:$st:$out" in
	16:2:*"at most 15 reps"* | 15:4:*"use the address on the console's top screen"*) ;;
	*) fail "an arm of $reps reps exited $st: $out" ;;
	esac
done
st=0
out=$(wide 1 "x[\$(touch $TMP/ran)]") || st=$?
[[ "$st" = 2 && "$out" == *"1048576-byte units"* && ! -e "$TMP/ran" ]] || fail "a size that is no decimal exited $st: $out"
# An arm with no rep that counted is refused, on stderr.
row nothing 1 void:req 300.00
st=0
out=$("$ROOT/tools/bench/seriesScore.sh" nothing "$results" 2>&1 >/dev/null) || st=$?
[[ "$st" = 1 && "$out" == *"no rep ran and counted"* ]] || fail "an arm with no rep counted (exit $st): $out"
echo "testSeriesScore: PASS"

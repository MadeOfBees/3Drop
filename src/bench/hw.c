// SPDX-License-Identifier: GPL-3.0-or-later
// bench hw lists a timed request's ring calls and wire gaps on one clock, so a stall shows beside
// the call that came before it.
#include "bench.h"

#include "cmdFs.h" // argIdle
#include "hash.h"  // DIGEST_UNIT, digestUnits
#include "stats.h" // gStats.request

#include <3ds.h>
#include <stdio.h>
#include <sys/param.h> // MIN

// Memory only, so a record makes no call (no log, no FS, no IPC). The two arrays, together with
// bench.c's tables, stay under check.sh's .bss bound while holding a 1073741824-byte put's digest
// calls and writes, and a gap of 30 ms or more at each of a 104857600-byte put's ~2400 receives.
#define HW_CALLS 2048
#define HW_GAPS 3072
// Low enough to show a fast retransmit, which STATS_STALL_MS (the gap `stats` counts as a stall)
// hides.
#define HW_GAP_MIN_MS 30
#define HW_GAP_MIN_TICKS (HW_GAP_MIN_MS * (SYSCLOCK_ARM11 / 1000))

// Times are ticks / 256 since the request's start. A step is 0.95 us, the range is 68 minutes, and
// recording needs no divide.
typedef struct {
	u32 start, end;
	u8 k; // the number of units in the ring call (a write or a read) it serves
	char kind;
} HwCall;
typedef struct {
	u32 quiet, call, end; // the wire went quiet, then the call that ended it ran from call to end
} HwGap;

// Two writers, one array each: the worker records calls and main records gaps. Main also resets
// both (benchTimelineReset, from statsReset). That is safe only because every timed verb refuses a
// stranded worker (argIdle) before statsStart, so no worker is recording then.
static struct {
	u64 base;
	u32 calls, gaps; // counted past the arrays too
	HwCall call[HW_CALLS];
	HwGap gap[HW_GAPS];
} hw;

static u32 hwAt(u64 tick) {
	return (u32)((tick - hw.base) >> 8);
}

static unsigned long hwUs(u32 at) {
	return (unsigned long)(((u64)at << 8) * 1000000 / SYSCLOCK_ARM11);
}

void benchTimelineReset(void) {
	hw.base = svcGetSystemTick();
	hw.calls = hw.gaps = 0;
}

void benchCall(CallKind kind, u64 start, u64 end, u32 k) {
	u32 i = hw.calls++;
	if (i < HW_CALLS) {
		hw.call[i] = (HwCall){hwAt(start), hwAt(end), (u8)k, (char)kind};
	}
}

void benchGap(u64 quiet, const TickCounter *call) {
	if (call->reference - quiet < HW_GAP_MIN_TICKS) {
		return;
	}
	u32 i = hw.gaps++;
	if (i < HW_GAPS) {
		u64 start = call->reference - call->elapsed;
		hw.gap[i] = (HwGap){hwAt(quiet), hwAt(start), hwAt(call->reference)};
	}
}

// Makes the calls digestUnits makes over a write, one unit at a time, so each is timed.
Result benchDigestUnits(const u8 *p, u32 n) {
	u32 k = (n + DIGEST_UNIT - 1) / DIGEST_UNIT;
	Result r = 0;
	for (u32 at = 0; at < n && R_SUCCEEDED(r); at += DIGEST_UNIT) {
		u64 start = svcGetSystemTick();
		r = digestUnits(p + at, MIN(n - at, DIGEST_UNIT));
		benchCall(CALL_DIGEST, start, svcGetSystemTick(), k);
	}
	return r;
}

// Lists the last timed request's calls, then its gaps. rows counts the lines sent, and calls and
// gaps count what was recorded, so a full array shows.
void benchHw(VerbContext *c) {
	if (!argIdle(c)) { // the listing's digest is taken in tier 1
		return;
	}
	u32 calls = MIN(hw.calls, (u32)HW_CALLS), gaps = MIN(hw.gaps, (u32)HW_GAPS);
	listingBegin(c,
				 " hw req=%s rows=%lu calls=%lu gaps=%lu gap_min_ms=%d "
				 "call_cols=kind,start_us,end_us,k gap_cols=g,quiet_us,call_us,end_us",
				 gStats.request[0] ? gStats.request : "-", (unsigned long)(calls + gaps),
				 (unsigned long)hw.calls, (unsigned long)hw.gaps, HW_GAP_MIN_MS);
	for (u32 i = 0; i < calls; i++) {
		const HwCall *h = &hw.call[i];
		char line[48];
		int len = snprintf(line, sizeof(line), "%c %lu %lu %u\n", h->kind, hwUs(h->start),
						   hwUs(h->end), (unsigned)h->k);
		listingLine(line, (size_t)len);
	}
	for (u32 i = 0; i < gaps; i++) {
		const HwGap *g = &hw.gap[i];
		char line[48];
		int len = snprintf(line, sizeof(line), "g %lu %lu %lu\n", hwUs(g->quiet), hwUs(g->call),
						   hwUs(g->end));
		listingLine(line, (size_t)len);
	}
	listingEnd(OUTCOME_COMMITTED, "ok", 0);
}

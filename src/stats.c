// SPDX-License-Identifier: GPL-3.0-or-later
// What `stats` reports (stats.h): the counters a transfer fills in as it runs (phases, AM and SD
// calls, wire stalls, the slow megabytes), and the code that prints them as text in a fixed order.
#include "stats.h"

#include "app.h"
#include "codec.h" // appendf
#include "knobs.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

InstallStats gStats;

void statsStart(VerbContext *c) {
	appTransferStart();
	statsReset(c->id);
	c->timed = true;
}

void statsReset(const char *request) {
	memset(&gStats, 0, sizeof(gStats));
	snprintf(gStats.request, sizeof(gStats.request), "%s", request);
	gStats.startMs = uptimeMs();
	BENCH(benchTimelineReset());
}

void statsFinish(void) {
	gStats.totalMs = uptimeMs() - gStats.startMs;
	gStats.totalKnown = true;
}

void phaseRecord(PhaseKind kind, double ms) {
	gStats.phaseUs[kind] += (u64)(ms * 1000.0);
	gStats.phaseN[kind]++;
}

void statsRing(const RingStats *w, u64 total, PhaseKind io) {
	// The join just ended, because ringBodyEnd calls this right after it.
	gStats.joinEndMs = uptimeMs();
	gStats.phaseUs[io] += w->ioUs;
	gStats.phaseN[io] += w->ios;
	gStats.phaseUs[PHASE_RING_WAIT] += w->waitUs;
	gStats.phaseN[PHASE_RING_WAIT] += w->waits;
	gStats.phaseUs[PHASE_HASH] += w->hashUs;
	gStats.phaseN[PHASE_HASH] += w->hashes;
	if (io == PHASE_AM_WRITE) {
		gStats.firstWriteMs = w->firstIoMs;
		gStats.amWriteMaxMs = w->maxIoMs;
		gStats.amWriteMaxKnown = w->laterIo;
	} else {
		gStats.firstSdIoMs = w->firstIoMs;
		gStats.sdIoMaxMs = w->maxIoMs;
		gStats.sdIoMaxKnown = w->laterIo;
	}
	gStats.ringCap = w->cap;
	gStats.ringHigh = w->high;
	gStats.join = w->failure == 0 && w->written == total ? STATS_JOIN_OK : STATS_JOIN_FAILED;
}

void statsMegabyte(u64 nowMs) {
	u64 since = gStats.mbCount == 0 ? gStats.firstByteMs : gStats.lastMbMs;
	u32 ms = nowMs > since ? (u32)(nowMs - since) : 0;
	if (ms >= STATS_SLOW_MB_MS) {
		gStats.slowMb++;
	}
	if (ms > gStats.worstMbMs) {
		gStats.worstMbMs = ms;
	}
	gStats.lastMbMs = nowMs;
	gStats.mbCount++;
	BENCH(benchMegabyte(nowMs));
}

void statsWireGap(u64 gapMs) {
	if (gapMs < STATS_STALL_MS) {
		return;
	}
	gStats.stallN++;
	gStats.stallMs += gapMs;
	if (gapMs > gStats.stallMaxMs) {
		gStats.stallMaxMs = (u32)gapMs;
	}
}

static bool field(char *out, size_t size, size_t *len, const char *key, u64 value, bool known) {
	int n = known ? snprintf(out + *len, size - *len, " %s=%" PRIu64, key, value)
				  : snprintf(out + *len, size - *len, " %s=-", key);
	if (n < 0 || (size_t)n >= size - *len) {
		return false;
	}
	*len += (size_t)n;
	return true;
}

static const char *const PHASE_NAMES[PHASE_KIND_COUNT] = {
	"recv",		 "recv_idle",  "sink",		"ring_acquire", "status",	"progress",
	"poll",		 "prefix_gap", "am_start",	"ring_wait",	"am_write", "drain_join",
	"am_finish", "send",	   "send_idle", "sd_io",		"hash",
};

bool statsFormat(char *out, size_t size) {
	out[0] = '\0';
	const InstallStats *s = &gStats;
	size_t len = 0;
	bool ring = s->ringCap != 0;
	// The first call's time is measured once the call has been made, even at 0 ms. The SD calls
	// count the hash's reads too, and those are not the ring's calls.
	bool amWrote = s->phaseN[PHASE_AM_WRITE] != 0;
	bool sdIo = ring && s->phaseN[PHASE_SD_IO] != 0;
	// One space query sets sd_needed, which is never 0, and sd_free_before, which can be.
	bool spaceAsked = s->sdNeeded != 0;
	bool ok =
		field(out, size, &len, "bytes", gApp.wireBytes, true) &&
		field(out, size, &len, "total_ms", s->totalMs, s->totalKnown) &&
		field(out, size, &len, "first_write_ms", s->firstWriteMs, amWrote) &&
		field(out, size, &len, "ring_cap", s->ringCap, ring) &&
		field(out, size, &len, "ring_high", s->ringHigh, ring) &&
		field(out, size, &len, "join", s->join == STATS_JOIN_OK, s->join != STATS_JOIN_NONE) &&
		field(out, size, &len, "am_write_max_ms", s->amWriteMaxMs, s->amWriteMaxKnown) &&
		field(out, size, &len, "first_sd_io_ms", s->firstSdIoMs, sdIo) &&
		field(out, size, &len, "sd_io_max_ms", s->sdIoMaxMs, s->sdIoMaxKnown) &&
		field(out, size, &len, "mb", s->mbCount, true) &&
		field(out, size, &len, "slow_mb", s->slowMb, s->mbCount != 0) &&
		field(out, size, &len, "worst_mb_ms", s->worstMbMs, s->mbCount != 0) &&
		field(out, size, &len, "stall_n", s->stallN, true) &&
		field(out, size, &len, "stall_ms", s->stallMs, true) &&
		field(out, size, &len, "stall_max_ms", s->stallMaxMs, s->stallN != 0) &&
		field(out, size, &len, "sd_needed", s->sdNeeded, spaceAsked) &&
		field(out, size, &len, "sd_free_before", s->sdFreeBefore, spaceAsked) &&
		field(out, size, &len, "sd_free_after", s->sdFreeAfter, s->sdFreeAfterKnown);
	for (int k = 0; ok && k < PHASE_KIND_COUNT; k++) {
		char key[32];
		snprintf(key, sizeof(key), "%s_us", PHASE_NAMES[k]);
		ok = field(out, size, &len, key, s->phaseUs[k], s->phaseN[k] != 0);
		snprintf(key, sizeof(key), "%s_n", PHASE_NAMES[k]);
		ok = ok && field(out, size, &len, key, s->phaseN[k], true);
	}
	// req comes last, so a probe that ran between an install and its read shows.
	return ok && appendf(out, size, " req=%s", s->request[0] ? s->request : "-");
}

// The body ends where the ring's join began, so what comes before the body, the body, the join and
// the rest (the digest check, flush, close and publish; AM's finish) add up to total_ms, because
// each is the gap between marks the transfer already read, and the join's own time is its phase.
// `-` marks what was not measured: no body began, or no ring was joined. The uptime dates the line
// against `stats self`'s up_ms.
bool statsLogTail(char *out, size_t size, bool timed) {
	const InstallStats *s = &gStats;
	size_t len = 0;
	u64 join = s->phaseUs[PHASE_DRAIN_JOIN] / 1000;
	u64 pre = s->firstByteMs - s->startMs, body = s->joinEndMs - join - s->firstByteMs;
	u64 post = s->startMs + s->totalMs - s->joinEndMs;
	bool begun = s->firstByteMs != 0, joined = s->joinEndMs != 0;
	out[0] = '\0';
	return (!timed || (field(out, size, &len, "total_ms", s->totalMs, s->totalKnown) &&
					   field(out, size, &len, "pre_ms", pre, begun) &&
					   field(out, size, &len, "body_ms", body, begun && joined) &&
					   field(out, size, &len, "join_ms", join, joined) &&
					   field(out, size, &len, "post_ms", post, joined && s->totalKnown))) &&
		   field(out, size, &len, "up_s", uptimeMs() / 1000, true);
}

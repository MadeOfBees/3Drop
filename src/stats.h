// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef STATS_H
#define STATS_H

#include "protocol.h" // VerbContext

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// `stats` prints these in order, but hosts read them by name, never by position.
typedef enum {
	PHASE_RECV,
	PHASE_RECV_IDLE,	// EAGAIN plus the backoff
	PHASE_SINK,			// the ring commit, a sink, or a send's source
	PHASE_RING_ACQUIRE, // the pump waited on the ring
	PHASE_STATUS,
	PHASE_PROGRESS,
	PHASE_POLL,
	PHASE_PREFIX_GAP, // the socket is silent during checks, gates and AM start
	PHASE_AM_START,
	PHASE_RING_WAIT, // the worker had nothing to do
	PHASE_AM_WRITE,
	PHASE_DRAIN_JOIN,
	PHASE_AM_FINISH,
	PHASE_SEND,
	PHASE_SEND_IDLE, // EAGAIN plus the backoff
	PHASE_SD_IO,
	PHASE_HASH, // the ring's digest calls: one per unit, one for the finish
	PHASE_KIND_COUNT,
} PhaseKind;

typedef enum {
	STATS_JOIN_NONE = 0, // no ring worker ran
	STATS_JOIN_OK,
	STATS_JOIN_FAILED,
} StatsJoin;

// `stats` prints what was not measured as `-`. Mostly that is a zero, because a real value cannot
// be zero. Where one can be (a time in whole ms, the SD card's free bytes), what measured it says
// so: for a ring's first call, the calls counted; for its maximum after the first and the request's
// total, a Known flag; for the free bytes, the space query that read them (sd_needed is never 0).
typedef struct {
	char request[33];
	u64 startMs;
	u64 totalMs;
	u64 firstByteMs;
	// The uptime when the ring's join ended (statsRing); 0 means no ring was joined.
	u64 joinEndMs;
	u32 firstWriteMs; // kept separate from amWriteMaxMs because it allocates the title's clusters
	u32 amWriteMaxMs; // after the first
	u32 firstSdIoMs;  // the SD ring's counterparts of those: reads for a get, writes for a put
	u32 sdIoMaxMs;
	u64 ringCap; // bytes; 0 = no ring
	u64 ringHigh;
	StatsJoin join;
	u32 mbCount; // decimal megabytes
	u32 slowMb;
	u32 worstMbMs;
	u64 lastMbMs;
	// Time the pump spent waiting on the ring is not a wire stall.
	u32 stallN;
	u64 stallMs;
	u32 stallMaxMs;
	u64 sdNeeded;
	u64 sdFreeBefore;
	u64 sdFreeAfter;
	u64 phaseUs[PHASE_KIND_COUNT];
	u32 phaseN[PHASE_KIND_COUNT];
	// Set when a call after the first was made (so its maximum is measured, even at 0), when the
	// space query after the install succeeded, and when statsFinish ran (so totalMs is measured,
	// even at 0).
	bool amWriteMaxKnown, sdIoMaxKnown, sdFreeAfterKnown, totalKnown;
} InstallStats;

typedef struct {
	Result failure;
	Result hashFailure;
	u64 written, cap, high;
	u64 ioUs, waitUs;
	u32 ios, waits;
	u32 firstIoMs; // kept separate from maxIoMs because it allocates a new file's clusters
	u32 maxIoMs;
	// Set when a call after the first was made, so maxIoMs is measured, even at 0.
	bool laterIo;
	u64 hashUs;
	u32 hashes;
	u8 digest[32];
} RingStats;

#define STATS_SLOW_MB_MS 2000u
#define STATS_STALL_MS 100u

extern InstallStats gStats;

// A transfer's first step, taken before any reply, so no counter or stat can belong to an earlier
// request.
void statsStart(VerbContext *c);
void statsReset(const char *request);
void statsFinish(void);
void phaseRecord(PhaseKind kind, double ms); // main thread only
// Closes the bracket osTickCounterStart opened on tick: its time goes to kind.
static inline void phaseSince(PhaseKind kind, TickCounter *tick) {
	osTickCounterUpdate(tick);
	phaseRecord(kind, osTickCounterRead(tick));
}
void statsRing(const RingStats *w, u64 total, PhaseKind io);
void statsMegabyte(u64 nowMs);
void statsWireGap(u64 gapMs);

#define STATS_TEXT_MAX 1408 // every field at its widest
bool statsFormat(char *out, size_t size);

// What a request's log line adds after its bytes (protocol.c): a transfer's total_ms and where it
// went, then the uptime in whole seconds. A request that moved no bytes (`timed` false) has only
// the uptime.
#define STATS_TAIL_MAX 192 // every field at its widest
bool statsLogTail(char *out, size_t size, bool timed);

#endif // STATS_H

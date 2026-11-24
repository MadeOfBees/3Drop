// SPDX-License-Identifier: GPL-3.0-or-later
#include "ring.h"

#include "fs.h" // appendLog
#include "hash.h"
#include "knobs.h" // BENCH
#include "result.h"

#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h> // MIN, MAX

_Static_assert(RING_UNIT % DIGEST_UNIT == 0, "a worker call hashes whole digest units");
// AM's stalls attach to calls, so fewer, larger writes cost less per byte; a read stays one unit,
// so the first bytes go out sooner.
#define WRITE_UNITS 4
// Core 0, like every thread, because the FS call yields it and a core 1 reservation takes CPU from
// SOC. The writer runs above the receive loop and a reader runs below the send loop, reading ahead
// of it.
#define WRITER_PRIORITY 0x18
#define READER_PRIORITY 0x31
// Past this a worker is stuck, not slow. A healthy first AM write has taken 178 s.
#define WORKER_STALL_MS 600000u
// Regular heap behind tier 1, so the wire keeps going through AM's first-write hold.
#define DRAIN_RING2_UNITS 16

static struct {
	u8 *tier[2];
	u32 tierUnits[2], units;
	RingDir dir;
	u64 offset; // the handle's byte offset of the body's first byte
	u64 total, pumped, done, high;
	LightSemaphore forPump, forWorker;
	bool unitOpen, digest;
	volatile bool parked, busy, failed, stop;
	volatile u32 progressMs; // wraps
	Handle target;
	Thread thread;
	void (*abandon)(const RingStats *stats);
	RingStats stats; // the worker's; it is read only after a DONE join
	u64 early;		 // the first write's early call ends here; 0 for none
	bool earlyIn;	 // the pump has passed `early`
} ring;
static bool stranded;

static u32 nowMs(void) {
	return (u32)uptimeMs();
}

// No unit straddles a tier.
static u8 *ringAt(u64 pos) {
	u32 unit = (u32)(pos / RING_UNIT % ring.units);
	int t = unit >= ring.tierUnits[0];
	return ring.tier[t] + (u64)(unit - (t ? ring.tierUnits[0] : 0)) * RING_UNIT + pos % RING_UNIT;
}

u8 *ringTier1(void) {
	static u8 *tier1;
	if (!tier1) {
		// Static buffers shrink the linear heap, and a large one has made this fail.
		tier1 = linearMemAlign(RING_TIER1_BYTES, 0x1000); // page-aligned for IPC
	}
	return tier1 && digestReserve() ? tier1 : NULL; // the unit list right behind it
}

bool ringArm(u64 total, RingDir dir, bool overflow, u64 offset) {
	memset(&ring, 0, sizeof(ring));
	ring.offset = offset;
	ring.tier[0] = ringTier1();
	if (!ring.tier[0]) {
		return false;
	}
	ring.tierUnits[0] = RING_TIER1_BYTES / RING_UNIT;
	if (overflow && total > RING_TIER1_BYTES) {
		ring.tier[1] =
			memalign(0x1000, (size_t)DRAIN_RING2_UNITS * RING_UNIT); // page-aligned for IPC
		ring.tierUnits[1] = ring.tier[1] ? DRAIN_RING2_UNITS : 0;
	}
	ring.units = ring.tierUnits[0] + ring.tierUnits[1];
	ring.stats.cap = (u64)ring.units * RING_UNIT;
	ring.dir = dir;
	ring.total = total;
	ring.progressMs = nowMs();
	s16 empty = (s16)ring.units;
	LightSemaphore_Init(&ring.forPump, dir == RING_TO_HANDLE ? empty : 0, empty);
	LightSemaphore_Init(&ring.forWorker, dir == RING_TO_HANDLE ? 0 : empty, empty);
	return true;
}

static bool pumpUnit(void) {
	if (!ring.unitOpen) {
		ring.parked = LightSemaphore_TryAcquire(&ring.forPump, 1) != 0;
		ring.unitOpen = !ring.parked;
	}
	return ring.unitOpen;
}

static size_t unitLeft(void) {
	u64 left = ring.total - ring.pumped, inUnit = RING_UNIT - ring.pumped % RING_UNIT;
	return (size_t)MIN(left, inUnit);
}

static void pumpAdvance(size_t size) {
	ring.pumped += size;
	if (ring.early && ring.pumped >= ring.early) {
		__atomic_store_n(&ring.earlyIn, true, __ATOMIC_RELEASE); // after the bytes it covers
	}
	if (ring.pumped % RING_UNIT == 0 || ring.pumped == ring.total) {
		ring.unitOpen = false;
		LightSemaphore_Release(&ring.forWorker, 1);
	}
}

bool ringAcquire(unsigned char **buffer, size_t *capacity) {
	*buffer = ringAt(ring.pumped);
	*capacity = pumpUnit() ? unitLeft() : 0;
	return !ring.failed;
}

bool ringCommit(size_t size) {
	pumpAdvance(size);
	u64 fill = ring.pumped - ringDone();
	ring.high = MAX(fill, ring.high);
	return !ring.failed;
}

bool ringFill(unsigned char *dst, size_t cap, size_t *got, bool *end) {
	*got = 0;
	if (ring.pumped < ring.total && pumpUnit()) {
		size_t n = MIN(unitLeft(), cap);
		memcpy(dst, ringAt(ring.pumped), n);
		pumpAdvance(n);
		__atomic_store_n(&ring.done, ring.pumped, __ATOMIC_RELEASE);
		*got = n;
	}
	*end = ring.pumped == ring.total;
	return !ring.failed;
}

// Compared as signed values, because the worker may stamp its progress just after this reads the
// clock, and a call that just finished must not read as ~49 days ago.
static bool workerStalled(void) {
	return (s32)(nowMs() - ring.progressMs) >= (s32)WORKER_STALL_MS;
}

// The worker is in a call, or the pump waits on it.
static bool workerOwes(void) {
	return ring.busy || ring.parked;
}

bool ringBusy(void) {
	return workerOwes() && !workerStalled();
}

bool ringStalled(void) {
	return workerOwes() && workerStalled();
}

static void addUs(u64 *us, TickCounter *tick) {
	osTickCounterUpdate(tick);
	*us += (u64)(osTickCounterRead(tick) * 1000.0);
}

// The early call must end inside the first unit, so that unit's own write takes up where the early
// call stopped.
void ringEarly(u64 end) {
	ring.early = end < RING_UNIT ? end : 0;
}

// A failed digest fails the ring.
static bool hashed(const u8 *p, u32 n) {
	if (!ring.digest) {
		return true;
	}
	TickCounter tick;
	osTickCounterStart(&tick);
	// This is an #if, not BENCH(), because BENCH() has no else-form (knobs.h).
#if THREEDROP_TEST_VERBS
	Result r = benchDigestUnits(p, n); // each call on bench hw's timeline
#else
	Result r = digestUnits(p, n);
#endif
	addUs(&ring.stats.hashUs, &tick);
	ring.stats.hashes += (n + DIGEST_UNIT - 1) / DIGEST_UNIT;
	if (R_FAILED(r)) {
		ring.stats.hashFailure = r;
		ring.failed = true;
	}
	return R_SUCCEEDED(r);
}

// recv() wrote through this core's data cache, so flush before FS reads it. One svc covers both
// tiers, since GSPGPU_FlushDataCache fails on the regular heap. FS maintains the cache for reads.
// The first write allocates a new file's clusters, so its calls are reported separately.
static Result timedCall(u64 pos, u8 *p, u32 n, bool first) {
	TickCounter tick;
	u32 moved = 0;
	Result r;
	osTickCounterStart(&tick);
	if (ring.dir == RING_TO_HANDLE) {
		r = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)p, n);
		if (R_SUCCEEDED(r)) {
			r = FSFILE_Write(ring.target, &moved, ring.offset + pos, p, n, 0);
		}
	} else {
		r = FSFILE_Read(ring.target, &moved, ring.offset + pos, p, n);
	}
	r = shortIo(r, moved, n);
	osTickCounterUpdate(&tick);
	BENCH(benchCall(ring.dir == RING_TO_HANDLE ? CALL_WRITE : CALL_READ,
					tick.reference - tick.elapsed, tick.reference,
					(n + RING_UNIT - 1) / RING_UNIT));
	double ms = osTickCounterRead(&tick);
	ring.stats.ioUs += (u64)(ms * 1000.0);
	ring.stats.ios++;
	if (first) {
		ring.stats.firstIoMs += (u32)ms;
	} else {
		ring.stats.laterIo = true;
		ring.stats.maxIoMs = MAX(ring.stats.maxIoMs, (u32)ms);
	}
	if (R_FAILED(r)) {
		ring.stats.failure = r;
		ring.failed = true;
	}
	return r;
}

// This call is made once the pump has passed its end, not once its unit fills, so AM's hold
// overlaps the body's wire time. The wait is a sleep, not a spin, because the pump runs below this
// thread on its core.
static bool earlyCall(void) {
	TickCounter tick;
	osTickCounterStart(&tick);
	while (!__atomic_load_n(&ring.earlyIn, __ATOMIC_ACQUIRE) && !ring.stop) {
		svcSleepThread(1000000);
	}
	addUs(&ring.stats.waitUs, &tick);
	ring.stats.waits++;
	if (ring.stop) {
		return true; // the unit's acquire sees it
	}
	ring.busy = true;
	ring.progressMs = nowMs();
	Result r = timedCall(0, ring.tier[0], (u32)ring.early, true); // the first unit is in tier 1
	ring.progressMs = nowMs();
	ring.busy = false;
	return R_SUCCEEDED(r);
}

static void worker(void *arg) {
	(void)arg;
	u64 pos = 0;
	while (pos < ring.total) {
		TickCounter tick;
		if (pos == 0 && ring.early && !earlyCall()) {
			break;
		}
		osTickCounterStart(&tick);
		LightSemaphore_Acquire(&ring.forWorker, 1);
		addUs(&ring.stats.waitUs, &tick);
		ring.stats.waits++;
		u32 unit = (u32)(pos / RING_UNIT % ring.units), k = 1;
		while (ring.dir == RING_TO_HANDLE && !ring.stop && k < WRITE_UNITS &&
			   unit + k != ring.tierUnits[0] && unit + k != ring.units &&
			   LightSemaphore_TryAcquire(&ring.forWorker, 1) == 0) {
			k++;
		}
		if (ring.stop) {
			break;
		}
		u64 left = ring.total - pos;
		u32 n = (u32)MIN(left, (u64)k * RING_UNIT);
		u8 *p = ringAt(pos);
		u32 skip = pos == 0 ? (u32)ring.early : 0; // AM took them in the early call
		ring.busy = true;		   // pauses the pump's idle clock, as one write can take tens of s
		ring.progressMs = nowMs(); // the stall bound times this call, not the wait before it
		// Before the write, because the engine called right after an SD write stalls the receive
		// ~300 ms.
		if (ring.dir == RING_TO_HANDLE && !hashed(p, n)) {
			break;
		}
		if (R_FAILED(timedCall(pos + skip, p + skip, n - skip, pos == 0))) {
			break;
		}
		pos += n;
		if (ring.dir == RING_TO_HANDLE) {
			__atomic_store_n(&ring.done, pos, __ATOMIC_RELEASE);
			// Hashed and written, so the pump refills it.
			LightSemaphore_Release(&ring.forPump, (s32)k);
		} else {
			u64 ahead = pos - ringDone();
			ring.high = MAX(ahead, ring.high);
			// Before its hash, because the pump only reads a unit and only this thread refills it.
			LightSemaphore_Release(&ring.forPump, (s32)k);
			if (!hashed(p, n)) {
				break;
			}
		}
		ring.progressMs = nowMs();
		ring.busy = false;
	}
	ring.busy = false;
	ring.stats.written = pos;
	if (ring.digest && pos == ring.total && !ring.failed) {
		TickCounter tick;
		osTickCounterStart(&tick);
		ring.stats.hashFailure = digestFinish(ring.stats.digest);
		addUs(&ring.stats.hashUs, &tick);
		ring.stats.hashes++;
		ring.failed = R_FAILED(ring.stats.hashFailure);
	}
}

bool ringStart(Handle target, bool digest, void (*abandon)(const RingStats *stats)) {
	ring.target = target;
	ring.digest = digest;
	ring.abandon = abandon;
	if (digest) {
		digestStart();
	}
	ring.thread =
		threadCreate(worker, NULL, 16 * 1024,
					 ring.dir == RING_TO_HANDLE ? WRITER_PRIORITY : READER_PRIORITY, 0, false);
	return ring.thread != NULL;
}

u64 ringDone(void) {
	return __atomic_load_n(&ring.done, __ATOMIC_ACQUIRE);
}

// The worker stops only after its current call. Called by ringJoin and ringBodyEnd(false, ...),
// and by qr.c's waiting callback when B arrives while a finished download drains into AM.
void ringStop(void) {
	ring.stop = true;
	LightSemaphore_Release(&ring.forWorker, 1); // wakes the worker waiting for a unit
}

typedef enum { JOIN_DONE, JOIN_PENDING, JOIN_STRANDED } JoinVerdict;

// Only 0 is DONE, because threadJoin's timeout, 0x09401BFE, also passes R_SUCCEEDED. A stranded
// worker keeps every buffer it can reach until a later join is DONE.
static JoinVerdict ringJoin(u64 timeoutNs, RingStats *out) {
	Result r = threadJoin(ring.thread, timeoutNs);
	if (r == 0) {
		threadFree(ring.thread);
		ring.thread = NULL;
		stranded = false;
		*out = ring.stats;
		out->high = ring.high;
		return JOIN_DONE;
	}
	if (!stranded && (R_DESCRIPTION(r) != RD_TIMEOUT || workerStalled())) {
		stranded = true;
		ringStop();
	}
	return stranded ? JOIN_STRANDED : JOIN_PENDING;
}

bool ringStranded(void) {
	return stranded;
}

bool ringBodyEnd(bool pumped, PhaseKind io, void (*waiting)(void *context), void *context,
				 RingStats *out, Operation *op) {
	TickCounter tick;
	JoinVerdict verdict;
	if (!pumped) {
		ringStop();
	}
	osTickCounterStart(&tick);
	while ((verdict = ringJoin(250000000ULL, out)) == JOIN_PENDING) {
		waiting(context);
	}
	if (verdict == JOIN_STRANDED) {
		appendLog("worker-stranded", 0, ringDone());
		opSet(op, OUTCOME_UNKNOWN, "stranded", 0);
		return false;
	}
	phaseSince(PHASE_DRAIN_JOIN, &tick);
	statsRing(out, ring.total, io);
	return true;
}

void ringPoll(void) {
	RingStats stats;
	if (stranded && ringJoin(0, &stats) == JOIN_DONE) {
		ringRelease();
		ring.abandon(&stats);
	}
}

void ringRelease(void) {
	if (stranded) {
		return; // the worker can still reach it
	}
	free(ring.tier[1]);
	ring.tier[1] = NULL;
}

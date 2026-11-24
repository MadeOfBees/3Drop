// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RING_H
#define RING_H

// The worker makes the Handle's calls, so the wire never waits on one. The pump is main's transfer
// loop, or download.c's thread in an install from a link. With hash.c, this is the only code
// that both a worker and a pump touch. It makes no AM calls, because the caller owns the Handle.

#include "result.h" // Operation
#include "stats.h"	// RingStats

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// One linear allocation lasts the whole process, because the linear heap fragments. The digest's
// unit list sits right behind it. It is lent between transfers, so a borrower first refuses
// `stranded`.
#define RING_TIER1_BYTES (24u * 1024 * 1024)
u8 *ringTier1(void);
// AM short-writes at an unaligned offset and reports success, so every write ends on a unit or at
// the body's end, except the early call (ringEarly), which ends one AES block into the first
// content. A short write fails the ring either way (shortIo).
#define RING_UNIT (1u << 20)

typedef enum { RING_TO_HANDLE, RING_FROM_HANDLE } RingDir;

// With `overflow`, tier 2 is used past tier 1. The handle's calls start `offset` bytes in, as a
// range of a file does.
bool ringArm(u64 total, RingDir dir, bool overflow, u64 offset);
// Call it between ringArm and ringStart. The first write's bytes up to `end` go in a call of
// their own as soon as the pump has passed it, not once their unit fills; an end past the first
// unit means no early call.
void ringEarly(u64 end);
// A capacity of 0 is a full ring, waited out like EAGAIN.
bool ringAcquire(unsigned char **buffer, size_t *capacity);
bool ringCommit(size_t size);
// *got can be 0 while the worker is still reading; only *end ends the body.
bool ringFill(unsigned char *dst, size_t cap, size_t *got, bool *end);
// Pauses the idle deadline, except for a worker stuck past WORKER_STALL_MS.
bool ringBusy(void);
// Returns true when the worker is stuck past WORKER_STALL_MS; the body ends there,
// whichever wait sees it.
bool ringStalled(void);
// `abandon` ends what the transfer held, once a stranded worker returns and no sooner.
bool ringStart(Handle target, bool digest, void (*abandon)(const RingStats *stats));
u64 ringDone(void);
bool ringStranded(void);
// The caller gives up on the body. The worker stops after its current call, and the caller then
// joins it through ringBodyEnd. The body is incomplete, so cancel the import and never commit it.
void ringStop(void);
// A failed pump stops the worker, which would wait on it forever. Returns false when stranded,
// with `op` saying so; otherwise the worker's account is in `stats`.
bool ringBodyEnd(bool pumped, PhaseKind io, void (*waiting)(void *context), void *context,
				 RingStats *out, Operation *op);
void ringPoll(void);
void ringRelease(void); // tier 1 lives as long as the process

#endif // RING_H

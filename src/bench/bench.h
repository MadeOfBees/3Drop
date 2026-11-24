// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BENCH_BENCH_H
#define BENCH_BENCH_H

#include "led.h"	  // Led
#include "protocol.h" // VerbContext, Verb

#include <3ds.h>
#include <stdbool.h>

// A bench build never ships. Its subs read and measure; a set changes state. A knob lasts until
// the next relaunch. `capture on` lasts until `capture off`, because it writes an arm file to the
// card, which survives a relaunch, and the first frame after boot arms from it (a card that
// refuses the file keeps the capture for this boot only, with a log line). A set spends a nonce,
// as every change does, so a request someone captured cannot set it again. A read spends none and
// may repeat, except `led`, whose read starts its window over once the reply has gone out, and
// the measuring subs, which each measure afresh.
void benchRun(VerbContext *c);

// The bench build's static tables must stay under check.sh's .bss bound over the release build
// (131072 bytes, because a large static starves the drain ring). The three big ones are bench.c's
// MB_ROWS rows (16 bytes each), hw.c's HW_CALLS (12) and HW_GAPS (12), which total 126976 bytes,
// with fsSteps, ledTrace, the capture slots and gKnobs on top. They are in two files, so no
// _Static_assert can total them. A table added or grown is caught by the gate's .bss line.

void benchMegabyte(u64 nowMs);

void benchLedWritten(Led led, Result result, u64 nowMs);

// `key` is the screen above CAPTURE_SCREEN_SHIFT bits of its variant, so each commit or finalize
// stage counts as a screen of its own. ui.c packs the variant: bit 2 is committing, bit 1 is
// finalizing, and bit 0 is finalizing with its counter showing.
#define CAPTURE_SCREEN_SHIFT 3
void benchFramePresented(u32 key, const char *title, u64 nowMs);

typedef struct {
	bool armed;
	unsigned frames, held, dropped, slots;
} CaptureState;
void benchCaptureSet(bool on);
CaptureState benchCaptureState(void);

// The put-hash timeline (hw.c), reset with each request's stats. A call's kind is its letter in
// `bench hw`; its start and end come from svcGetSystemTick; k is the number of units in the ring
// call (a write or a read) it serves, so a `d` row, one unit hashed, carries its group's count,
// not 1.
typedef enum {
	CALL_DIGEST = 'd', // the ring's digest, one unit a call
	CALL_WRITE = 'w',
	CALL_READ = 'r',
} CallKind;
void benchTimelineReset(void);
void benchCall(CallKind kind, u64 start, u64 end, u32 k);
// Records a wire gap, from `quiet` (the last byte moved, less ring and source waits) to the end of
// the call that ended it.
void benchGap(u64 quiet, const TickCounter *call);
Result benchDigestUnits(const u8 *p, u32 n);
void benchHw(VerbContext *c);

// For bench fscalls: which file step holds a change in the card's slow state. Each is named here,
// in bench.c's NAMES and in probe.sh's usage line; keep the three together. FS_DROP is the delete
// of a leftover backup, in three verbs: the replaced file after a put's swap, and the one beside
// a file a delete removed or a rename moved.
typedef enum {
	FS_ORPHAN,
	FS_OPEN,
	FS_SETSIZE,
	FS_FLUSH,
	FS_CLOSE,
	FS_PUBLISH,
	FS_ASIDE,
	FS_SWAP,
	FS_DROP,
	FS_DELETE,
	FS_RENAME,
	FS_LOG,
	FS_STEPS
} FsStep;
void benchFsBegin(const VerbContext *c);
void benchFsStep(FsStep step, u64 *at); // the ticks since *at, which moves on to now

#endif // BENCH_BENCH_H

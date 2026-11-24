// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KNOBS_H
#define KNOBS_H

// A release build compiles BENCH(...) away and KNOB(x) to 0. A knob of 0 is the compile-time
// default, so `bench <knob> 0` restores it. BENCH has no else-form, so a place that needs a release
// alternative spells it out with `#if THREEDROP_TEST_VERBS ... #else` (ring.c's digest call).

typedef struct {
	int sendSize; // per-send() cap; 0 = SEND_CHUNK_SIZE
	int rcvBuf;	  // listener SO_RCVBUF (the advertised window); 0 = LISTENER_WINDOW_BYTES
	int sndBuf;	  // listener SO_SNDBUF; 0 = LISTENER_SEND_BUFFER
	int noHash;	  // 1: an SD file put takes no digest (sdPutDigests); 0 = it does
	int noDisc;	  // 1: discovery's sockets stay closed (src/discover.c); 0 = open
} Knobs;

// Every knob reads through this, so 0 is its default wherever it is read.
static inline int knobOr(int knob, int def) {
	return knob > 0 ? knob : def;
}

#if THREEDROP_TEST_VERBS
// This include is not only for BENCH(): a file that includes knobs.h in a bench build sees every
// bench declaration (benchFsStep, benchCall, ...), whether its own hooks call them or not.
#include "bench/bench.h"
extern Knobs gKnobs; // all 0 until `bench <knob>` sets one; `run self` is the reset
#define KNOB(name) (gKnobs.name)
#define BENCH(...) __VA_ARGS__
#else
#define KNOB(name) (((const Knobs){0}).name) // every knob's default is 0; the compiler folds it
#define BENCH(...)
#endif

#endif // KNOBS_H

// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INSTALL_H
#define INSTALL_H

#include "cia.h" // CiaInfo
#include "result.h"

#include <3ds.h>
#include <stdbool.h>

// An overwrite needs a strictly newer version, because at an equal one AM reports success and
// changes nothing. It keeps the installed copy until the commit, so a failure loses nothing.
typedef struct {
	const char *name;
	bool self;
	bool overwrite;
} Route;
const Route *routeFind(const char *intent);

// The caller refuses `stranded` first, because that worker may still hold the ring and an import.
void installCia(int client, const Route *route, u64 ciaSize, u64 prefixSize,
				const u8 prefixDigest[32], Operation *op);

// The install's middle and tail, which installCia and a download share. The caller has armed the
// ring and pumped the CIA's prefix into its head; `ciaSize` is the size of the whole CIA.
typedef struct {
	const Route *route; // NULL means installOpen picks it (routePick) once it has read the CIA
	bool started;		// set by installOpen; true when the ring's writer runs
	TickCounter gap;	// the clock for the prefix gap, started by the caller before it parses
} InstallRun;
const Route *routePick(const CiaInfo *cia);
// Runs ciaParse and installGate, starts AM's import, then starts the writer. On false, *op says
// why, the ring is released and nothing is held (AM, HOME). On true, an import is live and
// installClose must follow, whether or not the writer started (`run->started`).
bool installOpen(InstallRun *run, u64 ciaSize, u64 prefixSize, Operation *op);
// Call this after the body was pumped, or failed to be (`received` false, with `receiveDetail` and
// `receiveWhy` as its refusal). It drains the writer, then commits or cancels. `waiting` keeps the
// screen alive through a join and the commit. A stranded writer returns with *op saying so, and
// HOME still held. A cancel in *op (installCancel), recorded before the call or by `waiting`
// during it, outranks every other refusal until AM_FinishCiaInstall is called; after that,
// nothing cancels.
void installClose(InstallRun *run, bool received, const char *receiveDetail, Result receiveWhy,
				  void (*waiting)(void *context), void *context, Operation *op);
// Records a stop the person asked for while a link downloads and installs (qr.c's B or START). A
// computer's install never records one, since its START fails the transfer itself (transfer.c).
static inline void installCancel(Operation *op) {
	opSet(op, OUTCOME_REJECTED, "canceled", TRANSFER_CANCELED);
}
static inline bool installCanceled(const Operation *op) {
	return op->result == TRANSFER_CANCELED;
}
// Returns the number of `new` imports AM started in this process, committed or not. AM's
// first-write hold has grown with registrations.
u32 installNewStarts(void);

#endif // INSTALL_H

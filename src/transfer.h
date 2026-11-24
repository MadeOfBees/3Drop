// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSFER_H
#define TRANSFER_H

// The pumps that move a body between the socket and the ring, or the caller's own sink or
// source. They receive up to the declared size, or send until the source ends, under an idle
// and a total deadline, with START to cancel and the screen and LED kept alive. ring.c's
// worker takes the other side.

#include "result.h"

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*TransferSink)(void *context, const unsigned char *buffer, size_t size);

// *got 0 means the ring's reader is behind. That is waited out until the reader
// stalls. Only *end ends the body.
typedef bool (*TransferSource)(void *context, unsigned char *dst, size_t cap, size_t *got,
							   bool *end);

typedef void (*TransferProgressFn)(void);

typedef struct {
	int client;
	u64 acceptedSize;
	u64 absoluteDurationMs;
	u64 bodyStartMs;
	u64 idleStartMs; // advances once the sink takes the bytes, or while a ring worker is busy
	bool ring;		 // a ring worker's long call holds the body back, not the peer
	u64 pollStartMs;
	u64 statusStartMs;
	u64 progressStartMs;
	u64 nextMbBoundary;
	bool started;
	TransferProgressFn onProgress;
	Result failure; // 0 when a sink or source failed, because it keeps its own reason
} TransferSession;

// ring is true when a worker sinks or sources the body, so the bar follows it and its
// calls pause the idle clock.
void transferSessionInit(TransferSession *session, int client, u64 totalSize,
						 TransferProgressFn onProgress, bool ring);

// A NULL sink receives into the armed ring (ringAcquire, ringCommit), whether or not a worker
// drains it.
bool transferReceive(TransferSession *session, u64 size, TransferSink sink, void *context);

// A full ring is waited out with the screen and START on their cadence, until its writer stalls.
bool transferRingAcquire(TransferSession *session, unsigned char **buffer, size_t *capacity);

// A NULL source sends from the armed ring (ringFill).
bool transferSend(TransferSession *session, TransferSource source, void *context);
// Services the pump's deadlines, screen and START on their cadence, from
// inside a source's long call.
bool transferPoll(TransferSession *session);

typedef struct {
	const unsigned char *base;
	size_t period;
	u64 size, at;
} TransferBytes;
bool transferBytes(void *context, unsigned char *dst, size_t cap, size_t *got, bool *end);

// Keeps the screen and LED alive through a drain join, which can wait out AM's first write.
void transferWaitTick(void *session);

u64 transferAbsoluteDuration(u64 totalSize);

static inline const char *transferFailDetail(Result failure) {
	switch (failure) {
	case TRANSFER_SHORT_READ:
		return "short"; // the sender closed early (a ghost's cancel or a killed tool,
						// not the network)
	case TRANSFER_CANCELED:
		return "canceled";
	case TRANSFER_IDLE_TIMEOUT:
	case TRANSFER_ABSOLUTE_TIMEOUT:
		return "timeout";
	case TRANSFER_SEND_FAILED:
		return "send";
	case TRANSFER_WORKER_STALL:
		return "worker-stall";
	case TRANSFER_EXITED:
		return "exit";
	default:
		return "receive";
	}
}

static inline void transferFailOp(Operation *op, const TransferSession *session) {
	opSet(op, OUTCOME_REJECTED, transferFailDetail(session->failure), session->failure);
}

#endif // TRANSFER_H

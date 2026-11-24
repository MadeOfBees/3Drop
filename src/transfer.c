// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer.h"

#include "app.h"
#include "fs.h" // appendLog
#include "net.h"
#include "result.h"
#include "ring.h"
#include "stats.h"

#include <3ds.h>
#include <errno.h>
#include <string.h>
#include <sys/param.h> // MIN
#include <sys/socket.h>

#define TRANSFER_PROGRESS_INTERVAL_MS 200ULL
// This sets a minimum rate, so a peer dribbling just enough to reset the idle clock cannot hold the
// listener.
#define TRANSFER_ABSOLUTE_BASE_MS 300000ULL
#define TRANSFER_ABSOLUTE_BYTES_PER_SECOND 16384ULL

static unsigned char staging[RECV_BUFFER_SIZE] __attribute__((aligned(0x1000)));

static inline void reportStatus(void) {
	TickCounter tick;
	osTickCounterStart(&tick);
	appStatusTick();
	phaseSince(PHASE_STATUS, &tick);
}

static void showProgress(TransferSession *session) {
	gApp.uiWrittenBytes = gApp.uiSunkProgress ? ringDone() : gApp.wireBytes;
	session->onProgress();
}

static inline void reportProgress(TransferSession *session) {
	TickCounter tick;
	osTickCounterStart(&tick);
	showProgress(session);
	phaseSince(PHASE_PROGRESS, &tick);
}

static void advance(TransferSession *session, u64 n, u64 now) {
	session->acceptedSize += n;
	gApp.wireBytes += n;
	while (session->acceptedSize >= session->nextMbBoundary) {
		statsMegabyte(now);
		session->nextMbBoundary += 1000000ULL;
	}
	session->idleStartMs = now;
}

// A NULL event logs nothing.
static bool socketFail(TransferSession *session, Result result, const char *event, int err) {
	session->failure = result;
	if (event) {
		appendLog(event, err, gApp.wireBytes);
	}
	return false;
}

static bool transferFail(TransferSession *session, Result result, const char *event) {
	return socketFail(session, result, event, result);
}

// The one sleep both pumps use, on EAGAIN or when a worker is behind. The call that could not go on
// and the sleep after it are timed as one wait.
static void backoff(PhaseKind wait, const TickCounter *call) {
	u64 start = svcGetSystemTick();
	svcSleepThread(WAIT_NS);
	phaseRecord(wait, (double)(call->elapsed + svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
}

u64 transferAbsoluteDuration(u64 totalSize) {
	u64 seconds = totalSize / TRANSFER_ABSOLUTE_BYTES_PER_SECOND +
				  (totalSize % TRANSFER_ABSOLUTE_BYTES_PER_SECOND != 0);
	return TRANSFER_ABSOLUTE_BASE_MS + seconds * 1000ULL;
}

static void startSession(TransferSession *session) {
	if (session->started) {
		return;
	}
	u64 now = uptimeMs();
	session->bodyStartMs = now;
	session->nextMbBoundary = 1000000ULL;
	gStats.firstByteMs = now;
	session->idleStartMs = now;
	session->pollStartMs = now;
	session->statusStartMs = now;
	session->progressStartMs = now;
	session->started = true;
}

static bool checkAbsoluteDeadline(TransferSession *session, u64 now) {
	if (now >= session->bodyStartMs + session->absoluteDurationMs) {
		return transferFail(session, TRANSFER_ABSOLUTE_TIMEOUT, "transfer-absolute-timeout");
	}
	return true;
}

static bool checkDeadlines(TransferSession *session, u64 now) {
	if (!checkAbsoluteDeadline(session, now)) {
		return false;
	}
	// A worker's long call holds the body back, not the peer, so the idle clock restarts after it.
	// Past the stall bound this ends the body here as a worker stall, whichever wait the pump is in
	// (a gzip get waits on deflate, not on the ring), and never at the idle timeout after it.
	if (session->ring && ringStalled()) {
		return transferFail(session, TRANSFER_WORKER_STALL, "transfer-worker-stall");
	}
	if (session->ring && ringBusy()) {
		session->idleStartMs = now;
		return true;
	}
	if (now >= session->idleStartMs + TRANSFER_WIRE_STALL_MS) {
		return transferFail(session, TRANSFER_IDLE_TIMEOUT, "transfer-idle-timeout");
	}
	return true;
}

// Runs on clocks, not on every turn, because the LED reads the lid over IPC, progress draws a
// frame, and START needs no more.
static bool serviceDeferred(TransferSession *session, u64 now, bool forceUiStatus) {
	if (forceUiStatus || now - session->statusStartMs >= STATUS_TICK_MS) {
		reportStatus();
		session->statusStartMs = uptimeMs();
	}

	if (session->onProgress &&
		(forceUiStatus || now - session->progressStartMs >= TRANSFER_PROGRESS_INTERVAL_MS)) {
		reportProgress(session);
		session->progressStartMs = uptimeMs();
	}

	if (now - session->pollStartMs < START_POLL_MS) {
		return true;
	}
	TickCounter tick;
	osTickCounterStart(&tick);
	bool canceled = startPressed();
	phaseSince(PHASE_POLL, &tick);
	session->pollStartMs = uptimeMs();
	if (canceled) {
		return transferFail(session, TRANSFER_CANCELED, "transfer-canceled");
	}
	return true;
}

static inline bool segmentService(TransferSession *session, u64 now) {
	return checkDeadlines(session, now) && serviceDeferred(session, now, false);
}

static bool finishService(TransferSession *session) {
	u64 now = uptimeMs();
	return checkAbsoluteDeadline(session, now) && serviceDeferred(session, now, true);
}

bool transferRingAcquire(TransferSession *session, unsigned char **buffer, size_t *capacity) {
	for (;;) {
		TickCounter tick;
		osTickCounterStart(&tick);
		bool acquired = ringAcquire(buffer, capacity);
		osTickCounterUpdate(&tick);
		if (!acquired || *capacity != 0) {
			phaseRecord(PHASE_RING_ACQUIRE, osTickCounterRead(&tick));
			return acquired;
		}
		// A full ring is waited out like EAGAIN, until checkDeadlines finds the worker stalled.
		backoff(PHASE_RING_ACQUIRE, &tick);
		if (!appRunning()) {
			return transferFail(session, TRANSFER_EXITED, "transfer-exit");
		}
		if (!segmentService(session, uptimeMs())) {
			return false;
		}
	}
}

void transferSessionInit(TransferSession *session, int client, u64 totalSize,
						 TransferProgressFn onProgress, bool ring) {
	memset(session, 0, sizeof(*session));
	session->client = client;
	session->absoluteDurationMs = transferAbsoluteDuration(totalSize);
	session->onProgress = onProgress;
	session->ring = ring;
	// statsStart cleared the rest. The checks before this have already recorded into this request's
	// stats.
	gApp.wireTotal = totalSize;
	gApp.uiSunkProgress = ring;
}

bool transferReceive(TransferSession *session, u64 size, TransferSink sink, void *context) {
	u64 got = 0;
	if (size == 0) {
		return true;
	}
	startSession(session);
	u64 quiet = svcGetSystemTick(); // time since the last byte moved, less the ring's waits
	while (got < size && appRunning()) {
		u64 now = uptimeMs();
		if (!segmentService(session, now)) {
			return false;
		}

		unsigned char *buffer = staging;
		size_t capacity = sizeof(staging);
		if (!sink) {
			u64 acquireStart = svcGetSystemTick();
			bool acquired = transferRingAcquire(session, &buffer, &capacity);
			quiet += svcGetSystemTick() - acquireStart; // ring back-pressure is not a wire gap
			if (!acquired) {
				return false;
			}
		}

		// This never reads past the body, because whatever follows on the connection does not
		// belong to this read.
		size_t want = (size_t)MIN(size - got, MIN(capacity, SOC_FAST_RECV_SIZE));
		TickCounter recvTick;
		osTickCounterStart(&recvTick);
		ssize_t count = recv(session->client, buffer, want, 0);
		osTickCounterUpdate(&recvTick);
		if (count > 0) {
			statsWireGap(ticksMs(svcGetSystemTick() - quiet));
			BENCH(benchGap(quiet, &recvTick));
			phaseRecord(PHASE_RECV, osTickCounterRead(&recvTick));
			// Counters advance only once the sink has taken the whole chunk.
			TickCounter sinkTick;
			osTickCounterStart(&sinkTick);
			bool accepted = sink ? sink(context, buffer, (size_t)count) : ringCommit((size_t)count);
			phaseSince(PHASE_SINK, &sinkTick);
			if (!accepted) {
				return false;
			}
			got += (u64)count;
			quiet = svcGetSystemTick();
			advance(session, (u64)count, ticksMs(quiet));
			if (got == size && !finishService(session)) {
				return false;
			}
			continue;
		}
		if (count == 0) {
			return transferFail(session, TRANSFER_SHORT_READ, "transfer-short-read");
		}
		if (!wouldBlock()) {
			return socketFail(session, TRANSFER_RECV_FAILED, "transfer-recv-failed", errno);
		}
		if (!segmentService(session, uptimeMs())) {
			return false;
		}
		backoff(PHASE_RECV_IDLE, &recvTick);
	}
	if (got == size) {
		return true;
	}
	return transferFail(session, TRANSFER_EXITED, "transfer-exit");
}

bool transferSend(TransferSession *session, TransferSource source, void *context) {
	size_t at = 0, staged = 0; // staging[at, staged) is still to go out
	bool end = false;
	startSession(session);
	u64 quiet = svcGetSystemTick(); // time since the last byte moved, less the source's time
	while (appRunning()) {
		u64 now = uptimeMs();
		if (!segmentService(session, now)) {
			return false;
		}
		if (at == staged) {
			if (end) {
				return finishService(session);
			}
			u64 fillStart = svcGetSystemTick();
			TickCounter fillTick;
			osTickCounterStart(&fillTick);
			bool filled = source ? source(context, staging, sizeof(staging), &staged, &end)
								 : ringFill(staging, sizeof(staging), &staged, &end);
			osTickCounterUpdate(&fillTick);
			at = 0;
			if (filled && staged == 0 && !end) {
				// The reader is behind, so this is waited out like EAGAIN until the deadlines find
				// it stalled.
				backoff(PHASE_RING_ACQUIRE, &fillTick);
			} else {
				phaseRecord(PHASE_SINK, osTickCounterRead(&fillTick));
			}
			// Neither a wait on the reader nor deflate is a wire gap.
			quiet += svcGetSystemTick() - fillStart;
			if (!filled) {
				return false;
			}
			continue;
		}
		size_t want = MIN(staged - at, effectiveSendSize());
		TickCounter sendTick;
		osTickCounterStart(&sendTick);
		ssize_t count = send(session->client, staging + at, want, 0);
		osTickCounterUpdate(&sendTick);
		if (count > 0) {
			u64 sent = svcGetSystemTick();
			statsWireGap(ticksMs(sent - quiet));
			BENCH(benchGap(quiet, &sendTick));
			phaseRecord(PHASE_SEND, osTickCounterRead(&sendTick));
			at += (size_t)count;
			quiet = sent;
			advance(session, (u64)count, ticksMs(sent));
			continue;
		}
		if (count == 0 || !wouldBlock()) {
			return socketFail(session, TRANSFER_SEND_FAILED, sendFailEvent("transfer-send-failed"),
							  count == 0 ? 0 : errno);
		}
		if (!segmentService(session, uptimeMs())) {
			return false;
		}
		backoff(PHASE_SEND_IDLE, &sendTick);
	}
	return transferFail(session, TRANSFER_EXITED, "transfer-exit");
}

bool transferPoll(TransferSession *session) {
	return segmentService(session, uptimeMs());
}

bool transferBytes(void *context, unsigned char *dst, size_t cap, size_t *got, bool *end) {
	TransferBytes *bytes = context;
	size_t offset = (size_t)(bytes->at % bytes->period);
	u64 left = bytes->size - bytes->at, inPeriod = bytes->period - offset;
	size_t n = MIN((size_t)MIN(left, inPeriod), cap);
	memcpy(dst, bytes->base + offset, n);
	bytes->at += n;
	*got = n;
	*end = bytes->at == bytes->size;
	return true;
}

void transferWaitTick(void *session) {
	TransferSession *s = session;
	reportStatus();
	if (s->onProgress) {
		showProgress(s);
	}
}

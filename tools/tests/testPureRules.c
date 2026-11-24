// cc: src/stats.c src/codec.c
// Checks the listener's fatal errnos, the word a pump failure gets by default, that `stats` fits
// its buffers at its widest, and its account of the wire's stalls.
#include "fakes.h"

#include "net.h"
#include "protocol.h"
#include "stats.h"
#include "transfer.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

// A dead listener is rebuilt; an empty backlog or an aborted peer is not a dead listener.
static void listenerErrnos(void) {
	assert(!listenerAcceptFatal(EAGAIN) && !listenerAcceptFatal(EINTR));
	assert(!listenerAcceptFatal(ECONNABORTED));
	assert(listenerAcceptFatal(EBADF) && listenerAcceptFatal(EINVAL));
}

// A failure with no word of its own is `receive`; testDispatch pins each of the other words.
static void defaultDetail(void) {
	assert(strcmp(transferFailDetail(TRANSFER_RECV_FAILED), "receive") == 0);
}

// `stats` at its widest fits its buffer and one reply, and is refused rather than cut short.
static void statsFits(void) {
	statsReset("");
	memset(&gStats, 0xFF, sizeof(gStats));
	memset(gStats.request, 'f', 32); // a request id: 32 hex digits
	gStats.request[32] = '\0';
	gStats.join = STATS_JOIN_FAILED;
	// A bool is 0 or 1, not 0xFF.
	gStats.amWriteMaxKnown = gStats.sdIoMaxKnown = gStats.sdFreeAfterKnown = true;
	gStats.totalKnown = true;
	gStats.firstByteMs = 1;
	gApp.wireBytes = UINT64_MAX;
	char text[STATS_TEXT_MAX];
	assert(statsFormat(text, sizeof(text)) && strchr(text, '-') == NULL);
	size_t len = strlen(text);
	assert(!statsFormat(text, len) && statsFormat(text, len + 1)); // refused, never truncated
	// With its header and req= it fits one reply. The header is copied from protocol.c: "response
	// <id> <nonce> stats ok ok 00000000".
	assert(strlen("response  ") + 32 + 64 + strlen(" stats ok ok 00000000") + len + 1 <= REPLY_MAX);
	assert(strcmp(strrchr(text, ' '), " req=ffffffffffffffffffffffffffffffff") == 0);
	statsReset("0123456789abcdef0123456789abcdef");
	assert(statsFormat(text, sizeof(text)) &&
		   strcmp(strrchr(text, ' '), " req=0123456789abcdef0123456789abcdef") == 0);
	statsReset("");
	assert(statsFormat(text, sizeof(text)) && strcmp(strrchr(text, ' '), " req=-") == 0);
}

// A wire stall is a gap of at least 100 ms (STATS_STALL_MS); the largest is kept, not the last.
static void wireStalls(void) {
	statsReset("");
	statsWireGap(99);
	statsWireGap(100);
	statsWireGap(400);
	statsWireGap(250);
	assert(gStats.stallN == 3 && gStats.stallMs == 750 && gStats.stallMaxMs == 400);
}

static void expectTail(bool timed, const char *want) {
	char text[STATS_TAIL_MAX];
	assert(statsLogTail(text, sizeof(text), timed) && strcmp(text, want) == 0);
}

// A request's log line says where a transfer's total_ms went and, for every request, gives the
// uptime. The time before the body, the body, the join and the rest add up to the total. A `-`
// marks what was not measured, and a request that moved no bytes has only the uptime.
static void logTail(void) {
	char text[STATS_TAIL_MAX];
	fake.nowMs = 3723456; // the uptime: 1 h 2 min 3.456 s
	statsReset("");
	expectTail(false, " up_s=3723");
	expectTail(true, " total_ms=- pre_ms=- body_ms=- join_ms=- post_ms=- up_s=3723");
	// A put that started at 1000 ms, read its first body byte at 1040, joined the ring for 500 ms
	// (as the tick counter reads it) and replied at 1860. The join's 499 ms were part of the 800
	// ms between the first byte and the join's end, so the body is 301.
	gStats.startMs = 1000;
	gStats.firstByteMs = 1040;
	gStats.phaseUs[PHASE_DRAIN_JOIN] = 499976;
	gStats.joinEndMs = 1840;
	gStats.totalMs = 860;
	gStats.totalKnown = true;
	expectTail(true, " total_ms=860 pre_ms=40 body_ms=301 join_ms=499 post_ms=20 up_s=3723");
	// A request refused before its body, or with an empty one, shows only what was measured.
	gStats.firstByteMs = gStats.joinEndMs = 0;
	gStats.phaseUs[PHASE_DRAIN_JOIN] = 0;
	expectTail(true, " total_ms=860 pre_ms=- body_ms=- join_ms=- post_ms=- up_s=3723");
	gStats.joinEndMs = 1840;
	expectTail(true, " total_ms=860 pre_ms=- body_ms=- join_ms=0 post_ms=20 up_s=3723");
	// At its widest it fits.
	memset(&gStats, 0xFF, sizeof(gStats));
	gStats.totalKnown = true;
	assert(statsLogTail(text, sizeof(text), true) && strlen(text) < sizeof(text));
}

// On the normal path the line adds one tick read at the join's end and one for the uptime, never
// one per field. Every other mark it prints was already read by the transfer.
static void logTailReads(void) {
	char text[STATS_TAIL_MAX];
	RingStats ring = {0};
	statsReset("");
	fake.stepMs = 1; // each read of the clock moves it a millisecond
	u64 at = fake.nowMs;
	statsRing(&ring, 0, PHASE_SD_IO);
	assert(fake.nowMs - at == 1 && gStats.joinEndMs != 0);
	at = fake.nowMs;
	assert(statsLogTail(text, sizeof(text), true));
	assert(fake.nowMs - at == 1);
	fake.stepMs = 0;
}

int main(void) {
	listenerErrnos();
	defaultDetail();
	statsFits();
	wireStalls();
	logTail();
	logTailReads();
	TEST_PASS();
	return 0;
}

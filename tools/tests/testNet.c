// cc: -DTHREEDROP_TEST_VERBS=1 src/net.c src/transfer.c src/ring.c src/hash.c src/stats.c
// cc: src/bench/bench.c src/bench/capture.c src/bench/ledRead.c src/led.c src/screenshot.c
// cc: src/protocol.c src/codec.c src/bench/hw.c src/ui.c src/urls.c
// Tests the real net.c and transfer.c on a scripted clock, socket and NDM, and the QR flow's waits
// (qr.c's stopRequested, ui.c's uiAsk), which serve a client meanwhile.
#include "fakes.h"

#include "auth.h"
#include "net.h"
#include "ring.h"
#include "stats.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

// qr.c is included for its stopRequested, the wait whose keys a request served meanwhile must
// not take.
#define APP_VERSION "test" // the real value comes from tools/build.sh, which no test runs
#include "qr.c"

// With sendStepBack the system clock steps back 36 ms at the next EAGAIN; with sendsOpen every send
// takes all it is given. sendWidest is the most one send has taken.
static ssize_t sendScript[4];
static unsigned sendCount, sendIndex;
static char sent[512];
static bool sendStepBack, sendsOpen;
static size_t sendWidest;
ssize_t send(int fd, const void *buf, size_t len, int flags) {
	ssize_t r = sendsOpen ? (ssize_t)len : sendIndex < sendCount ? sendScript[sendIndex++] : -1;
	if (r < 0) {
		fake.wallMs -= sendStepBack ? 36 : 0;
		sendStepBack = false;
		errno = EAGAIN;
		return -1;
	}
	r = r > (ssize_t)len ? (ssize_t)len : r;
	sendWidest = (size_t)r > sendWidest ? (size_t)r : sendWidest;
	snprintf(sent, sizeof(sent), "%.*s", (int)r, (const char *)buf);
	return r;
}
// Each byte arrives at its own time on the fake clock. Once recvEof is set, the close arrives at
// recvEofAt. MSG_PEEK takes nothing. The system clock steps by recvWallStep at the next call.
static const char *recvBytes;
static u64 recvAt[128], recvEofAt;
static size_t recvCount, recvIndex;
static bool recvEof;
static s64 recvWallStep;
static int recvErrno; // non-zero: recv() fails with it
ssize_t recv(int fd, void *buf, size_t len, int flags) {
	size_t n = 0;
	fake.wallMs += recvWallStep;
	recvWallStep = 0;
	if (recvErrno) {
		errno = recvErrno;
		return -1;
	}
	while (n < len && recvIndex + n < recvCount && recvAt[recvIndex + n] <= fake.nowMs) {
		((char *)buf)[n] = recvBytes[recvIndex + n];
		n++;
	}
	if (n == 0 && recvEof && recvIndex == recvCount && fake.nowMs >= recvEofAt) {
		return 0;
	}
	if (n == 0) {
		errno = EAGAIN;
		return -1;
	}
	recvIndex += (flags & MSG_PEEK) ? 0 : n;
	return (ssize_t)n;
}
// A peer that sends bytes, each at `at` on the fake clock, and never closes.
static void feed(const char *bytes, u64 at) {
	recvBytes = bytes;
	recvCount = strlen(bytes);
	recvIndex = 0;
	recvEof = false;
	for (size_t i = 0; i < recvCount; i++) {
		recvAt[i] = at;
	}
}
// The fake sockets are numbers, never open files: close only records.
int shutdown(int fd, int how) {
	return 0;
}
static int closed = -1;
int close(int fd) {
	closed = fd;
	return 0;
}

// readbackCap: a stack that takes less than it was asked.
static int refuseOption, window, sendBuffer, readbackCap;
// Each socket gets its own number, and only the newest one answers getsockopt, as a closed one
// would not.
static unsigned sockets;
static int newest = -1;
static bool listening;
int socket(int domain, int type, int protocol) {
	sockets++;
	listening = false;
	return newest = 100 + (int)sockets;
}
static bool isNewest(int fd) {
	return fd == newest;
}
static bool ipUnknown;
Result SOCU_GetIPInfo(struct in_addr *ip, struct in_addr *netmask, struct in_addr *broadcast) {
	ip->s_addr = ipUnknown ? 0 : inet_addr("192.0.2.50");
	netmask->s_addr = inet_addr("255.255.255.0");
	broadcast->s_addr = inet_addr("192.0.2.255");
	return 0;
}
int setsockopt(int fd, int level, int name, const void *value, socklen_t size) {
	if (name == refuseOption) {
		errno = ENOBUFS;
		return -1;
	}
	if (name == SO_RCVBUF) {
		window = *(const int *)value;
	}
	if (name == SO_SNDBUF) {
		sendBuffer = *(const int *)value;
	}
	return 0;
}
// Once listening, it reads back what it holds (bench state's measurement). Earlier, it reads back
// 65536 whatever was set (the record's other reading), so a readback taken too early fails.
int getsockopt(int fd, int level, int name, void *value, socklen_t *size) {
	if (fd != 7 && !isNewest(fd)) {
		errno = EBADF;
		return -1;
	}
	// The request's own socket (7) reads back as the console's accepted one once did:
	// 8192 (net.h).
	int got = fd == 7 ? 8192 : !listening ? 65536 : name == SO_RCVBUF ? window : sendBuffer;
	*(int *)value = readbackCap && got > readbackCap ? readbackCap : got;
	return 0;
}
// NDM, with Luma's kernel in front of it. Once Rosalina's network functions have run (rosalinaRan),
// the kernel answers Enter and Leave itself on a session it knows as "ndm:u" (SendSyncRequest.c).
// It knows libctru's own session (0x6C) as that, and srv's (0x6D) by the name bytes srv was asked
// for. NDM counts every Lock in locks; locked is a lock that holds Wi-Fi up. otherHolds means
// another process (Rosalina) holds INFRASTRUCTURE and its lock, so NDM refuses 3Drop's Enter and
// shows that state, and Wi-Fi stays up with the lid closed. ndmuInit and ndmuExit count references
// as libctru's do; closes counts each session's closes.
Handle ndmuHandle;
static bool rosalinaRan, otherHolds;
static Result ndmuResult;
static int ndmuRefs;
Result ndmuInit(void) {
	if (ndmuResult) {
		return ndmuResult;
	}
	if (ndmuRefs++ == 0) {
		ndmuHandle = 0x6C;
	}
	return 0;
}
static unsigned libctruCloses, ownCloses;
Result svcCloseHandle(Handle handle) {
	libctruCloses += handle == 0x6C;
	ownCloses += handle == 0x6D;
	return 0;
}
void ndmuExit(void) {
	if (--ndmuRefs == 0) {
		svcCloseHandle(ndmuHandle);
		ndmuHandle = 0;
	}
}
static bool kernelAnswers(void) {
	const char *name = ndmuHandle == 0x6C ? "ndm:u" : ndmuHandle == 0x6D ? fake.srvName : "";
	return rosalinaRan && strcmp(name, "ndm:u") == 0;
}
static bool exclusive, locked, stateNone; // stateNone: GetExclusiveState shows NONE regardless
static Result enterResult, lockResult, stateResult;
static unsigned enters, locks;
static u64 lockedAtMs;
Result NDMU_EnterExclusiveState(u32 state) {
	enters++;
	if (kernelAnswers()) {
		return 0;
	}
	if (otherHolds) {
		return (Result)0xC8A14FF0; // any refusal
	}
	exclusive = enterResult == 0 && state == NDM_EXCLUSIVE_STATE_INFRASTRUCTURE;
	return enterResult;
}
Result NDMU_LockState(void) {
	if (!locked) {
		lockedAtMs = fake.nowMs; // Wi-Fi comes back from the lock that took it, not a later one
	}
	locks += lockResult == 0;
	locked = exclusive && lockResult == 0;
	return lockResult;
}
Result NDMU_GetExclusiveState(ndmExclusiveState *state) {
	bool shown = (exclusive || otherHolds) && !stateNone;
	*state = shown ? NDM_EXCLUSIVE_STATE_INFRASTRUCTURE : NDM_EXCLUSIVE_STATE_NONE;
	return stateResult;
}
Result NDMU_LeaveExclusiveState(void) {
	exclusive = exclusive && kernelAnswers();
	return 0;
}
Result NDMU_UnlockState(void) {
	locks -= locks > 0;
	locked = false;
	return 0;
}
// lidClosed: Wi-Fi stays down until the keep-alive lock has been held for wifiBackMs.
static bool lidClosed;
static u64 wifiBackMs;
static int windowAtBind, sendBufferAtBind;
int bind(int fd, const struct sockaddr *address, socklen_t size) {
	if (lidClosed && !otherHolds && !(locked && fake.nowMs >= lockedAtMs + wifiBackMs)) {
		errno = ENETDOWN;
		return -1;
	}
	windowAtBind = window;
	sendBufferAtBind = sendBuffer;
	return 0;
}
int listen(int fd, int backlog) {
	listening = true;
	return 0;
}
int fcntl(int fd, int command, ...) {
	return 0;
}

static unsigned polls, vblanks;
static bool pollFails;
int poll(struct pollfd *fds, nfds_t n, int timeoutMs) {
	polls++;
	return pollFails ? -1 : 0;
}
void gspWaitForVBlank(void) {
	vblanks++;
}
// Counted because a socket wait ticks the LED beside each read of START.
static unsigned statusTicks;
void appStatusTick(void) {
	statusTicks++;
}
// tapKeys (START, unless a test says otherwise) are held through [tapFrom, tapTo) on the fake
// clock. As in libctru, a scan reports a key down only when it is held now and was not at the last
// scan; fake.keysDown is down at every scan.
static u64 tapFrom, tapTo;
static u32 tapKeys = KEY_START, scanHeld, scanDown;
void hidScanInput(void) {
	u64 now = __atomic_load_n(&fake.nowMs, __ATOMIC_SEQ_CST);
	u32 held = now >= tapFrom && now < tapTo ? tapKeys : 0;
	scanDown = held & ~scanHeld;
	scanHeld = held;
}
u32 hidKeysDown(void) {
	return scanDown | fake.keysDown;
}

static bool discardSink(void *context, const unsigned char *buffer, size_t size) {
	return true;
}
// The system clock steps back 36 ms inside the first chunk's sink, after that chunk's wire time is
// taken.
static bool stepBackSink(void *context, const unsigned char *buffer, size_t size) {
	int *calls = context;
	fake.wallMs -= (*calls)++ == 0 ? 36 : 0;
	return true;
}
static bool slowSource(void *context, unsigned char *dst, size_t cap, size_t *got, bool *end) {
	int *calls = context;
	fake.nowMs += 500;
	memset(dst, 'x', 1000);
	*got = 1000;
	*end = ++*calls == 3;
	return true;
}

static void reset(u64 nowMs, u64 stepMs) {
	fake.nowMs = nowMs;
	fake.stepMs = stepMs;
	fake.wallMs = 0;
	fake.log[0] = '\0';
	sendCount = sendIndex = 0;
}

// A peer that stops reading is dropped at the wire stall bound, and what went out is kept. EAGAIN
// inside the deadlines is waited out. A control reply has only the idle timeout.
static void sendDeadlines(void) {
	reset(4321, 0);
	TransferSession s;
	transferSessionInit(&s, 7, 50000, NULL, false);
	assert(s.absoluteDurationMs == 300000 + 4000); // base + a second per started 16384 bytes
	transferSessionInit(&s, 7, 4 * 16384, NULL, false);
	assert(s.absoluteDurationMs == 304000);
	assert(transferAbsoluteDuration(UINT64_MAX) > transferAbsoluteDuration(UINT64_MAX / 2));
	// Dropped at the wire stall bound, before even the smallest body's absolute deadline.
	assert(TRANSFER_WIRE_STALL_MS < transferAbsoluteDuration(1));

	static unsigned char body[4096];
	TransferBytes bytes = {body, sizeof(body), sizeof(body), 0};
	reset(0, 100); // 100 bytes go out, then EAGAIN until the peer counts as gone
	transferSessionInit(&s, 7, sizeof(body), NULL, false);
	sendScript[sendCount++] = 100;
	assert(!transferSend(&s, transferBytes, &bytes));
	assert(s.failure == TRANSFER_IDLE_TIMEOUT && s.acceptedSize == 100);
	// The idle bound, 3 minutes after the last byte went, give or take the clock's steps.
	assert(fake.nowMs >= 180000 && fake.nowMs <= 182000);
	// A session with no ring of its own (a bench tx's) isn't held by a busy ring, since the idle
	// clock restarts only for a session that has a ring (transfer.c:114).
	reset(0, 100);
	unsigned char unit[16];
	size_t filled;
	bool end;
	assert(ringArm(sizeof(unit), RING_FROM_HANDLE, false, 0) &&
		   ringFill(unit, sizeof(unit), &filled, &end) && filled == 0 && ringBusy());
	transferSessionInit(&s, 7, sizeof(body), NULL, false);
	sendScript[sendCount++] = 100;
	bytes.at = 0;
	assert(!transferSend(&s, transferBytes, &bytes) && s.failure == TRANSFER_IDLE_TIMEOUT);
	assert(fake.nowMs >= 180000 && fake.nowMs <= 182000 && ringBusy());

	reset(0, 0); // a body started past its absolute deadline stops at once
	transferSessionInit(&s, 7, sizeof(body), NULL, false);
	s.started = true;
	fake.nowMs = s.absoluteDurationMs;
	bytes.at = 0;
	assert(!transferSend(&s, transferBytes, &bytes) && s.failure == TRANSFER_ABSOLUTE_TIMEOUT);

	reset(0, 0); // EAGAIN inside the deadlines is waited out
	transferSessionInit(&s, 7, sizeof(body), NULL, false);
	sendScript[sendCount++] = -1;
	sendScript[sendCount++] = 4096;
	bytes.at = 0;
	assert(transferSend(&s, transferBytes, &bytes) && s.acceptedSize == sizeof(body));

	// Time the source spends making bytes (deflate) is not a wire stall.
	reset(0, 0);
	statsReset("");
	transferSessionInit(&s, 7, 3000, NULL, false);
	for (int i = 0; i < 3; i++) {
		sendScript[sendCount++] = 1000;
	}
	int calls = 0;
	assert(transferSend(&s, slowSource, &calls) && s.acceptedSize == 3000);
	assert(gStats.stallN == 0 && gStats.phaseN[PHASE_SEND] == 3);

	// A reply or a listing waits on the same peer as a body: one taking nothing ends at the pumps'
	// idle bound.
	reset(1000000, 100);
	gApp.requestSpent = true;
	assert(!sendBytes(7, body, sizeof(body)) && strcmp(fake.log, "send-idle-timeout") == 0);
	assert(fake.nowMs >= 1000000 + TRANSFER_WIRE_STALL_MS &&
		   fake.nowMs <= 1000000 + TRANSFER_WIRE_STALL_MS + 2000);
	// ...and a system clock step back while it takes nothing is not an idle timeout.
	reset(1000000, 0);
	sendScript[sendCount++] = 100;
	sendScript[sendCount++] = -1;
	sendScript[sendCount++] = sizeof(body) - 100;
	sendStepBack = true;
	assert(sendBytes(7, body, sizeof(body)));

	// A failed socket call logs the socket's errno, on both pumps and the reply sends.
	reset(0, 0);
	transferSessionInit(&s, 7, sizeof(body), NULL, false);
	recvErrno = ECONNRESET;
	assert(!transferReceive(&s, sizeof(body), discardSink, NULL));
	assert(s.failure == TRANSFER_RECV_FAILED && strcmp(fake.log, "transfer-recv-failed") == 0 &&
		   fake.logResult == ECONNRESET);
	recvErrno = 0;
	// A send the peer refuses is logged only for a request that spent a nonce, because a peer
	// resets as fast as it reconnects, and a captured read replays at will.
	for (int spent = 0; spent < 2; spent++) {
		gApp.requestSpent = spent;
		reset(0, 0);
		transferSessionInit(&s, 7, sizeof(body), NULL, false);
		sendScript[sendCount++] = 0; // send() took nothing
		bytes.at = 0;
		assert(!transferSend(&s, transferBytes, &bytes) && s.failure == TRANSFER_SEND_FAILED);
		assert(strcmp(fake.log, spent ? "transfer-send-failed" : "") == 0 &&
			   (!spent || fake.logResult == 0));
		reset(0, 0);
		sendScript[sendCount++] = 0;
		assert(!sendBytes(7, body, sizeof(body)));
		assert(strcmp(fake.log, spent ? "send-zero" : "") == 0);
		reset(1000000, 100); // and one that took nothing until the idle bound
		assert(!sendBytes(7, body, sizeof(body)));
		assert(strcmp(fake.log, spent ? "send-idle-timeout" : "") == 0);
	}
	gApp.requestSpent = false;
}

// An unsigned refusal the peer resets is never logged, though the request before it spent a nonce.
// Junk sent as fast as the peer reconnects writes nothing to the card.
static void junkUnlogged(void) {
	feed("info 0123456789abcdef0123456789abcdef\n00112233445566778899\n", 0);
	recvEof = true; // the peer is gone, so the close drains nothing
	recvEofAt = 0;
	reset(0, 1);
	sendScript[sendCount++] = 0; // the refusal goes nowhere
	gApp.requestSpent = true;
	protocolServe(7, NULL, 0, NULL, NOT_BUSY);
	assert(sendIndex == 1 && fake.log[0] == '\0' && !gApp.requestSpent);
	closeParked(true);
	recvEof = false;
}

// A listing, however long, goes out in sends of at most 2048 bytes (protocol.c says why).
static void listingSends(void) {
	static const Verb LS = {"ls", 2, 4, false, NULL, NULL, NOT_BUSY};
	VerbContext c = {
		.client = 7, .request = "ls", .id = "0123456789abcdef0123456789abcdef", .verb = &LS};
	char row[32];
	reset(0, 0);
	sendsOpen = true;
	sendWidest = 0;
	assert(listingBegin(&c, ""));
	for (int i = 0; i < 400; i++) { // about 6 KB
		int n = snprintf(row, sizeof(row), "f %d name%04d\n", i, i);
		assert(listingLine(row, (size_t)n));
	}
	listingEnd(OUTCOME_COMMITTED, "ok", 0);
	assert(strncmp(sent, "trailer ", 8) == 0 && strstr(sent, " ls ok ok 00000000 400 u:"));
	assert(sendWidest <= 2048);
	sendsOpen = false;
}

// A request and its MAC line from a slow peer: the request line is whole at requestDoneMs on the
// fake clock, and the MAC line, with the body's first byte after it, at macDoneMs.
static void feedRequest(u64 requestDoneMs, u64 macDoneMs) {
	static const char stream[] = "info 0123456789abcdef0123456789abcdef\n00112233445566778899\nB";
	size_t requestLen = strchr(stream, '\n') + 1 - stream;
	feed(stream, 0);
	for (size_t i = 0; i < recvCount; i++) {
		recvAt[i] = i < requestLen
						? requestDoneMs * (i + 1) / requestLen
						: requestDoneMs + (macDoneMs - requestDoneMs) * (i + 1 - requestLen) /
											  (recvCount - requestLen);
	}
	reset(0, 1); // a millisecond a read, so a step of the system clock lands right after the start
}

static LinesRead readLines(u64 requestDoneMs, u64 macDoneMs, size_t cap) {
	feedRequest(requestDoneMs, macDoneMs);
	char lines[128], *mac = NULL;
	LinesRead got = readRequestLines(7, lines, cap, &mac, PREAUTH_INPUT_TIMEOUT_MS);
	// Both lines, and not the body byte after them.
	assert(got != LINES_OK ||
		   (strcmp(lines, "info 0123456789abcdef0123456789abcdef") == 0 &&
			strcmp(mac, "00112233445566778899") == 0 && recvBytes[recvIndex] == 'B'));
	return got;
}

// The request read and the close drain wait on poll, never a VBlank, except for a partial request
// that MSG_PEEK leaves unread, which poll would not wait for.
static void netWaits(void) {
	feed("info 0123456789abcdef0123456789abcdef\n00112233445566778899\n", 1000); // a second in
	reset(0, 50);
	polls = vblanks = statusTicks = 0;
	char lines[128], *mac = NULL;
	assert(readRequestLines(7, lines, sizeof(lines), &mac, PREAUTH_INPUT_TIMEOUT_MS) == LINES_OK);
	assert(polls > 0 && vblanks == 0 && statusTicks > 0);
	recvIndex = 0;
	reset(0, 50);
	polls = vblanks = 0;
	pollFails = true;
	assert(readRequestLines(7, lines, sizeof(lines), &mac, PREAUTH_INPUT_TIMEOUT_MS) == LINES_OK);
	assert(vblanks > 0 && vblanks == polls);
	pollFails = false;
	recvAt[recvCount - 1] = 3000; // the MAC line's newline comes late
	recvIndex = 0;
	reset(0, 50);
	polls = vblanks = 0;
	assert(readRequestLines(7, lines, sizeof(lines), &mac, PREAUTH_INPUT_TIMEOUT_MS) == LINES_OK);
	assert(vblanks > 0 && polls > 0);

	recvCount = recvIndex = 0; // a peer that never sends or closes, so the drain hits its bound
	recvEof = false;
	reset(0, 100);
	polls = vblanks = statusTicks = 0;
	closed = -1;
	gracefulClose(7);
	assert(closed == 7 && polls > 0 && vblanks == 0 && statusTicks > 0);
}

// One pre-auth deadline for the request and MAC lines together: each fits alone, not both.
static void preauthDeadline(void) {
	const u64 t = PREAUTH_INPUT_TIMEOUT_MS;
	assert(t <= 10000);
	assert(readLines(4000, t - 1000, 128) == LINES_OK);
	assert(readLines(0, 0, 128) == LINES_OK); // every byte, the body's 'B' too, at once
	assert(readLines(t - 1000, t + 5000, 128) == LINES_GONE); // each fits alone, not together
	assert(readLines(t + 1000, t + 2000, 128) == LINES_GONE);
	assert(readLines(4000, t - 1000, 59) == LINES_OK);		 // 59 bytes hold both lines exactly
	assert(readLines(4000, t - 1000, 58) == LINES_TOO_LONG); // one byte short
}

// The system clock stepping at a read: 36 ms back, as the console's did, and an hour each way.
static const s64 STEPS[] = {-36, 3600000, -3600000};

// The pre-auth read, the drain and the park are timed by the uptime, so a step of the system clock
// either way neither ends one early, cutting a request or a reply's tail, nor stretches it.
static void clockSteps(void) {
	const u64 t = PREAUTH_INPUT_TIMEOUT_MS;
	for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
		recvWallStep = STEPS[i];
		assert(readLines(4000, t - 1000, 128) == LINES_OK);
		recvWallStep = STEPS[i];
		assert(readLines(t - 1000, t + 5000, 128) == LINES_GONE);
		reset(1000, 1); // a peer that half-closes two seconds after the reply
		recvCount = recvIndex = 0;
		recvEof = true;
		recvEofAt = 3000;
		recvWallStep = STEPS[i];
		closed = -1;
		gracefulClose(7);
		assert(closed == -1); // parked: the drain waited for the half-close
		fake.wallMs += STEPS[i];
		closeParked(false);
		assert(closed == -1);
		fake.nowMs += PARKED_CLOSE_MS;
		closeParked(false);
		assert(closed == 7);
	}
	recvEofAt = 0;
	recvEof = false;
}

// Opening the transfer session keeps the request's stats and start time.
static void sessionKeepsStats(void) {
	reset(1000, 0);
	statsReset("");
	gStats.sdNeeded = 4096;
	phaseRecord(PHASE_AM_START, 2.5);
	fake.nowMs = 1500;
	TransferSession session;
	transferSessionInit(&session, 7, 1u << 20, NULL, false);
	fake.nowMs = 2000;
	statsFinish();
	assert(gStats.sdNeeded == 4096 && gStats.phaseN[PHASE_AM_START] == 1);
	assert(gStats.startMs == 1000 && gStats.totalMs == 1000);
}

// A served client is parked, not closed, until the next client or PARKED_CLOSE_MS, because closing
// lost the tail of a long reply. A peer still sending after the drain is closed at once.
static void parkedClose(void) {
	reset(1000, 0);
	recvCount = recvIndex = 0;
	recvEof = true; // the host half-closed as soon as its request was out
	closed = -1;
	gracefulClose(7);
	fake.nowMs += PARKED_CLOSE_MS - 1;
	closeParked(false);
	assert(closed == -1);
	fake.nowMs++;
	closeParked(false);
	assert(closed == 7);
	gracefulClose(8);
	closeParked(true); // the next client connected
	assert(closed == 8);
	closed = -1;
	closeParked(true);
	assert(closed == -1);

	reset(0, 1000);
	feed("a refused body the sender keeps sending", 0);
	gracefulClose(9);
	assert(closed == 9);
	closed = -1;
	closeParked(true);
	assert(closed == -1);
}

// One idle rule: a sender quiet for just under TRANSFER_WIRE_STALL_MS is waited out,
// whether or not the clock stepped back as its last bytes went in.
static void quietSender(void) {
	TransferSession s;
	int calls = 0;
	feed("the first ten bytes, then the rest", 0);
	for (size_t i = 0; i < recvCount; i++) {
		recvAt[i] = i < 10 ? 100000 : 100000 + TRANSFER_WIRE_STALL_MS - 1000;
	}
	reset(100000, 1);
	transferSessionInit(&s, 7, recvCount, NULL, false);
	assert(transferReceive(&s, recvCount, stepBackSink, &calls) && calls == 2);
}

static Result receivePump(void) {
	TransferSession s;
	feed("the body of an install", 0);
	reset(0, 0);
	transferSessionInit(&s, 7, recvCount, NULL, false);
	return transferReceive(&s, recvCount, discardSink, NULL) ? 0 : s.failure;
}

static Result sendPump(void) {
	static unsigned char body[4096];
	TransferBytes bytes = {body, sizeof(body), sizeof(body), 0};
	TransferSession s;
	reset(0, 0);
	transferSessionInit(&s, 7, sizeof(body), NULL, false);
	sendScript[sendCount++] = sizeof(body);
	return transferSend(&s, transferBytes, &bytes) ? 0 : s.failure;
}

// With an import open, a POWER press ends no socket loop or pump. aptMainLoop would follow it to
// HOME with the import open.
static void homeGuard(void) {
	static unsigned char body[4096];
	fake.homeBlocked = fake.powerPressed = true;
	assert(readLines(4000, 9000, 128) == LINES_OK);
	reset(0, 0);
	sendScript[sendCount++] = sizeof(body);
	assert(sendBytes(7, body, sizeof(body)));
	recvCount = recvIndex = 0;
	recvEof = true;
	closed = -1;
	gracefulClose(7); // the drain sees the half-close: parked, not closed
	assert(closed == -1);
	closeParked(true);
	assert(receivePump() == 0 && sendPump() == 0);
	fake.homeBlocked = false;
	assert(readLines(4000, 9000, 128) == LINES_GONE);
	assert(receivePump() == TRANSFER_EXITED && sendPump() == TRANSFER_EXITED);
	reset(0, 0); // and a reply cut short by the exit says so, as the pumps do
	gApp.requestSpent = true;
	assert(!sendBytes(7, body, sizeof(body)) && strcmp(fake.log, "send-exit") == 0);
	reset(0, 0);
	gApp.requestSpent = false; // only a spent request is logged, so a replayed read adds nothing
	assert(!sendBytes(7, body, sizeof(body)) && fake.log[0] == '\0');
	fake.powerPressed = false;
}

// START is read on the pumps' and a reply's clocks, and no read of it waits for a VBlank, because
// that would leave the socket unserved for 16.7 ms.
static void startPolls(void) {
	static unsigned char body[4096];
	TransferBytes bytes = {body, sizeof(body), sizeof(body), 0};
	TransferSession s;
	feed("a body that comes a second in", 1000);
	reset(0, 10);
	statsReset("");
	vblanks = 0;
	transferSessionInit(&s, 7, recvCount, NULL, false);
	assert(transferReceive(&s, recvCount, discardSink, NULL) && gStats.phaseN[PHASE_POLL] >= 3);
	// START ends a receive, a send whose peer takes nothing, and such a reply alike.
	fake.keysDown = KEY_START;
	recvIndex = 0;
	reset(0, 10);
	transferSessionInit(&s, 7, recvCount, NULL, false);
	assert(!transferReceive(&s, recvCount, discardSink, NULL) && s.failure == TRANSFER_CANCELED);
	reset(0, 10);
	transferSessionInit(&s, 7, sizeof(body), NULL, false);
	assert(!transferSend(&s, transferBytes, &bytes) && s.failure == TRANSFER_CANCELED);
	reset(0, 10);
	statusTicks = 0;
	gApp.requestSpent = true;
	assert(!sendBytes(7, body, sizeof(body)) && strcmp(fake.log, "send-canceled") == 0);
	assert(statusTicks > 0);
	reset(0, 10);
	gApp.requestSpent = false; // only a spent request is logged, so a replayed read adds nothing
	assert(!sendBytes(7, body, sizeof(body)) && fake.log[0] == '\0');
	fake.keysDown = 0;
	assert(vblanks == 0);
}

// Starts the clock at 0, a millisecond a read, and holds START for `ms` ms from `from`.
static void tapAt(u64 from, u64 ms) {
	reset(0, 1);
	tapFrom = from;
	tapTo = from + ms;
	scanHeld = 0;
}

// Returns true if it ended, with the event a long press logs, while START was held.
static bool tapCanceled(const char *event) {
	return fake.nowMs >= tapFrom && fake.nowMs < tapTo + 10 && strcmp(fake.log, event) == 0;
}

// A quick tap, START held 60 ms, cancels a receive (a put), a send (a get), a reply, a request's
// wait and a close's drain wherever it falls, as a long press does; the LED keeps its own clock. On
// the idle screen a request's START (at its reply, its lines or its drain) goes on to 3Drop's loop
// as well; a transfer's does not.
static void startTaps(void) {
	static unsigned char body[4096];
	TransferSession s;
	gApp.status = SERVER_READY;
	// Nothing left over from the earlier tests counts here.
	idleStartTaken();
	feed("late", 2000); // every tap is over before it comes
	gApp.requestSpent = true;
	tapAt(0, 0);
	statusTicks = 0;
	transferSessionInit(&s, 7, recvCount, NULL, false);
	assert(transferReceive(&s, recvCount, discardSink, NULL) && fake.nowMs >= 2000);
	assert(statusTicks <= fake.nowMs / STATUS_TICK_MS + 2);
	for (u64 from = 40; from < 1500; from += 37) {
		feed("late", 2000);
		tapAt(from, 60);
		transferSessionInit(&s, 7, recvCount, NULL, false);
		assert(!transferReceive(&s, recvCount, discardSink, NULL));
		assert(tapCanceled("transfer-canceled") &&
			   strcmp(transferFailDetail(s.failure), "canceled") == 0);
		assert(!idleStartTaken());
		TransferBytes bytes = {body, sizeof(body), sizeof(body), 0};
		tapAt(from, 60);
		transferSessionInit(&s, 7, sizeof(body), NULL, false);
		assert(!transferSend(&s, transferBytes, &bytes));
		assert(tapCanceled("transfer-canceled") &&
			   strcmp(transferFailDetail(s.failure), "canceled") == 0);
		assert(!idleStartTaken());
		tapAt(from, 60);
		assert(!sendBytes(7, body, sizeof(body)) && tapCanceled("send-canceled"));
		assert(idleStartTaken());
		tapAt(from, 60);
		assert(readLines(4000, 9000, 128) == LINES_GONE && tapCanceled(""));
		assert(idleStartTaken());
		feed("", 0); // a peer that neither sends nor closes, so only the tap ends the drain
		tapAt(from, 60);
		closed = -1;
		gracefulClose(7);
		assert(closed == 7 && tapCanceled("") && idleStartTaken());
	}
	gApp.requestSpent = false;
	tapAt(0, 0);
}

// The real protocolServe serves the client in `state`, with room for the one reply a request is
// owed. Then `closed` is 7 if its socket was closed, and -1 if it was kept for the next client's
// connect (net.c's gracefulClose).
static void serveClient(Busy state) {
	After none = AFTER_NOTHING;
	closed = -1;
	sent[0] = '\0';
	sendScript[sendCount++] = sizeof(sent);
	protocolServe(7, NULL, 0, &none, state);
}

// The QR flow's waits serve a client meanwhile (main.c's serveBusy). Here that is the client
// a test says is waiting, in the busy state it is given.
static bool clientWaits;
static Busy clientState;
void serveBusy(void) {
	if (clientWaits) {
		clientWaits = false;
		serveClient(clientState);
	}
}

// `key` is tapped from 100 to 300 ms into the clock.
static void tapKey(u32 key) {
	tapKeys = key;
	tapFrom = 100;
	tapTo = 300;
	scanHeld = scanDown = 0;
}

// A client that connects and says nothing, which the next wait serves in `state`. Its request
// holds the wait until the pre-auth allowance runs out (silentPeer times that).
static void clientWaitsIn(Busy state) {
	feed("", 0);
	reset(0, 1);
	clientWaits = true;
	clientState = state;
}

// libctru reports a key down at one scan only, and a request served inside a wait scans for START
// (startPressed) at its own times, so a B it does not act on, and a START that ends it, were gone
// when the wait they were pressed for scanned next. With no request, that scan finds the tap. A QR
// scan runs on the idle status: its START is the wait's, never handed to 3Drop's own loop as well.
static void keysDuringBusyRequest(void) {
	static const u32 STOPS[] = {KEY_B, KEY_START};
	gApp.status = SERVER_READY;
	idleStartTaken(); // clears what the taps of the earlier tests left
	for (size_t i = 0; i < sizeof(STOPS) / sizeof(STOPS[0]); i++) {
		tapKey(STOPS[i]);
		reset(150, 0);
		assert(stopRequested());
		tapKey(STOPS[i]);
		clientWaitsIn(BUSY_AT_SCREEN);
		bool stops = stopRequested();
		// The request was served, and the wait's own scan found no key (START ends the request
		// as it goes down, so that tap is still held). The wait reports what the request kept, and
		// reports it once.
		assert(!clientWaits && scanDown == 0);
		assert(stops);
		assert(!stopRequested());
		assert(!idleStartTaken());
	}
}

// The same at a question (ui.c's uiAsk, as qr.c asks one), whose keys are A, Y and B: a tap of any
// of them while the request is served is the answer. The wait ends after its first look past the
// request, so an answer it missed reads as none.
static void endAfterRequest(void) {
	serveBusy();
	fake.powerPressed = true;
}

static void keysAtQuestion(void) {
	static const u32 ANSWERS[] = {KEY_A, KEY_Y, KEY_B};
	for (size_t i = 0; i < sizeof(ANSWERS) / sizeof(ANSWERS[0]); i++) {
		tapKey(ANSWERS[i]);
		clientWaitsIn(BUSY_AT_SCREEN);
		u32 answer = uiAsk(UI_QR_CONFIRM, "x", KEY_A | KEY_Y | KEY_B, endAfterRequest);
		fake.powerPressed = false;
		// The wait's own scan found no key: the answer is the request's, and it came once.
		assert(!clientWaits && scanDown == 0);
		assert(answer == ANSWERS[i]);
		assert(pressedKeys() == 0);
	}
}

// Outside a busy request START ends the request or transfer that scans for it, and no wait keeps
// it, so one that begins next (a QR scan) does not stop at it. On the idle screen the request's
// START is that screen's too: 3Drop's own loop reads it once (idleStartTaken) and quits. A
// transfer's goes no further, even one that keeps the idle screen (a screenshot), so canceling one
// never quits 3Drop; nor does a request's on a card, which START dismisses.
static void startEndsOnlyItsRequest(void) {
	gApp.status = SERVER_READY;
	tapKey(KEY_START);
	clientWaitsIn(NOT_BUSY);
	serveBusy();
	assert(fake.nowMs < PREAUTH_INPUT_TIMEOUT_MS); // START ended the wait for a request line
	assert(!stopRequested());
	assert(idleStartTaken());
	assert(!idleStartTaken()); // read once
	gApp.status = SERVER_INSTALL_FAILED;
	tapKey(KEY_START);
	clientWaitsIn(NOT_BUSY);
	serveBusy();
	assert(fake.nowMs < PREAUTH_INPUT_TIMEOUT_MS && !idleStartTaken());
	gApp.status = SERVER_READY;
	TransferSession s;
	feed("late", 2000);
	tapAt(40, 60);
	transferSessionInit(&s, 7, recvCount, NULL, false);
	assert(!transferReceive(&s, recvCount, discardSink, NULL) && s.failure == TRANSFER_CANCELED);
	assert(!stopRequested() && !idleStartTaken());
	tapAt(0, 0); // no tap left for the next test
}

// What a busy request may cost the wait that serves it: the allowance, and a close that takes no
// time (a frame at most, for the last look).
#define BUSY_TOTAL_MS (BUSY_PREAUTH_INPUT_TIMEOUT_MS + 100)
// DRAIN_MS is net.c's CLOSE_DRAIN_TIMEOUT_MS, which a test cannot see.
#define DRAIN_MS 5000

// A peer that connects and says nothing, and never closes, held a wait for 15 s on this clock: the
// pre-auth wait, then the close's drain. A busy request is dropped at the busy allowance and closed
// at once, with nothing sent; one served idle keeps its allowance and its drain.
static void silentPeer(void) {
	const u64 allowance = BUSY_PREAUTH_INPUT_TIMEOUT_MS;
	assert(allowance <= 1000);
	clientWaitsIn(BUSY_AT_SCREEN);
	serveBusy();
	assert(fake.nowMs <= BUSY_TOTAL_MS && closed == 7 && sendIndex == 0);
	clientWaitsIn(NOT_BUSY);
	serveBusy();
	assert(fake.nowMs >= PREAUTH_INPUT_TIMEOUT_MS + DRAIN_MS && closed == 7 && sendIndex == 0);
}

// A request that comes in pieces, as on a poor link: its request line is whole at half the time its
// MAC line is. Served in a wait it is answered if the MAC line is whole inside the busy allowance;
// past it, the request is dropped, with nothing sent and no drain. Served idle, the same request is
// answered, because the allowance there is longer. A step of the system clock moves no deadline,
// which is the uptime's.
static void slowRequest(void) {
	static const struct {
		Busy state;
		u64 macDoneMs;
		bool answered;
	} CASES[] = {
		// Whole at once, as a request on the LAN is.
		{BUSY_AT_SCREEN, 0, true},
		// Just inside the busy allowance, and just past it.
		{BUSY_AT_SCREEN, BUSY_PREAUTH_INPUT_TIMEOUT_MS - 100, true},
		{BUSY_AT_SCREEN, BUSY_PREAUTH_INPUT_TIMEOUT_MS + 100, false},
		// Idle, the one that was dropped.
		{NOT_BUSY, BUSY_PREAUTH_INPUT_TIMEOUT_MS + 100, true},
	};
	for (size_t s = 0; s < sizeof(STEPS) / sizeof(STEPS[0]); s++) {
		for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			feedRequest(CASES[i].macDoneMs / 2, CASES[i].macDoneMs);
			recvEof = true; // the host half-closes once its request is out
			recvEofAt = 0;
			recvWallStep = STEPS[s];
			serveClient(CASES[i].state);
			if (CASES[i].answered) {
				// One reply (a refusal: no key here), and the socket kept for the drain.
				assert(sendIndex == 1 && strncmp(sent, "ERR ", 4) == 0 && closed == -1);
			} else {
				assert(sendIndex == 0 && closed == 7 && fake.nowMs <= BUSY_TOTAL_MS);
			}
			closeParked(true);
		}
	}
	recvEof = false;
}

// Both buffers are set before bind; one the stack refuses is logged once and the listener still
// serves.
static void listener(void) {
	unsigned logs = fake.logs;
	assert(isNewest(openListener(DEFAULT_PORT)) && fake.logs == logs);
	assert(windowAtBind == LISTENER_WINDOW_BYTES && sendBufferAtBind == LISTENER_SEND_BUFFER);
	refuseOption = SO_RCVBUF;
	assert(isNewest(openListener(DEFAULT_PORT)) && fake.logs == logs + 1);
	assert(strcmp(fake.log, "rcvbuf-failed") == 0 && fake.logBytes == LISTENER_WINDOW_BYTES);
	refuseOption = SO_SNDBUF;
	assert(isNewest(openListener(DEFAULT_PORT)) && fake.logs == logs + 2);
	assert(strcmp(fake.log, "sndbuf-failed") == 0);
	refuseOption = 0;
	// One the stack takes but holds smaller is not set either, and is logged with no errno.
	readbackCap = 32768;
	fake.log[0] = '\0';
	assert(isNewest(openListener(DEFAULT_PORT)));
	assert(strcmp(fake.log, "sndbuf-failed") == 0 && fake.logResult == 0 &&
		   fake.logBytes == LISTENER_SEND_BUFFER);
	readbackCap = 0;
}

static int bringUpWithin60s(void) {
	int server = -1;
	for (int second = 0; second < 60 && server < 0; second++, fake.nowMs += 1000) {
		server = bringNetworkUp();
	}
	return server;
}

// A lid-closed relaunch: the keep-alive lock comes before the listener, or Wi-Fi stays down until
// the lid opens. A refused lock is logged once on failure and once on recovery, never per retry.
static void lidClosedRelaunch(void) {
	reset(0, 0);
	lidClosed = true;
	unsigned logs = fake.logs;
	wifiBackMs = 0; // Wi-Fi back as soon as the lock is taken
	assert(isNewest(bringNetworkUp()) && fake.logs == logs);
	unlockNetworkKeepalive();
	assert(!exclusive && !locked);
	wifiBackMs = 3000;
	u64 start = fake.nowMs;
	assert(isNewest(bringUpWithin60s()) && fake.nowMs - start <= 5000 && exclusive && locked);
	unsigned asked = enters;
	assert(isNewest(bringNetworkUp()) && enters == asked);

	unlockNetworkKeepalive();
	enterResult = (Result)0xC8A14FF0; // any refusal, for the next 10 retries
	for (int retry = 0; retry < 10; retry++, fake.nowMs += 1000) {
		assert(bringNetworkUp() == -1 && errno == ENETDOWN);
	}
	assert(fake.logs == logs + 1 && strcmp(fake.log, "ndm-lock-failed") == 0 &&
		   fake.logResult == enterResult);
	enterResult = 0;
	assert(isNewest(bringUpWithin60s()) && fake.logs == logs + 2);
	assert(strcmp(fake.log, "ndm-lock-recovered") == 0 && fake.logBytes == 10);

	unlockNetworkKeepalive(); // a refused lock backs out of the exclusive state
	lockResult = (Result)0xC8A14FF0;
	assert(bringNetworkUp() == -1 && !exclusive && strcmp(fake.log, "ndm-lock-failed") == 0);
	lockResult = 0;
	assert(isNewest(bringUpWithin60s()) && exclusive && locked);
	lidClosed = false;

	// With the lid open a refused lock leaves the listener up, so the idle loop asks again each
	// KEEPALIVE_RETRY_MS, logs the refusal once, and stops asking once the lock is held.
	unlockNetworkKeepalive();
	enterResult = (Result)0xC8A14FF0;
	logs = fake.logs;
	assert(isNewest(bringNetworkUp()) && !locked && fake.logs == logs + 1);
	asked = enters;
	retryNetworkKeepalive(); // too soon
	fake.nowMs += KEEPALIVE_RETRY_MS;
	retryNetworkKeepalive();
	assert(enters == asked + 1 && !locked && fake.logs == logs + 1);
	enterResult = 0;
	fake.nowMs += KEEPALIVE_RETRY_MS;
	retryNetworkKeepalive();
	assert(locked && fake.logs == logs + 2 && strcmp(fake.log, "ndm-lock-recovered") == 0);
	fake.nowMs += KEEPALIVE_RETRY_MS;
	retryNetworkKeepalive();
	assert(enters == asked + 2);
}

// Luma's ndm:u workaround: after Rosalina's network functions, a lock through libctru's session
// holds nothing, and the lid takes Wi-Fi down. initNetworkKeepalive uses NDM's own session. A lock
// that is faked anyway is let go, logged once and asked for again; with the lid open, the listener
// serves. Each block between ndmuExit calls is one 3Drop process as far as its ndm:u sessions go,
// each closed once, by the swap or at exit. net.c's keepalive state (failures, retry time) carries
// over from block to block.
static void lockPastLuma(void) {
	reset(0, 0);
	unlockNetworkKeepalive(); // lidClosedRelaunch leaves it held
	locks = 0;				  // NDM's count from here on
	lidClosed = true;
	wifiBackMs = 0;
	rosalinaRan = true;
	unsigned logs = fake.logs;
	initNetworkKeepalive();
	assert(fake.srvAsks == 1 && fake.srvOpens == 1 && fake.srvFlags == 1 && ndmuHandle == 0x6D);
	assert(libctruCloses == 1 && ownCloses == 0 && fake.logs == logs);
	assert(isNewest(bringNetworkUp()) && exclusive && locked && locks == 1 && fake.logs == logs);
	unlockNetworkKeepalive();
	assert(!exclusive && !locked && locks == 0);
	stateResult = (Result)0xC8A06C01; // a state NDM will not tell leaves its lock held
	assert(isNewest(bringNetworkUp()) && locked && fake.logs == logs);
	stateResult = 0;
	unlockNetworkKeepalive();
	stateNone = true; // a lock NDM does not show is let go whole
	assert(bringNetworkUp() == -1 && !exclusive && !locked && locks == 0);
	assert(strcmp(fake.log, "ndm-lock-failed") == 0 && fake.logResult == KEEPALIVE_FAKED);
	stateNone = false;
	assert(isNewest(bringNetworkUp()) && locked && strcmp(fake.log, "ndm-lock-recovered") == 0);
	unlockNetworkKeepalive();
	// When Rosalina holds the lock as 3Drop asks, NDM refuses 3Drop's, which is logged once, and
	// Wi-Fi stays up on Rosalina's. 3Drop takes it at its first retry after Rosalina lets it go.
	otherHolds = true;
	logs = fake.logs;
	assert(isNewest(bringNetworkUp()) && !locked && fake.logs == logs + 1);
	assert(strcmp(fake.log, "ndm-lock-failed") == 0);
	fake.nowMs += KEEPALIVE_RETRY_MS;
	retryNetworkKeepalive();
	assert(!locked && fake.logs == logs + 1);
	otherHolds = false;
	fake.nowMs += KEEPALIVE_RETRY_MS;
	retryNetworkKeepalive();
	assert(locked && strcmp(fake.log, "ndm-lock-recovered") == 0);
	unlockNetworkKeepalive();
	ndmuExit();
	assert(ndmuHandle == 0 && libctruCloses == 1 && ownCloses == 1);

	fake.srvResult = (Result)0xD0401834; // the port is full, so libctru's session stays
	initNetworkKeepalive();
	assert(ndmuHandle == 0x6C && strcmp(fake.log, "ndm-open-failed") == 0 &&
		   fake.logResult == fake.srvResult && fake.logBytes == 1);
	fake.srvResult = 0;
	logs = fake.logs;
	for (int retry = 0; retry < 10; retry++, fake.nowMs += 1000) {
		assert(bringNetworkUp() == -1 && errno == ENETDOWN && !locked && locks == 0);
	}
	assert(fake.logs == logs + 1 && strcmp(fake.log, "ndm-lock-failed") == 0 &&
		   fake.logResult == KEEPALIVE_FAKED);
	lidClosed = false;
	assert(isNewest(bringNetworkUp()) && !locked && locks == 0 && fake.logs == logs + 1);
	unsigned asked = enters;
	fake.nowMs += KEEPALIVE_RETRY_MS;
	retryNetworkKeepalive();
	assert(enters == asked + 1 && locks == 0 && fake.logs == logs + 1);
	ndmuExit();
	assert(libctruCloses == 2 && ownCloses == 1);

	fake.srvUnknown = true; // an sm that does not match the name is never asked to open it
	unsigned opens = fake.srvOpens;
	initNetworkKeepalive();
	assert(fake.srvOpens == opens && ndmuHandle == 0x6C);
	assert(fake.logResult == 0 && fake.logBytes == 0);
	fake.srvUnknown = false;
	ndmuExit();
	fake.srvNoHandle = true; // an open that answers 0 with no handle keeps libctru's session too
	initNetworkKeepalive();
	assert(ndmuHandle == 0x6C && fake.logResult == 0 && fake.logBytes == 1);
	fake.srvNoHandle = false;
	ndmuExit();
	assert(libctruCloses == 4 && ownCloses == 1);

	fake.srvIpcResult = (Result)0xD8E007F7; // the request itself failed; libctru's session stays
	initNetworkKeepalive();
	assert(ndmuHandle == 0x6C && fake.logResult == fake.srvIpcResult && fake.logBytes == 0);
	fake.srvIpcResult = 0;
	ndmuExit();
	// The open's request failed after srv knew the name: its words are still the request's, with
	// no handle in them.
	fake.srvOpenIpcResult = (Result)0xD8E007F7;
	initNetworkKeepalive();
	assert(ndmuHandle == 0x6C && fake.logResult == fake.srvOpenIpcResult && fake.logBytes == 1);
	fake.srvOpenIpcResult = 0;
	rosalinaRan = false; // a reboot, so no workaround until Rosalina's network functions run again
	fake.nowMs += KEEPALIVE_RETRY_MS;
	retryNetworkKeepalive();
	assert(locked && locks == 1 && exclusive); // through libctru's session
	unlockNetworkKeepalive();
	ndmuExit();

	ndmuResult = (Result)0xD8E06406; // libctru's ndm:u would not open, so srv is not asked
	unsigned asks = fake.srvAsks;
	initNetworkKeepalive();
	assert(fake.srvAsks == asks && ndmuHandle == 0 && libctruCloses == 6 && ownCloses == 1);
	ndmuResult = 0;
	lidClosed = false;
}

// benchReq: one request through benchAs, its reply scripted only when `reply`. No reply, on a clock
// that steps stepMs a read, is the read a host retries because it got none.
static const char *benchReq(const char *args, bool reply, u64 stepMs) {
	reset(fake.nowMs, stepMs); // at the test's time: `bench led` reads the clock
	if (reply) {
		sendScript[sendCount++] = sizeof(sent);
	}
	sent[0] = '\0'; // else a run that sent nothing reads as the last one's reply
	benchAs("0123456789abcdef0123456789abcdef", args);
	return sent;
}
static const char *bench(const char *args) {
	return benchReq(args, true, 0);
}
// A set spends a nonce, as every change does: `bench <id> <nonce> <knob> <value>`, using this
// harness's nonce (src/auth.c isn't linked; fakes.c's nonceLive). A read keeps `bench <id> <sub>`.
static const char *benchSet(const char *args) {
	char with[80];
	snprintf(with, sizeof(with), "%s %s", nonceLive(), args);
	return benchReq(with, true, 0);
}

// A knob reports what the stack read back, never what was asked; a refused or misread one puts
// the previous value and listener back.
static void knobs(void) {
	gListener = openListener(DEFAULT_PORT); // as main opens it
	assert(strstr(bench(""), " bench ok ok 00000000 state rcvbuf=49152 sndbuf=65536 "
							 "sendsize=65536 nohash=0 nodisc=0 capture=0 mb_rows=4096\n"));
	assert(strstr(benchSet("rcvbuf 61440"), " bench ok ok 00000000 rcvbuf applied=61440 "
											"default=49152 client=8192\n") &&
		   window == 61440 && gKnobs.rcvBuf == 61440);
	refuseOption = SO_RCVBUF;
	assert(strstr(benchSet("rcvbuf 32768"), " bench rejected refused 00000000 rcvbuf "
											"applied=61440 asked=32768\n") &&
		   gKnobs.rcvBuf == 61440 && strcmp(fake.log, "rcvbuf-failed") == 0);
	refuseOption = 0;
	readbackCap = 98304; // the stack takes 131072 but holds 98304
	assert(strstr(benchSet("sndbuf 131072"), " bench rejected refused 00000000 sndbuf "
											 "applied=65536 asked=131072\n") &&
		   gKnobs.sndBuf == 0 && sendBuffer == LISTENER_SEND_BUFFER);
	readbackCap = 0;
	unsigned opened = sockets;
	// The window stops at 65535 because the console's stack has no window scaling.
	assert(
		strstr(benchSet("rcvbuf 65536"), " bench rejected range 00000000 rcvbuf applied=61440\n") &&
		gKnobs.rcvBuf == 61440 && sockets == opened);
	assert(strstr(benchSet("rcvbuf 65535"), " rcvbuf applied=65535 ") && window == 65535);
	assert(strstr(benchSet("rcvbuf 0"), " rcvbuf applied=49152 default=49152 client=8192\n") &&
		   gKnobs.rcvBuf == 0 && window == LISTENER_WINDOW_BYTES);
	assert(strstr(benchSet("sendsize 16384"), " sendsize applied=16384 default=65536\n") &&
		   effectiveSendSize() == 16384);
	assert(strstr(bench("sendsize"), " sendsize applied=16384 ") && gKnobs.sendSize == 16384);
	assert(strstr(bench("sndbuf"), " sndbuf applied=65536 default=65536 client=8192\n"));
	assert(strstr(benchSet("sendsize 0"), " sendsize applied=65536 ") && gKnobs.sendSize == 0);
	assert(strstr(bench("rcvbuf 1 2"), " bench rejected grammar 00000000\n"));
}

// The screen's address is `unknown`, never 0.0.0.0, until Wi-Fi gives one. After that it is the
// last one SOC gave while it gives none.
static void ipText(void) {
	ipUnknown = true;
	assert(strcmp(getIPText(), "unknown") == 0);
	ipUnknown = false;
	assert(strcmp(getIPText(), "192.0.2.50") == 0);
	ipUnknown = true;
	assert(strcmp(getIPText(), "192.0.2.50") == 0);
	ipUnknown = false;
}

// `bench led` reports the gap a lit pattern went unwritten, counted to now.
// A refused write doesn't count as a write.
static void benchLed(void) {
	u64 t = fake.nowMs;
	fake.mcuInfo[0x0D] = 0xFF;
	fake.mcuInfo[0x0E] = 0x00;
	fake.mcuInfo[0x0F] = 0xAE;
	benchLedWritten((Led){0xFF00FF, true}, 0, t);
	benchLedWritten((Led){0xFF00FF, true}, 0, t + 4000);
	benchLedWritten((Led){0xFF0000, false}, (Result)0xD0401834, t + 4250);
	benchLedWritten((Led){0xFF0000, false}, 0, t + 9000);
	fake.nowMs = t + 10000;
	assert(strstr(bench("led"), " bench ok ok 00000000 led rgb=ff00ae shown=ff0000 breathes=0 "
								"gap_max_ms=5000 writes=3 failed=1"));
	fake.nowMs = t + 30000; // nothing written since, so the lit red has gone 21 s
	fake.mcuReadResult = (Result)0xD8E0806A;
	assert(strstr(bench("led"), " led rgb=- shown=ff0000 breathes=0 gap_max_ms=21000 writes=0 "
								"failed=0"));
	fake.mcuReadResult = 0;
	benchLedWritten(LED_OFF, 0, t + 31000);
	fake.nowMs = t + 40000;
	assert(strstr(bench("led"), " led rgb=ff00ae shown=000000 breathes=0 gap_max_ms=22000 "));
	assert(strstr(bench("led"), " gap_max_ms=0 writes=0 failed=0")); // off: no gap runs
	// A reply the console could not send doesn't start the window over, because the host retries
	// a lost read and the retry must still report the longest gap. One 6 s lit gap, then a read
	// whose reply goes nowhere, on a clock that steps a second a read.
	benchLedWritten((Led){0xFF00FF, true}, 0, t + 41000);
	u64 lost = t + 47000;
	fake.nowMs = lost;
	benchReq("led", false, 1000);
	fake.nowMs = lost; // the reply's own clock reads do not move the test's
	assert(strstr(bench("led"), " gap_max_ms=6000 writes=1 failed=0"));
}

int main(void) {
	ipText();
	listener();
	lidClosedRelaunch();
	lockPastLuma();
	knobs();
	benchLed();
	sendDeadlines();
	quietSender();
	listingSends();
	netWaits();
	preauthDeadline();
	clockSteps();
	sessionKeepsStats();
	parkedClose();
	homeGuard();
	startPolls();
	startTaps();
	keysDuringBusyRequest();
	keysAtQuestion();
	startEndsOnlyItsRequest();
	silentPeer();
	slowRequest();
	junkUnlogged();
	TEST_PASS();
	return 0;
}

// cc: -DTHREEDROP_TEST_VERBS=1 src/bench/hw.c src/bench/bench.c src/bench/capture.c
// cc: src/bench/ledRead.c src/ring.c src/hash.c src/stats.c src/transfer.c src/protocol.c
// cc: src/codec.c src/led.c src/screenshot.c
// Tests the put-hash timeline (src/bench/hw.c) on the fake clock. It checks bench hw's gaps from
// both pumps, the order of a ring's digest calls, writes and reads, and the reset with each
// request's stats.
#include "fakes.h"

#include "hash.h"
#include "knobs.h" // benchRun
#include "ring.h"
#include "stats.h"
#include "transfer.h"

#include <assert.h>
#include <errno.h>
#include <mbedtls/sha256.h>
#include <stdlib.h>
#include <sys/param.h> // MIN
#include <sys/socket.h>
#include <unistd.h>

// On each call the clock jumps stepMs[i] ms, and a call with eagain[i] set takes nothing. A gate
// holds the call numbered gateAt[g], counting from 0 (a gateAt of 0 means no gate), until the
// ring's worker has written gateDone[g] bytes. That way a row's sd_kb is read at the same place in
// the worker on every run, not wherever the threads happened to get to.
static const unsigned char *wire;
static size_t wireLen, wireAt, calls;
static u64 stepMs[16];
static bool eagain[16];
static size_t gateAt[2];
static u64 gateDone[2];
static void script(const unsigned char *bytes, size_t n) {
	wire = bytes;
	wireLen = n;
	wireAt = calls = 0;
	memset(stepMs, 0, sizeof(stepMs));
	memset(eagain, 0, sizeof(eagain));
}
static ssize_t scripted(size_t len) {
	size_t i = calls++;
	for (int g = 0; g < 2; g++) {
		for (int wait = 0; gateAt[g] && i == gateAt[g] && ringDone() < gateDone[g] && wait < 5000;
			 wait++) {
			usleep(1000);
		}
	}
	fake.nowMs += i < 16 ? stepMs[i] : 0;
	if (i < 16 && eagain[i]) {
		errno = EAGAIN;
		return -1;
	}
	size_t n = MIN(wireLen - wireAt, MIN(len, (size_t)65536));
	__atomic_add_fetch(&wireAt, n, __ATOMIC_SEQ_CST); // the worker reads it
	return (ssize_t)n;
}
ssize_t recv(int fd, void *buffer, size_t len, int flags) {
	size_t at = wireAt;
	ssize_t n = scripted(len);
	if (n > 0) {
		memcpy(buffer, wire + at, (size_t)n);
	}
	return n;
}
ssize_t send(int fd, const void *data, size_t len, int flags) {
	return scripted(len);
}

// Works as the fake's write does, but takes only bytes the wire has brought, since a ring writes a
// unit only once it is received. The end of the last write is kept. While held is set, a write
// waits, as a card's first allocation does.
static u64 writtenTo;
static bool held;
static u64 heldUntilMs;
Result FSFILE_Write(Handle h, u32 *written, u64 offset, const void *buffer, u32 size, u32 flags) {
	assert(offset + size <= __atomic_load_n(&wireAt, __ATOMIC_SEQ_CST));
	while (__atomic_load_n(&held, __ATOMIC_SEQ_CST)) {
		usleep(100);
	}
	ssize_t n = pwrite((int)h - 1, buffer, size, (off_t)offset);
	*written = n > 0 ? (u32)n : 0;
	__atomic_store_n(&writtenTo, offset + *written, __ATOMIC_SEQ_CST);
	return n >= 0 ? 0 : (Result)0xC8804464;
}
// Records every engine call: its bytes, plus ringDone() and the file's written end at its start.
#define SHA_MAX 16
static struct {
	unsigned n;
	const u8 *p[SHA_MAX];
	u64 done[SHA_MAX];
	u64 written[SHA_MAX];
} sha;
Result FSUSER_UpdateSha256Context(const void *data, u32 size, u8 *hash) {
	unsigned i = __atomic_fetch_add(&sha.n, 1, __ATOMIC_SEQ_CST);
	assert(((uintptr_t)data & 0xFFF) == 0 && size > 0); // the IPC's pages, as the fake's
	if (i < SHA_MAX) {
		sha.p[i] = data;
		sha.done[i] = ringDone();
		sha.written[i] = __atomic_load_n(&writtenTo, __ATOMIC_SEQ_CST);
	}
	return mbedtls_sha256_ret(data, size, hash, 0);
}
// With sleepMoves set, each sleep moves the clock by its whole milliseconds and ends a hold once
// the clock is past it. It also sleeps for real, because the worker is a real thread: sleeps that
// took no time ran the clock ahead of a worker the scheduler had set aside, past WORKER_STALL_MS.
static bool sleepMoves;
void svcSleepThread(s64 ns) {
	fake.nowMs += sleepMoves ? (u64)ns / 1000000 : 0;
	if (held && fake.nowMs >= heldUntilMs) {
		__atomic_store_n(&held, false, __ATOMIC_SEQ_CST);
	}
	if (sleepMoves) {
		usleep((useconds_t)(ns / 1000));
	}
}

static const char *bench(const char *args) {
	fake.replies = fake.bodyLen = 0;
	fake.body[0] = '\0';
	benchAs("0123456789abcdef0123456789abcdef", args);
	return fake.reply;
}

// got must contain want somewhere, or the test fails and prints both.
static void expectHas(const char *want, const char *got) {
	if (!strstr(got, want)) {
		fprintf(stderr, "testBenchHw: got `%s`, want `%s`\n", got, want);
		abort();
	}
}

// Parses `bench hw`'s lines of one kind: a call's start, end and k, or a gap's quiet, call and end.
typedef struct {
	char kind;
	unsigned long a, b, c;
} Row;
static size_t rows(char kind, Row *out, size_t max) {
	size_t n = 0;
	for (const char *at = fake.body; *at; at = strchr(at, '\n') + 1) {
		Row r;
		assert(sscanf(at, "%c %lu %lu %lu", &r.kind, &r.a, &r.b, &r.c) == 4);
		if (r.kind == kind && n < max) {
			out[n] = r;
		}
		n += r.kind == kind;
	}
	return n;
}
// `bench hw`'s calls in order, by kind.
static const char *callKinds(void) {
	static char kinds[64];
	kinds[0] = '\0';
	for (const char *at = fake.body; *at; at = strchr(at, '\n') + 1) {
		strncat(kinds, at, *at == 'g' || strlen(kinds) == sizeof(kinds) - 1 ? 0 : 1);
	}
	return kinds;
}

// Microseconds within 3 of ms, because the timeline counts ticks / 256 and the fake counts a ms as
// 268111 ticks.
static bool near(unsigned long us, u64 ms) {
	return us + 3 >= ms * 1000 && us <= ms * 1000 + 3;
}

static bool discard(void *context, const unsigned char *buffer, size_t size) {
	return true;
}

// A transferBytes whose second fill is 50 ms long, as a reader behind or deflate makes one.
typedef struct {
	TransferBytes *bytes;
	int *fills;
} SlowFill;
static bool slowFill(void *context, unsigned char *dst, size_t cap, size_t *got, bool *end) {
	SlowFill *f = context;
	fake.nowMs += ++*f->fills == 2 ? 50 : 0;
	return transferBytes(f->bytes, dst, cap, got, end);
}

// bench hw lists every gap of 30 ms or more, and none shorter, on the tick. A gap runs from the
// wire going quiet to the end of the call that ended it; one past the stall bound is also a stall.
static void gaps(void) {
	static unsigned char stream[4 * 65536];
	TransferSession s;
	fake.nowMs = 5000;
	statsReset("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	script(stream, sizeof(stream));
	stepMs[1] = 29; // under the bound: none
	eagain[2] = true;
	stepMs[2] = 10; // quiet 10 ms, then a 20 ms receive
	stepMs[3] = 20;
	stepMs[4] = 150;
	transferSessionInit(&s, 7, sizeof(stream), NULL, false);
	assert(transferReceive(&s, sizeof(stream), discard, NULL));
	assert(gStats.stallN == 1 && gStats.stallMs == 150);
	expectHas(" bench ok ok 00000000 hw req=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
			  "rows=2 calls=0 gaps=2 gap_min_ms=30 ",
			  bench("hw"));
	Row g[2];
	assert(rows('g', g, 2) == 2);
	assert(near(g[0].a, 29) && near(g[0].b, 39) && near(g[0].c, 59));
	assert(near(g[1].a, 59) && near(g[1].b, 59) && near(g[1].c, 209));

	// A send's gap is treated likewise, and a slow fill is not a gap, because its time moves the
	// quiet start.
	fake.nowMs = 9000;
	statsReset("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	script(stream, 3 * 65536);
	stepMs[1] = 40;
	TransferBytes bytes = {stream, 65536, 3 * 65536, 0};
	int fills = 0;
	transferSessionInit(&s, 7, bytes.size, NULL, false);
	assert(transferSend(&s, slowFill, &(SlowFill){&bytes, &fills}));
	expectHas(" hw req=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb rows=1 calls=0 gaps=1 ", bench("hw"));
	assert(rows('g', g, 2) == 1 && near(g[0].a, 50) && near(g[0].b, 50) && near(g[0].c, 90));
}

static void abandon(const RingStats *stats) {
	(void)stats;
}
static void waiting(void *context) {
	(void)context;
}

static unsigned char body[3 * RING_UNIT + 100]; // 4 units, the last of 100 bytes
static Handle file;

// A put's body through the ring to ./sdmc:/hw.bin, as sdPut drives it.
static RingStats ringPut(const char *id) {
	TransferSession s;
	RingStats stats;
	Operation op;
	statsReset(id);
	script(body, sizeof(body));
	writtenTo = 0;
	assert(ringArm(sizeof(body), RING_TO_HANDLE, false, 0) && ringStart(file, true, abandon));
	transferSessionInit(&s, 7, sizeof(body), NULL, true);
	assert(transferReceive(&s, sizeof(body), NULL, NULL));
	assert(ringBodyEnd(true, PHASE_SD_IO, waiting, &s, &stats, &op) && stats.failure == 0);
	return stats;
}

// Ring back-pressure is not a wire gap. The worker's first write is held for 150 ms with the ring
// full, and main's wait on the ring comes out of the quiet time as it does out of waitStart.
static void backPressure(void) {
	size_t n = RING_TIER1_BYTES + 2 * RING_UNIT;
	unsigned char *big = malloc(n);
	TransferSession s;
	RingStats stats;
	Operation op;
	memset(big, 0x5A, n);
	statsReset("aeaeaeaeaeaeaeaeaeaeaeaeaeaeaeae");
	script(big, n);
	held = sleepMoves = true;
	heldUntilMs = fake.nowMs + 150;
	assert(ringArm(n, RING_TO_HANDLE, false, 0) && ringStart(file, false, abandon));
	transferSessionInit(&s, 7, n, NULL, true);
	assert(transferReceive(&s, n, NULL, NULL));
	assert(ringBodyEnd(true, PHASE_SD_IO, waiting, &s, &stats, &op) && stats.failure == 0);
	sleepMoves = false;
	// The hold's 150 waits read 999 us each: the fake has SYSCLOCK_ARM11 / 1000 whole ticks a ms.
	assert(!held && gStats.phaseUs[PHASE_RING_ACQUIRE] >= 149000 && gStats.stallN == 0);
	expectHas(" rows=", bench("hw"));
	expectHas(" gaps=0 ", fake.reply);
	free(big);
}

// A ring put records each write and each unit's digest call: each unit is hashed before any of its
// bytes are written, and the digest is unchanged. A get records its reads, and each unit is hashed
// after its read. The next request's stats start the timeline over.
static void ringCalls(void) {
	for (size_t i = 0; i < sizeof(body); i++) {
		body[i] = (unsigned char)(i * 7u + 1u);
	}
	FS_Path path = fsMakePath(PATH_ASCII, "/hw.bin");
	assert(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), path,
								   FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE, 0) == 0);
	TransferSession s;
	sha.n = 0;
	RingStats stats = ringPut("cccccccccccccccccccccccccccccccc");
	Operation op;
	u8 want[32];
	refDigest(body, sizeof(body), want);
	assert(memcmp(stats.digest, want, 32) == 0 && stats.hashes == 5);
	assert(sha.n == 3); // the short unit and the list are hashed in software
	for (unsigned i = 0; i < sha.n; i++) {
		u64 at = (u64)(sha.p[i] - ringTier1());
		assert(at == (u64)i * RING_UNIT && sha.done[i] <= at && sha.written[i] <= at);
	}
	expectHas(" hw req=cccccccccccccccccccccccccccccccc ", bench("hw"));
	Row d[8], w[8];
	char order[64] = "";
	size_t units = 0, digests = rows('d', d, 8), writes = rows('w', w, 8);
	assert(digests == 4 && writes >= 1 && writes <= 4);
	for (size_t i = 0; i < writes; i++) {
		units += w[i].c;
		assert(w[i].a <= w[i].b);
		// Its units' digest calls, then the write.
		snprintf(order + strlen(order), sizeof(order) - strlen(order), "%.*sw", (int)w[i].c,
				 "dddd");
	}
	assert(units == 4); // the last unit's 100 bytes are a unit of their own
	for (size_t i = 0; i < digests; i++) {
		assert(d[i].c >= 1 && d[i].c <= 4 && d[i].a <= d[i].b);
	}
	assert(strcmp(callKinds(), order) == 0);

	statsReset("dddddddddddddddddddddddddddddddd");
	memset(ringTier1(), 0xA5, sizeof(body)); // a unit hashed before its read would hash these
	script(body, sizeof(body));
	assert(ringArm(sizeof(body), RING_FROM_HANDLE, false, 0) && ringStart(file, true, abandon));
	transferSessionInit(&s, 7, sizeof(body), NULL, true);
	assert(transferSend(&s, NULL, NULL));
	assert(ringBodyEnd(true, PHASE_SD_IO, waiting, &s, &stats, &op));
	assert(memcmp(stats.digest, want, 32) == 0);
	expectHas(" hw req=dddddddddddddddddddddddddddddddd rows=8 calls=8 gaps=0 ", bench("hw"));
	assert(rows('d', d, 8) == 4 && rows('r', w, 8) == 4 && w[0].c == 1);
	assert(strcmp(callKinds(), "rdrdrdrd") == 0);

	statsReset("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
	expectHas(" hw req=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee rows=0 calls=0 gaps=0 ", bench("hw"));
	assert(fake.body[0] == '\0');
}

// One row of `bench mb`, parsed, and the rows it listed.
typedef struct {
	unsigned long wallMs, waitMs, eagain, sdKb;
} MbRow;
static size_t mbRows(MbRow *out, size_t max) {
	size_t n = 0;
	for (const char *at = fake.body; *at && n < max; at = strchr(at, '\n') + 1, n++) {
		assert(sscanf(at, "%lu %lu %lu %lu", &out[n].wallMs, &out[n].waitMs, &out[n].eagain,
					  &out[n].sdKb) == 4);
	}
	return n;
}

// `bench rx`'s account of a receive the script fixes: a megabyte in 16 calls of 64 KiB. The
// second call finds nothing (EAGAIN) after a 10 ms step, and the first and the third take 3 and
// 5 ms. The fake's millisecond is 268111 ticks, a microsecond short of the real one, so each
// timed call reads a microsecond under (2999, 4999, 9999), and a sum in whole milliseconds reads a
// millisecond under: busy 2999 + 4999 us is 7, idle 9. Its `mb` row then has the wait (wait_ms is
// the idle phase, never busy's 7), the one EAGAIN, and no ring behind it (sd_kb 0).
static void rxAccount(void) {
	static unsigned char stream[1u << 20];
	MbRow row[2];
	fake.nowMs = 20000;
	script(stream, sizeof(stream));
	stepMs[0] = 3;
	eagain[1] = true;
	stepMs[1] = 10;
	stepMs[2] = 5;
	expectHas(" bench ok ok 00000000 rx bytes=1048576 ms=18 busy_ms=7 idle_ms=9 calls=16 eagain=1",
			  bench("rx 1"));
	expectHas(" mb req=0123456789abcdef0123456789abcdef dir=rx rows=1 ", bench("mb"));
	assert(mbRows(row, 2) == 1);
	assert(row[0].wallMs == 18 && row[0].waitMs == 9 && row[0].eagain == 1 && row[0].sdKb == 0);
}

// A put's `mb` rows, with the worker held at the same place for each: sd_kb is the ring's written
// bytes in thousands (not 1024), one unit by the second megabyte and two by the third. The first
// row is not pinned: its unit completes in the call that crosses it, and its write races the row.
static void putRows(void) {
	MbRow row[4];
	gateAt[0] = 16; // unit 1 is complete, and written, before any call that comes after it
	gateDone[0] = RING_UNIT;
	gateAt[1] = 32;
	gateDone[1] = 2 * (u64)RING_UNIT;
	(void)ringPut("f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0");
	gateAt[0] = gateAt[1] = 0;
	expectHas(" mb req=f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0 dir=rx rows=3 ", bench("mb"));
	assert(mbRows(row, 4) == 3);
	assert(row[1].sdKb == 1048 && row[2].sdKb == 2097 && row[1].waitMs == 0 && row[1].eagain == 0);
}

// More calls and gaps than the arrays hold. `rows` counts the lines sent (the arrays' size), and
// `calls` and `gaps` count what was recorded, so a full array shows.
static void hwFull(void) {
	statsReset("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1");
	u64 base = svcGetSystemTick();
	TickCounter call = {.elapsed = 0, .reference = base + SYSCLOCK_ARM11}; // a gap of a second
	for (int i = 0; i < 2049; i++) {
		benchCall(CALL_WRITE, base, base, 1);
	}
	for (int i = 0; i < 3073; i++) {
		benchGap(base, &call);
	}
	expectHas(" hw req=a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1 rows=5120 calls=2049 gaps=3073 "
			  "gap_min_ms=30 ",
			  bench("hw"));
}

// A put that may overflow past tier 1 gets tier 2 behind it, so `stats` says its ring is 24 MiB and
// 16 MiB (the capacity bench.sh checks an install of a large CIA against), not tier 1's alone.
static void overTier1(void) {
	size_t n = RING_TIER1_BYTES + 2 * RING_UNIT;
	unsigned char *big = malloc(n);
	TransferSession s;
	RingStats stats;
	Operation op;
	char text[2048];
	memset(big, 0x5A, n);
	statsReset("b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2");
	script(big, n);
	assert(ringArm(n, RING_TO_HANDLE, true, 0) && ringStart(file, false, abandon));
	transferSessionInit(&s, 7, n, NULL, true);
	assert(transferReceive(&s, n, NULL, NULL));
	assert(ringBodyEnd(true, PHASE_SD_IO, waiting, &s, &stats, &op) && stats.failure == 0);
	ringRelease();
	assert(statsFormat(text, sizeof(text)));
	expectHas(" ring_cap=41943040 ", text);
	free(big);
}

// A count typed with a leading zero is the decimal it spells, never octal: probe.sh hands the
// console `file 010` as typed. The fake write takes only bytes the wire has brought.
static void typedCount(void) {
	wireAt = wireLen = 16u << 20;
	expectHas(" bench ok ok 00000000 file bytes=10485760 ", bench("file 010"));
}

int main(void) {
	gaps();
	ringCalls();
	backPressure();
	rxAccount();
	putRows();
	hwFull();
	overTier1();
	typedCount();
	FSFILE_Close(file);
	TEST_PASS();
	return 0;
}

// cc: src/protocol.c src/auth.c src/stats.c src/titles.c src/install.c src/cmdFs.c
// cc: src/hash.c src/cia.c src/transfer.c src/ring.c src/codec.c src/path.c src/sdXfer.c
// cc: src/screenshot.c src/sysinfo.c src/hbldr.c src/led.c src/layeredFs.c -lz
// Tests the whole request path with a real signing key: the unsigned refusals (ERR auth is only for
// a MAC that does not verify), the canonical split, the central nonce spend, and every verb.
#include "fakes.h"

#include "codec.h"
#include "stats.h"

// Includes fs.c with its appendLog renamed, so the appendLog functions below see every log line and
// what `stats` held at that moment.
#define appendLog fsAppendLog
#define appendLogTail fsAppendLogTail
#include "fs.c"
#undef appendLog
#undef appendLogTail
static char logged[1024]; // the events since a test last cleared it, one per line
// The text after the bytes on the last logged line: a request's times and uptime, or nothing.
static char loggedTail[STATS_TAIL_MAX];
static u64 loggedTotalMs; // total_ms as the last line was written
static bool logToCard;	  // logCap: each line goes to 3drop.log too
void appendLogTail(const char *event, Result result, u64 bytes, const char *tail) {
	fakeLog(event, result, bytes);
	appendf(logged, sizeof(logged), "%s\n", event);
	snprintf(loggedTail, sizeof(loggedTail), "%s", tail);
	loggedTotalMs = gStats.totalMs;
	if (logToCard) {
		fsAppendLogTail(event, result, bytes, tail);
	}
}
void appendLog(const char *event, Result result, u64 bytes) {
	appendLogTail(event, result, bytes, "");
}

#include "auth.h"
#include "ciaBuild.h"
#define APP_VERSION "test" // the real one comes from tools/build.sh, which no test runs
#include "cmd.c"
#include "led.h"
#include "net.h"
#include "sdXfer.h"

#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static const char ID[] = "0123456789abcdef0123456789abcdef";
// The epoch that v1 ships. Every reply names it after its nonce (protocol.h PROTOCOL_EPOCH).
#define PROTO_TOKEN "proto=1 "

// zlib allocates its state from the regular heap, which the console may not have to spare. While
// this is set, both init functions answer the way zlib does when it is out of memory. Otherwise
// they call libz's own versions, found past this program's.
static bool zlibNoMemory;
static unsigned zlibLive;
// The real zlib runs, observed at its API; its compressed bytes are never replaced.
static bool gzipWatch, gzipOverlap, gzipCancel, gzipParamsFail;
static pthread_t gzipCaller;
static unsigned gzipOffThread, gzipStored, gzipResumed, gzipInSend;
static bool gzipSending, gzipSent;
static uLong gzipStoredAt, gzipResumedAt;

int deflate(z_streamp z, int flush) {
	int (*libz)(z_streamp, int);
	*(void **)&libz = dlsym(RTLD_NEXT, "deflate");
	if (gzipWatch && !pthread_equal(pthread_self(), gzipCaller)) {
		__atomic_add_fetch(&gzipOffThread, 1, __ATOMIC_SEQ_CST);
		if (gzipOverlap && z->total_out >= 65536 && !__atomic_load_n(&gzipSent, __ATOMIC_ACQUIRE)) {
			for (unsigned i = 0; i < 5000 && !__atomic_load_n(&gzipSending, __ATOMIC_ACQUIRE);
				 i++) {
				usleep(1000);
			}
			if (__atomic_load_n(&gzipSending, __ATOMIC_ACQUIRE)) {
				int result = libz(z, flush);
				__atomic_add_fetch(&gzipInSend, 1, __ATOMIC_RELEASE);
				return result;
			}
		}
	}
	return libz(z, flush);
}

int deflateParams(z_streamp z, int level, int strategy) {
	int (*libz)(z_streamp, int, int);
	*(void **)&libz = dlsym(RTLD_NEXT, "deflateParams");
	if (gzipWatch) {
		if (level == 0) {
			if (gzipStored++ == 0) {
				gzipStoredAt = z->total_in;
			}
		} else if (gzipResumed++ == 0) {
			gzipResumedAt = z->total_in;
		}
		if (gzipParamsFail) {
			return Z_MEM_ERROR;
		}
	}
	return libz(z, level, strategy);
}

int deflateEnd(z_streamp z) {
	int (*libz)(z_streamp);
	*(void **)&libz = dlsym(RTLD_NEXT, "deflateEnd");
	int result = libz(z);
	__atomic_sub_fetch(&zlibLive, 1, __ATOMIC_SEQ_CST);
	return result;
}

int deflateInit2_(z_streamp strm, int level, int method, int windowBits, int memLevel, int strategy,
				  const char *version, int size) {
	int (*libz)(z_streamp, int, int, int, int, int, const char *, int);
	*(void **)&libz = dlsym(RTLD_NEXT, "deflateInit2_");
	int result = zlibNoMemory
					 ? Z_MEM_ERROR
					 : libz(strm, level, method, windowBits, memLevel, strategy, version, size);
	if (result == Z_OK) {
		__atomic_add_fetch(&zlibLive, 1, __ATOMIC_SEQ_CST);
	}
	return result;
}
int inflateInit2_(z_streamp strm, int windowBits, const char *version, int size) {
	int (*libz)(z_streamp, int, const char *, int);
	*(void **)&libz = dlsym(RTLD_NEXT, "inflateInit2_");
	return zlibNoMemory ? Z_MEM_ERROR : libz(strm, windowBits, version, size);
}

#define OUT_MAX (32u << 20) // what the host receives in one request
static unsigned char *out;
static size_t outLen;
static u64 sendDelayMs; // the next send takes this long
ssize_t send(int fd, const void *data, size_t len, int flags) {
	if (gzipWatch && !__atomic_load_n(&gzipSent, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&gzipSending, true, __ATOMIC_RELEASE);
		if (gzipOverlap) {
			for (unsigned i = 0; i < 5000 && !__atomic_load_n(&gzipInSend, __ATOMIC_ACQUIRE); i++) {
				usleep(1000);
			}
		}
		__atomic_store_n(&gzipSent, true, __ATOMIC_RELEASE);
		__atomic_store_n(&gzipSending, false, __ATOMIC_RELEASE);
		if (gzipCancel) {
			__atomic_add_fetch(&fake.nowMs, START_POLL_MS, __ATOMIC_SEQ_CST);
		}
	}
	fake.nowMs += sendDelayMs;
	sendDelayMs = 0;
	if (fake.bodySendFails) {
		errno = ECONNRESET;
		return -1;
	}
	assert(outLen + len <= OUT_MAX);
	memcpy(out + outLen, data, len);
	outLen += len;
	return (ssize_t)len;
}
// Past the end of `in` the sender has closed. The system clock steps back once, at stepBackAt.
static const unsigned char *in;
// unplugAt is where the charger is unplugged.
static size_t inLen, inAt, stepBackAt = SIZE_MAX, unplugAt = SIZE_MAX;
static bool stepBackQuiet;
static u64 recvDelayMs; // the next receive takes this long
ssize_t recv(int fd, void *buffer, size_t len, int flags) {
	fake.nowMs += recvDelayMs;
	recvDelayMs = 0;
	if (inAt >= unplugAt) {
		osSharedConfigFake.led_battery &= (u8)~1u;
		unplugAt = SIZE_MAX;
	}
	if (inAt >= stepBackAt) {
		fake.wallMs -= 36;
		stepBackAt = SIZE_MAX;
		if (stepBackQuiet) {
			errno = EAGAIN; // the deadlines run next, across the step
			return -1;
		}
	}
	size_t n = inLen - inAt < len ? inLen - inAt : len;
	// memcpy can't take NULL, even for zero bytes, and `in` is NULL until the first feed().
	if (n) {
		memcpy(buffer, in + inAt, n);
	}
	inAt += n;
	return (ssize_t)n;
}
// Sets what the peer sends after the next request's line.
static void feed(const unsigned char *body, size_t length) {
	in = body;
	inLen = length;
	inAt = 0;
}
// On aptMainLoop's homeAt-th call after homeAt is set (0 means never), the user is in HOME with the
// console suspended for longer than the wire may stall.
static unsigned homeAt;
bool aptMainLoop(void) {
	if (homeAt && --homeAt == 0) {
		fake.nowMs += TRANSFER_WIRE_STALL_MS + 1000;
	}
	return !fake.powerPressed;
}
// The first START check waits, in real time, until readTo reaches readAhead, so the reader is as
// far ahead as the console's.
static unsigned progressTicks, cancelAtTick, startPolls, cancelAtPoll;
static u64 readTo, readAhead;
void uiProgressTick(void) {
	progressTicks++;
}
// A put's file row goes up before its body and comes down after its publish. Its first status
// change takes statusDelayMs[0] and its second takes statusDelayMs[1], which puts time before the
// body and after it.
static u64 statusDelayMs[2];
void appSetStatus(ServerStatus status) {
	gApp.status = status;
	fake.nowMs += statusDelayMs[0];
	statusDelayMs[0] = statusDelayMs[1];
	statusDelayMs[1] = 0;
}
bool startPressed(void) {
	for (int i = 0; readAhead && i < 5000 && __atomic_load_n(&readTo, __ATOMIC_ACQUIRE) < readAhead;
		 i++) {
		usleep(1000);
	}
	readAhead = 0;
	startPolls++;
	return (gzipCancel && __atomic_load_n(&gzipSent, __ATOMIC_ACQUIRE)) ||
		   (cancelAtTick != 0 && progressTicks >= cancelAtTick) ||
		   (cancelAtPoll != 0 && startPolls >= cancelAtPoll);
}
// A read waits out the fake card's hold, as FSFILE_Write does. When stuckStepMs is set, the clock
// starts stepping once the read is waiting, so a reader thread that starts late can't record its
// stall late. With stuckForDeflate, the clock starts only after deflate's thread is also sleeping,
// waiting on the reader, so a gzip get's waits are the ring's whenever the scheduler runs that
// thread.
static u64 readFailAt = UINT64_MAX, stuckStepMs, readMs; // readMs: how far a read moves the clock
static bool stuckForDeflate;
static unsigned offMainSleeps; // sleeps off gzipCaller's thread, which in a get are only deflate's
void svcSleepThread(s64 ns) {
	(void)ns;
	if (!pthread_equal(pthread_self(), gzipCaller)) {
		__atomic_add_fetch(&offMainSleeps, 1, __ATOMIC_SEQ_CST);
	}
}
Result FSFILE_Read(Handle file, u32 *read, u64 offset, void *buffer, u32 size) {
	while (fake.cardHeld && stuckForDeflate && !__atomic_load_n(&offMainSleeps, __ATOMIC_SEQ_CST)) {
		usleep(100);
	}
	fake.stepMs = fake.cardHeld && stuckStepMs ? stuckStepMs : fake.stepMs;
	__atomic_add_fetch(&fake.nowMs, readMs, __ATOMIC_SEQ_CST);
	while (fake.cardHeld) {
		usleep(100);
	}
	__atomic_add_fetch(&fake.fsCalls, 1, __ATOMIC_SEQ_CST);
	ssize_t n = offset + size > readFailAt ? -1 : pread((int)file - 1, buffer, size, (off_t)offset);
	*read = n > 0 ? (u32)n : 0;
	__atomic_store_n(&readTo, offset + *read, __ATOMIC_RELEASE);
	return n >= 0 ? 0 : (Result)0xC8804464;
}
// The GPU refuses every frame while this is set, so a screenshot captures nothing.
static bool gpuRefuses;
bool C3D_FrameBegin(u8 flags) {
	return !gpuRefuses;
}
#define ZERO_DIGEST "u:0000000000000000000000000000000000000000000000000000000000000000"
static char line[REQUEST_LINE_MAX + 8], macHex[MAC_HEX + 2];
static LinesRead lines = LINES_OK;
static After after;
static u64 readDelayMs; // the next request takes this long to come

LinesRead readRequestLines(int client, char *buf, size_t cap, char **second, u64 timeoutMs) {
	size_t n = strlen(line), m = strlen(macHex);
	fake.nowMs += readDelayMs;
	readDelayMs = 0;
	if (lines == LINES_OK && n + 1 + m + 1 > cap) {
		return LINES_TOO_LONG;
	}
	memcpy(buf, line, n + 1);
	*second = memcpy(buf + n + 1, macHex, m + 1);
	return lines;
}

static Busy busyNow; // anything but NOT_BUSY makes cmdServeBusy serve, as main.c's serveBusy does

static bool serve(void) {
	fake.replies = fake.bodyLen = fake.fsCalls = 0;
	fake.body[0] = '\0';
	outLen = 0;
	after = AFTER_NOTHING;
	if (busyNow != NOT_BUSY) {
		cmdServeBusy(7, busyNow);
		return true;
	}
	return cmdServe(7, &after);
}

// Serves `line`, signed with the tests' key.
static void serveSigned(void) {
	u8 mac[MAC_SIZE];
	assert(macRequest(mac, line));
	hexEncode(macHex, mac, sizeof(mac));
	assert(serve() && fake.replies >= 1);
}

// Every signed line's MAC is checked, a reply's and a trailer's alike.
static const char *request(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	u8 mac[MAC_SIZE];
	char hex[MAC_HEX + 1];
	serveSigned();
	if (strncmp(fake.reply, "response ", 9) != 0) {
		return fake.reply;
	}
	macResponse(mac, line, fake.reply);
	hexEncode(hex, mac, sizeof(mac));
	assert(strcmp(fake.replyMac, hex) == 0);
	if (strncmp(fake.last, "trailer ", 8) == 0) {
		macTrailer(mac, line, fake.reply, fake.last);
		hexEncode(hex, mac, sizeof(mac));
		assert(strcmp(fake.lastMac, hex) == 0);
	}
	// Every signed reply carries the nonce, and it is the live one as the reply left. The protocol
	// epoch follows the nonce in every reply, so a host can check each exchange without an exchange
	// of its own.
	const char *nonce = fake.reply + 9 + REQUEST_ID_SIZE * 2 + 1;
	assert(strncmp(nonce, nonceLive(), NONCE_HEX) == 0);
	const char *epoch = nonce + NONCE_HEX + 1;
	assert(strncmp(epoch, PROTO_TOKEN, strlen(PROTO_TOKEN)) == 0);
	return epoch + strlen(PROTO_TOKEN);
}

// Expects a reply that starts with want. What follows it (a digest, a nonce) is checked elsewhere
// or can be anything.
static void expectPrefix(const char *want, const char *got) {
	if (strncmp(got, want, strlen(want)) != 0) {
		fprintf(stderr, "testDispatch: `%s` answered `%s`, want `%s...`\n", line, got, want);
		abort();
	}
}

// Expects the reply to be exactly want, so every field of it is pinned.
static void expectIs(const char *want, const char *got) {
	if (strcmp(got, want) != 0) {
		fprintf(stderr, "testDispatch: `%s` answered `%s`, want `%s`\n", line, got, want);
		abort();
	}
}

// Expects a whole put's commit reply for these bytes: their size and digest, and every field the
// host checks.
static void expectCommitted(const unsigned char *data, size_t size, const char *got) {
	char want[160];
	snprintf(want, sizeof(want), "put committed ok 00000000 %zu %s", size,
			 refDigestText(data, size));
	expectIs(want, got);
}

// Expects the last body's trailer, for the request that `request()` sent under ID, to start with
// rest.
static void expectTrailer(const char *rest) {
	char want[200];
	snprintf(want, sizeof(want), "trailer %s %s", ID, rest);
	expectPrefix(want, fake.last);
}

// Expects a whole get's trailer: the digest of these bytes and the bytes the body took on the wire,
// every field pinned.
static void expectGetTrailer(const void *data, size_t size) {
	char want[200];
	snprintf(want, sizeof(want), "trailer %s get ok ok 00000000 %s %zu", ID,
			 refDigestText(data, size), outLen);
	expectIs(want, fake.last);
}

// Returns a copy, because a spend replaces the live nonce in place.
static const char *live(void) {
	static char nonce[NONCE_HEX + 1];
	snprintf(nonce, sizeof(nonce), "%s", nonceLive());
	return nonce;
}

// A refused mutation touches no SD file and keeps its nonce.
static void refused(const char *detail, const char *verb, const char *fields) {
	char want[64];
	const char *nonce = live();
	snprintf(want, sizeof(want), "%s rejected %s ", verb, detail);
	expectPrefix(want, request("%s %s %s %s", verb, ID, nonce, fields));
	assert(strcmp(nonceLive(), nonce) == 0);
}

// Refused before any FS call is made.
static void gated(const char *detail, const char *verb, const char *fields) {
	refused(detail, verb, fields);
	assert(fake.fsCalls == 0);
}

// Creates or replaces a whole file as the test needs it, on the card, in a save or in an extdata.
static void writeFile(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	assert(f && fwrite(data, 1, size, f) == size && fclose(f) == 0);
}

// Fills p with bytes that gzip can't shrink; the same seed gives the same bytes every run.
static void fillRandom(unsigned char *p, size_t n, u32 seed) {
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		p[i] = (unsigned char)(seed >> 16);
	}
}

// Fills p with log lines, which gzip shrinks to almost nothing.
static void fillLog(unsigned char *p, size_t n) {
	static const char LINE[] = "three drop log line 0123456789\n";
	for (size_t i = 0; i < n; i++) {
		p[i] = (unsigned char)LINE[i % (sizeof(LINE) - 1)];
	}
}

static void intake(void) {
	expectIs("error rejected unsupported 00000000", request("nosuch %s", ID));
	expectIs("error rejected grammar 00000000", request("nosuch %s x", ID));
#if !THREEDROP_TEST_VERBS
	// bench is not a release verb.
	expectPrefix("error rejected unsupported ", request("bench %s", ID));
#endif
	expectPrefix("error rejected unsupported ", request("ls %s", ID));
	expectPrefix("error rejected grammar ", request("ls %s / d x", ID));
	expectPrefix("error rejected grammar ",
				 request("ls %s  /a", ID)); // a doubled space is not canonical
	const char *nonce = live();
	expectPrefix("error rejected grammar ", request("mkdir %s %s  /a", ID, nonce));
	assert(strcmp(nonceLive(), nonce) == 0);
	// The longest line read is REQUEST_LINE_MAX less its NUL, which is wire.sh's REQUEST_LINE_MAX.
	static char path[REQUEST_LINE_MAX];
	size_t room = REQUEST_LINE_MAX - 1 - strlen("ls  ") - 2 * REQUEST_ID_SIZE;
	memset(path, 'a', sizeof(path));
	path[0] = '/';
	path[room] = '\0';
	// The line is read, but the path is over SD_PATH_MAX.
	expectPrefix("ls rejected path ", request("ls %s %s", ID, path));
	path[room] = 'a';
	path[room + 1] = '\0';
	expectIs("ERR malformed", request("ls %s %s", ID, path));
	// The widest request, a rename of two of the longest paths in hex, is read and answered. Those
	// paths are 255 bytes, which only wider characters reach within SD_PATH_UNITS.
	char longest[SD_PATH_MAX], hex[2 * SD_PATH_MAX], paths[4 * SD_PATH_MAX + 16];
	longest[0] = '/';
	for (size_t i = 1; i + 1 < SD_PATH_MAX; i += 2) {
		memcpy(longest + i, "\xc3\xa9", 2);
	}
	longest[SD_PATH_MAX - 1] = '\0';
	hexEncode(hex, (const u8 *)longest, SD_PATH_MAX - 1);
	snprintf(paths, sizeof(paths), "hex:%s hex:%s", hex, hex);
	refused("notfound", "rename", paths);
	expectIs("ERR malformed", request("stats"));
	expectIs("ERR malformed", request("stats 0123"));
	// More than MAX_FIELDS fields is not canonical, and the split goes no further than the fields
	// it holds.
	char fields[160] = "";
	for (int i = 0; i < 40; i++) {
		strcat(fields, " f");
	}
	expectIs("error rejected grammar 00000000", request("stats %s%s", ID, fields));
	request("stats %s", ID);
	macHex[0] = macHex[0] == '0' ? '1' : '0'; // a MAC that does not verify
	assert(!serve() && strcmp(fake.reply, "ERR auth") == 0);
	macHex[0] = 'A'; // not the one spelling of a MAC, so this is framing, not a wrong key
	assert(serve() && strcmp(fake.reply, "ERR malformed") == 0);
	lines = LINES_TOO_LONG;
	assert(serve() && strcmp(fake.reply, "ERR malformed") == 0);
	lines = LINES_GONE; // the peer left, so there is nothing to answer and no AUTH_FAILED
	assert(serve() && fake.replies == 0);
	lines = LINES_OK;
}

// A reply too long for REPLY_MAX is refused whole, never cut short under a valid MAC.
static void replyNeverCut(void) {
	static char wide[REPLY_MAX];
	char want[200];
	memset(wide, 'x', sizeof(wide) - 1);
	VerbContext c = {.client = 7, .request = "stats 0123456789abcdef0123456789abcdef", .id = ID};
	c.verb = &VERBS[0];
	fake.replies = 0;
	assert(!reply(&c, OUTCOME_COMMITTED, "ok", 0, " %s", wide));
	snprintf(want, sizeof(want),
			 "response %s %s " PROTO_TOKEN "error rejected response-too-long 00000000", ID,
			 nonceLive());
	assert(strcmp(fake.reply, want) == 0);
	// A trailer too long for TRAILER_MAX is not sent, so the host fails closed, and it is logged.
	fake.replies = 0;
	replyTrailer(&c, OUTCOME_COMMITTED, "ok", 0, " %s", wide + REPLY_MAX - TRAILER_MAX);
	assert(fake.replies == 0 && strcmp(fake.log, "trailer-too-long") == 0);
}

// A spent nonce is refused on replay, and `stats op` keeps the last mutation's reply; screenshot
// is a mutation too, so a captured one cannot be replayed.
static void spend(void) {
	char mkdir[128];
	snprintf(mkdir, sizeof(mkdir), "mkdir %s %s /made/deep", ID, live());
	expectIs("mkdir committed ok 00000000", request("%s", mkdir));
	struct stat st;
	assert(stat("sdmc:/made/deep", &st) == 0 && S_ISDIR(st.st_mode));
	expectIs("mkdir rejected challenge 00000000", request("%s", mkdir));
	// The replay was refused before anything happened, so the receipt is still the mkdir's. The
	// release name comes before it, so a host reads the receipt to the reply's end.
	expectPrefix("stats ok ok 00000000 ver=52 rel=test op 0123456789abcdef0123456789abcdef mkdir "
				 "committed ok",
				 request("stats %s op", ID));
	expectIs("screenshot committed ok 00000000 1974 raw",
			 request("screenshot %s %s top", ID, live()));
	expectPrefix("stats ok ok 00000000 ver=52 rel=test op 0123456789abcdef0123456789abcdef "
				 "screenshot committed ok",
				 request("stats %s op", ID));
	gApp.installedTitleVersion = -1; // AM could not say at boot
	expectPrefix("stats ok ok 00000000 ver=- rel=test op ", request("stats %s op", ID));
	gApp.installedTitleVersion = 52;
}

// Every FAT spelling of a key file is `protected`, and a trailing dot or space is not a path, on
// every path verb, before any SD call or spend.
static void pathGate(void) {
	static const char *const KEYS[] = {
		"/3ds/3drop/key.txt",
		"/3ds/3drop/key.cache",
		"sdmc:/3ds/3drop/key.txt",
		"/3DS/3Drop/KEY.TXT",
		"/3ds/3drop/KEY~1.TXT",
		"/3ds/3drop",
		"/3ds",
		"hex:2f3364732f3364726f702f6b65792e747874", // hex: key.txt
		"/3ds/3drop/key.cache/x",					// under a key name
	};
	static const char *const NOT_PATHS[] = {
		"/3ds./3drop/key.txt",
		"hex:2f3364732f3364726f702f6b65792e74787420", // a trailing space
		"/3DS/3DROP..",
		"/",
	};
	char fields[160];
	const size_t keys = sizeof(KEYS) / sizeof(KEYS[0]);
	for (size_t i = 0; i < keys + sizeof(NOT_PATHS) / sizeof(NOT_PATHS[0]); i++) {
		const char *p = i < keys ? KEYS[i] : NOT_PATHS[i - keys];
		const char *detail = i < keys ? "protected" : "path";
		snprintf(fields, sizeof(fields), "%s 4 " ZERO_DIGEST, p);
		gated(detail, "put", fields);
		gated(detail, "delete", p);
		gated(detail, "mkdir", p);
		snprintf(fields, sizeof(fields), "%s /x", p);
		gated(detail, "rename", fields);
		snprintf(fields, sizeof(fields), "/x %s", p);
		gated(detail, "rename", fields);
		char want[32];
		snprintf(want, sizeof(want), "get rejected %s ", detail);
		expectPrefix(want, request("get %s %s", ID, p));
		assert(fake.fsCalls == 0);
		snprintf(want, sizeof(want), "hash rejected %s ", detail);
		expectPrefix(want, request("hash %s %s", ID, p));
		assert(fake.fsCalls == 0);
	}
	// A write treats a look-alike of the key's path as the key (a non-ASCII character stands for
	// any), so no folding by FS can turn a look-alike into the key. A read compares ASCII names
	// only (pathProtected).
	static const char *const LOOKALIKES[] = {
		"hex:2f3364c5bf2f3364726f702f6b65792e747874", // /3dſ/3drop/key.txt, U+017F
		"hex:2f3364732f336472d0be702f6b65792e747874", // /3ds/3drоp/key.txt, Cyrillic о
	};
	for (size_t i = 0; i < sizeof(LOOKALIKES) / sizeof(LOOKALIKES[0]); i++) {
		snprintf(fields, sizeof(fields), "%s 4 " ZERO_DIGEST, LOOKALIKES[i]);
		gated("protected", "put", fields);
		gated("protected", "delete", LOOKALIKES[i]);
		gated("protected", "mkdir", LOOKALIKES[i]);
		snprintf(fields, sizeof(fields), "%s /x", LOOKALIKES[i]);
		gated("protected", "rename", fields);
		snprintf(fields, sizeof(fields), "/x %s", LOOKALIKES[i]);
		gated("protected", "rename", fields);
	}
	// "title:" is matched on the raw field only. Hex-encoded, it is a path and never an uninstall.
	fake.installed = true;
	refused("path", "delete", "hex:7469746c653a30303034303030303066383030313030");
	assert(fake.titleDeletes == 0);
	// A CIA prefix of anything from 1 byte up to CIA_MAX_PREFIX_SIZE, short of the whole CIA, is
	// accepted. Here the put ends with `short` because the sender closes without having sent
	// anything.
	char cia[160];
	refused("grammar", "put", "new 4096 0 " ZERO_DIGEST);
	refused("grammar", "put", "new 4096 4096 " ZERO_DIGEST);
	snprintf(cia, sizeof(cia), "new 99999999 %llu " ZERO_DIGEST, CIA_MAX_PREFIX_SIZE + 1);
	refused("grammar", "put", cia);
	inLen = inAt = 0;
	expectPrefix("put rejected short d963f802",
				 request("put %s %s new 4096 4095 " ZERO_DIGEST, ID, live()));
	snprintf(cia, sizeof(cia), "new 99999999 %llu " ZERO_DIGEST, CIA_MAX_PREFIX_SIZE);
	expectPrefix("put rejected short ", request("put %s %s %s", ID, live(), cia));
	// Only the batch position is checked here: 1 <= index <= total <= 9999 and installed < index.
	refused("grammar", "put", "new 4096 4095 " ZERO_DIGEST " 0 1 0");
	refused("grammar", "put", "new 4096 4095 " ZERO_DIGEST " 2 1 0");
	refused("grammar", "put", "new 4096 4095 " ZERO_DIGEST " 1 10000 0");
	refused("grammar", "put", "new 4096 4095 " ZERO_DIGEST " 3 5 3");
	inLen = inAt = 0;
	expectPrefix("put rejected short ",
				 request("put %s %s new 4096 4095 " ZERO_DIGEST " 9999 9999 9998", ID, live()));
	// 3Drop's own LayeredFS directory accepts no write, not even a rename into it.
	gated("self", "put", "/luma/titles/0004000003d37200/code.bin 4 " ZERO_DIGEST);
	gated("self", "put", "/LUMA/Titles/0004000003D37200/romfs/a 4 " ZERO_DIGEST);
	gated("self", "mkdir", "/luma/titles/0004000003d37200");
	mkdir("sdmc:/stage", 0755);
	refused("self", "rename", "/stage /luma/titles/0004000003d37200");
	refused("refused", "rename", "/stage /luma"); // a tree renamed onto their parent
	gated("self", "put", "/luma/plugins/0004000003d37200/plugin.3gx 4 " ZERO_DIGEST);
	// Nor does a write go through for a title that is not a managed SD application, such as HOME
	// Menu's or a mistyped id.
	gated("unmanaged", "put", "/luma/titles/0004003000008f02/code.bin 4 " ZERO_DIGEST);
	gated("unmanaged", "put", "/LUMA/titles/0004003000008F02/romfs/a 4 " ZERO_DIGEST);
	gated("unmanaged", "mkdir", "/luma/titles/0004000100001234");
	gated("unmanaged", "put", "/luma/plugins/0004003000008f02/plugin.3gx 4 " ZERO_DIGEST);
	refused("unmanaged", "rename", "/stage /luma/titles/0004003000008f02");
	gated("refused", "put", "/luma/titles/000400~1/code.bin 4 " ZERO_DIGEST); // not a title id
	// Nor can anything the console boots from be written, moved or deleted.
	gated("system", "put", "/boot.firm 4 " ZERO_DIGEST);
	gated("system", "put", "/luma/payloads/x.firm 4 " ZERO_DIGEST);
	refused("system", "rename", "/stage /luma/sysmodules");
	gated("system", "delete", "/boot.firm");
	gated("system", "delete", "/luma r");
	expectPrefix("mkdir committed ok ",
				 request("mkdir %s %s /luma/titles/000400000f800100", ID, live()));
}

static void files(void) {
	mkdir("sdmc:/d", 0755);
	mkdir("sdmc:/d/sub", 0755);
	writeFile("sdmc:/d/a", "hello", 5);
	writeFile("sdmc:/d/sub/b", "x", 1);

	// ls streams one line per entry; the trailer counts them and hashes the body.
	expectIs("ls ok ok 00000000", request("ls %s /d", ID));
	assert(strstr(fake.body, "f 5 - 61\n") && strstr(fake.body, "d - - 737562\n"));
	expectTrailer("ls ok ok 00000000 2 ");
	request("ls %s hex:2f64 r", ID); // the tree, listed by relative path
	assert(strstr(fake.body, "f 1 - 7375622f62\n") && strstr(fake.last, " ok ok 00000000 3 "));
	request("ls %s /d/a dt", ID); // a file, with its modification time
	assert(strncmp(fake.body, "f 5 1", 5) == 0 && strstr(fake.body, " 61\n"));
	request("ls %s / d", ID);
	assert(strcmp(fake.body, "d - - 2f\n") == 0);
	expectPrefix("ls rejected notfound ", request("ls %s /nope", ID));
	expectPrefix("ls rejected grammar ", request("ls %s /d x", ID));
	expectPrefix("ls rejected path ", request("ls %s /d/../d", ID));
	// A walk that stops short says why in its trailer, so a partial listing, which sync deletes by,
	// never passes for a whole one.
	fake.dirReadResult = (Result)0xC8804464;
	expectIs("ls ok ok 00000000", request("ls %s /d", ID));
	expectTrailer("ls rejected read c8804464 0 ");
	fake.dirReadResult = 0;
	char deep[SD_PATH_MAX] = "sdmc:/deep";
	for (int level = 0; level <= FS_WALK_DEPTH; level++) {
		assert(mkdir(deep, 0755) == 0);
		strcat(deep, "/d");
	}
	request("ls %s /deep r", ID);
	expectTrailer("ls rejected depth 00000000 16 ");
	// A path FS would not open is not listed. A path of SD_PATH_UNITS units is listed, and one more
	// unit gets it skipped.
	char longName[SD_PATH_MAX + 16] = "sdmc:/long/";
	memset(longName + strlen(longName), 'n', SD_PATH_UNITS - strlen("/long/"));
	assert(mkdir("sdmc:/long", 0755) == 0);
	writeFile(longName, "", 0);
	strcat(longName, "n");
	writeFile(longName, "", 0);
	request("ls %s /long", ID);
	expectTrailer("ls rejected name 00000000 1 ");
	// The walk's deadline is a point in time, like every other: the entry read at exactly 120 s is
	// the walk's last. Each clock read is 60 s later than the previous one, in this order: the
	// deadline's, the header's nonce, then one per entry.
	char *walk[] = {"ls", "0123456789abcdef0123456789abcdef", "/d", "r"};
	VerbContext c = {.client = 7, .request = "ls", .id = ID, .field = walk, .n = 4};
	c.verb = &VERBS[1];
	u64 t0 = fake.nowMs;
	fake.stepMs = 60000;
	lsPath(&c, "r");
	fake.stepMs = 0;
	assert(fake.nowMs == t0 + 3 * 60000); // another read would move the entry this pins
	expectTrailer("ls rejected deadline 00000000 1 ");

	// delete takes a full directory only with `r`, and never /, /3ds or /Nintendo 3DS. rename never
	// overwrites a target, and mkdir never goes over a file.
	refused("notfound", "delete", "/nope");
	refused("path", "delete", "/ r");
	refused("protected", "delete", "/3ds r");
	refused("refused", "delete", "hex:2f4e696e74656e646f203344532f78 r"); // /Nintendo 3DS/x
	refused("refused", "delete", "/NINTEN~1 r");
	refused("grammar", "delete", "/d x");
	refused("exists", "rename", "/d/a /d/sub");
	expectIs("mkdir rejected exists c82044be", request("mkdir %s %s /d/a", ID, live()));
	refused("notfound", "rename", "/nope /d/z");
	mkdir("sdmc:/Nintendo 3DS", 0755);
	refused("refused", "rename", "hex:2f4e696e74656e646f20334453 /x"); // /Nintendo 3DS
	// Nothing is written into /Nintendo 3DS, the console's own data, or moved out of it.
	refused("refused", "put",
			"hex:2f4e696e74656e646f203344532f78 4 " ZERO_DIGEST); // /Nintendo 3DS/x
	refused("refused", "mkdir", "/NINTEN~1/x");
	refused("refused", "rename", "/d/a hex:2f4e696e74656e646f203344532f61"); // to /Nintendo 3DS/a
	writeFile("sdmc:/Nintendo 3DS/f", "", 0);
	refused("refused", "rename", "hex:2f4e696e74656e646f203344532f66 /f"); // /Nintendo 3DS/f
	refused("refused", "delete", "hex:2f4e696e74656e646f203344532f66");	   // a plain delete too
	expectPrefix("delete rejected delete ", request("delete %s %s /d", ID, live()));
	fake.fsFail = (Result)0xC8804464; // FS's own Result reaches the reply unchanged
	expectIs("delete rejected delete c8804464", request("delete %s %s /d/sub/b", ID, live()));
	fake.fsFail = 0;
	expectIs("rename committed ok 00000000", request("rename %s %s /d/a /d/c", ID, live()));
	expectIs("delete committed ok 00000000", request("delete %s %s /d/c", ID, live()));
	// A tree is deleted a file at a time, so one that fails partway is partly gone and is never
	// `rejected`.
	static const char *const TREE[] = {"sdmc:/d/sub/b", "sdmc:/d/sub/x", "sdmc:/d/sub/y",
									   "sdmc:/d/sub/z"};
	for (size_t i = 1; i < 4; i++) {
		writeFile(TREE[i], "", 0);
	}
	fake.treeFail = (Result)0xC8804464;
	fake.treeFailAfter = 1;
	expectIs("delete unknown delete c8804464", request("delete %s %s /d r", ID, live()));
	fake.treeFail = 0;
	int left = 0;
	for (size_t i = 0; i < 4; i++) {
		left += access(TREE[i], F_OK) == 0;
	}
	assert(left == 3);
	expectIs("delete committed ok 00000000", request("delete %s %s /d r", ID, live()));
	struct stat st;
	assert(stat("sdmc:/d", &st) != 0);
}

// get: the header, the body, and a trailer whose digest and length the host checks.
static void getFile(const char *name, const unsigned char *data, size_t size, bool gzip) {
	static unsigned char raw[32u << 20];
	char path[64], want[200];
	snprintf(path, sizeof(path), "sdmc:/%s", name);
	writeFile(path, data, size);
	snprintf(want, sizeof(want), "get ok ok 00000000 %zu %s", size, gzip ? "gzip" : "raw");
	expectIs(want, request("get %s /%s%s", ID, name, gzip ? " gzip" : ""));
	if (gzip) {
		z_stream z = {.next_in = out, .avail_in = (uInt)outLen, .next_out = raw};
		z.avail_out = sizeof(raw);
		assert(inflateInit2(&z, 15 + 16) == Z_OK && inflate(&z, Z_FINISH) == Z_STREAM_END);
		assert(z.total_out == size && z.avail_in == 0 && memcmp(raw, data, size) == 0);
		inflateEnd(&z);
	} else {
		assert(outLen == size && memcmp(out, data, size) == 0);
	}
	expectGetTrailer(data, size);
}

// Equal and one-byte-smaller first blocks exercise the stored threshold with real deflate. It
// searches a synthetic random block's zero tail instead of hard-coding the result of one zlib
// version.
static void gzipBoundary(unsigned char *data, size_t size, uLong wanted) {
	unsigned char zipped[131072];
	for (size_t tail = 0; tail < 512; tail++) {
		fillRandom(data, size, 12345);
		memset(data + 65536 - tail, 0, tail);
		z_stream z = {
			.next_in = data, .avail_in = 65536, .next_out = zipped, .avail_out = sizeof(zipped)};
		assert(deflateInit2(&z, 1, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK);
		assert(deflate(&z, Z_BLOCK) == Z_OK && z.avail_in == 0);
		uLong produced = z.total_out;
		deflateEnd(&z);
		if (produced == wanted) {
			return;
		}
	}
	assert(!"no synthetic gzip boundary found");
}

// Each case is also selected alone by GZ_GET_CASE for the base and mutation controls.
static void gzipPipeline(unsigned which) {
	size_t size = 4u << 20;
	unsigned char *data = malloc(size);
	assert(data);
	fillRandom(data, size, 12345);
	if (which == 0) {
		for (size_t at = 0; at < size; at += 65536) {
			memset(data + at + 32768, 0, 32768);
		}
	} else if (which == 2) {
		fillLog(data + (1u << 20), size - (1u << 20));
	} else if (which == 4 || which == 5) {
		gzipBoundary(data, size, which == 4 ? 65536 : 65535);
	}
	gzipCaller = pthread_self();
	gzipStored = gzipResumed = gzipOffThread = gzipInSend = 0;
	gzipStoredAt = gzipResumedAt = 0;
	gzipSent = gzipSending = false;
	gzipOverlap = which == 0;
	gzipCancel = which == 3;
	gzipParamsFail = which == 7;
	gzipWatch = true;
	if (which == 6) {
		fake.threadFailPrio = 0x32;
	}
	if (which == 3 || which == 7) {
		writeFile("sdmc:/pipeline.txt", data, size);
		expectIs("get ok ok 00000000 4194304 gzip", request("get %s /pipeline.txt gzip", ID));
		expectTrailer(which == 3 ? "get rejected canceled d963f803 "
								 : "get rejected gzip d963f811 ");
		assert(which != 3 || (outLen > 0 && outLen < size));
	} else if (which == 6) {
		writeFile("sdmc:/pipeline.txt", data, size);
		expectIs("get ok ok 00000000 4194304 raw", request("get %s /pipeline.txt gzip", ID));
		assert(outLen == size && memcmp(out, data, size) == 0);
		expectGetTrailer(data, size);
	} else {
		getFile("pipeline.txt", data, size, true);
		writeFile("pipeline.gz", out, outLen);
		assert(system("gzip -t pipeline.gz") == 0 && remove("pipeline.gz") == 0);
	}
	gzipWatch = gzipOverlap = gzipCancel = gzipParamsFail = false;
	fake.stepMs = 0;
	fake.threadFailPrio = 0;
	assert(fake.threads == 0 && zlibLive == 0);
	if (which == 0) {
		assert(gzipOffThread > 0 && gzipInSend > 0 && outLen < size * 3 / 4 && gzipStored == 0);
	} else if (which == 3) {
		assert(gzipOffThread > 0);
	} else if (which == 1 || which == 2) {
		assert(gzipStored > 0 && gzipStoredAt <= 131072);
		if (which == 2) {
			assert(gzipResumed > 0 && gzipResumedAt > (1u << 20) && outLen < size / 2);
		}
	} else if (which == 4) {
		assert(gzipStored > 0 && gzipStoredAt == 65536);
	} else if (which == 5) {
		assert(gzipStored > 0 && gzipStoredAt > 65536);
	}
	assert(remove("sdmc:/pipeline.txt") == 0);
	free(data);
}

// The ring cuts the wire's units as the kit does; the 3670016-byte vector is used literally.
static const char VECTOR[] = "u:1a123ad2daaf45074139d526e884c29c5c1f3e5c08fb7746ccd08ffbbcbdb43b";
static void vectorBody(unsigned char out[static 3670016]) {
	for (u32 i = 0; i < 3670016; i++) {
		out[i] = (unsigned char)"3drop\n"[i % 6];
	}
}

// Sends every body shape through the reader and the send pump, with each unit hashed as it is read
// (none when the body is empty). Also covers a gzip log whose deflated length rides in the trailer,
// and a read failure mid-body, which ends in a `rejected` trailer.
static void getVerb(void) {
	size_t big = 25u << 20;
	unsigned char *data = malloc(big);
	fillRandom(data, big, 12345);
	getFile("empty", data, 0, false);
	getFile("small", data, 100, false);
	assert(fake.fsCalls == 4); // open, size, read, close; a body this small hashes in software
	getFile("units", data, (3u << 20) + 17, false);
	getFile("four", data, 4u << 20, false); // no short last unit
	getFile("big", data, big, false);
	assert(strstr(request("stats %s", ID), " hash_n=26 ")); // 25 units and the finish
	// Random bytes, so gzip does not win, but the output is still correct.
	getFile("units", data, (3u << 20) + 17, true);
	vectorBody(data);
	getFile("vector", data, 3670016, false);
	assert(strstr(fake.last, VECTOR));
	fillLog(data, 2000000);
	getFile("log.txt", data, 2000000, true);
	assert(outLen < 20000); // the log deflates
	const char *stats = request("stats %s", ID);
	// Expect two reads of a unit, their two units and the finish. The reader's lead shows as
	// ring_high.
	assert(strstr(stats, " sd_io_n=2 ") && !strstr(stats, " send_n=0 ") &&
		   strstr(stats, " hash_n=3 req=0123456789abcdef0123456789abcdef") &&
		   !strstr(stats, " ring_high=0 "));
	// gzip is requested, not guaranteed. A body under wire.sh's 1024-byte threshold goes raw; one
	// at the threshold does not.
	getFile("kib", data, 1024, true);
	writeFile("sdmc:/under", data, 1023);
	expectIs("get ok ok 00000000 1023 raw", request("get %s /under gzip", ID));
	// Nor does it go gzip for a name on the skip list, in any letter case. A directory's extension
	// is not the file name's.
	writeFile("sdmc:/x.ZIP", data, 1024);
	expectIs("get ok ok 00000000 1024 raw", request("get %s /x.ZIP gzip", ID));
	mkdir("sdmc:/a.zip", 0755);
	getFile("a.zip/f", data, 1024, true);
	readFailAt = 1u << 20;
	expectIs("get ok ok 00000000 3145745 raw", request("get %s /units", ID));
	expectTrailer("get rejected read c8804464 - ");
	readFailAt = UINT64_MAX;
	expectIs("get rejected notfound c8804478", request("get %s /nope", ID));
	fake.sendFails = true; // the header does not go out whole, so no body or trailer follows
	expectIs("get ok ok 00000000 3145745 raw", request("get %s /units", ID));
	assert(outLen == 0 && fake.replies == 1);
	fake.sendFails = false;
	free(data);
}

static bool sameFile(const char *path, const unsigned char *data, size_t size) {
	static unsigned char back[26u << 20];
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(back, 1, sizeof(back), f) : 0;
	if (f) {
		fclose(f);
	}
	return f && n == size && memcmp(back, data, size) == 0;
}

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

// Returns the number of names in `folder` (a host path) that FS was given to look up since the last
// call. FAT searches the whole folder for each of them, which is what a put, a get or a delete
// costs in a big one.
static unsigned lookedUp(const char *folder) {
	unsigned n = fake.watched;
	fake.watch = folder;
	fake.watched = 0;
	return n;
}

// A put whose ring never ends would hang the test until the runner's timeout, so it fails here
// instead, and the message names the request.
static void unanswered(int sig) {
	static const char WHY[] = "testDispatch: a put unanswered after 60 s: ";
	(void)sig;
	(void)!write(2, WHY, sizeof(WHY) - 1);
	(void)!write(2, line, strlen(line));
	(void)!write(2, "\n", 1);
	_exit(1);
}

static const char *putFile(const char *path, const unsigned char *data, size_t size,
						   const unsigned char *body, size_t bodyLen, bool gzip) {
	char fields[400];
	snprintf(fields, sizeof(fields), "%s %zu %s", path, size, refDigestText(data, size));
	if (gzip) {
		snprintf(fields + strlen(fields), sizeof(fields) - strlen(fields), " gzip %zu", bodyLen);
	}
	feed(body, bodyLen);
	alarm(60);
	const char *got = request("put %s %s %s", ID, live(), fields);
	alarm(0);
	return got;
}

// Sends a put whose body is the file it names, uncompressed.
static const char *putPlain(const char *path, const unsigned char *data, size_t size) {
	return putFile(path, data, size, data, size, false);
}

static size_t gzipOf(const unsigned char *data, size_t size, unsigned char *out, size_t cap) {
	z_stream z = {.next_in = (Bytef *)data, .avail_in = (uInt)size, .next_out = out};
	z.avail_out = (uInt)cap;
	assert(deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK &&
		   deflate(&z, Z_FINISH) == Z_STREAM_END);
	deflateEnd(&z);
	return z.total_out;
}

// AM commits the ticket ahead of the TMD, and a foreign ticket over HOME Menu's bricks the console,
// so an installable TMD with another title's ticket never reaches AM. The reply is `metadata`, with
// no AM call.
static void ciaTicketTitle(void) {
	TestCIA c = build(0x00010004, 0x00010004, 0x0004000E00F80100ULL, 1, 1); // a patch's TMD
	put(c.bytes + CERT_OFFSET + 0xA00 + signatureSize(0x00010004) + 0x9C, 0x0004003000008F02ULL, 8,
		true); // HOME Menu's ticket
	feed(c.bytes, c.extent);
	fake.amCalls = 0;
	expectPrefix("put rejected metadata ",
				 request("put %s %s new %llu %llu %s", ID, live(), (unsigned long long)c.extent,
						 (unsigned long long)c.prefix, refDigestText(c.bytes, c.prefix)));
	assert(fake.amCalls == 0);
	free(c.bytes);
}

// ciaAlign rounds up to a multiple of 64 and keeps an offset that is already aligned (a `new`'s
// early call ends one block into the first content, which starts there). 14628 is the TMD end of a
// two-content CIA from build().
static void ciaAlignment(void) {
	TestCIA c = build(0x00010004, 0x00010004, 0x000400000F800100ULL, 1, 2);
	assert(c.prefix == 14628 && ciaAlign(c.prefix) == 14656 && ciaAlign(14656) == 14656);
	free(c.bytes);
}

// Returns a request field for a path: its bytes as hex, as wire.sh does for any path outside
// printable ASCII. It alternates between two buffers so a rename's two paths fit in one request.
static const char *hexPath(const char *path) {
	static char field[2][4 + 2 * SD_PATH_MAX];
	static int next;
	next ^= 1;
	memcpy(field[next], "hex:", 4);
	hexEncode(field[next] + 4, (const u8 *)path, strlen(path));
	return field[next];
}

// A UTF-8 path reaches FS as UTF-16 on every verb. The card holds each name in its own bytes (the
// fake FS names nothing under PATH_ASCII with a non-ASCII byte), and ls lists it that way. A
// non-ASCII top folder takes writes unless it could be a protected name.
static void unicodePaths(void) {
	static const char DIR[] = "/3ds/Checkpoint/saves/0x0055D \xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2"
							  "\xe3\x83\xb3", // ポケモン
		SAVE[] = "/3ds/Checkpoint/saves/0x0055D \xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3/"
				 "\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96.bin", // セーブ.bin
		MOVED[] = "/3ds/Checkpoint/saves/0x0055D \xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3/"
				  "\xf0\x9f\x98\x80.bin"; // 😀.bin
	static const unsigned char BODY[] = "progress";
	char fields[600], want[200], name[64];
	expectPrefix("mkdir committed ok ", request("mkdir %s %s %s", ID, live(), hexPath(DIR)));
	snprintf(fields, sizeof(fields), "%s 8 %s", hexPath(SAVE), refDigestText(BODY, 8));
	feed(BODY, 8);
	expectPrefix("put committed ok 00000000 8 ", request("put %s %s %s", ID, live(), fields));
	snprintf(name, sizeof(name), "sdmc:%s", SAVE);
	assert(sameFile(name, BODY, 8));
	expectIs("ls ok ok 00000000", request("ls %s %s", ID, hexPath(DIR)));
	snprintf(want, sizeof(want), "f 8 - %s\n", hexPath(strrchr(SAVE, '/') + 1) + 4);
	assert(strcmp(fake.body, want) == 0);
	expectIs("ls ok ok 00000000", request("ls %s %s dt", ID, hexPath(SAVE)));
	assert(strncmp(fake.body, "f 8 - ", 6) != 0); // its mtime, read through the UTF-16 walk path
	expectIs("get ok ok 00000000 8 raw", request("get %s %s", ID, hexPath(SAVE)));
	assert(outLen == 8 && memcmp(out, BODY, 8) == 0);
	expectPrefix("rename committed ok ",
				 request("rename %s %s %s %s", ID, live(), hexPath(SAVE), hexPath(MOVED)));
	snprintf(name, sizeof(name), "sdmc:%s", MOVED);
	assert(sameFile(name, BODY, 8));
	expectPrefix("delete committed ok ", request("delete %s %s %s", ID, live(), hexPath(MOVED)));
	expectPrefix("delete committed ok ", request("delete %s %s %s", ID, live(), hexPath(DIR)));
	snprintf(name, sizeof(name), "sdmc:%s", DIR);
	assert(!exists(name));

	// /音楽, two characters, is not a root name: it is written, read and deleted like any other
	// folder, and so is its 曲.mp3. A root look-alike as long as a name is still that name.
	mkdir("sdmc:/\xe9\x9f\xb3\xe6\xa5\xbd", 0755);
	writeFile("sdmc:/\xe9\x9f\xb3\xe6\xa5\xbd/x", "song", 4);
	snprintf(fields, sizeof(fields), "%s 8 %s", hexPath("/\xe9\x9f\xb3\xe6\xa5\xbd/y"),
			 refDigestText(BODY, 8));
	feed(BODY, 8);
	expectPrefix("put committed ok 00000000 8 ", request("put %s %s %s", ID, live(), fields));
	assert(sameFile("sdmc:/\xe9\x9f\xb3\xe6\xa5\xbd/y", BODY, 8));
	expectIs("get ok ok 00000000 4 raw",
			 request("get %s %s", ID, hexPath("/\xe9\x9f\xb3\xe6\xa5\xbd/x")));
	assert(outLen == 4 && memcmp(out, "song", 4) == 0);
	writeFile("sdmc:/\xe9\x9f\xb3\xe6\xa5\xbd/\xe6\x9b\xb2.mp3", "tune", 4);
	expectIs("get ok ok 00000000 4 raw",
			 request("get %s %s", ID, hexPath("/\xe9\x9f\xb3\xe6\xa5\xbd/\xe6\x9b\xb2.mp3")));
	expectPrefix("delete committed ok ",
				 request("delete %s %s %s r", ID, live(), hexPath("/\xe9\x9f\xb3\xe6\xa5\xbd")));
	assert(!exists("sdmc:/\xe9\x9f\xb3\xe6\xa5\xbd"));
	snprintf(fields, sizeof(fields), "%s 8 %s", hexPath("/N\xc4\xb1ntendo 3DS/x"),
			 refDigestText(BODY, 8)); // Nıntendo 3DS
	refused("refused", "put", fields);
	// A dot segment beside a non-ASCII blank is not a path, and that is decided before any FS call.
	expectPrefix("get rejected path ", request("get %s %s", ID, hexPath("/x/..\xc2\xa0")));
	assert(fake.fsCalls == 0);

	// The screen's file name is cut at a character boundary: 20 three-byte characters keep 15.
	char wide[64] = "/u/";
	for (int i = 0; i < 20; i++) {
		strcat(wide, "\xe3\x81\x82"); // あ
	}
	snprintf(fields, sizeof(fields), "%s 8 %s", hexPath(wide), refDigestText(BODY, 8));
	feed(BODY, 8);
	expectPrefix("put committed ok ", request("put %s %s %s", ID, live(), fields));
	assert(strlen(gApp.fileName) == 45 && strncmp(gApp.fileName, wide + 3, 45) == 0);
}

// A file put is published only once it is whole and matches, and its backup is gone when the client
// closes. A failure leaves the path as it was, and the next put undoes a put that died mid-publish.
static void putVerb(void) {
	static unsigned char data[25u << 20];
	fillRandom(data, sizeof(data), 777);
	char want[160];
	expectCommitted(data, 3u << 20, putPlain("/new/dir/f.bin", data, 3u << 20));
	assert(sameFile("sdmc:/new/dir/f.bin", data, 3u << 20) &&
		   !exists("sdmc:/new/dir/f.bin.3drop.tmp") && !exists("sdmc:/new/dir/f.bin.3drop.bak"));
	const char *stats = request("stats %s", ID);
	// The writer takes up to 4 filled units per call, however many the pump has filled by then, and
	// hashes each unit it wrote.
	assert(strstr(stats, " bytes=3145728 ") && !strstr(stats, " sd_io_n=0 ") &&
		   strstr(stats, " hash_n=4 req=0123456789abcdef0123456789abcdef"));
	expectPrefix("put committed ok ", putPlain("/new/dir/f.bin", data + 1, 1000));
	assert(sameFile("sdmc:/new/dir/f.bin", data + 1, 1000) &&
		   !exists("sdmc:/new/dir/f.bin.3drop.bak")); // the backup is gone once the client closed
	expectPrefix("put committed ok 00000000 0 ", putPlain("/new/empty", data, 0));
	// A small put makes 8 FS calls: the check's one (is it a folder), orphan rename, open, size,
	// write, flush, close and publish (its digest is software). An overwrite adds the refused
	// publish, the move aside and the backup's delete.
	int open = fake.files; // a put closes every file handle it opens
	expectCommitted(data, 4096, putPlain("/new/small", data, 4096));
	assert(fake.fsCalls == 8 && fake.files == open);
	expectCommitted(data + 1, 4096, putPlain("/new/small", data + 1, 4096));
	assert(fake.fsCalls == 11 && fake.files == open);
	// A committed put leaves no temp name behind, so a later get that fails before its ring starts
	// deletes nothing, not even a file moved onto that name since.
	writeFile("sdmc:/new/small.3drop.tmp", "", 0);
	expectPrefix("get rejected notfound ", request("get %s /new/none", ID));
	assert(exists("sdmc:/new/small.3drop.tmp") && remove("sdmc:/new/small.3drop.tmp") == 0);
	// The temp name must fit too, in FS's units and in SD_PATH_MAX's bytes. The longest path that
	// leaves it room is accepted, and one more is refused before the spend, because a cut temp name
	// could be another file's.
	char longest[SD_PATH_MAX], tooLong[4 * SD_PATH_MAX];
	memset(longest, 'n', sizeof(longest));
	longest[0] = '/';
	longest[SD_PATH_UNITS - strlen(SD_PUT_TEMP)] = '\0';
	expectCommitted(data, 10, putPlain(longest, data, 10));
	snprintf(tooLong, sizeof(tooLong), "%sn 10 %s", longest, refDigestText(data, 10));
	refused("path", "put", tooLong);
	for (size_t i = 2; i + 1 < SD_PATH_MAX - strlen(SD_PUT_TEMP); i += 2) {
		memcpy(longest + i, "\xc3\xa9", 2); // "/n" and 2-byte characters make 246 bytes, 124 units
	}
	longest[SD_PATH_MAX - strlen(SD_PUT_TEMP)] = '\0';
	snprintf(tooLong, sizeof(tooLong), "%s 10 %s", hexPath(longest), refDigestText(data, 10));
	refused("path", "put", tooLong);

	// Refused: a body whose digest is not the request's, a sender that leaves early.
	unsigned char other[1000];
	memcpy(other, data + 1, sizeof(other));
	other[500] ^= 1;
	expectIs("put rejected digest d963f80a",
			 putFile("/new/dir/f.bin", data + 1, 1000, other, sizeof(other), false));
	expectIs("put rejected short d963f802",
			 putFile("/new/dir/f.bin", data + 1, 1000, data + 1, 999, false));
	assert(sameFile("sdmc:/new/dir/f.bin", data + 1, 1000) &&
		   !exists("sdmc:/new/dir/f.bin.3drop.tmp"));
	// A directory at the path is refused before the spend, so no byte of the body moves.
	refused("exists", "put", "/new/dir 1000 " ZERO_DIGEST);
	assert(exists("sdmc:/new/dir/f.bin") && !exists("sdmc:/new/dir.3drop.tmp"));

	// The old file left at the backup by a put that died between its renames goes back first, so
	// even a refused put leaves the path whole; a stale backup never blocks an overwrite.
	assert(rename("sdmc:/new/dir/f.bin", "sdmc:/new/dir/f.bin.3drop.bak") == 0);
	expectIs("put rejected digest d963f80a", putFile("/new/dir/f.bin", data, 10, other, 10, false));
	assert(sameFile("sdmc:/new/dir/f.bin", data + 1, 1000));
	writeFile("sdmc:/new/dir/f.bin.3drop.bak", "", 0);
	expectCommitted(data, 10, putPlain("/new/dir/f.bin", data, 10));
	assert(sameFile("sdmc:/new/dir/f.bin", data, 10) && !exists("sdmc:/new/dir/f.bin.3drop.bak"));

	// A backup left beside a live file (power lost after a publish) goes with the file, so a
	// deleted or moved file never comes back at the next put to its path, even one that fails.
	for (int moved = 0; moved < 2; moved++) {
		writeFile("sdmc:/new/dir/f.bin.3drop.bak", "old", 3);
		if (moved) {
			expectPrefix("rename committed ok ",
						 request("rename %s %s /new/dir/f.bin /new/dir/g.bin", ID, live()));
		} else {
			expectPrefix("delete committed ok ",
						 request("delete %s %s /new/dir/f.bin", ID, live()));
		}
		assert(!exists("sdmc:/new/dir/f.bin.3drop.bak"));
		expectIs("put rejected digest d963f80a",
				 putFile("/new/dir/f.bin", data, 10, other, 10, false));
		assert(!exists("sdmc:/new/dir/f.bin"));
		expectCommitted(data, 10, putPlain("/new/dir/f.bin", data, 10));
	}

	// Past tier 1 the hashing is the same: a unit per write, then the finish.
	expectPrefix("put committed ok ", putPlain("/big", data, sizeof(data)));
	assert(sameFile("sdmc:/big", data, sizeof(data)) &&
		   strstr(request("stats %s", ID), " hash_n=26 "));
	// Each unit is hashed before its write because an engine call right after an SD write stalls
	// the wire.
	fake.writtenTo = 0;
	fake.shaCalls = 0;
	expectCommitted(data, 4u << 20, putPlain("/four", data, 4u << 20));
	assert(sameFile("sdmc:/four", data, 4u << 20) && fake.shaCalls == 4);
	for (u32 unit = 0; unit < 4; unit++) {
		assert(fake.shaWrittenTo[unit] <= (u64)unit << 20); // none of its bytes are on the card yet
	}
	// While one write is held until the join, the units filled behind it go in one more write, each
	// hashed.
	fake.cardHeld = true;
	expectCommitted(data, 5u << 20, putPlain("/five", data, 5u << 20));
	const char *held = request("stats %s", ID);
	assert(sameFile("sdmc:/five", data, 5u << 20) && strstr(held, " sd_io_n=2 ") &&
		   strstr(held, " hash_n=6 "));
	unsigned char *vector = malloc(3670016);
	vectorBody(vector);
	feed(vector, 3670016);
	snprintf(want, sizeof(want), "put committed ok 00000000 3670016 %s", VECTOR);
	expectIs(want, request("put %s %s /vector 3670016 %s", ID, live(), VECTOR));
	assert(sameFile("sdmc:/vector", vector, 3670016));
	free(vector);
}

// In any letter case, a file named like a put's temp or backup is treated as one, but it may still
// be removed.
static void putSiblings(void) {
	static const char *const NAMES[] = {"/sib/f.3drop.bak", "/sib/F.3DROP.TMP", "/sib/g.3Drop.Bak",
										"/boot.firm.3drop.tmp"};
	char fields[160];
	mkdir("sdmc:/sib", 0755);
	writeFile("sdmc:/sib/f", "", 0);
	writeFile("sdmc:/sib/f" SD_PUT_BACKUP, "mine", 4);
	for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
		snprintf(fields, sizeof(fields), "%s 4 " ZERO_DIGEST, NAMES[i]);
		gated("reserved", "put", fields);
		gated("reserved", "mkdir", NAMES[i]);
		snprintf(fields, sizeof(fields), "/sib/f %s", NAMES[i]);
		refused("reserved", "rename", fields);
	}
	// A folder with that name on the way is refused too, because a put or a mkdir would create it.
	gated("reserved", "mkdir", "/sib/h.3drop.tmp/child");
	gated("reserved", "put", "/sib/H.3DROP.BAK/c/d 4 " ZERO_DIGEST);
	refused("reserved", "rename", "/sib/f /sib/h.3drop.bak/x");
	assert(sameFile("sdmc:/sib/f" SD_PUT_BACKUP, (const unsigned char *)"mine", 4));
	fake.renameFrom = "/sib/f";
	fake.renameResult = (Result)0xC8804464;
	expectIs("rename rejected rename c8804464", request("rename %s %s /sib/f /sib/h", ID, live()));
	fake.renameResult = 0;
	fake.fsFail = (Result)0xC8804464;
	fake.fsFailOn = "/sib/f";
	expectIs("delete rejected delete c8804464", request("delete %s %s /sib/f", ID, live()));
	fake.fsFail = 0;
	fake.fsFailOn = NULL;
	assert(sameFile("sdmc:/sib/f" SD_PUT_BACKUP, (const unsigned char *)"mine", 4));
	// A backup that can't be deleted is logged and the delete still answers; a backup that was
	// never made is not logged.
	writeFile("sdmc:/sib/g", "", 0);
	writeFile("sdmc:/sib/g" SD_PUT_BACKUP, "old", 3);
	fake.fsFail = (Result)0xC8804464;
	fake.fsFailOn = SD_PUT_BACKUP;
	logged[0] = '\0';
	expectIs("delete committed ok 00000000", request("delete %s %s /sib/g", ID, live()));
	fake.fsFail = 0;
	fake.fsFailOn = NULL;
	assert(strcmp(logged, "backup-drop-failed\ndelete-ok\n") == 0);
	expectIs("rename committed ok 00000000",
			 request("rename %s %s /sib/f" SD_PUT_BACKUP " /sib/kept", ID, live()));
	writeFile("sdmc:/sib/f" SD_PUT_TEMP, "", 0);
	logged[0] = '\0';
	expectIs("delete committed ok 00000000",
			 request("delete %s %s /sib/f" SD_PUT_TEMP, ID, live()));
	assert(strcmp(logged, "delete-ok\n") == 0);
	assert(sameFile("sdmc:/sib/kept", (const unsigned char *)"mine", 4) &&
		   !exists("sdmc:/sib/f" SD_PUT_TEMP));
}

// gzip bodies are inflated into the ring, whether under a unit, in whole units or past tier 1. A
// stream must end with the body, at the declared size. Any other stream is refused as `gzip` and
// leaves no temp, and so is one short of the size, whose last unit never fills. A whole stream of
// other bytes is `digest`.
static void gzipPuts(void) {
	static unsigned char data[25u << 20], zipped[1u << 20], bad[1u << 20];
	fillLog(data, sizeof(data));
	size_t z = gzipOf(data, 2000000, zipped, sizeof(zipped));
	expectCommitted(data, 2000000, putFile("/log.txt", data, 2000000, zipped, z, true));
	assert(sameFile("sdmc:/log.txt", data, 2000000));
	assert(strstr(request("stats %s", ID), " join=1 ")); // the file's size joined, not the wire's
	static const size_t SIZES[] = {4096, 25u << 20, (25u << 20) - 5};
	for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
		size_t n = gzipOf(data, SIZES[i], bad, sizeof(bad));
		expectCommitted(data, SIZES[i], putFile("/log.txt", data, SIZES[i], bad, n, true));
		assert(sameFile("sdmc:/log.txt", data, SIZES[i]));
	}
	// A bomb at the smallest size the grammar accepts stops there, before it fills a ring that no
	// worker will drain.
	size_t bomb = gzipOf(data, sizeof(data), bad, sizeof(bad));
	expectIs("put rejected gzip 00000000", putFile("/log2.txt", data, bomb + 1, bad, bomb, true));
	expectIs("put rejected gzip 00000000", putFile("/log2.txt", data, 1999999, zipped, z, true));
	expectIs("put rejected gzip 00000000", putFile("/log2.txt", data, 2000001, zipped, z, true));
	expectIs("put rejected gzip 00000000",
			 putFile("/log2.txt", data, 2000000, zipped, z - 4, true));
	expectIs("put rejected gzip 00000000", putFile("/log2.txt", data, 1000, zipped, 0, true));
	// Damages each of these in turn: the magic, a reserved flag, a deflate byte, the CRC-32 and the
	// length.
	const struct {
		size_t at;
		unsigned char flip;
	} DAMAGE[] = {{0, 0x01}, {3, 0x20}, {z / 2, 0x01}, {z - 8, 0x01}, {z - 4, 0x01}};
	for (size_t i = 0; i < sizeof(DAMAGE) / sizeof(DAMAGE[0]); i++) {
		memcpy(bad, zipped, z);
		bad[DAMAGE[i].at] ^= DAMAGE[i].flip;
		expectIs("put rejected gzip 00000000", putFile("/log2.txt", data, 2000000, bad, z, true));
	}
	memcpy(bad, zipped, z);
	memcpy(bad + z, zipped, z); // a second member
	expectIs("put rejected gzip 00000000", putFile("/log2.txt", data, 2000000, bad, 2 * z, true));
	bad[z] = 0;
	expectIs("put rejected gzip 00000000", putFile("/log2.txt", data, 2000000, bad, z + 1, true));
	size_t other = gzipOf(data + 1, 2000000, bad, sizeof(bad));
	expectIs("put rejected digest d963f80a", putFile("/log2.txt", data, 2000000, bad, other, true));
	assert(!exists("sdmc:/log2.txt") && !exists("sdmc:/log2.txt.3drop.tmp"));
	// The wire's length is under the size, so an empty file is never gzip.
	char fields[200];
	snprintf(fields, sizeof(fields), "/log2.txt 1000 %s gzip 1000", refDigestText(data, 1000));
	refused("grammar", "put", fields);
	snprintf(fields, sizeof(fields), "/log2.txt 0 %s gzip %zu", refDigestText(data, 0),
			 gzipOf(data, 0, bad, sizeof(bad)));
	refused("grammar", "put", fields);
}

// The word `system` opens up a put or a delete of one file directly in /luma/payloads or
// /luma/sysmodules, spelled exactly, and nothing else. Whatever else answers `system` without the
// word answers `system` with it too, before any FS call where the path alone decides.
static void systemWord(void) {
	static const unsigned char BODY[] = "payload";
	static const char *const FILES[] = {"/luma/payloads/3drop-fixture.firm",
										"/LUMA/SYSMODULES/X.CXI"};
	static const char *const STILL[] = {
		"/luma/paylo~1/x",		// an alias could be any boot input
		"/luma/payloads",		// the directories themselves
		"/luma/sysmodules/a/b", // deeper
		"/boot~1.fir",
		"/bo~2",
		"/boot.firm/x",
		"hex:2f6cc3bc6d612f7061796c6f6164732f78", // /lüma/payloads/x, a look-alike
	};
	char fields[200], name[80];
	mkdir("sdmc:/stage", 0755); // the renames' source, made here so the case runs alone
	for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
		snprintf(fields, sizeof(fields), "%s 7 %s", FILES[i], refDigestText(BODY, 7));
		gated("system", "put", fields);
		feed(BODY, 7);
		expectPrefix("put committed ok 00000000 7 ",
					 request("put %s %s %s system", ID, live(), fields));
		snprintf(name, sizeof(name), "sdmc:%s", FILES[i]);
		assert(sameFile(name, BODY, 7));
		gated("system", "delete", FILES[i]);
		expectPrefix("delete committed ok ",
					 request("delete %s %s %s system", ID, live(), FILES[i]));
		assert(!exists(name));
	}
	for (size_t i = 0; i < sizeof(STILL) / sizeof(STILL[0]); i++) {
		snprintf(fields, sizeof(fields), "%s 7 %s system", STILL[i], refDigestText(BODY, 7));
		gated("system", "put", fields);
		snprintf(fields, sizeof(fields), "%s system", STILL[i]);
		gated("system", "delete", fields);
	}
	// Luma's external FIRMs and an alias take neither a put nor a delete, with the word or without
	// it. A neighbor stays open.
	static const char *const FIRMS[] = {"/luma/native.firm",	 "/LUMA/TWL.FIRM",
										"/luma/agb.firm",		 "/luma/safe.firm",
										"/luma/sysupdater.firm", "/luma/native~1.fir"};
	static const char *const WORDS[] = {"", " system"};
	for (size_t i = 0; i < sizeof(FIRMS) / sizeof(FIRMS[0]); i++) {
		for (size_t w = 0; w < 2; w++) {
			snprintf(fields, sizeof(fields), "%s 7 %s%s", FIRMS[i], refDigestText(BODY, 7),
					 WORDS[w]);
			gated("system", "put", fields);
			snprintf(fields, sizeof(fields), "%s%s", FIRMS[i], WORDS[w]);
			gated("system", "delete", fields);
		}
	}
	refused("system", "rename", "/stage /luma/native.firm");
	snprintf(fields, sizeof(fields), "/luma/native.fir 7 %s", refDigestText(BODY, 7));
	feed(BODY, 7);
	expectPrefix("put committed ok 00000000 7 ", request("put %s %s %s", ID, live(), fields));
	expectPrefix("delete committed ok ", request("delete %s %s /luma/native.fir", ID, live()));
	gated("system", "delete", "/luma system");
	gated("system", "delete", "/luma/payloads r");
	expectPrefix("error rejected grammar ",
				 request("delete %s %s /luma/payloads/x r system", ID, live()));
	gated("system", "mkdir", "/luma/payloads/x");
	// A file there can be moved neither in nor out, and a directory there is not a single file.
	writeFile("sdmc:/luma/payloads/old.firm", "", 0);
	refused("system", "rename", "/luma/payloads/old.firm /old.firm");
	refused("system", "rename", "/stage /luma/payloads/stage");
	assert(mkdir("sdmc:/luma/payloads/sub", 0755) == 0);
	refused("system", "delete", "/luma/payloads/sub system");
	assert(exists("sdmc:/luma/payloads/sub") && exists("sdmc:/luma/payloads/old.firm"));
	// The word changes nothing elsewhere; it is the option a body's shape takes, alone.
	snprintf(fields, sizeof(fields), "/plain.txt 7 %s", refDigestText(BODY, 7));
	feed(BODY, 7);
	expectPrefix("put committed ok 00000000 7 ",
				 request("put %s %s %s system", ID, live(), fields));
	expectPrefix("delete committed ok ", request("delete %s %s /plain.txt system", ID, live()));
	snprintf(fields, sizeof(fields), "%s 7 %s gzip 5 system", FILES[0], refDigestText(BODY, 7));
	refused("grammar", "put", fields);
}

// Puts a file with the word `system`, its body the whole file.
static const char *putSystem(const char *path, const unsigned char *data, size_t size) {
	char fields[200];
	snprintf(fields, sizeof(fields), "%s %zu %s system", path, size, refDigestText(data, size));
	feed(data, size);
	return request("put %s %s %s", ID, live(), fields);
}

// A FIRM boot9strap would boot but where it loads: in its first sector the ARM9 entry point, in
// section 0 (one sector, under ENGINE_FLOOR), the ARM11 one in section 1 (over the floor, its hash
// taken there), and an empty section whose offset is past the end, which nothing reads.
#define FIRM_SIZE 0x2800u
static void firmBuild(unsigned char firm[FIRM_SIZE], unsigned char fill) {
	const u32 sections[][3] = {
		{0x200, 0x08006000, 0x200}, {0x400, 0x1FF80000, 0x2400}, {0xFFFFFE00, 0, 0}};
	memset(firm, 0, 0x200);
	memcpy(firm, "FIRM", 4);
	LE32(firm + 0x08, 0x1FF80004);
	LE32(firm + 0x0C, 0x08006000);
	for (u32 i = 0x200; i < FIRM_SIZE; i++) {
		firm[i] = (unsigned char)(i * 7 + fill);
	}
	for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
		unsigned char *header = firm + 0x40 + 0x30 * i;
		LE32(header, sections[i][0]);
		LE32(header + 4, sections[i][1]);
		LE32(header + 8, sections[i][2]);
		if (sections[i][2] > 0) {
			assert(mbedtls_sha256_ret(firm + sections[i][0], sections[i][2], header + 0x10, 0) ==
				   0);
		}
	}
}

// A put of these bytes as `path` is answered with `system` after the spend, and leaves /boot.firm
// as `firm` with no temp or backup.
static void firmRefused(const char *name, const char *path, const unsigned char *bytes,
						const unsigned char *firm) {
	char temp[64];
	const char *got = putSystem(path, bytes, FIRM_SIZE);
	if (strncmp(got, "put rejected system d963f809", 28) != 0) {
		fprintf(stderr, "testDispatch: a FIRM with its %s answered `%s`\n", name, got);
		abort();
	}
	snprintf(temp, sizeof(temp), "sdmc:%s.3drop.tmp", path);
	assert(sameFile("sdmc:/boot.firm", firm, FIRM_SIZE) && !exists(temp) &&
		   !exists("sdmc:/boot.firm.3drop.bak"));
}

// /boot.firm takes a put with `system` only when it is a FIRM that boot9strap would boot
// (testFirmRules.sh pins each check). The check runs on the temp read back, before it publishes,
// and applies to any spelling of the name. A read or hash that fails is answered `read` or `hash`,
// and the file is kept. Its delete stays refused.
static void bootFirm(void) {
	static unsigned char old[FIRM_SIZE], firm[FIRM_SIZE], flipped[FIRM_SIZE];
	char fields[200];
	mkdir("sdmc:/stage", 0755); // the rename's source, made here so the case runs alone
	firmBuild(old, 1);
	firmBuild(firm, 2);
	expectPrefix("put committed ok ", putSystem("/boot.firm", old, FIRM_SIZE));
	expectPrefix("put committed ok ", putSystem("/boot.firm", firm, FIRM_SIZE));
	assert(sameFile("sdmc:/boot.firm", firm, FIRM_SIZE));
	// Off its charger the console takes no /boot.firm put and refuses it before the spend. A
	// payload still goes.
	char onBattery[200];
	snprintf(onBattery, sizeof(onBattery), "/boot.firm %u %s system", FIRM_SIZE,
			 refDigestText(old, FIRM_SIZE));
	osSharedConfigFake.led_battery &= (u8)~1u;
	gated("system", "put", onBattery);
	assert(strstr(fake.reply, " put rejected system d963f818"));
	expectPrefix("put committed ok ", putSystem("/luma/payloads/old.firm", old, FIRM_SIZE));
	osSharedConfigFake.led_battery |= 1u;
	assert(sameFile("sdmc:/boot.firm", firm, FIRM_SIZE));
	// ...and none if the charger leaves while the body arrives. The charger is checked again before
	// the renames.
	unplugAt = 0;
	expectIs("put rejected system d963f818", putSystem("/boot.firm", old, FIRM_SIZE));
	osSharedConfigFake.led_battery |= 1u;
	assert(sameFile("sdmc:/boot.firm", firm, FIRM_SIZE) && !exists("sdmc:/boot.firm.3drop.tmp"));
	memcpy(flipped, firm, FIRM_SIZE);
	flipped[0] ^= 1;
	firmRefused("magic flipped", "/boot.firm", flipped, firm);
	firmRefused("magic flipped, its name in capitals", "/BOOT.FIRM", flipped, firm);
	readFailAt = 0x300;
	expectIs("put rejected read c8804464", putSystem("/boot.firm", old, FIRM_SIZE));
	readFailAt = UINT64_MAX;
	fake.shaResult = (Result)0xC8804478;
	fake.shaFailAt = 2; // the calls are the body's one unit, then section 1 (section 0 is software)
	expectIs("put rejected hash c8804478", putSystem("/boot.firm", old, FIRM_SIZE));
	fake.shaResult = 0;
	fake.shaFailAt = 0;
	assert(sameFile("sdmc:/boot.firm", firm, FIRM_SIZE) && !exists("sdmc:/boot.firm.3drop.tmp"));
	// The FIRM is read back whole into tier 1, so no larger one is accepted, and the refusal comes
	// before the spend.
	snprintf(fields, sizeof(fields), "/boot.firm %u " ZERO_DIGEST " system", RING_TIER1_BYTES + 1);
	refused("size", "put", fields);
	gated("system", "delete", "/boot.firm system");
	assert(sameFile("sdmc:/boot.firm", firm, FIRM_SIZE));
	// Its backup takes no change, so the restore that a put starts with, after a power cut between
	// the two renames, brings back only a /boot.firm that a put moved there. Here that happens past
	// a refused FIRM.
	gated("system", "put", "/boot.firm.3drop.bak 4 " ZERO_DIGEST " system");
	gated("system", "delete", "/boot.firm.3drop.bak system");
	refused("system", "rename", "/stage /boot.firm.3drop.bak");
	assert(rename("sdmc:/boot.firm", "sdmc:/boot.firm.3drop.bak") == 0);
	firmRefused("magic flipped, after a power cut", "/boot.firm", flipped, firm);
}

// get's range returns the file's bytes from an offset, at most a length, or as many as there are.
// The header adds the file's size and mtime, the trailer digests the range alone, and the plain get
// is unchanged. The mtime takes a call that names the path, which FAT looks up in its whole folder,
// so a range of the whole file is joined to no other and names none (`-`).
static void getRanges(void) {
	static unsigned char data[(3u << 20) + 17];
	static const struct {
		u64 offset, length, sent;
		bool timed; // the header names the file's mtime
	} RANGES[] = {
		{0, 1u << 20, 1u << 20, true},
		{1u << 20, 1u << 20, 1u << 20, true}, // from the offset
		{5, 1u << 20, 1u << 20, true},	 // anywhere: the digest counts units from the range's start
		{3u << 20, 32u << 20, 17, true}, // past the end: what there is, after others
		{(3u << 20) + 17, 1, 0, true},	 // at the end: nothing
		{0, (3u << 20) + 17, (3u << 20) + 17, false}, // the file's size exactly: whole, one range
		{0, (3u << 20) + 16, (3u << 20) + 16, true},  // a byte short of it: another range follows
	};
	static const char *const BAD[] = {"range 0 0",	"range 0",	  "range x 1",
									  "range 0 1x", "range -1 1", "gzip 0"};
	char want[200], mtime[24];
	struct stat st;
	fillRandom(data, sizeof(data), 99);
	writeFile("sdmc:/ranged", data, sizeof(data));
	assert(stat("sdmc:/ranged", &st) == 0);
	snprintf(mtime, sizeof(mtime), "%llu", (unsigned long long)st.st_mtime);
	for (size_t i = 0; i < sizeof(RANGES) / sizeof(RANGES[0]); i++) {
		snprintf(want, sizeof(want), "get ok ok 00000000 %llu raw %zu %s",
				 (unsigned long long)RANGES[i].sent, sizeof(data), RANGES[i].timed ? mtime : "-");
		expectIs(want,
				 request("get %s /ranged range %llu %llu", ID, (unsigned long long)RANGES[i].offset,
						 (unsigned long long)RANGES[i].length));
		assert(outLen == RANGES[i].sent && memcmp(out, data + RANGES[i].offset, outLen) == 0);
		expectGetTrailer(data + RANGES[i].offset, outLen);
	}
	expectPrefix("get rejected size ", request("get %s /ranged range %zu 1", ID, sizeof(data) + 1));
	// Each get looks its path up for the open. A range of part of the file asks for the time too,
	// which is a second lookup of the folder that a range of the whole file, or a plain get,
	// avoids.
	lookedUp("sdmc:");
	expectPrefix("get ok ok 00000000 1048576 raw ", request("get %s /ranged range 0 1048576", ID));
	assert(lookedUp("sdmc:") == 2);
	expectPrefix("get ok ok 00000000 3145745 raw ", request("get %s /ranged range 0 33554432", ID));
	assert(lookedUp("sdmc:") == 1);
	expectPrefix("get ok ok 00000000 3145745 raw", request("get %s /ranged", ID));
	assert(lookedUp(NULL) == 1);
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		expectPrefix("get rejected grammar ", request("get %s /ranged %s", ID, BAD[i]));
		assert(fake.fsCalls == 0);
	}
	expectPrefix("error rejected grammar ", request("get %s /ranged range 0 1 x", ID));
	expectPrefix("get rejected notfound ", request("get %s /none range 0 1", ID));
}

// put's range: the body goes into the path's temp after the file's first `offset` bytes, which the
// temp must keep, and a range that ends the file publishes it. Each is answered with how many bytes
// the temp, or the path, then holds and their digest.
static const char *putRange(const char *path, const unsigned char *file, size_t size, size_t offset,
							size_t length, const unsigned char *body, size_t bodyLen,
							const unsigned char *kept) {
	char fields[400];
	snprintf(fields, sizeof(fields), "%s %zu %s range %zu %zu ", path, size,
			 refDigestText(file, offset + length), offset, length);
	appendf(fields, sizeof(fields), "%s", refDigestText(kept, offset));
	feed(body, bodyLen);
	return request("put %s %s %s", ID, live(), fields);
}

// A range that fails is cut off again, so the temp keeps whole ranges; a failed range with nothing
// kept before it removes the temp. What the temp keeps is checked before the spend, read back from
// the card when no digest of it is held (a relaunch, a delete or a rename since). A put that starts
// the temp again, a range from 0 or a whole put, discards what it kept.
static void putRanges(void) {
	static unsigned char data[(3u << 20) + 5],
		other[1u << 20]; // other: the third range, one bit off
	static const char PATH[] = "/ranges/f.bin", TEMP[] = "sdmc:/ranges/f.bin" SD_PUT_TEMP;
	const size_t mb = 1u << 20, size = sizeof(data);
	char fields[400];
	fillRandom(data, size, 4242);
	memcpy(other, data + 2 * mb, mb);
	other[100] ^= 1;
	for (size_t at = 0; at < 2 * mb; at += mb) {
		// The held digest checks the temp, and a read back would fail.
		readFailAt = at ? 0 : UINT64_MAX;
		expectCommitted(data, at + mb, putRange(PATH, data, size, at, mb, data + at, mb, data));
		assert(sameFile(TEMP, data, at + mb) && !exists("sdmc:/ranges/f.bin"));
	}
	readFailAt = UINT64_MAX;
	expectIs("put rejected digest d963f80a",
			 putRange(PATH, data, size, 2 * mb, mb, other, mb, data));
	assert(sameFile(TEMP, data, 2 * mb));
	expectPrefix("put rejected short ",
				 putRange(PATH, data, size, 2 * mb, mb, data + 2 * mb, 9, data));
	assert(sameFile(TEMP, data, 2 * mb));
	// Refused before the spend: a temp that does not keep these bytes, or as many; a range off the
	// units, or past the size; a range into a save.
	const char *nonce = live();
	expectIs("put rejected digest 00000000",
			 putRange(PATH, data, size, 2 * mb, mb, data + 2 * mb, mb, data + 1));
	assert(strcmp(nonceLive(), nonce) == 0 && sameFile(TEMP, data, 2 * mb));
	snprintf(fields, sizeof(fields), "%s %zu %s range %zu 5 ", PATH, size,
			 refDigestText(data, size), 3 * mb);
	appendf(fields, sizeof(fields), "%s", refDigestText(data, 3 * mb));
	refused("size", "put", fields);
	static const struct {
		const char *range, *word;
	} OFF[] = {
		{"range 1 1048576 " ZERO_DIGEST, "grammar"}, // off a unit
		{"range 0 1000 " ZERO_DIGEST, "grammar"},	 // ends off one, short of the file's end
		{"range 3145728 6 " ZERO_DIGEST, "size"},	 // past the size
		{"range 0 1048576", "grammar"},				 // no kept digest
		{"gzip 10 range 0", "grammar"},				 // gzip or a range, never both
	};
	for (size_t i = 0; i < sizeof(OFF) / sizeof(OFF[0]); i++) {
		snprintf(fields, sizeof(fields), "%s %zu %s %s", PATH, size, refDigestText(data, size),
				 OFF[i].range);
		refused(OFF[i].word, "put", fields);
	}
	refused("grammar", "put", "save:000400000f800100/x 10 " ZERO_DIGEST " range 0 10 " ZERO_DIGEST);
	// The temp is read back once no digest of it is held, so a byte changed on the card meanwhile
	// is found. The read back belongs to a check, so `stats` still describes the last transfer.
	sdPutForget();
	FILE *f = fopen(TEMP, "r+b");
	assert(f && fseek(f, 5, SEEK_SET) == 0 && fputc(data[5] ^ 1, f) != EOF && fclose(f) == 0);
	char stats[REPLY_MAX];
	snprintf(stats, sizeof(stats), "%s", request("stats %s", ID));
	expectIs("put rejected digest 00000000",
			 putRange(PATH, data, size, 2 * mb, mb, data + 2 * mb, mb, data));
	assert(strcmp(request("stats %s", ID), stats) == 0);
	f = fopen(TEMP, "r+b");
	assert(f && fseek(f, 5, SEEK_SET) == 0 && fputc(data[5], f) != EOF && fclose(f) == 0);
	writeFile("sdmc:/ranges/gone", "", 0);
	expectPrefix("delete committed ok ", request("delete %s %s /ranges/gone", ID, live()));
	expectCommitted(data, 3 * mb, putRange(PATH, data, size, 2 * mb, mb, data + 2 * mb, mb, data));
	// The range that ends the file publishes it whole, as a whole put's reply names it.
	expectCommitted(data, size, putRange(PATH, data, size, 3 * mb, 5, data + 3 * mb, 5, data));
	assert(sameFile("sdmc:/ranges/f.bin", data, size) && !exists(TEMP) &&
		   !exists("sdmc:/ranges/f.bin" SD_PUT_BACKUP));
	// Started again: from 0 the temp keeps that range alone, and a whole put replaces it.
	for (size_t at = 0; at < 2 * mb; at += mb) {
		putRange(PATH, data, size, at, mb, data + at, mb, data);
	}
	putRange(PATH, data, size, 0, mb, data, mb, data);
	assert(sameFile(TEMP, data, mb));
	expectCommitted(data, 10, putPlain(PATH, data, 10));
	assert(sameFile("sdmc:/ranges/f.bin", data, 10) && !exists(TEMP));
	snprintf(fields, sizeof(fields), "%s %zu %s range %zu %zu ", PATH, size,
			 refDigestText(data, 2 * mb), mb, mb);
	appendf(fields, sizeof(fields), "%s", refDigestText(data, mb));
	refused("size", "put", fields);
	// A temp the card won't read back is answered `read`, with its Result. A request on a spent
	// nonce, a replay, is refused before any read back.
	putRange(PATH, data, size, 0, mb, data, mb, data);
	sdPutForget();
	readFailAt = 0;
	snprintf(fields, sizeof(fields), "%s %zu %s range %zu %zu ", PATH, size,
			 refDigestText(data, 2 * mb), mb, mb);
	appendf(fields, sizeof(fields), "%s", refDigestText(data, mb));
	expectIs("put rejected challenge 00000000", request("put %s %064d %s", ID, 0, fields));
	expectIs("put rejected read c8804464", putRange(PATH, data, size, mb, mb, data + mb, mb, data));
	readFailAt = UINT64_MAX;
	startPolls = 0; // START cancels the read back too, as it does a hash
	cancelAtPoll = 1;
	expectIs("put rejected canceled d963f803",
			 putRange(PATH, data, size, mb, mb, data + mb, mb, data));
	cancelAtPoll = 0;
	// A rename forgets the held digest, as a delete does, so another file moved onto the temp's
	// name (by a card reader, since 3Drop moves nothing there) is read back and refused.
	for (size_t at = 0; at < 2 * mb; at += mb) {
		putRange("/ranges/h.bin", data, size, at, mb, data + at, mb, data);
	}
	writeFile("sdmc:/ranges/other.bin", data + 1, 2 * mb);
	expectPrefix("rename committed ok ",
				 request("rename %s %s /ranges/h.bin%s /ranges/aside", ID, live(), SD_PUT_TEMP));
	refused("reserved", "rename", "/ranges/other.bin /ranges/h.bin" SD_PUT_TEMP);
	assert(rename("sdmc:/ranges/other.bin", "sdmc:/ranges/h.bin" SD_PUT_TEMP) == 0);
	expectIs("put rejected digest 00000000",
			 putRange("/ranges/h.bin", data, size, 2 * mb, mb, data + 2 * mb, mb, data));
	// A failed first range keeps nothing, so its temp goes.
	expectPrefix("put rejected short ",
				 putRange("/ranges/g.bin", data, size, 0, mb, data, 9, data));
	assert(!exists("sdmc:/ranges/g.bin" SD_PUT_TEMP));
}

// A slow card shows up on SD as it does on AM, through the ring's first and slowest calls. A hash
// skips the ring.
static void sdIoTimes(void) {
	static unsigned char data[9u << 20];
	unsigned first = 0, most = 0;
	memset(data, 0x5A, sizeof(data));
	fake.writeMs = 7; // each write takes 7 ms, which the ring's tick reads as 6.99 ms
	expectCommitted(data, sizeof(data), putPlain("/timed.bin", data, sizeof(data)));
	fake.writeMs = 0;
	const char *stats = strstr(request("stats %s", ID), " first_sd_io_ms=");
	assert(stats && sscanf(stats, " first_sd_io_ms=%u sd_io_max_ms=%u", &first, &most) == 2 &&
		   first == 6 && most == 6);
	readMs = 5;
	expectPrefix("get ok ok 00000000 9437184 raw", request("get %s /timed.bin", ID));
	readMs = 0;
	stats = strstr(request("stats %s", ID), " first_sd_io_ms=");
	assert(stats && sscanf(stats, " first_sd_io_ms=%u sd_io_max_ms=%u", &first, &most) == 2 &&
		   first == 4 && most == 4);
	// A card under a whole millisecond a call measures 0. A read of one call has no slowest after
	// the first.
	expectPrefix("get ok ok 00000000 9437184 raw", request("get %s /timed.bin", ID));
	assert(strstr(request("stats %s", ID), " first_sd_io_ms=0 sd_io_max_ms=0 "));
	writeFile("sdmc:/small.bin", data, 4096);
	expectPrefix("get ok ok 00000000 4096 raw", request("get %s /small.bin", ID));
	assert(strstr(request("stats %s", ID), " first_sd_io_ms=0 sd_io_max_ms=- "));
	expectPrefix("hash ok ok 00000000 9437184 ", request("hash %s /timed.bin", ID));
	assert(strstr(request("stats %s", ID), " first_sd_io_ms=- sd_io_max_ms=- "));
}

// hash reports what a get of the file would end with, its size and wire digest, read on the card
// with nothing sent and no nonce spent. `stats` times its reads and digests. A failed read or
// digest says which failed, with its Result.
static void hashVerb(void) {
	static unsigned char data[(3u << 20) + 17], vector[3670016];
	char want[200];
	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = (unsigned char)(i * 31 + 7);
	}
	writeFile("sdmc:/hashed.bin", data, sizeof(data));
	const char *nonce = live();
	snprintf(want, sizeof(want), "hash ok ok 00000000 %zu %s", sizeof(data),
			 refDigestText(data, sizeof(data)));
	expectIs(want, request("hash %s /hashed.bin", ID));
	assert(strcmp(nonceLive(), nonce) == 0 && outLen == 0);
	const char *stats = request("stats %s", ID);
	assert(strstr(stats, " sd_io_n=4 ") && strstr(stats, " hash_n=5 "));
	vectorBody(vector);
	writeFile("sdmc:/vector.bin", vector, sizeof(vector));
	expectPrefix("hash ok ok 00000000 3670016 ", request("hash %s /vector.bin", ID));
	assert(strstr(fake.reply, VECTOR));
	writeFile("sdmc:/nothing.bin", "", 0);
	expectIs("hash ok ok 00000000 0 "
			 "u:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
			 request("hash %s /nothing.bin", ID));
	expectPrefix("hash rejected notfound ", request("hash %s /none.bin", ID));
	// A folder reads as no file, so it is reported as a folder (`isdir`), where notfound would
	// mislead.
	mkdir("sdmc:/folder", 0755);
	expectPrefix("hash rejected isdir ", request("hash %s /folder", ID));
	expectPrefix("get rejected isdir ", request("get %s /folder", ID));
	expectPrefix("get rejected isdir ", request("get %s /folder range 0 1", ID));
	expectPrefix("error rejected grammar ", request("hash %s /hashed.bin x", ID));
	readFailAt = 1u << 20;
	expectIs("hash rejected read c8804464", request("hash %s /hashed.bin", ID));
	readFailAt = UINT64_MAX;
	fake.shaResult = (Result)0xC8804478;
	expectIs("hash rejected hash c8804478", request("hash %s /hashed.bin", ID));
	fake.shaResult = 0;
	// START and APT are seen between units, as a get's pump sees them, because a 4 GB file takes
	// minutes to read.
	startPolls = 0;
	cancelAtPoll = 2;
	expectIs("hash rejected canceled d963f803", request("hash %s /hashed.bin", ID));
	cancelAtPoll = 0;
	assert(strstr(request("stats %s", ID), " sd_io_n=1 "));
	fake.powerPressed = true;
	expectPrefix("hash rejected exit ", request("hash %s /hashed.bin", ID));
	fake.powerPressed = false;
}

// A range before the file's last may give its size as `-`, as a stream that has not ended does.
// Such a range must end on a unit and below FAT32's largest file, and it never publishes. The
// screen and `stats` count its own bytes. The last range names the size and publishes.
static void putUnsized(void) {
	static unsigned char data[(2u << 20) + 7];
	static const char PATH[] = "/unsized.bin", TEMP[] = "sdmc:/unsized.bin" SD_PUT_TEMP;
	const size_t mb = 1u << 20, size = sizeof(data);
	char fields[400];
	for (size_t i = 0; i < size; i++) {
		data[i] = (unsigned char)(i * 7 + 3);
	}
	for (size_t at = 0; at < 2 * mb; at += mb) {
		snprintf(fields, sizeof(fields), "%s - %s range %zu %zu ", PATH,
				 refDigestText(data, at + mb), at, mb);
		appendf(fields, sizeof(fields), "%s", refDigestText(data, at));
		feed(data + at, mb);
		expectCommitted(data, at + mb, request("put %s %s %s", ID, live(), fields));
		assert(sameFile(TEMP, data, at + mb) && !exists("sdmc:/unsized.bin"));
		assert(gApp.wireTotal == mb && strstr(request("stats %s", ID), " bytes=1048576 "));
	}
	expectCommitted(data, size, putRange(PATH, data, size, 2 * mb, 7, data + 2 * mb, 7, data));
	assert(sameFile("sdmc:/unsized.bin", data, size) && !exists(TEMP));
	// Refused before the spend: `-` off a unit, past FAT32's largest file, or on no range.
	static const struct {
		const char *fields, *word;
	} BAD[] = {
		{" - " ZERO_DIGEST " range 0 1000 " ZERO_DIGEST, "grammar"},
		{" - " ZERO_DIGEST " range 4293918720 1048575 " ZERO_DIGEST, "grammar"}, // ends at `-`
		{" - " ZERO_DIGEST " range 4293918720 2097152 " ZERO_DIGEST, "size"},
		{" - " ZERO_DIGEST, "grammar"},
		{" - " ZERO_DIGEST " gzip 10", "grammar"},
	};
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		snprintf(fields, sizeof(fields), "%s%s", PATH, BAD[i].fields);
		refused(BAD[i].word, "put", fields);
	}
}

// A clock step back is not counted as a wire stall, and never as a wrapped one. A quiet wire passes
// no deadline.
static void clockStepBack(void) {
	static unsigned char data[2u << 20];
	fake.stepMs = 1;
	stepBackAt = 1u << 20;
	expectPrefix("put committed ok ", putPlain("/clock.bin", data, sizeof(data)));
	const char *stats = request("stats %s", ID);
	assert(strstr(stats, " stall_n=0 ") && strstr(stats, " stall_max_ms=- "));
	stepBackQuiet = true;
	for (size_t at = 0; at <= 1u << 20; at += 1u << 20) {
		stepBackAt = at;
		expectPrefix("put committed ok ", putPlain("/clock.bin", data, sizeof(data)));
	}
	stepBackQuiet = false;
	fake.stepMs = 0;
}

// A put the card fails once its body is in (size, flush, publish) is refused with FS's Result
// and leaves the path as it was, with no temp or backup beside it.
static void putCardFailures(void) {
	static unsigned char data[4096], old[100];
	memset(data, 0x42, sizeof(data));
	memset(old, 0x17, sizeof(old));
	mkdir("sdmc:/card", 0755);
	expectPrefix("put committed ok ", putPlain("/card/old.bin", old, sizeof(old)));
	fake.setSizeResult = (Result)0xC8804464;
	expectIs("put rejected space c8804464", putPlain("/card/new.bin", data, sizeof(data)));
	fake.setSizeResult = 0;
	fake.fileFlushResult = (Result)0xC8804464;
	expectIs("put rejected write c8804464", putPlain("/card/new.bin", data, sizeof(data)));
	fake.fileFlushResult = 0;
	fake.renameFrom = SD_PUT_TEMP;
	fake.renameResult = (Result)0xC8804464;
	expectIs("put rejected publish c8804464", putPlain("/card/new.bin", data, sizeof(data)));
	assert(!exists("sdmc:/card/new.bin") && !exists("sdmc:/card/new.bin" SD_PUT_TEMP));
	expectIs("put rejected publish c8804464", putPlain("/card/old.bin", data, sizeof(data)));
	fake.renameResult = 0;
	assert(sameFile("sdmc:/card/old.bin", old, sizeof(old)) &&
		   !exists("sdmc:/card/old.bin" SD_PUT_TEMP) &&
		   !exists("sdmc:/card/old.bin" SD_PUT_BACKUP));
}

// A gzip put behind a stuck card waits on the full ring inside its sink. The screen ticks, START
// cancels, and a card that finishes nothing counts as a worker stall. The body outgrows tier 1, and
// its random first 8388608 bytes keep the absolute deadline from coming first.
static void gzipPutWaits(void) {
	size_t size = 48u << 20, cap = 9u << 20;
	unsigned char *raw = calloc(size, 1), *zipped = malloc(cap);
	fillRandom(raw, 8u << 20, 1);
	size_t z = gzipOf(raw, size, zipped, cap);
	fake.stepMs = 50;
	progressTicks = 0;
	cancelAtTick = 8;
	fake.cardHeld = true;
	expectIs("put rejected canceled d963f803", putFile("/zeros.bin", raw, size, zipped, z, true));
	assert(progressTicks >= cancelAtTick && !exists("sdmc:/zeros.bin") &&
		   !exists("sdmc:/zeros.bin.3drop.tmp"));
	cancelAtTick = 0;
	fake.cardHeld = true;
	logged[0] = '\0';
	u64 held = fake.nowMs;
	expectIs("put rejected worker-stall d963f807",
			 putFile("/zeros.bin", raw, size, zipped, z, true));
	// A busy worker pauses the idle clock up to the stall bound, and the wait is counted as the
	// ring's.
	assert(strstr(logged, "transfer-worker-stall\n") && fake.nowMs - held >= 600000);
	assert(gStats.phaseN[PHASE_RING_ACQUIRE] > 100 && gStats.phaseN[PHASE_RECV_IDLE] == 0);
	fake.stepMs = 0;
	free(raw);
	free(zipped);
}

// A gzip put's absolute deadline goes by the file's bytes, which inflate and the card take their
// time over, and not by the wire's bytes. This test uses 67108864 bytes of zeros, ~65 KB on the
// wire, onto a card that takes 50 s a write, so the body's last unit waits on at least 10 writes,
// past what ~65 KB would allow.
static void gzipPutDeadline(void) {
	size_t size = 64u << 20, cap = 1u << 20;
	unsigned char *raw = calloc(size, 1), *zipped = malloc(cap);
	size_t z = gzipOf(raw, size, zipped, cap);
	u64 start = fake.nowMs;
	fake.writeMs = 50000;
	expectCommitted(raw, size, putFile("/slow.bin", raw, size, zipped, z, true));
	fake.writeMs = 0;
	assert(fake.nowMs - start > transferAbsoluteDuration(z) && remove("sdmc:/slow.bin") == 0);
	free(raw);
	free(zipped);
}

// With no memory for zlib's state, a gzip get answers raw before its header promises gzip, which a
// host that asked for gzip accepts too. A gzip put is `gzip` with OUT_OF_MEMORY, not the none that
// a damaged stream gets. Each is logged, because no stat sees zlib's heap.
static void gzipNoMemory(void) {
	static unsigned char data[100000], zipped[100000];
	fillLog(data, sizeof(data));
	size_t z = gzipOf(data, sizeof(data), zipped, sizeof(zipped));
	writeFile("sdmc:/mem.txt", data, sizeof(data));
	zlibNoMemory = true;
	logged[0] = '\0';
	expectIs("get ok ok 00000000 100000 raw", request("get %s /mem.txt gzip", ID));
	assert(outLen == sizeof(data) && memcmp(out, data, outLen) == 0);
	expectGetTrailer(data, sizeof(data));
	assert(strstr(logged, "gzip-no-memory\n"));
	logged[0] = '\0';
	expectIs("put rejected gzip d963f811",
			 putFile("/mem2.txt", data, sizeof(data), zipped, z, true));
	zlibNoMemory = false;
	assert(strstr(logged, "gzip-no-memory\n") && !exists("sdmc:/mem2.txt") &&
		   !exists("sdmc:/mem2.txt.3drop.tmp") && remove("sdmc:/mem.txt") == 0);
}

// Level 1 takes ~4 MB of zeros to produce its first byte, so START must be seen between input
// chunks.
static void gzipGetCancels(void) {
	unsigned char *zeros = calloc(16u << 20, 1);
	writeFile("sdmc:/zeros.txt", zeros, 16u << 20);
	free(zeros);
	fake.stepMs = START_POLL_MS; // each clock read moves the clock on by one poll
	startPolls = 0;
	cancelAtPoll = 3;
	readAhead = 16u << 20; // the whole file read before the first deflate, as on the console
	expectIs("get ok ok 00000000 16777216 gzip", request("get %s /zeros.txt gzip", ID));
	cancelAtPoll = 0;
	fake.stepMs = 0;
	assert(outLen == 0); // not a byte out before START was seen
	expectTrailer("get rejected canceled d963f803 ");
	assert(remove("sdmc:/zeros.txt") == 0);
}

// A get behind a reader stuck on the card ends the way a put behind a stuck writer does: at the
// stall bound, with its word and event, before the idle timeout that the busy reader holds off.
// This holds for raw and gzip alike, and for a gzip get whose deflate thread the scheduler never
// runs; there the send loop waits on deflate, not on the ring, and the deadlines still see the
// reader stalled. Other waits count as the ring's. The body is 8388608 bytes, so the absolute
// deadline (812 s) comes later, whereas a 1048576-byte body's (364 s) comes first, busy reader or
// not. The request is served without request() because the header's nonce has been rotated by the
// time the body ends.
static void getReaderStalls(void) {
	enum { PLAIN, DEFLATE_WAITS, DEFLATE_HELD };
	static const struct {
		const char *name, *reply, *trailer, *event;
		size_t size;
		u64 atLeast, before;
		int deflate;
	} GETS[] = {
		{"/stuck.bin", " get ok ok 00000000 8388608 raw", "worker-stall d963f807",
		 "transfer-worker-stall", 8u << 20, 600000, 600000 + TRANSFER_WIRE_STALL_MS, PLAIN},
		{"/stuck.bin gzip", " get ok ok 00000000 8388608 gzip", "worker-stall d963f807",
		 "transfer-worker-stall", 8u << 20, 600000, 600000 + TRANSFER_WIRE_STALL_MS, DEFLATE_WAITS},
		{"/stuck.bin gzip", " get ok ok 00000000 8388608 gzip", "worker-stall d963f807",
		 "transfer-worker-stall", 8u << 20, 600000, 600000 + TRANSFER_WIRE_STALL_MS, DEFLATE_HELD},
		{"/stuck1.bin", " get ok ok 00000000 1048576 raw", "timeout d963f806",
		 "transfer-absolute-timeout", 1u << 20, 364000, 600000, PLAIN},
	};
	char path[32], want[128];
	stuckStepMs = 50;
	gzipCaller = pthread_self();
	for (size_t i = 0; i < sizeof(GETS) / sizeof(GETS[0]); i++) {
		snprintf(path, sizeof(path), "sdmc:%.*s", (int)strcspn(GETS[i].name, " "), GETS[i].name);
		FILE *f = fopen(path, "wb");
		assert(f && fseek(f, (long)GETS[i].size - 1, SEEK_SET) == 0 && fputc(0, f) == 0 &&
			   fclose(f) == 0);
		fake.cardHeld = true;
		offMainSleeps = 0;
		stuckForDeflate = GETS[i].deflate == DEFLATE_WAITS;
		fake.threadHoldPrio = GETS[i].deflate == DEFLATE_HELD ? 0x32 : 0; // GZIP_PRIORITY
		logged[0] = '\0';
		u64 held = fake.nowMs;
		snprintf(line, sizeof(line), "get %s %s", ID, GETS[i].name);
		serveSigned();
		fake.stepMs = 0;
		assert(strstr(fake.reply, GETS[i].reply));
		snprintf(want, sizeof(want), "trailer %s get rejected %s - ", ID, GETS[i].trailer);
		expectPrefix(want, fake.last);
		snprintf(want, sizeof(want), "%s\n", GETS[i].event);
		assert(strstr(logged, want) && fake.nowMs - held >= GETS[i].atLeast &&
			   fake.nowMs - held < GETS[i].before);
		assert(GETS[i].deflate == DEFLATE_HELD ? gStats.phaseN[PHASE_RING_ACQUIRE] == 0
											   : gStats.phaseN[PHASE_RING_ACQUIRE] > 100);
		assert(gStats.phaseN[PHASE_SEND_IDLE] == 0);
	}
	stuckStepMs = 0;
	stuckForDeflate = false;
	fake.threadHoldPrio = 0;
}

// screenshot returns the screen's BMP in get's frame; any other screen word is refused before the
// spend.
static void screenshots(void) {
	char want[160] = "trailer 0123456789abcdef0123456789abcdef screenshot committed ok 00000000 ";
	expectIs("screenshot committed ok 00000000 1974 raw",
			 request("screenshot %s %s bottom", ID, live()));
	// 240 x 2 raw is a 2-pixel-wide image 240 rows tall, with rows padded from 6 to 8 bytes.
	assert(outLen == 1974 && memcmp(out, "BM", 2) == 0 && out[18] == 2 && out[22] == 240 &&
		   out[54] == 0x5A && out[60] == 0 && out[62] == 0x5A);
	strcat(want, refDigestText(out, outLen));
	strcat(want, " 1974");
	assert(strcmp(fake.last, want) == 0);
	refused("grammar", "screenshot", "left");
}

// A failed install's card stays its own until the next install: a get and a screenshot after it
// leave it as it is, and a batch position ends with its put.
static void installCard(void) {
	static unsigned char head[100];
	writeFile("sdmc:/after.txt", "after", 5);
	feed(head, sizeof(head));
	expectPrefix("put rejected short ",
				 request("put %s %s new 4096 4095 " ZERO_DIGEST " 1 2 0", ID, live()));
	Operation card = gApp.last;
	assert(gApp.status == SERVER_INSTALL_FAILED && gApp.lastBytes == sizeof(head) &&
		   gApp.batchActive);
	expectIs("get ok ok 00000000 5 raw", request("get %s /after.txt", ID));
	expectPrefix("screenshot committed ok ", request("screenshot %s %s top", ID, live()));
	assert(gApp.status == SERVER_INSTALL_FAILED && gApp.lastBytes == sizeof(head) &&
		   gApp.last.result == card.result && strcmp(gApp.last.detail, "short") == 0);
	expectCommitted(head, 10, putPlain("/after.txt", head, 10));
	assert(!gApp.batchActive);
}

// Every spent mutation logs one line, `<verb>-<detail>`, after its last reply, so no total_ms
// includes a log write. A refusal before the spend logs nothing, and only a transfer's line has
// bytes.
static void logLines(void) {
	static const char *const MUTATIONS[][2] = {
		{"mkdir %s %s /logged/in", "mkdir-ok"},
		{"rename %s %s /logged/in /logged/out", "rename-ok"},
		{"mkdir %s %s /logged/file", "mkdir-exists"},
		{"delete %s %s /logged r", "delete-ok"},
		{"delete %s %s title:000400000f800100", "delete-ok"},
		{"run %s %s self", "run-ok"},
		{"mkdir %s %s /3ds/3drop/key.txt", ""}, // refused before the spend
	};
	static unsigned char body[100];
	char want[64];
	mkdir("sdmc:/logged", 0755);
	writeFile("sdmc:/logged/file", "", 0);
	fake.installed = true;
	for (size_t i = 0; i < sizeof(MUTATIONS) / sizeof(MUTATIONS[0]); i++) {
		logged[0] = '\0';
		request(MUTATIONS[i][0], ID, live());
		snprintf(want, sizeof(want), "%s%s", MUTATIONS[i][1], MUTATIONS[i][1][0] ? "\n" : "");
		// A mutation that moves no bytes has no times to report, only the uptime, after its bytes.
		bool upOnly = strncmp(loggedTail, " up_s=", 6) == 0 && !strchr(loggedTail + 1, ' ');
		if (strcmp(logged, want) != 0 || (want[0] && (fake.logBytes != 0 || !upOnly))) {
			fprintf(stderr, "testDispatch: `%s` logged `%s` with `%s`, want `%s` and the uptime\n",
					line, logged, loggedTail, want);
			abort();
		}
	}
	// A body's log line reports its trailer's outcome, since the header said ok before the body
	// failed.
	logged[0] = '\0';
	expectIs("screenshot committed ok 00000000 1974 raw",
			 request("screenshot %s %s top", ID, live()));
	assert(strcmp(logged, "screenshot-ok\n") == 0 && fake.logBytes == 1974);
	logged[0] = '\0';
	fake.bodySendFails = true;
	expectIs("screenshot committed ok 00000000 1974 raw",
			 request("screenshot %s %s top", ID, live()));
	fake.bodySendFails = false;
	assert(strcmp(logged, "transfer-send-failed\nscreenshot-send\n") == 0);
	// A put's line has its outcome and wire bytes, and is written once `stats` has finished.
	fake.stepMs = 1;
	feed(body, sizeof(body));
	logged[0] = '\0';
	expectPrefix("put rejected short ",
				 request("put %s %s new 4096 4095 " ZERO_DIGEST, ID, live()));
	assert(strcmp(logged, "transfer-short-read\nput-short\n") == 0 &&
		   fake.logBytes == sizeof(body) && loggedTotalMs != 0);
	snprintf(want, sizeof(want), " total_ms=%llu ", (unsigned long long)loggedTotalMs);
	assert(strstr(request("stats %s", ID), want));
	logged[0] = '\0';
	expectCommitted(body, 10, putPlain("/logged.bin", body, 10));
	assert(strcmp(logged, "put-ok\n") == 0 && fake.logBytes == 10 && loggedTotalMs != 0);
	snprintf(want, sizeof(want), " total_ms=%llu ", (unsigned long long)loggedTotalMs);
	assert(strstr(request("stats %s", ID), want));
	fake.stepMs = 0;
}

// A put's line says where its time went: before the body (its file row going up), in the body (the
// wire), the ring's join (a card slow to write) and after the body (the row coming down), which add
// up to total_ms. It also has the uptime in seconds, which dates the line. On the card these come
// last, after the line's own fields.
static void logTimes(void) {
	static unsigned char body[100];
	char text[256], want[256];
	unsigned total, pre, bodyMs, join, post, up;
	fake.nowMs += 7200000; // two hours up
	statusDelayMs[0] = 40;
	statusDelayMs[1] = 20;
	recvDelayMs = 300;
	fake.writeMs = 500;
	fake.cardHeld = true; // the write waits for the join, so none of its time is the body's
	logToCard = true;
	expectCommitted(body, sizeof(body), putPlain("/times.bin", body, sizeof(body)));
	logToCard = false;
	fake.writeMs = 0;
	assert(sscanf(loggedTail, " total_ms=%u pre_ms=%u body_ms=%u join_ms=%u post_ms=%u up_s=%u",
				  &total, &pre, &bodyMs, &join, &post, &up) == 6);
	// The tick counter reads 500 ms as 499.97, so the join is 499 and the body has the other ms.
	assert(total == 860 && pre == 40 && bodyMs == 301 && join == 499 && post == 20);
	assert(up == (fake.nowMs - fake.bootMs) / 1000 && up >= 7200);
	FILE *card = fopen(LOG_PATH, "r");
	assert(card && fgets(text, sizeof(text), card) && fgetc(card) == EOF);
	fclose(card);
	snprintf(want, sizeof(want), "put-ok result=0x00000000 bytes=%zu%s\n", sizeof(body),
			 loggedTail);
	assert(strcmp(text, want) == 0);
	assert(remove(LOG_PATH) == 0 && remove("sdmc:/times.bin") == 0);
}

static long fileLength(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

// A read the peer cuts logs nothing, because a keyless peer replays a captured one as fast as it
// reconnects, and would wear the card and push the log's history out.
static void cutReadsUnlogged(void) {
	static unsigned char data[4096];
	writeFile("sdmc:/cut.bin", data, sizeof(data));
	fake.bodySendFails = true;
	logged[0] = '\0';
	for (int replay = 0; replay < 3; replay++) {
		expectIs("get ok ok 00000000 4096 raw", request("get %s /cut.bin", ID));
	}
	fake.bodySendFails = false;
	assert(logged[0] == '\0' && remove("sdmc:/cut.bin") == 0);
}

// 3drop.log at its cap becomes the older file, replacing it, so the two never pass twice the cap.
// Where the older one will not go (a folder holds its name), a line is lost instead of card space.
static void logCap(void) {
	static char full[LOG_MAX_BYTES];
	memset(full, 'x', sizeof(full));
	logToCard = true;
	fake.bodySendFails = true; // a spent request the peer cuts logs two lines each
	for (int round = 0; round < 2; round++) {
		writeFile(LOG_PATH, full, sizeof(full));
		if (round == 1) {
			assert(remove(LOG_OLD_PATH) == 0 && mkdir(LOG_OLD_PATH, 0700) == 0);
			writeFile(LOG_OLD_PATH "/held", "", 0);
		}
		for (int cut = 0; cut < 3; cut++) {
			request("screenshot %s %s top", ID, live());
		}
		if (round == 0) {
			assert(fileLength(LOG_OLD_PATH) == LOG_MAX_BYTES && fileLength(LOG_PATH) > 0 &&
				   fileLength(LOG_PATH) < 1024);
		} else {
			assert(fileLength(LOG_PATH) == LOG_MAX_BYTES);
		}
	}
	fake.bodySendFails = false;
	logToCard = false;
	assert(remove(LOG_OLD_PATH "/held") == 0 && rmdir(LOG_OLD_PATH) == 0 && remove(LOG_PATH) == 0);
}

// A transfer's last line ends its total_ms, and so does a refusal after statsStart; no later
// request moves it. A request that ends within the clock's millisecond is measured as 0, which is
// not `-`.
static void totalMs(void) {
	char stats[REPLY_MAX];
	fake.stepMs = 1; // a clock that moves, so total_ms is a number
	expectPrefix("get rejected notfound ", request("get %s /nothing", ID));
	snprintf(stats, sizeof(stats), "%s", request("stats %s", ID));
	assert(!strstr(stats, " total_ms=- "));
	request("stats %s op", ID);
	assert(strcmp(request("stats %s", ID), stats) == 0);
	gpuRefuses = true;
	expectIs("screenshot rejected capture d963f813", request("screenshot %s %s top", ID, live()));
	gpuRefuses = false;
	assert(!strstr(request("stats %s", ID), " total_ms=- "));
	fake.stepMs = 0; // a held clock, so the whole request is within one millisecond
	writeFile("sdmc:/tiny.bin", "x", 1);
	expectIs("get ok ok 00000000 1 raw", request("get %s /tiny.bin", ID));
	expectPrefix("stats ok ok 00000000 bytes=1 total_ms=0 ", request("stats %s", ID));
	assert(remove("sdmc:/tiny.bin") == 0);
}

// A failed flush or engine call is `hash` with its Result on every verb that digests. A failed unit
// stops the worker there: a get sends no more, a put writes no more, and neither publishes. An
// empty body needs neither call.
static void hashFailures(void) {
	static unsigned char data[1u << 20];
	size_t big = 25u << 20;
	unsigned char *body = calloc(big, 1);
	memset(data, 0x3D, sizeof(data));
	writeFile("sdmc:/hashed", data, sizeof(data));
	writeFile("sdmc:/void", "", 0);
	fake.flushResult = (Result)0xE0E01BF5;
	expectIs("get ok ok 00000000 1048576 raw", request("get %s /hashed", ID));
	expectTrailer("get rejected hash e0e01bf5 - ");
	expectIs("get ok ok 00000000 0 raw", request("get %s /void", ID));
	assert(strcmp(fake.last,
				  "trailer 0123456789abcdef0123456789abcdef get ok ok 00000000 "
				  "u:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 0") == 0);
	fake.flushResult = 0;
	fake.shaResult = (Result)0xC8804478;
	fake.writtenTo = 0;
	expectIs("put rejected hash c8804478", putPlain("/hashed", data + 1, sizeof(data) - 1));
	// A unit that failed its hash is never written, so the worker never joins the whole body.
	assert(sameFile("sdmc:/hashed", data, sizeof(data)) && !exists("sdmc:/hashed.3drop.tmp") &&
		   fake.writtenTo == 0 && strstr(request("stats %s", ID), " join=0 "));
	fake.shaFailAt = 3; // the third unit read
	expectIs("get ok ok 00000000 26214400 raw", request("get %s /big", ID));
	expectTrailer("get rejected hash c8804478 - ");
	assert(strtoull(strrchr(fake.last, ' ') + 1, NULL, 10) <= 3u << 20);
	fake.shaFailAt = 13; // the thirteenth unit hashed
	expectIs("put rejected hash c8804478", putPlain("/hashbig", body, big));
	assert(strstr(request("stats %s", ID), " join=0 ") && !exists("sdmc:/hashbig") &&
		   !exists("sdmc:/hashbig.3drop.tmp"));
	// The finish's own engine call, reached from 256 units, fails as `hash` too, and never as a
	// digest of nothing (a dropped Result). The body is sparse zeros, deflated to fit what send()
	// keeps.
	FILE *zeros = fopen("sdmc:/units256", "wb");
	assert(zeros && ftruncate(fileno(zeros), 256u << 20) == 0 && fclose(zeros) == 0);
	fake.shaFailAt = 257; // 256 units, then the finish
	expectIs("get ok ok 00000000 268435456 gzip", request("get %s /units256 gzip", ID));
	expectTrailer("get rejected hash c8804478 - ");
	assert(remove("sdmc:/units256") == 0);
	fake.shaResult = 0;
	fake.shaFailAt = 0;
	free(body);
}

// A ring body reports its first failure in this order: the worker's, the hash's, gzip's, the
// pump's, then the verb's own. A put past tier 1 leaves the pump waiting on a worker that failed,
// so both fail and the pump's failure loses; a get past tier 1 whose send fails must stop a reader
// that is waiting on the pump.
static void ringLadders(void) {
	size_t big = 25u << 20;
	unsigned char *body = calloc(big, 1);
	writeFile("sdmc:/ladder", body, big);
	fake.bodySendFails = true;
	expectIs("get ok ok 00000000 26214400 raw", request("get %s /ladder", ID));
	fake.bodySendFails = false;
	expectTrailer("get rejected send d963f816 - ");
	fake.writeResult = (Result)0xC86044BE;
	expectIs("put rejected write c86044be", putPlain("/ladder", body, big));
	fake.writeResult = 0;
	fake.shaResult = (Result)0xC8804478;
	expectIs("put rejected hash c8804478", putPlain("/ladder", body, big));
	fake.shaResult = 0;
	// A gzip put's pump waits in its sink. The worker's word still applies, and a cut body gets the
	// pump's.
	size_t cap = big / 100;
	unsigned char *zipped = malloc(cap);
	size_t z = gzipOf(body, big, zipped, cap);
	fake.writeResult = (Result)0xC86044BE;
	expectIs("put rejected write c86044be", putFile("/ladder", body, big, zipped, z, true));
	fake.writeResult = 0;
	fake.shaResult = (Result)0xC8804478;
	expectIs("put rejected hash c8804478", putFile("/ladder", body, big, zipped, z, true));
	fake.shaResult = 0;
	char fields[200];
	snprintf(fields, sizeof(fields), "/ladder %zu %s gzip %zu", big, refDigestText(body, big), z);
	feed(zipped, z / 2);
	expectIs("put rejected short d963f802", request("put %s %s %s", ID, live(), fields));
	assert(sameFile("sdmc:/ladder", body, big) && !exists("sdmc:/ladder.3drop.tmp"));
	free(zipped);
	free(body);
}

// A ring body's bar follows its worker; an archive put has none, so its bar follows the wire.
static void ringSessions(void) {
	static unsigned char head[100];
	writeFile("sdmc:/session", "x", 1);
	expectIs("get ok ok 00000000 1 raw", request("get %s /session", ID));
	assert(gApp.uiSunkProgress);
	expectCommitted(head, 10, putPlain("/session", head, 10));
	assert(gApp.uiSunkProgress);
	feed(head, sizeof(head));
	expectPrefix("put rejected short ",
				 request("put %s %s new 4096 4095 " ZERO_DIGEST, ID, live()));
	assert(gApp.uiSunkProgress);
	fake.installed = true;
	expectPrefix("put committed ok ", putPlain("save:000400000f800100/session", head, 10));
	assert(!gApp.uiSunkProgress);
}

// A digest is `u:` and 64 lowercase hex digits; anything else, bare hex too, is `grammar`, before
// the spend and before any body byte.
static void digestSpelling(void) {
	static const char *const GRAMMAR[] = {
		"0000000000000000000000000000000000000000000000000000000000000000",
		"u:000000000000000000000000000000000000000000000000000000000000000",
		"u:00000000000000000000000000000000000000000000000000000000000000000",
		"U:0000000000000000000000000000000000000000000000000000000000000000",
		"u:A000000000000000000000000000000000000000000000000000000000000000",
		"x",
	};
	static unsigned char body[4096];
	char fields[160];
	feed(body, sizeof(body));
	for (size_t i = 0; i < sizeof(GRAMMAR) / sizeof(GRAMMAR[0]); i++) {
		snprintf(fields, sizeof(fields), "/spelled 4096 %s", GRAMMAR[i]);
		refused("grammar", "put", fields);
		snprintf(fields, sizeof(fields), "new 4096 2048 %s", GRAMMAR[i]);
		refused("grammar", "put", fields);
	}
	assert(inAt == 0 && !exists("sdmc:/spelled"));
}

// A listing is cut into units at byte offsets, so a line may straddle two units. With 30,000 lines
// of 37 bytes the boundary falls inside line 28,340, and the trailer is the digest of exactly the
// bytes sent.
static void listingUnits(void) {
	static char sent[30000 * 37 + 1];
	char want[200];
	VerbContext c = {.client = 7, .request = "ls 0123456789abcdef0123456789abcdef /", .id = ID};
	c.verb = &VERBS[1];
	assert(strcmp(c.verb->name, "ls") == 0 && listingBegin(&c, ""));
	for (int i = 0; i < 30000; i++) {
		snprintf(sent + i * 37, 38, "f %09d - %022d\n", i, i);
		assert(listingLine(sent + i * 37, 37));
	}
	listingEnd(OUTCOME_COMMITTED, "ok", 0);
	snprintf(want, sizeof(want), "trailer %s ls ok ok 00000000 30000 %s", ID,
			 refDigestText(sent, 30000 * 37));
	assert(strcmp(fake.last, want) == 0);
	// A unit whose digest fails stops the walk at the line that filled it, and the trailer has no
	// digest; a walk that failed on its own keeps its word.
	for (int walkFailed = 0; walkFailed < 2; walkFailed++) {
		int i = 0;
		fake.shaResult = (Result)0xC8804478;
		assert(listingBegin(&c, ""));
		while (i < 30000 && listingLine(sent + i * 37, 37)) {
			i++;
		}
		fake.shaResult = 0;
		assert(i == 28339);
		listingEnd(walkFailed ? OUTCOME_REJECTED : OUTCOME_COMMITTED, walkFailed ? "read" : "ok",
				   walkFailed ? (Result)0xC8804464 : 0);
		snprintf(want, sizeof(want), "trailer %s ls rejected %s 28340 -", ID,
				 walkFailed ? "read c8804464" : "hash c8804478");
		assert(strcmp(fake.last, want) == 0);
	}
}

// With no room for the unit list beside tier 1, a get, a listing and a screenshot are refused
// `ring` before any body byte, and they go through once there is room.
static void unitListMissing(void) {
	writeFile("sdmc:/listed", "x", 1);
	fake.linearFull = true;
	expectIs("get rejected ring d963f811", request("get %s /listed", ID));
	expectIs("hash rejected ring d963f811", request("hash %s /listed", ID));
	expectIs("ls rejected ring d963f811", request("ls %s /listed d", ID));
	assert(outLen == 0 && fake.bodyLen == 0 && fake.replies == 1);
	expectIs("screenshot rejected ring d963f811", request("screenshot %s %s top", ID, live()));
	fake.linearFull = false;
	expectIs("get ok ok 00000000 1 raw", request("get %s /listed", ID));
	assert(remove("sdmc:/listed") == 0);
}

// run <path.3dsx> checks every precondition before the spend. argv is set first and the target
// last, so a refusal leaves no target, and the chainload happens only once both succeeded.
static void run3dsx(void) {
	volatile LumaSharedConfig *luma = LUMA_SHARED_CONFIG;
	const char *argv = (const char *)&fake.hbldrArgv[1];
	mkdir("sdmc:/3ds", 0755);
	writeFile("sdmc:/3ds/demo.3dsx", "", 0);
	luma->hbldr3dsxTid = 0x000400000D921E00ULL;
	luma->selectedHbldr3dsxTid = 0x000400000D921F00ULL; // this title runs once 3Drop has exited
	luma->useHbldr = true;
	fake.titleID = 0x000400000D921F00ULL;
	fake.installed = true;
	fake.chainload = 0;
	expectIs("run committed ok 00000000 3dsx 000400000d921f00",
			 request("run %s %s /3ds/demo.3dsx stdio", ID, live()));
	assert(after == AFTER_LAUNCH && fake.chainload == 0x000400000D921F00ULL &&
		   strcmp(fake.hbldrTarget, "/3ds/demo.3dsx") == 0 && fake.hbldrArgv[0] == 2 &&
		   strcmp(argv, "sdmc:/3ds/demo.3dsx") == 0 &&
		   strcmp(argv + strlen(argv) + 1, "490200c0_3DSLINK_") == 0 && fake.hbldrOpen == 0);
	expectPrefix("run committed ok 00000000 3dsx ",
				 request("run %s %s hex:2f3364732f64656d6f2e33647378", ID, live()));
	assert(fake.hbldrArgv[0] == 1 && strcmp(argv, "sdmc:/3ds/demo.3dsx") == 0);

	// Refused before the spend, and nothing armed.
	fake.hbldrTarget[0] = '\0';
	refused("grammar", "run", "/3ds/demo.3dsx stdout");
	refused("path", "run", "/3ds/demo.txt");
	refused("protected", "run", "/3ds/3drop/key.3dsx");
	refused("path", "run", "save:000400000d921f00/x.3dsx");
	refused("notfound", "run", "/3ds/none.3dsx");
	mkdir("sdmc:/3ds/dir.3dsx", 0755);
	refused("notfound", "run", "/3ds/dir.3dsx"); // a directory
	luma->useHbldr = false;
	refused("hbldr", "run", "/3ds/demo.3dsx");
	luma->useHbldr = true;
	fake.hbldrResult = (Result)0xD88007FA;
	refused("hbldr", "run", "/3ds/demo.3dsx");
	fake.hbldrResult = 0;
	fake.installed = false;
	refused("hbldr-title", "run", "/3ds/demo.3dsx");
	fake.installed = true;
	luma->selectedHbldr3dsxTid = fake.selfTitleID;
	refused("hbldr-title", "run", "/3ds/demo.3dsx");
	luma->selectedHbldr3dsxTid = fake.titleID = 0x0004008C0D921F00ULL; // installed DLC: no code
	refused("hbldr-title", "run", "/3ds/demo.3dsx");
	luma->selectedHbldr3dsxTid = fake.titleID = 0x000400000D921F00ULL;
	assert(fake.hbldrTarget[0] == '\0' && fake.hbldrOpen == 0);

	// If hb:ldr refuses after the spend, no target is left, there is no chainload and 3Drop stays;
	// the reply names the title it would have run, as every title mutation's does.
	fake.chainload = 0;
	fake.argvResult = (Result)0xD9001830;
	expectIs("run rejected hbldr d9001830 3dsx 000400000d921f00",
			 request("run %s %s /3ds/demo.3dsx", ID, live()));
	assert(after == AFTER_NOTHING && fake.chainload == 0 && fake.hbldrTarget[0] == '\0');
	fake.argvResult = 0;
	fake.targetResult = (Result)0xD9001830;
	expectPrefix("run rejected hbldr d9001830", request("run %s %s /3ds/demo.3dsx", ID, live()));
	assert(after == AFTER_NOTHING && fake.chainload == 0 && fake.hbldrOpen == 0);
	fake.targetResult = 0;
	fake.titleID = 0x000400000F800100ULL;
}

// ls and get inside a save or extdata close the archive whatever the outcome; mtimes there are
// `-`; mkdir and rename take SD paths only.
static void archiveReads(void) {
	mkdir("save:000400000f800100", 0755);
	mkdir("save:000400000f800100/dir", 0755);
	writeFile("save:000400000f800100/dir/game.sav", "progress", 8);
	expectIs("ls ok ok 00000000", request("ls %s save:000400000f800100/ rt", ID));
	assert(strcmp(fake.body, "d - - 646972\nf 8 - 6469722f67616d652e736176\n") == 0);
	expectIs("ls ok ok 00000000", request("ls %s save:000400000f800100/dir/game.sav", ID));
	assert(strcmp(fake.body, "f 8 - 67616d652e736176\n") == 0 && fake.archives == 0);
	expectIs("get ok ok 00000000 8 raw", request("get %s save:000400000f800100/dir/game.sav", ID));
	assert(outLen == 8 && memcmp(out, "progress", 8) == 0 && fake.archives == 0);
	expectGetTrailer("progress", 8);
	expectIs("get ok ok 00000000 4 raw 8 -",
			 request("get %s save:000400000f800100/dir/game.sav range 4 9", ID));
	assert(outLen == 4 && memcmp(out, "ress", 4) == 0 && fake.archives == 0);
	expectGetTrailer("ress", 4);
	mkdir("extdata:0000055d", 0755);
	writeFile("extdata:0000055d/a.bin", "xy", 2);
	expectIs("get ok ok 00000000 2 raw", request("get %s extdata:0000055d/a.bin", ID));
	assert(outLen == 2 && memcmp(out, "xy", 2) == 0 && fake.archives == 0);
	char want[200];
	snprintf(want, sizeof(want), "hash ok ok 00000000 8 %s", refDigestText("progress", 8));
	expectIs(want, request("hash %s save:000400000f800100/dir/game.sav", ID));
	assert(fake.archives == 0);
	readFailAt = 1; // a read that fails mid-body still closes the file and its archive
	expectIs("get ok ok 00000000 8 raw", request("get %s save:000400000f800100/dir/game.sav", ID));
	expectTrailer("get rejected read c8804464 - ");
	assert(fake.archives == 0);
	readFailAt = UINT64_MAX;
	expectIs("ls rejected notfound c8804478", request("ls %s save:000400000f800200/", ID));
	expectIs("get rejected notfound c8804478", request("get %s extdata:0000055e/a.bin", ID));
	expectIs("get rejected notfound c8804478", request("get %s save:000400000f800100/nope", ID));
	fake.archiveResult = (Result)0xC8A04554;
	expectIs("ls rejected notfound c8a04554", request("ls %s save:000400000f800100/", ID));
	fake.archiveResult = 0;
	expectPrefix("get rejected path ", request("get %s save:000400000f800100/", ID)); // "/" itself
	expectPrefix("ls rejected path ", request("ls %s save:000400000F800100/", ID));
	assert(fake.archives == 0);
	refused("path", "mkdir", "save:000400000f800100/new");
	refused("path", "rename", "save:000400000f800100/dir /x");
	refused("path", "rename", "/x extdata:0000055d/b");
}

// put and delete inside a save or extdata: the body is checked before the archive opens, a failed
// write is never committed, a save's secure value is deleted and only then its change committed,
// and every refusal comes before the spend.
static void archiveWrites(void) {
	static const char SAVE[] = "save:000400000f800100";
	static unsigned char body[8192];
	char path[96], hex[DIGEST_TEXT_SIZE];
	for (size_t i = 0; i < sizeof(body); i++) {
		body[i] = (unsigned char)(i * 7 + 3);
	}
	snprintf(hex, sizeof(hex), "%s", refDigestText(body, 1000));
	fake.installed = true;
	fake.commits = fake.secureDeletes = 0;
	snprintf(path, sizeof(path), "%s/dir/game.sav", SAVE);
	expectCommitted(body, 1000, putPlain(path, body, 1000));
	assert(sameFile("save:000400000f800100/dir/game.sav", body, 1000) && fake.commits == 1 &&
		   fake.secureDeletes == 1 && fake.secureValue == (0x1000ULL << 32 | 0x0F800100) &&
		   fake.archives == 0);
	snprintf(path, sizeof(path), "%s/new/deep.bin", SAVE); // its parent folders are made
	expectCommitted(body, 10, putPlain(path, body, 10));
	assert(sameFile("save:000400000f800100/new/deep.bin", body, 10) && fake.commits == 2);

	// Nothing reaches the archive until the whole body is in and matches.
	snprintf(path, sizeof(path), "%s/dir/game.sav", SAVE);
	unsigned char other[1000];
	memcpy(other, body, sizeof(other));
	other[5] ^= 1;
	expectIs("put rejected digest d963f80a", putFile(path, body + 1, 1000, other, 1000, false));
	expectIs("put rejected short d963f802", putFile(path, body + 1, 1000, body + 1, 999, false));
	fake.shaResult = (Result)0xC8804478;
	expectIs("put rejected hash c8804478", putPlain(path, body, sizeof(body)));
	fake.shaResult = 0;
	assert(sameFile("save:000400000f800100/dir/game.sav", body, 1000) && fake.commits == 2 &&
		   fake.archives == 0);

	// A failed or short write is never committed, and it leaves the secure value, so the save is as
	// it was. A failed commit, after the value went, is `unknown`. A value that will not go commits
	// nothing. The log names the step that failed, because a failed write after a delete that went
	// through is what shows FS dropping an uncommitted change on hardware.
	fake.writeResult = (Result)0xC86044BE;
	logged[0] = '\0';
	expectIs("put rejected write c86044be", putPlain(path, body + 1, 1000));
	assert(strcmp(logged, "archive-write-failed\nput-write\n") == 0);
	fake.writeResult = 0;
	fake.fsFail = (Result)0xC8804464;
	logged[0] = '\0';
	expectIs("put rejected write c8804464", putPlain(path, body + 1, 1000));
	assert(strcmp(logged, "archive-delete-failed\nput-write\n") == 0);
	fake.fsFail = 0;
	fake.shortWrite = true;
	expectIs("put rejected write d963f80b", putPlain(path, body + 1, 1000));
	fake.shortWrite = false;
	assert(fake.commits == 2 && fake.secureDeletes == 2 && fake.archives == 0);
	fake.commitResult = (Result)0xC8804464;
	expectIs("put unknown commit c8804464", putPlain(path, body, 1000));
	assert(fake.secureDeletes == 3 && fake.archives == 0);
	fake.commitResult = 0;
	fake.secureValueResult = (Result)0xC8A04554;
	expectIs("put rejected secure-value c8a04554", putPlain(path, body, 1000));
	assert(fake.commits == 2 && fake.archives == 0);
	fake.secureValueResult = 0;

	// Refused before the spend.
	char fields[160];
	snprintf(fields, sizeof(fields), "%s/dir/game.sav 1000 %s gzip 900", SAVE, hex);
	refused("grammar", "put", fields);
	snprintf(fields, sizeof(fields), "%s/big 25165825 %s", SAVE, hex);
	refused("size", "put", fields);
	snprintf(fields, sizeof(fields), "%s/dir 1000 %s", SAVE, hex);
	refused("exists", "put", fields);
	snprintf(fields, sizeof(fields), "%s/ 1000 %s", SAVE, hex);
	refused("path", "put", fields);
	refused("self", "put", "save:0004000003d37200/a 4 " ZERO_DIGEST);
	refused("not-application", "put", "save:0004008c00030000/a 4 " ZERO_DIGEST);
	refused("unmanaged", "put", "extdata:0000008f/a 4 " ZERO_DIGEST); // HOME Menu's
	refused("unmanaged", "put", "extdata:000014d1/a 4 " ZERO_DIGEST);
	mkdir("save:000400000f800200", 0755); // its save opens, so only AM says it is not installed
	refused("notfound", "put", "save:000400000f800200/a 4 " ZERO_DIGEST);
	fake.infoResult = (Result)0xD8A08004;
	refused("am-info", "put", "save:000400000f800100/a 4 " ZERO_DIGEST);
	fake.infoResult = 0;
	fake.archiveResult = (Result)0xC8A04554;
	refused("notfound", "put", "save:000400000f800100/a 4 " ZERO_DIGEST);
	fake.archiveResult = 0;
	assert(fake.commits == 2 && fake.archives == 0);

	// In an extdata a file is written over at its size or made at that size, with no commit and no
	// secure value.
	fake.titleID = 0x0004000000055D00ULL;
	expectPrefix("put committed ok 00000000 2 ", putPlain("extdata:0000055d/a.bin", body, 2));
	assert(sameFile("extdata:0000055d/a.bin", body, 2));
	expectPrefix("put committed ok ", putPlain("extdata:0000055d/b/c.bin", body, 64));
	assert(sameFile("extdata:0000055d/b/c.bin", body, 64) && fake.commits == 2 &&
		   fake.secureDeletes == 3 && fake.archives == 0);
	fake.writeResult = (Result)0xC86044BE; // written as it goes, with no commit to hold it back
	expectIs("put unknown write c86044be", putPlain("extdata:0000055d/b/c.bin", body, 64));
	fake.writeResult = 0;
	refused("size", "put", "extdata:0000055d/a.bin 3 " ZERO_DIGEST);
	refused("notfound", "put", "extdata:0000055e/a.bin 3 " ZERO_DIGEST); // no such title installed

	// delete takes a file or an empty directory, never a subtree. A save's delete is committed, and
	// a failed one is `unknown` there (extdata has no commit).
	expectIs("delete committed ok 00000000",
			 request("delete %s %s extdata:0000055d/a.bin", ID, live()));
	assert(!exists("extdata:0000055d/a.bin") && fake.commits == 2);
	refused("unmanaged", "delete", "extdata:0000008f/a.bin");
	fake.titleID = 0x000400000F800100ULL;
	refused("path", "delete", "save:000400000f800100/ r");
	refused("refused", "delete", "save:000400000f800100/new r");
	refused("refused", "delete", "extdata:0000055d/b r");
	refused("notfound", "delete", "save:000400000f800100/nope");
	fake.commitResult = (Result)0xC8804464;
	expectIs("delete unknown commit c8804464",
			 request("delete %s %s save:000400000f800100/new/deep.bin", ID, live()));
	fake.commitResult = 0;
	writeFile("save:000400000f800100/new/deep.bin", "", 0);
	fake.secureValueResult = (Result)0xC8A04554; // as for a put: nothing committed
	expectIs("delete rejected secure-value c8a04554",
			 request("delete %s %s save:000400000f800100/new/deep.bin", ID, live()));
	assert(fake.commits == 2 && fake.secureDeletes == 4);
	fake.secureValueResult = 0;
	writeFile("save:000400000f800100/new/deep.bin", "", 0); // the fake's delete is not held back
	fake.fsFail = (Result)0xC8804464;
	expectIs("delete unknown delete c8804464",
			 request("delete %s %s save:000400000f800100/new/deep.bin", ID, live()));
	fake.fsFail = 0;
	// If the archive opens for the check but not for the run, the answer is `open`, as for a put,
	// and nothing is touched.
	fake.archiveResult = (Result)0xC8A04554;
	fake.archiveOpensOk = 1;
	expectIs("delete rejected open c8a04554",
			 request("delete %s %s save:000400000f800100/new/deep.bin", ID, live()));
	fake.archiveOpensOk = 1;
	expectIs("put rejected open c8a04554",
			 putPlain("save:000400000f800100/new/deep.bin", body, 10));
	fake.archiveResult = 0;
	expectIs("delete committed ok 00000000",
			 request("delete %s %s save:000400000f800100/new/deep.bin", ID, live()));
	expectIs("delete committed ok 00000000",
			 request("delete %s %s save:000400000f800100/new", ID, live()));
	assert(!exists("save:000400000f800100/new") && fake.commits == 4 && fake.secureDeletes == 6 &&
		   fake.archives == 0);
}

// A tree of files under folders goes into a save and into an extdata, whatever word the archive
// returns for a folder that already exists. Here those are two words the SD card never uses, in two
// descriptions: 0xBE for the save and 0x9F for the extdata. Each file's own folder is made under
// the folders that the files before it made. On a console, the put of the benchmark's stub, 8 files
// in three folders, stopped at the second folder.
static void archiveTree(void) {
	static const Result WORDS[] = {(Result)0xC8E044BE, (Result)0xC8A0449F};
	static const char *const ROOTS[] = {"save:000400000f800100/stub", "extdata:0000055d/stub"};
	static const u64 TITLES[] = {0x000400000F800100ULL, 0x0004000000055D00ULL};
	static unsigned char body[4096];
	char path[96];
	fake.installed = true;
	for (size_t root = 0; root < 2; root++) {
		fake.titleID = TITLES[root];
		fake.dirExists = WORDS[root];
		mkdir(root ? "extdata:0000055d" : "save:000400000f800100", 0755);
		for (int file = 0; file < 8; file++) {
			snprintf(path, sizeof(path), "%s/romfs/d%d/s%d.bin", ROOTS[root], file / 3, file);
			memset(body, file + 1, sizeof(body));
			expectCommitted(body, sizeof(body), putPlain(path, body, sizeof(body)));
			assert(sameFile(path, body, sizeof(body)));
		}
		// A file where a folder is wanted still fails, in the create's word, and makes nothing;
		// an extdata is written as it goes, so there the put is `unknown`.
		const char *fails = root ? "put unknown write c8804478" : "put rejected write c8804478";
		snprintf(path, sizeof(path), "%s/romfs/d0/s0.bin/x.bin", ROOTS[root]);
		expectIs(fails, putPlain(path, body, sizeof(body)));
		assert(!exists(path));
	}
	fake.dirExists = 0;
	fake.titleID = 0x000400000F800100ULL;
	assert(fake.archives == 0);
}

// An archive put writes from tier 1 with no worker, so it flushes the body itself. Under
// ENGINE_FLOOR its digest flushes nothing (hash.c), and FS would then write what memory held
// before.
static void archiveFlush(void) {
	static unsigned char body[4096];
	fillRandom(body, sizeof(body), 4);
	fake.installed = true;
	fake.titleID = 0x000400000F800100ULL;
	fake.flushes = 0;
	fake.writtenTo = 0;
	expectCommitted(body, sizeof(body),
					putPlain("save:000400000f800100/flush.bin", body, sizeof(body)));
	u64 from = (u32)(uintptr_t)ringTier1(), to = from + sizeof(body);
	bool before = false;
	for (unsigned i = 0; i < fake.flushes && i < 8; i++) {
		before |= fake.flushAddr[i] <= from && fake.flushAddr[i] + (u64)fake.flushSize[i] >= to &&
				  fake.flushWrittenTo[i] == 0;
	}
	assert(before && sameFile("save:000400000f800100/flush.bin", body, sizeof(body)));
}

// AM is asked in chunks of 32 (src/titles.c), so n titles cost ceil(n / 32) calls after the count
// and the list. Each line carries its own title's info, in AM's order.
static void moreLines(char *dst, size_t cap, unsigned n) {
	for (unsigned i = 0; i < n; i++) {
		size_t at = strlen(dst);
		snprintf(dst + at, cap - at, "t %u %u %016llx\n", i + 1, 4096 * (i + 1),
				 0x0004000000100000ULL + ((unsigned long long)i << 8));
	}
}

static void titleChunks(void) {
	// titles listed, calls after the count and the list
	static const unsigned SIZES[][2] = {{1, 1}, {32, 1}, {33, 2}, {64, 2}, {65, 3}};
	fake.installed = false;
	fake.moreInfo = true;
	for (unsigned s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); s++) {
		char want[4096] = "";
		fake.moreTitles = SIZES[s][0];
		moreLines(want, sizeof(want), fake.moreTitles);
		unsigned amCalls = fake.amCalls;
		expectIs("ls ok ok 00000000", request("ls %s title:", ID));
		assert(fake.amCalls - amCalls == 2 + SIZES[s][1]);
		assert(strcmp(fake.body, want) == 0);
	}
	fake.moreTitles = 0;
	fake.moreInfo = false;
}

// A chunk that AM fails on is asked again one title at a time. The title it has no info for is
// still listed, and the others keep theirs.
static void titleFallback(void) {
	char want[4096] = "t - - 000400000f800100\n";
	fake.installed = true;
	fake.infoResult = (Result)0xD8A08004;
	fake.moreInfo = true;
	fake.moreTitles = 40;
	moreLines(want, sizeof(want), fake.moreTitles);
	unsigned amCalls = fake.amCalls;
	expectIs("ls ok ok 00000000", request("ls %s title:", ID));
	assert(fake.amCalls - amCalls == 2 + (1 + 32) + 1); // the failed chunk and its 32 alone, then 9
	assert(strcmp(fake.body, want) == 0);
	fake.infoResult = 0;
	fake.moreInfo = false;
	fake.moreTitles = 0;
}

// A false return from `each` stops the walk: no title after it, and no chunk or fallback call for
// one.
static unsigned eachSeen;
static bool eachStopsAt40(u64 titleID, const AM_TitleInfo *info) {
	(void)titleID;
	(void)info;
	return ++eachSeen < 40;
}
static void titleStops(void) {
	fake.installed = false;
	fake.moreTitles = 100;
	for (unsigned batched = 0; batched < 2; batched++) {
		fake.moreInfo = batched;
		eachSeen = 0;
		unsigned amCalls = fake.amCalls;
		assert(R_SUCCEEDED(titleEnumerate(eachStopsAt40)));
		// The calls are the count and the list, then two chunks, or the first chunk and 8 titles of
		// the second one at a time.
		assert(eachSeen == 40 && fake.amCalls - amCalls == 2 + (batched ? 2 : 33 + 1 + 8));
	}
	fake.moreTitles = 0;
	fake.moreInfo = false;
}

static void titles(void) {
	titleChunks();
	titleFallback();
	titleStops();
	fake.installed = true;
	expectIs("ls ok ok 00000000", request("ls %s title:", ID));
	assert(strcmp(fake.body, "t 1 0 000400000f800100\n") == 0);
	fake.infoResult = (Result)0xD8A08004; // AM has no info for the title, which is still listed
	request("ls %s title:", ID);
	assert(strcmp(fake.body, "t - - 000400000f800100\n") == 0);
	expectTrailer("ls ok ok 00000000 1 ");
	// A host gone mid-listing: AM is asked about no title after the send that failed, which is the
	// flush that the 90th 23-byte line forces out of a 2048-byte buffer. No trailer follows. AM has
	// no info for any title, so each chunk is asked again one title at a time. The calls are the
	// count and the list, then 32 and 32 titles each with their chunk's call, then the third
	// chunk's call and its 26 titles.
	fake.moreTitles = 200;
	fake.bodySendFails = true;
	unsigned amCalls = fake.amCalls;
	expectIs("ls ok ok 00000000", request("ls %s title:", ID));
	assert(fake.amCalls - amCalls == 2 + 33 + 33 + 27 && strncmp(fake.last, "trailer ", 8) != 0);
	fake.bodySendFails = false;
	fake.sendFails = true; // AM is not asked at all when the header did not go out
	amCalls = fake.amCalls;
	request("ls %s title:", ID);
	assert(fake.amCalls == amCalls);
	fake.sendFails = false;
	fake.moreTitles = 0;
	expectIs("ls rejected am-info d8a08004", request("ls %s title:000400000f800100", ID));
	fake.infoResult = 0;
	request("ls %s title:000400000f800100", ID);
	assert(strcmp(fake.body, "t 1 0 000400000f800100\n") == 0);
	expectPrefix("ls rejected self ", request("ls %s title:0004000003d37200", ID)); // 3Drop itself
	expectPrefix("ls rejected grammar ", request("ls %s title: d", ID));
	// Whatever title hosts the process, `self` is 3Drop's own, and the host is not.
	assert(strstr(request("stats %s self", ID), " tid=0004000003d37200 "));
	refused("notfound", "delete", "title:000400000d921e00");

	// uninstall: every refusal comes before the spend, and the delete spends one.
	fake.titleDeletes = fake.ticketDeletes = 0;
	refused("unmanaged", "delete", "title:0004001000021000"); // a system application
	refused("self", "delete", "title:0004000003d37200");
	refused("grammar", "delete", "title:000400000F800100");
	refused("grammar", "delete", "title:000400000f800100 r");
	refused("grammar", "delete", "/x ticket");
	// Its ticket is held, and goes only when `ticket` is sent, because a retail title's ticket is
	// its license.
	fake.ticketID = 0x000400000F800100ULL;
	expectIs("delete committed ok 00000000 000400000f800100",
			 request("delete %s %s title:000400000f800100", ID, live()));
	assert(fake.titleDeletes == 1 && fake.ticketDeletes == 0 && fake.ticketID != 0);
	fake.installed = true;
	expectIs("delete committed ok 00000000 000400000f800100",
			 request("delete %s %s title:000400000f800100 ticket", ID, live()));
	assert(fake.titleDeletes == 2 && fake.ticketDeletes == 1 &&
		   fake.ticketDeleted == 0x000400000F800100ULL);
	refused("notfound", "delete", "title:000400000f800100");
	expectPrefix("ls rejected notfound ", request("ls %s title:000400000f800100", ID));
	// A DLC's ticket is its purchase, so `ticket` is refused, and the title goes without it.
	fake.installed = true;
	fake.titleID = fake.ticketID = 0x0004008C00030002ULL;
	fake.titleDeletes = fake.ticketDeletes = 0;
	refused("not-application", "delete", "title:0004008c00030002 ticket");
	expectIs("delete committed ok 00000000 0004008c00030002",
			 request("delete %s %s title:0004008c00030002", ID, live()));
	assert(fake.titleDeletes == 1 && fake.ticketDeletes == 0 && fake.ticketID != 0);
	fake.ticketID = 0;

	// run takes self, 3Drop's own id (also self), or an installed application; a patch has no code.
	fake.installed = true;
	fake.titleID = 0x0004000E00030000ULL;
	refused("not-application", "run", "title:0004000e00030000");
	fake.installed = false;
	fake.titleID = 0x000400000F800100ULL;
	refused("not-application", "run", "title:0004008c00030000");
	refused("notfound", "run", "title:000400000f800100");
	expectIs("run committed ok 00000000 self", request("run %s %s self", ID, live()));
	assert(after == AFTER_RELAUNCH && fake.chainload == fake.selfTitleID);
	request("run %s %s title:0004000003d37200", ID, live());
	assert(after == AFTER_RELAUNCH);
	fake.installed = true;
	expectIs("run committed ok 00000000 000400000f800100",
			 request("run %s %s title:000400000f800100", ID, live()));
	assert(after == AFTER_LAUNCH && fake.chainload == 0x000400000F800100ULL);
	refused("path", "run", "reboot");			// no reboot word: it hung the device
	refused("path", "run", "000400000f800100"); // a title's one spelling is `title:<id>`
	refused("grammar", "run", "title:000400000F800100");
	refused("grammar", "run", "self stdio");
	refused("grammar", "run", "title:000400000f800100 stdio");
	refused("grammar", "run", "self game"); // game is a title's word
	refused("grammar", "run", "/3ds/demo.3dsx game");
	run3dsx();
}

// A launch that Luma's loader would stop the console on is refused as `layeredfs`: a title with a
// romfs folder that the user did not call a game, since Luma cannot apply one to homebrew, just as
// it could not to the Testing Tool. The word `game` launches it, without looking at the card. An
// exheader.bin goes only at a size Luma takes.
static void layeredFs(void) {
#define LFS "/luma/titles/000400000f800100"
	const char *launch = "run committed ok 00000000 000400000f800100";
	fake.installed = true;
	fake.titleID = 0x000400000F800100ULL;
	// pathGate made the title's folder, so it is made here for the case to run alone.
	mkdir("sdmc:/luma", 0755);
	mkdir("sdmc:/luma/titles", 0755);
	mkdir("sdmc:" LFS, 0755);
	expectIs(launch, request("run %s %s title:000400000f800100", ID, live()));
	assert(fake.fsCalls == 1); // no romfs folder: one open of it, a launch's only FS call
	// FS has two words for a name that is not there, and a launch goes ahead on either.
	fake.notFound = (Result)0xC92044FA;
	expectIs(launch, request("run %s %s title:000400000f800100", ID, live()));
	fake.notFound = 0;
	expectIs(launch, request("run %s %s title:000400000f800100 game", ID, live()));
	assert(fake.fsCalls == 0); // and with the word, there are no FS calls
	assert(mkdir("sdmc:" LFS "/romfs", 0755) == 0);
	fake.chainload = 0;
	int dirs = fake.dirs;
	expectIs("run rejected layeredfs 00000000",
			 request("run %s %s title:000400000f800100", ID, live()));
	assert(fake.chainload == 0 && fake.dirs == dirs); // the folder it opened is closed
	expectIs(launch, request("run %s %s title:000400000f800100 game", ID, live()));
	assert(fake.fsCalls == 0 && fake.chainload == 0x000400000F800100ULL);
	fake.sdGone = (Result)0xC8804471; // a card failing to say whether the folder is there
	fake.chainload = 0;
	expectIs("run rejected layeredfs c8804471",
			 request("run %s %s title:000400000f800100", ID, live()));
	assert(fake.chainload == 0);
	expectIs(launch, request("run %s %s title:000400000f800100 game", ID, live()));
	fake.sdGone = 0;
	refused("layeredfs", "put", LFS "/exheader.bin 4 " ZERO_DIGEST);
	writeFile("sdmc:/exheader.bin", "12345", 5);
	refused("layeredfs", "rename", "/exheader.bin " LFS "/exheader.bin");
	assert(remove("sdmc:/exheader.bin") == 0 && rmdir("sdmc:" LFS "/romfs") == 0);
	expectIs(launch, request("run %s %s title:000400000f800100", ID, live()));
#undef LFS
}

// A folder moved onto a title's directory brings its exheader.bin along, and is judged by the size
// of the one inside it, as a put is. FS looks it up, so a card that will not say refuses the move.
// A folder moved anywhere else, or holding no exheader.bin, goes through.
static void layeredFsFolder(void) {
#define LFS "/luma/titles/000400000f800100"
	static const Result NOT_THERE[] = {(Result)0xC8804478, (Result)0xC92044FA};
	const char *moved = "rename committed ok 00000000";
	unsigned char right[0x400] = {0};
	int dirs = fake.dirs;
	fake.installed = true;
	fake.titleID = 0x000400000F800100ULL;
	mkdir("sdmc:/luma", 0755);
	mkdir("sdmc:/luma/titles", 0755);
	rmdir("sdmc:" LFS); // a rename onto it would find it there
	assert(mkdir("sdmc:/lfsFolder", 0755) == 0);
	writeFile("sdmc:/lfsFolder/exheader.bin", "12345", 5);
	expectIs("rename rejected layeredfs 00000000",
			 request("rename %s %s /lfsFolder " LFS, ID, live()));
	// Refused in any letter case of the name.
	refused("layeredfs", "rename", "/lfsFolder /LUMA/Titles/000400000F800100");
	// Anywhere else the rename goes through.
	expectIs(moved, request("rename %s %s /lfsFolder /lfsMoved", ID, live()));
	expectIs(moved, request("rename %s %s /lfsMoved /lfsFolder", ID, live()));
	writeFile("sdmc:/lfsFolder/exheader.bin", right, sizeof(right));
	expectIs(moved, request("rename %s %s /lfsFolder " LFS, ID, live()));
	assert(remove("sdmc:" LFS "/exheader.bin") == 0 && rmdir("sdmc:" LFS) == 0);
	for (size_t i = 0; i < sizeof(NOT_THERE) / sizeof(NOT_THERE[0]); i++) { // holding none
		fake.notFound = NOT_THERE[i];
		assert(mkdir("sdmc:/lfsFolder", 0755) == 0);
		expectIs(moved, request("rename %s %s /lfsFolder " LFS, ID, live()));
		assert(rmdir("sdmc:" LFS) == 0);
	}
	fake.notFound = 0;
	// An exheader.bin that FS cannot look up (a link to itself) is not treated as missing.
	assert(mkdir("sdmc:/lfsFolder", 0755) == 0);
	assert(symlink("exheader.bin", "sdmc:/lfsFolder/exheader.bin") == 0);
	const char *reply = request("rename %s %s /lfsFolder " LFS, ID, live());
	expectPrefix("rename rejected layeredfs ", reply);
	assert(strcmp(reply, "rename rejected layeredfs 00000000") != 0); // with FS's Result
	// Elsewhere the exheader.bin is not looked at.
	expectIs(moved, request("rename %s %s /lfsFolder /lfsMoved", ID, live()));
	assert(remove("sdmc:/lfsMoved/exheader.bin") == 0 && rmdir("sdmc:/lfsMoved") == 0);
	assert(mkdir("sdmc:" LFS, 0755) == 0 && fake.dirs == dirs);
#undef LFS
}

// There is one word per title condition on every verb that names a title, and a verb that maps a
// condition its own way turns its cell red. An id's own word comes before any AM call (and before
// `stranded`, see stranded()) and keeps the nonce. `notfound` is AM's TITLE_ABSENT, "" means the
// verb takes the title, and NULL is not asked here (an install: testInstall's).
static void titleWords(void) {
	enum { LS, RUN, SEED, UNINSTALL, TICKET, NEW, UPDATE, SAVE, LUMA, VERBS };
	static const char *const NAMES[VERBS] = {"ls",	"run", "seed", "delete", "delete",
											 "put", "put", "put",  "mkdir"};
#define ALL(w)                                                                                     \
	{ w, w, w, w, w, w, w, w, w }
	static const struct {
		u64 id;
		const char *word[VERBS];
	} ROWS[] = {
		// ls title:, run, seed, delete title:, ... ticket, CIA put new, ... update, put save:,
		// mkdir
		// /luma/titles
		{0x0004003000008F02ULL, ALL("unmanaged")}, // HOME Menu
		{0x0004800400123400ULL, ALL("unmanaged")}, // DSiWare
		{0x000400020F800100ULL, ALL("unmanaged")}, // a demo
		{0x000400010F800100ULL, ALL("unmanaged")}, // a Download Play child
		{0x000400000F800102ULL, ALL("unmanaged")}, // variation 02
		// 3Drop itself: run relaunches it, and it installs as `self` and no other way (`route`).
		{0x0004000003D37200ULL,
		 {"self", "", "self", "self", "self", "route", "route", "self", "self"}},
		{0x0004000003D37201ULL, ALL("self")}, // another variation of 3Drop's
		{0x0004000E03D37200ULL, ALL("self")}, // 3Drop's patch
		{0x0004008C03D37200ULL, ALL("self")}, // 3Drop's DLC
		{0x0004000E0F800100ULL,				  // a patch, not installed
		 {"notfound", "not-application", "not-application", "notfound", "not-application", NULL,
		  "notfound", "not-application", "not-application"}},
		{0x0004008C0F800100ULL, // DLC, not installed
		 {"notfound", "not-application", "not-application", "notfound", "not-application", NULL,
		  "notfound", "not-application", "not-application"}},
		// An application not installed: its LayeredFS may go before its install.
		{0x000400000F800200ULL,
		 {"notfound", "notfound", "notfound", "notfound", "notfound", NULL, "notfound", "notfound",
		  ""}},
		// For an application that AM lists but gives no info for, nothing is done on a guess.
		{0x000400000F800300ULL,
		 {"am-info", "am-info", "am-info", "am-info", "am-info", "am-info", "am-info", "am-info",
		  ""}},
	};
#undef ALL
	for (size_t r = 0; r < sizeof(ROWS) / sizeof(ROWS[0]); r++) {
		char tid[17], want[64];
		bool noInfo = ROWS[r].id == 0x000400000F800300ULL;
		snprintf(tid, sizeof(tid), "%016llx", (unsigned long long)ROWS[r].id);
		fake.titleID = noInfo ? ROWS[r].id : 0x000400000F800100ULL;
		fake.installed = noInfo;
		fake.infoResult = noInfo ? (Result)0xD8A08004 : 0;
		for (int v = 0; v < VERBS; v++) {
			const char *word = ROWS[r].word[v], *nonce = live(), *got = NULL;
			unsigned amCalls = fake.amCalls;
			if (!word) {
				continue;
			}
			TestCIA cia = build(0x00010004, 0x00010004, ROWS[r].id, 1, 1);
			switch (v) {
			case LS:
				got = request("ls %s title:%s", ID, tid);
				break;
			case RUN:
				got = request("run %s %s title:%s", ID, nonce, tid);
				break;
			case SEED:
				got =
					request("seed %s %s title:%s 00112233445566778899aabbccddeeff", ID, nonce, tid);
				break;
			case UNINSTALL:
				got = request("delete %s %s title:%s", ID, nonce, tid);
				break;
			case TICKET:
				got = request("delete %s %s title:%s ticket", ID, nonce, tid);
				break;
			case NEW:
			case UPDATE:
				feed(cia.bytes, cia.extent);
				got = request("put %s %s %s %llu %llu %s", ID, nonce, v == NEW ? "new" : "update",
							  (unsigned long long)cia.extent, (unsigned long long)cia.prefix,
							  refDigestText(cia.bytes, cia.prefix));
				break;
			case SAVE:
				got = request("put %s %s save:%s/a 4 " ZERO_DIGEST, ID, nonce, tid);
				break;
			case LUMA:
				got = request("mkdir %s %s /luma/titles/%s", ID, nonce, tid);
				break;
			}
			free(cia.bytes);
			if (!*word) {
				snprintf(want, sizeof(want), "%s committed ok ", NAMES[v]);
			} else {
				snprintf(want, sizeof(want), "%s rejected %s %s", NAMES[v], word,
						 strcmp(word, "notfound") == 0	? "d8a083fa"
						 : strcmp(word, "am-info") == 0 ? "d8a08004"
														: "");
			}
			expectPrefix(want, got);
			if (*word && strcmp(word, "notfound") != 0 && strcmp(word, "am-info") != 0) {
				assert(fake.amCalls == amCalls);
				assert(v == NEW || v == UPDATE || strcmp(nonceLive(), nonce) == 0);
			}
		}
	}
	fake.titleID = 0x000400000F800100ULL;
	fake.installed = false;
	fake.infoResult = 0;
}

// seed works for an installed application only, and every refusal comes before the spend. The seed
// goes in under exactly the requested title, once, and a different seed already held is never
// replaced.
static void seeds(void) {
	static const u8 SEED[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
								0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	static const char HEX[] = "00112233445566778899aabbccddeeff";
	char fields[80];
	fake.titleID = 0x000400000F800100ULL;
	fake.installed = true;
	gated("grammar", "seed", "000400000f800100 00112233445566778899aabbccddeeff");
	gated("grammar", "seed", "title:000400000F800100 00112233445566778899aabbccddeeff");
	gated("grammar", "seed", "title:000400000f800100 00112233445566778899AABBCCDDEEFF");
	gated("grammar", "seed", "title:000400000f800100 00112233445566778899aabbccddee");
	gated("not-application", "seed", "title:0004000e0f800100 00112233445566778899aabbccddeeff");
	gated("not-application", "seed", "title:0004008c0f800101 00112233445566778899aabbccddeeff");
	gated("unmanaged", "seed", "title:0004001000021000 00112233445566778899aabbccddeeff");
	gated("self", "seed", "title:0004000003d37200 00112233445566778899aabbccddeeff");
	fake.installed = false;
	gated("notfound", "seed", "title:000400000f800100 00112233445566778899aabbccddeeff");
	fake.installed = true;
	fake.seedGetResult = (Result)0xD8E0806A; // the database refused the read, so nothing is written
	refused("seed", "seed", "title:000400000f800100 00112233445566778899aabbccddeeff");
	assert(strstr(fake.reply, " seed rejected seed d8e0806a") && fake.seedAdds == 0);
	fake.seedGetResult = 0;

	snprintf(fields, sizeof(fields), "title:000400000f800100 %s", HEX);
	expectIs("seed committed ok 00000000 000400000f800100 added",
			 request("seed %s %s %s", ID, live(), fields));
	assert(fake.seedAdds == 1 && fake.seedTitleID == 0x000400000F800100ULL &&
		   memcmp(fake.seed, SEED, sizeof(SEED)) == 0);
	expectIs("seed committed ok 00000000 000400000f800100 present",
			 request("seed %s %s %s", ID, live(), fields));
	assert(fake.seedAdds == 1);
	refused("exists", "seed", "title:000400000f800100 00112233445566778899aabbccddeefe");
	assert(fake.seedAdds == 1 && memcmp(fake.seed, SEED, sizeof(SEED)) == 0);
	// Another application's seed goes under its own id, never the one a request before it named.
	fake.titleID = 0x000400000F800200ULL;
	expectIs("seed committed ok 00000000 000400000f800200 added",
			 request("seed %s %s title:000400000f800200 %s", ID, live(), HEX));
	assert(fake.seedAdds == 2 && fake.seedTitleID == 0x000400000F800200ULL);
	fake.titleID = 0x000400000F800100ULL;

	// If FS refuses the add after the spend, the reply says so with its Result and nothing is
	// taken.
	fake.seedTitleID = 0;
	fake.seedAddResult = (Result)0xC92044E6;
	expectIs("seed rejected seed c92044e6 000400000f800100",
			 request("seed %s %s %s", ID, live(), fields));
	assert(fake.seedAdds == 2 && fake.seedTitleID == 0);
	fake.seedAddResult = 0;
}

// While stranded, whatever needs tier 1 or AM is refused before the spend and changes nothing.
static void stranded(void) {
	static const char *const MUTATIONS[][2] = {
		{"put", "new 8192 2048 " ZERO_DIGEST},
		{"put", "/stranded 4 " ZERO_DIGEST},
		{"put", "save:000400000f800100/a 4 " ZERO_DIGEST},
		{"delete", "save:000400000f800100/a"},
		{"delete", "title:000400000f800100"},
		{"screenshot", "top"},
		{"run", "self"},
		{"run", "title:000400000f800100"},
		{"seed", "title:000400000f800100 00112233445566778899aabbccddeeff"},
	};
	static const char *const READS[] = {
		"get %s /stranded",
		"hash %s /stranded",
		"ls %s /",
		"ls %s title:",
		"ls %s title:000400000f800100",
		BENCH("bench %s mb", "bench %s rx 1", "bench %s tx 1", "bench %s file 1", "bench %s hw")};
	static unsigned char body[4096];
	char receipt[REPLY_MAX], stats[REPLY_MAX], want[64];
	fake.joinResult = (Result)0xD8E007F7;
	expectIs("put unknown stranded 00000000", putPlain("/stranded", body, 4096));
	assert(ringStranded());
	snprintf(receipt, sizeof(receipt), "%s", request("stats %s op", ID));
	snprintf(stats, sizeof(stats), "%s", request("stats %s", ID));
	unsigned amCalls = fake.amCalls, logs = fake.logs;
	ServerStatus screen = gApp.status;
	for (size_t i = 0; i < sizeof(MUTATIONS) / sizeof(MUTATIONS[0]); i++) {
		refused("stranded", MUTATIONS[i][0], MUTATIONS[i][1]);
	}
	// A title's own word needs no AM, so it comes first, as a path's does: parse, gate, then state.
	refused("unmanaged", "delete", "title:0004003000008f02");
	refused("not-application", "run", "title:0004000e0f800100");
	refused("self", "seed", "title:0004000003d37200 00112233445566778899aabbccddeeff");
	refused("unmanaged", "put", "save:0004003000008f02/a 4 " ZERO_DIGEST);
	expectPrefix("ls rejected unmanaged ", request("ls %s title:0004003000008f02", ID));
	for (size_t i = 0; i < sizeof(READS) / sizeof(READS[0]); i++) {
		snprintf(want, sizeof(want), "%.*s rejected stranded ", (int)strcspn(READS[i], " "),
				 READS[i]);
		expectPrefix(want, request(READS[i], ID));
	}
	refused("grammar", "run", "self stdio");
	assert(strcmp(request("stats %s op", ID), receipt) == 0 &&
		   strcmp(request("stats %s", ID), stats) == 0);
	assert(fake.amCalls == amCalls && fake.logs == logs && gApp.status == screen);
	assert(exists("sdmc:/stranded" SD_PUT_TEMP));
	fake.joinResult = 0;
	ringPoll(); // the worker returned, so the ring is lent again and the put's temp goes
	assert(!ringStranded() && !exists("sdmc:/stranded" SD_PUT_TEMP));
}

// `stats self` at its widest, key by key in this order. A new key or a wider format is priced
// against REPLY_MAX here before it ships.
static void statsSelfFits(const char *reply) {
	static const char WIDEST[] =
		" ver=65535 tid=0004000003d37200 state=stranded am_pending=4294967295 am_cleanup=255"
		" am_rc=ffffffff free=18446744073709551615 cluster=4294967295 clusters=4294967295"
		" am_titles=4294967295 am_tickets=4294967295 wifi=255 bat=7 charging=1 ac=1"
		" lin_free=4294967295 model=n2dsxl new3ds=1 sysver=xxxxxxxxxxxxxxxxxxxxxxx"
		" luma=255.255.255 region=JPN up_ms=18446744073709551615 new_n=4294967295 lid=closed";
	const char *field = strstr(reply, " ver="), *widest = WIDEST;
	assert(field);
	while (*field && *widest) {
		size_t length = strcspn(field + 1, " ") + 1, widestLength = strcspn(widest + 1, " ") + 1;
		assert(strncmp(field, widest, strcspn(widest, "=") + 1) == 0 && length <= widestLength);
		field += length;
		widest += widestLength;
	}
	assert(*field == '\0' && *widest == '\0');
	size_t width = strlen("response  ") + 32 + 64 + strlen(PROTO_TOKEN) +
				   strlen(" stats ok ok 00000000") + strlen(WIDEST);
	assert(width == 528 && width + 1 <= REPLY_MAX);
}

// A transfer's `stats`, with every counter at its declared type's maximum, is the widest reply. It
// must still be answered whole, never as `response-too-long`. A new key or a wider format is priced
// here, as `stats self`'s is above.
static void statsTransferFits(void) {
	const InstallStats kept = gStats;
	const u64 wireBytes = gApp.wireBytes;
	statsReset(ID);
	gApp.wireBytes = gStats.totalMs = UINT64_MAX;
	gStats.firstWriteMs = gStats.amWriteMaxMs = UINT32_MAX;
	gStats.firstSdIoMs = gStats.sdIoMaxMs = UINT32_MAX;
	gStats.ringCap = gStats.ringHigh = gStats.stallMs = UINT64_MAX;
	gStats.sdNeeded = gStats.sdFreeBefore = gStats.sdFreeAfter = UINT64_MAX;
	gStats.mbCount = gStats.slowMb = gStats.worstMbMs = UINT32_MAX;
	gStats.stallN = gStats.stallMaxMs = UINT32_MAX;
	gStats.join = STATS_JOIN_FAILED;
	gStats.amWriteMaxKnown = gStats.sdIoMaxKnown = gStats.sdFreeAfterKnown = true;
	gStats.totalKnown = true;
	for (int k = 0; k < PHASE_KIND_COUNT; k++) {
		gStats.phaseUs[k] = UINT64_MAX;
		gStats.phaseN[k] = UINT32_MAX;
	}
	char text[STATS_TEXT_MAX];
	assert(statsFormat(text, sizeof(text)));
	const char *reply = request("stats %s", ID);
	expectPrefix("stats ok ok 00000000 bytes=18446744073709551615 ", reply);
	assert(strlen(fake.reply) + 1 <= REPLY_MAX);
	gStats = kept;
	gApp.wireBytes = wireBytes;
}

static void stats(void) {
	statsReset("");
	gApp.wireBytes = 42;
	gStats.phaseUs[PHASE_AM_WRITE] = 1500;
	gStats.phaseN[PHASE_AM_WRITE] = 3;
	const char *reply = request("stats %s", ID);
	// A first write under a whole millisecond was measured, so it reads 0, not `-`. With no write
	// after the first, as with a CIA of one unit, there is no measurement.
	expectPrefix("stats ok ok 00000000 bytes=42 total_ms=- first_write_ms=0 ", reply);
	assert(strstr(reply, " am_write_max_ms=- ") && strstr(reply, " first_sd_io_ms=- "));
	assert(strstr(reply, " am_write_us=1500 am_write_n=3 ") && strstr(reply, " stall_n=0 "));
	gStats.amWriteMaxMs = 7;
	gStats.amWriteMaxKnown = true;
	assert(strstr(request("stats %s", ID), " am_write_max_ms=7 "));
	// The SD calls count as the ring's only when there is a ring, because a hash's reads are timed
	// as SD calls too.
	gStats.phaseN[PHASE_SD_IO] = 2;
	assert(strstr(request("stats %s", ID), " first_sd_io_ms=- sd_io_max_ms=- "));
	gStats.ringCap = 1 << 20;
	assert(strstr(request("stats %s", ID), " first_sd_io_ms=0 sd_io_max_ms=- "));
	statsReset("");
	reply = request("stats %s", ID);
	expectPrefix("stats ok ok 00000000 bytes=42 total_ms=- first_write_ms=- ", reply);
	assert(strstr(reply, " sd_needed=- sd_free_before=- sd_free_after=- ")); // no space asked yet
	fake.pending = 2;
	fake.needsCleanup = 1;
	reply = request("stats %s self", ID);
	assert(strstr(reply, " ver=64341 tid=0004000003d37200 state=ready am_pending=2 am_cleanup=1 "
						 "am_rc=00000000 free="));
	expectPrefix("stats rejected grammar ", request("stats %s nosuch", ID));
	// A megabyte of exactly STATS_SLOW_MB_MS is slow, because slow_mb counts 2 s or more.
	statsReset("");
	gStats.firstByteMs = 1000;
	statsMegabyte(1000 + STATS_SLOW_MB_MS);
	statsMegabyte(1000 + 2 * STATS_SLOW_MB_MS - 1);
	assert(gStats.mbCount == 2 && gStats.slowMb == 1 && gStats.worstMbMs == STATS_SLOW_MB_MS);

	// No mcu::HWC session is opened, because one opener waits for another's to close.
	fake.installed = true;
	fake.ticketID = fake.titleID;
	fake.importTicket = true; // a canceled install's ticket is still held, so the gap shows
	sysinfoInit();
	unsigned mcuOpens = fake.mcuOpens;
	// up_ms is the console's uptime, from the system tick, never the wall clock; no `new` has
	// started in this process.
	u64 nowMs = fake.nowMs;
	fake.nowMs = 7261003;
	fake.bootMs = 1003;
	reply = request("stats %s self", ID);
	fake.nowMs = nowMs;
	fake.bootMs = 0;
	assert(strstr(reply, " clusters=2097152 am_titles=2 am_tickets=2 wifi=2 bat=4 charging=0 ac=1 "
						 "lin_free=0 model=o3dsxl new3ds=0 sysver=11.17.0-50U "
						 "luma=13.1.2 region=USA up_ms=7260000 new_n=0 lid=") &&
		   fake.mcuOpens == mcuOpens);
	statsSelfFits(reply);
	// The lid is read as the reply is made. It is `-` when PTM does not answer, never a guess.
	assert(strcmp(strrchr(reply, ' '), " lid=-") == 0);
	statsTransferFits();
	ledInit();
	fake.shell = 0;
	assert(strcmp(strrchr(request("stats %s self", ID), ' '), " lid=closed") == 0);
	fake.shell = 1;
	assert(strcmp(strrchr(request("stats %s self", ID), ' '), " lid=open") == 0);
	fake.shellResult = (Result)0xD8E0806A; // any refusal
	assert(strcmp(strrchr(request("stats %s self", ID), ' '), " lid=-") == 0);
	fake.shellResult = 0;
	// A count that AM refuses is `-`, with am_rc saying why, and so is a kernel that is not Luma's.
	// A key is never left out.
	fake.ticketCountResult = (Result)0xD8E0806A;
	fake.luma = 0;
	sysinfoInit();
	reply = request("stats %s self", ID);
	assert(strstr(reply, " am_rc=d8e0806a ") && strstr(reply, " am_titles=2 am_tickets=- ") &&
		   strstr(reply, " sysver=11.17.0-50U luma=- region=USA"));
	fake.ticketCountResult = 0;
	fake.importTicket = false;
}

// While the QR flow holds the console at a screen (main.c's serveBusy), `stats` and `screenshot`
// are answered. Every other verb is refused `busy` before its own check: no FS or AM call, no log
// line, no nonce spent, the receipt and the screen as they were, and nothing written to the card,
// however well formed the request.
static void busyServe(void) {
	static const char *const MUTATIONS[][2] = {
		{"put", "/busy.bin 4 " ZERO_DIGEST},
		{"put", "/busy.bin 4 nothex"}, // its own check would say grammar
		{"put", "new 8192 2048 " ZERO_DIGEST},
		{"delete", "/busy.bin"},
		{"delete", "title:000400000f800100"},
		{"mkdir", "/busy"},
		{"rename", "/busy.bin /busy2.bin"},
		{"run", "self"},
		{"seed", "title:000400000f800100 00112233445566778899aabbccddeeff"},
	};
	static const char *const READS[] = {"get %s /busy.bin", "hash %s /busy.bin", "ls %s /",
										"ls %s title:"};
	char receipt[REPLY_MAX], want[64];
	busyNow = BUSY_AT_SCREEN;
	snprintf(receipt, sizeof(receipt), "%s", request("stats %s op", ID));
	unsigned amCalls = fake.amCalls, logs = fake.logs;
	ServerStatus status = gApp.status;
	for (size_t i = 0; i < sizeof(MUTATIONS) / sizeof(MUTATIONS[0]); i++) {
		gated("busy", MUTATIONS[i][0], MUTATIONS[i][1]);
	}
	for (size_t i = 0; i < sizeof(READS) / sizeof(READS[0]); i++) {
		snprintf(want, sizeof(want), "%.*s rejected busy 00000000", (int)strcspn(READS[i], " "),
				 READS[i]);
		expectIs(want, request(READS[i], ID));
		assert(fake.fsCalls == 0);
	}
	// The framing is the main loop's: a word that is not a verb, and a verb's wrong arity.
	expectIs("error rejected unsupported 00000000", request("nosuch %s", ID));
	expectIs("error rejected grammar 00000000", request("put %s %s /busy.bin", ID, live()));
	assert(strcmp(request("stats %s op", ID), receipt) == 0 && gApp.status == status);
	assert(fake.amCalls == amCalls && fake.logs == logs && fake.fsCalls == 0);
	assert(!exists("sdmc:/busy.bin") && !exists("sdmc:/busy.bin" SD_PUT_TEMP));
	assert(!exists("sdmc:/busy"));

	// stats has every key of `self`, but nothing is asked of AM or the card. The version is the one
	// the boot cached, and the state is the one it names.
	const char *reply = request("stats %s self", ID);
	assert(strstr(reply, " ver=52 tid=0004000003d37200 state=qr am_pending=- am_cleanup=- "
						 "am_rc=00000000 free=- cluster=- clusters=- am_titles=- "
						 "am_tickets=- "));
	statsSelfFits(reply);
	expectPrefix("stats ok ok 00000000 bytes=", request("stats %s", ID));
	assert(fake.amCalls == amCalls && fake.fsCalls == 0);

	// screenshot returns the BMP as the main loop sends it.
	expectIs("screenshot committed ok 00000000 1974 raw",
			 request("screenshot %s %s top", ID, live()));
	assert(outLen == 1974 && memcmp(out, "BM", 2) == 0);
	busyNow = NOT_BUSY;
	expectPrefix("stats ok ok 00000000 bytes=", request("stats %s", ID));
	assert(strstr(request("stats %s self", ID), " state=ready ")); // the main loop is as it was
}

#if THREEDROP_TEST_VERBS
// A set spends a nonce: `bench <id> <nonce> <knob|capture> [value]`, with its own nonce each time,
// as the host's `_wireNonce` hands one out. A read keeps `bench <id> <sub>` and spends nothing.
static const char *benchSet(const char *fmt, ...) {
	char args[64];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(args, sizeof(args), fmt, ap);
	va_end(ap);
	return request("bench %s %s %s", ID, live(), args);
}

// The bench build's verb (testDispatchBench.c): a read that spends no nonce and a set that spends
// one, with every sub and its refusals. rx takes the body behind the request and says `short` for
// one cut early. tx is framed like get. mb streams the rows of the transfer `stats` describes,
// named by its request, and none of an earlier transfer's. file writes and deletes its file, and
// reports FS's Result for a failed write. A knob whose listener does not come up is refused and not
// kept. capture arms now and by its file.
static void benchVerb(void) {
	static const char RX[] = "fedcba9876543210fedcba9876543210";
	static unsigned char body[2u << 20];
	unsigned long row[4];
	char minted[NONCE_HEX + 1]; // copied now, before the reads, and compared after them
	snprintf(minted, sizeof(minted), "%s", nonceLive());
	expectIs("bench ok ok 00000000 state rcvbuf=- sndbuf=- sendsize=65536 nohash=0 nodisc=0 "
			 "capture=0 mb_rows=4096",
			 request("bench %s", ID));
	feed(body, sizeof(body));
	fake.stepMs = 1; // each clock read is 1 ms later, so each megabyte row has its own time
	expectPrefix("bench ok ok 00000000 rx bytes=2097152 ms=", request("bench %s rx 2", RX));
	fake.stepMs = 0;
	assert(inAt == sizeof(body) && strstr(fake.reply, " calls=") && strstr(fake.reply, " eagain="));
	assert(strstr(request("stats %s", ID), " req=fedcba9876543210fedcba9876543210"));
	expectIs("bench ok ok 00000000 mb req=fedcba9876543210fedcba9876543210 dir=rx rows=2 "
			 "cols=wall_ms,wait_ms,eagain,sd_kb",
			 request("bench %s mb", ID));
	// Two rows, the second later than the first; rx has no ring, so its sd_kb is 0.
	unsigned long later[4];
	assert(sscanf(fake.body, "%lu %lu %lu %lu\n%lu %lu %lu %lu\n", &row[0], &row[1], &row[2],
				  &row[3], &later[0], &later[1], &later[2], &later[3]) == 8 &&
		   row[0] > 0 && later[0] > row[0] && row[3] == 0 && later[3] == 0);
	char want[160];
	snprintf(want, sizeof(want), "trailer %s bench ok ok 00000000 2 %s", ID,
			 refDigestText(fake.body, fake.bodyLen));
	assert(strcmp(fake.last, want) == 0);
	feed(body, (1u << 20) - 12345);
	expectPrefix("bench rejected short d963f802 rx bytes=1036231 ", request("bench %s rx 1", ID));

	expectIs("bench ok ok 00000000 tx bytes=1048576", request("bench %s tx 1", ID));
	assert(outLen == 1u << 20);
	for (size_t i = 0; i < outLen; i++) {
		assert(out[i] == (unsigned char)(i * 31u + 7u));
	}
	expectTrailer("bench ok ok 00000000 tx bytes=1048576 ms=");
	expectPrefix("bench ok ok 00000000 mb req=0123456789abcdef0123456789abcdef dir=tx rows=1 ",
				 request("bench %s mb", ID));

	expectPrefix("bench ok ok 00000000 file bytes=2097152 chunk=1048576 writes=2 prealloc_ms=0 ",
				 request("bench %s file 2", ID));
	// The file is APP_PATH's and not in a directory of the bench's, since a release build never
	// gets one made.
	assert(strstr(fake.reply, " bytes_done=2097152") && !exists("sdmc:/3ds/3drop/filebench.tmp") &&
		   !exists("sdmc:/3ds/3drop/tmp"));
	expectPrefix("bench ok ok 00000000 file bytes=1048576 chunk=12288 writes=86 ",
				 request("bench %s file 1 1 12 1",
						 ID)); // AM's own write size, 1048576 / 12288 bytes rounded up
	expectPrefix(
		"bench ok ok 00000000 file bytes=8388608 chunk=4194304 writes=2 ",
		request("bench %s file 8 0 4096", ID)); // the ring's largest write, only when asked
	assert(strstr(fake.reply, " bytes_done=8388608 reads=0 read_first_ms=0 read_steady_ms=0 "
							  "read_steady_max_ms=0 read_done=0"));
	// read 1 reads the file back in its chunks, times each read, and reports FS's Result for a
	// failed one.
	fake.stepMs = 1;
	expectPrefix("bench ok ok 00000000 file bytes=3145728 chunk=1048576 writes=3 ",
				 request("bench %s file 3 1 1024 0 1", ID));
	fake.stepMs = 0;
	assert(strstr(fake.reply, " bytes_done=3145728 reads=3 read_first_ms=1 read_steady_ms=2 "
							  "read_steady_max_ms=1 read_done=3145728") &&
		   !exists("sdmc:/3ds/3drop/filebench.tmp"));
	readFailAt = 1u << 20;
	expectPrefix("bench rejected file-io c8804464 file bytes=2097152 chunk=1048576 writes=2 ",
				 request("bench %s file 2 0 1024 0 1", ID));
	readFailAt = UINT64_MAX;
	assert(strstr(fake.reply, " bytes_done=2097152 reads=2 ") &&
		   strstr(fake.reply, " read_done=1048576") && !exists("sdmc:/3ds/3drop/filebench.tmp"));
	fake.writeResult = (Result)0xC8804464;
	expectPrefix("bench rejected file-io c8804464 file bytes=1048576 chunk=1048576 writes=1 ",
				 request("bench %s file 1", ID));
	assert(strstr(fake.reply, " bytes_done=0") && !exists("sdmc:/3ds/3drop/filebench.tmp"));
	fake.writeResult = 0;

	// The fake heap is the console's measured size, and the host's malloc finds all of it.
	expectIs("bench ok ok 00000000 mem lin_heap=33554432 lin_free=0 heap=31621120 "
			 "heap_touched_max=31457280 tier1=25165824",
			 request("bench %s mem", ID));
	expectIs("bench ok ok 00000000 capture capture=0 frames=0 held=0 dropped=0 slots=0",
			 request("bench %s capture", ID));
	// Every read above, rx and tx and mb and file among them, left the nonce live as it was. One
	// that spent it and minted another would pass a copy taken after it.
	assert(strcmp(nonceLive(), minted) == 0);
	expectIs("bench ok ok 00000000 capture capture=1 frames=0 held=0 dropped=0 slots=4",
			 benchSet("capture on"));
	assert(exists("sdmc:/3ds/3drop/capture/arm"));
	expectPrefix("bench ok ok 00000000 state rcvbuf=- sndbuf=- sendsize=65536 nohash=0 nodisc=0 "
				 "capture=1 ",
				 request("bench %s", ID));
	expectPrefix("bench ok ok 00000000 capture capture=0 ", benchSet("capture off"));
	assert(!exists("sdmc:/3ds/3drop/capture/arm"));
	// disc returns the discovery text, which the test's fake leaves empty; its row is dispatched,
	// not merely present in the table.
	expectIs("bench ok ok 00000000", request("bench %s disc", ID));

	expectIs("bench ok ok 00000000 sendsize applied=65536 default=65536",
			 request("bench %s sendsize", ID));
	expectIs("bench rejected range 00000000 rcvbuf applied=-", benchSet("rcvbuf 70000"));
	expectIs("bench rejected refused 00000000 rcvbuf applied=- asked=32768",
			 benchSet("rcvbuf 32768"));
	assert(gKnobs.rcvBuf == 0);
	static const char *const REFUSED[][2] = {
		{"rx 0", " rx"},
		{"rx 257", " rx"},
		{"tx 129", " tx"},
		{"file 1 2", " file"},
		{"file 1 0 0", " file"},
		{"file 1 0 24577", " file"}, // past tier 1, which holds the pattern
		{"file 1 0 1024 0 2", " file"},
		{"disc x", ""},
		{"rx", ""},
		{"nosuch", ""},
		{"mb x", ""},
	};
	const char *unspent = live(); // a read spends nothing, and a refusal does not spend its nonce
	for (size_t i = 0; i < sizeof(REFUSED) / sizeof(REFUSED[0]); i++) {
		snprintf(want, sizeof(want), "bench rejected grammar 00000000%s", REFUSED[i][1]);
		assert(strcmp(request("bench %s %s", ID, REFUSED[i][0]), want) == 0);
	}
	assert(strcmp(nonceLive(), unspent) == 0); // no sub spent it

	// A set spends its nonce, so a replay of one is refused and changes nothing.
	const char *once = live();
	expectIs("bench ok ok 00000000 nodisc applied=1 default=0",
			 request("bench %s %s nodisc 1", ID, once));
	expectIs("bench rejected challenge 00000000", request("bench %s %s nodisc 1", ID, once));
	assert(gKnobs.noDisc == 1); // refused before the knob code ran, so it still holds
	// A set with no nonce, the old form, is refused and spends nothing, so the nonce the host holds
	// still works, and the same request with it sets the knob.
	const char *kept = live();
	expectIs("bench rejected challenge 00000000", request("bench %s nodisc 0", ID));
	expectIs("bench rejected challenge 00000000", request("bench %s capture maybe", ID));
	assert(strcmp(nonceLive(), kept) == 0); // a refusal spent nothing
	expectIs("bench ok ok 00000000 nodisc applied=0 default=0",
			 request("bench %s %s nodisc 0", ID, kept));
	// A malformed set, a capture that is neither on nor off, is refused before the spend, so its
	// nonce still sets it.
	const char *bad = live();
	expectIs("bench rejected grammar 00000000 capture",
			 request("bench %s %s capture maybe", ID, bad));
	expectPrefix("bench ok ok 00000000 capture capture=0 ",
				 request("bench %s %s capture off", ID, bad));
	// A set that names nothing, `bench <id> <nonce>`, is refused as grammar, and its nonce still
	// holds.
	const char *bare = live();
	expectIs("bench rejected grammar 00000000", request("bench %s %s", ID, bare));
	expectPrefix("bench ok ok 00000000 capture capture=0 ",
				 request("bench %s %s capture off", ID, bare));

	// With nohash 1, an SD file put skips the digest check, even for a wrong digest, and says so;
	// the ring hashed nothing. With nohash 0, the default, a wrong digest is refused as ever. Saves
	// always hash.
	unsigned char wrong[1000];
	memset(wrong, 0x5A, sizeof(wrong));
	expectIs("bench ok ok 00000000 nohash applied=1 default=0", benchSet("nohash 1"));
	expectIs("put committed ok 00000000 1000 digest=skipped",
			 putFile("/unhashed", body, sizeof(wrong), wrong, sizeof(wrong), false));
	assert(sameFile("sdmc:/unhashed", wrong, sizeof(wrong)) &&
		   strstr(request("stats %s", ID), " hash_n=0 "));
	// Without hashing, the write's cache flush is the body's only flush, so its failure fails the
	// put.
	fake.flushResult = (Result)0xE0E01BF5;
	expectIs("put rejected write e0e01bf5",
			 putFile("/unflushed", body, sizeof(wrong), wrong, sizeof(wrong), false));
	fake.flushResult = 0;
	assert(!exists("sdmc:/unflushed"));
	expectIs("bench ok ok 00000000 nohash applied=0 default=0", benchSet("nohash 0"));
	expectPrefix("put rejected digest ",
				 putFile("/unhashed", body, sizeof(wrong), wrong, sizeof(wrong), false));
	assert(sameFile("sdmc:/unhashed", wrong, sizeof(wrong)));
	expectIs("bench rejected range 00000000 nohash applied=0", benchSet("nohash 2"));
	// A flag knob with no code of its own to ask reads back as stored: 0 or 1.
	expectIs("bench ok ok 00000000 nodisc applied=1 default=0", benchSet("nodisc 1"));
	assert(gKnobs.noDisc == 1);
	expectIs("bench rejected range 00000000 nodisc applied=1", benchSet("nodisc 2"));
	expectIs("bench ok ok 00000000 nodisc applied=0 default=0", benchSet("nodisc 0"));
	assert(gKnobs.noDisc == 0);

	// A row's eagain is its own count, not the running total. Here a 3 MiB SD put goes through the
	// ring with one EAGAIN in its second megabyte (quiet: a stepped clock, not a stall), so row 2's
	// own count is 1 and row 3's is 0. (sd_kb and wait_ms are pinned in testBenchHw.c, because this
	// put's worker never advances ringDone, so every row's sd_kb here is 0 whatever the divisor,
	// and the fake's EAGAIN does not count as wait_ms.)
	unsigned long mb[12];
	static unsigned char big[3u << 20];
	fake.stepMs = 1;
	stepBackAt = (1u << 20) + 65536; // inside the second megabyte, past its row's boundary
	stepBackQuiet = true;
	expectPrefix("put committed ok ", putPlain("/ring3", big, sizeof(big)));
	stepBackQuiet = false;
	fake.stepMs = 0;
	expectPrefix("bench ok ok 00000000 mb req=0123456789abcdef0123456789abcdef dir=rx rows=3 ",
				 request("bench %s mb", ID));
	assert(sscanf(fake.body, "%lu %lu %lu %lu\n%lu %lu %lu %lu\n%lu %lu %lu %lu\n", &mb[0], &mb[1],
				  &mb[2], &mb[3], &mb[4], &mb[5], &mb[6], &mb[7], &mb[8], &mb[9], &mb[10],
				  &mb[11]) == 12);
	assert(mb[2] == 0 && mb[6] == 1 && mb[10] == 0); // row 2's own EAGAIN, once; row 3's none
	assert(mb[4] > mb[0] && mb[8] > mb[4]);			 // the wall is the running total

	// A transfer under a megabyte has no rows, and never the previous transfer's.
	expectCommitted(body, 1000, putPlain("/small", body, 1000));
	expectPrefix("bench ok ok 00000000 mb req=0123456789abcdef0123456789abcdef dir=rx rows=0 ",
				 request("bench %s mb", ID));
	assert(fake.bodyLen == 0);

	// fscalls reports the last spent change's file steps and its log line, with `-` for a step it
	// never took; a read starts none over. The clock stands still here, so each step took 0 us.
	expectCommitted(body, 1000, putPlain("/fresh", body, 1000));
	expectIs("bench ok ok 00000000 fscalls req=0123456789abcdef0123456789abcdef orphan_us=0 "
			 "open_us=0 setsize_us=0 flush_us=0 close_us=0 publish_us=0 aside_us=- swap_us=- "
			 "drop_us=- delete_us=- rename_us=- log_us=0",
			 request("bench %s fscalls", ID));
	expectCommitted(body, 1000, putPlain("/small", body, 1000)); // overwrites the earlier /small
	request("stats %s", ID);
	expectIs("bench ok ok 00000000 fscalls req=0123456789abcdef0123456789abcdef orphan_us=0 "
			 "open_us=0 setsize_us=0 flush_us=0 close_us=0 publish_us=0 aside_us=0 swap_us=0 "
			 "drop_us=0 delete_us=- rename_us=- log_us=0",
			 request("bench %s fscalls", ID));
	expectPrefix("rename committed ok ", request("rename %s %s /small /smaller", ID, live()));
	expectIs("bench ok ok 00000000 fscalls req=0123456789abcdef0123456789abcdef orphan_us=- "
			 "open_us=- setsize_us=- flush_us=- close_us=- publish_us=- aside_us=- swap_us=- "
			 "drop_us=0 delete_us=- rename_us=0 log_us=0",
			 request("bench %s fscalls", ID));
	expectPrefix("delete committed ok ", request("delete %s %s /smaller", ID, live()));
	expectPrefix("bench ok ok 00000000 fscalls req=0123456789abcdef0123456789abcdef orphan_us=- "
				 "open_us=- setsize_us=- flush_us=- close_us=- publish_us=- aside_us=- "
				 "swap_us=- drop_us=0 delete_us=0 rename_us=- log_us=0",
				 request("bench %s fscalls", ID));
	// A bench set is a spent change of its own. Its record names it and holds its own log write
	// alone, not the delete's with one more log write added.
	expectIs("bench ok ok 00000000 nodisc applied=0 default=0",
			 request("bench %s %s nodisc 0", RX, live()));
	expectIs("bench ok ok 00000000 fscalls req=fedcba9876543210fedcba9876543210 orphan_us=- "
			 "open_us=- setsize_us=- flush_us=- close_us=- publish_us=- aside_us=- swap_us=- "
			 "drop_us=- delete_us=- rename_us=- log_us=0",
			 request("bench %s fscalls", ID));
}

// Returns a step's time in us from an `fscalls` reply, or -1 for the `-` of a step the change never
// took.
static long fsUs(const char *reply, const char *step) {
	char key[24];
	snprintf(key, sizeof(key), " %s_us=", step);
	const char *at = strstr(reply, key);
	assert(at);
	at += strlen(key);
	return *at == '-' ? -1 : atol(at);
}

// fscalls on a clock that moves a millisecond per read, unlike benchVerb's, which stands still:
// each step a change took then reads at least one fake millisecond (268111 ticks), and a step it
// never took still reads `-`. The one exact figure is a set's log write, the only step of its
// record, one clock read after its start: 268111 ticks at the real 268111.856 a millisecond are
// 999.997 us, cut to 999.
static void benchFscallsMoving(void) {
	static const char RX[] = "fedcba9876543210fedcba9876543210";
	static const unsigned char body[1000];
	const char *taken[] = {"orphan", "open", "setsize", "flush", "close", "publish", "log"};
	const char *skipped[] = {"aside", "swap", "drop", "delete", "rename"};
	fake.stepMs = 1;
	expectCommitted(body, sizeof(body), putPlain("/moving", body, sizeof(body)));
	fake.stepMs = 0;
	const char *reply = request("bench %s fscalls", ID);
	expectPrefix("bench ok ok 00000000 fscalls req=0123456789abcdef0123456789abcdef ", reply);
	for (size_t i = 0; i < sizeof(taken) / sizeof(taken[0]); i++) {
		assert(fsUs(reply, taken[i]) >= 999);
	}
	for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
		assert(fsUs(reply, skipped[i]) == -1);
	}
	fake.stepMs = 1;
	expectIs("bench ok ok 00000000 nodisc applied=0 default=0",
			 request("bench %s %s nodisc 0", RX, live()));
	fake.stepMs = 0;
	expectIs("bench ok ok 00000000 fscalls req=fedcba9876543210fedcba9876543210 orphan_us=- "
			 "open_us=- setsize_us=- flush_us=- close_us=- publish_us=- aside_us=- swap_us=- "
			 "drop_us=- delete_us=- rename_us=- log_us=999",
			 request("bench %s fscalls", ID));
}
#endif

static void heldGet(u64 ms) { // its reply hands out the nonce live then, which the hold may expire
	snprintf(line, sizeof(line), "get %s /slow.bin range 0 100", ID);
	sendDelayMs = ms;
	serveSigned();
	assert(strstr(fake.reply, " get ok ok "));
}
// A request that held the console past a host's wait expires the nonce handed out before it, so a
// change that queued behind it, maybe from a tool that gave up, is refused unrun (`challenge`), and
// a host still waiting sends it again. A shorter hold, the nonce a long change's reply mints, and
// an unsigned peer's hold expire nothing.
static void queuedBehind(void) {
	static const unsigned char DATA[100] = "a file a long get takes its time over";
	char held[NONCE_HEX + 1];
	writeFile("sdmc:/slow.bin", DATA, sizeof(DATA));
	static const struct {
		u64 ms;
		const char *want;
	} HOLDS[] = {{HOST_REPLY_WAIT_MS + 1000, "mkdir rejected challenge "},
				 {HOST_REPLY_WAIT_MS - 1000, "mkdir committed ok "}};
	for (size_t i = 0; i < sizeof(HOLDS) / sizeof(HOLDS[0]); i++) {
		snprintf(held, sizeof(held), "%s", nonceLive());
		fake.nowMs++; // the next request comes after the reply that handed it out
		heldGet(HOLDS[i].ms);
		expectPrefix(HOLDS[i].want, request("mkdir %s %s /queued", ID, held));
	}
	snprintf(held, sizeof(held), "%s", nonceLive()); // handed out in the millisecond it starts
	heldGet(HOST_REPLY_WAIT_MS + 1000);
	expectPrefix("mkdir rejected challenge ", request("mkdir %s %s /queued/1", ID, held));
	readDelayMs = HOST_REPLY_WAIT_MS + 1000; // a long change, but its reply's fresh nonce stays
	expectPrefix("put committed ok ", putPlain("/slowput.bin", DATA, sizeof(DATA)));
	snprintf(held, sizeof(held), "%s", nonceLive());
	expectPrefix("mkdir committed ok ", request("mkdir %s %s /queued/2", ID, held));
	snprintf(held, sizeof(held), "%s", nonceLive());
	fake.nowMs++;
	lines = LINES_GONE; // a peer that sends nothing, held the pre-auth deadline
	readDelayMs = HOST_REPLY_WAIT_MS + 1000;
	assert(serve() && fake.replies == 0);
	lines = LINES_OK;
	expectPrefix("mkdir committed ok ", request("mkdir %s %s /queued/3", ID, held));
}

// HOME suspends a file put. On return, past the wire's stall bound, the range has timed out and the
// temp is cut back to the pieces it kept. POWER's close ends it there instead, as `exit` (net.c
// sends that reply nowhere). Either way the same range goes again from what was kept.
static void homeAndPower(void) {
	static unsigned char data[2u << 20];
	static const char PATH[] = "/home.bin", TEMP[] = "sdmc:/home.bin" SD_PUT_TEMP;
	const size_t mb = 1u << 20, size = sizeof(data);
	for (size_t i = 0; i < size; i++) {
		data[i] = (unsigned char)(i * 13 + 1);
	}
	expectPrefix("put committed ok ", putRange(PATH, data, size, 0, mb, data, mb, data));
	homeAt = 1;
	expectPrefix("put rejected timeout ", putRange(PATH, data, size, mb, mb, data + mb, mb, data));
	assert(homeAt == 0 && sameFile(TEMP, data, mb) && !exists("sdmc:/home.bin"));
	fake.powerPressed = true;
	expectPrefix("put rejected exit ", putRange(PATH, data, size, mb, mb, data + mb, mb, data));
	fake.powerPressed = false;
	assert(sameFile(TEMP, data, mb));
	expectPrefix("put committed ok ", putRange(PATH, data, size, mb, mb, data + mb, mb, data));
	assert(sameFile("sdmc:/home.bin", data, size) && !exists(TEMP));
}

// With the card gone or locked, every FS call fails. Each verb is refused with FS's Result and the
// card keeps what it held.
static void cardGone(void) {
	static const unsigned char KEEP[] = "kept";
	// Any FS failure will do, because the card's own failure is unmeasured.
	static const Result GONE = (Result)0xC8804471;
	char fields[200];
	mkdir("sdmc:/gone", 0755);
	writeFile("sdmc:/gone/keep.bin", KEEP, sizeof(KEEP));
	fake.sdGone = fake.sdmcResourceResult = GONE;
	expectIs("ls rejected notfound c8804471", request("ls %s /gone", ID));
	expectIs("get rejected notfound c8804471", request("get %s /gone/keep.bin range 0 5", ID));
	expectIs("hash rejected notfound c8804471", request("hash %s /gone/keep.bin", ID));
	expectIs("put rejected open c8804471", putPlain("/gone/new.bin", KEEP, sizeof(KEEP)));
	refused("notfound", "delete", "/gone/keep.bin");
	expectIs("mkdir rejected mkdir c8804471", request("mkdir %s %s /gone/dir", ID, live()));
	refused("notfound", "rename", "/gone/keep.bin /gone/moved.bin");
	snprintf(fields, sizeof(fields), "save:000400000f800100/x 4 %s", refDigestText(KEEP, 4));
	refused("notfound", "put", fields);
	fake.sdGone = fake.sdmcResourceResult = 0;
	assert(sameFile("sdmc:/gone/keep.bin", KEEP, sizeof(KEEP)) && !exists("sdmc:/gone/new.bin") &&
		   !exists("sdmc:/gone/new.bin" SD_PUT_TEMP) && !exists("sdmc:/gone/dir") &&
		   !exists("sdmc:/gone/moved.bin"));
}

// A power cut between a put's two publish renames leaves the path absent, the old file at its
// backup and the new one at its temp. A get finds nothing there, and the next put to the path puts
// the backup back first, so one that fails leaves the old file, and one that goes through
// publishes.
static void publishWindow(void) {
	// WRONG is NEW's size but not its bytes, so a put of it is refused at the digest.
	static const unsigned char OLD[] = "the old file", NEW[] = "the new file, now published",
							   WRONG[] = "the new file, not published";
	mkdir("sdmc:/window", 0755);
	writeFile("sdmc:/window/f.txt" SD_PUT_BACKUP, OLD, sizeof(OLD));
	writeFile("sdmc:/window/f.txt" SD_PUT_TEMP, NEW, sizeof(NEW));
	expectPrefix("get rejected notfound ", request("get %s /window/f.txt range 0 100", ID));
	expectPrefix("put rejected digest ",
				 putFile("/window/f.txt", NEW, sizeof(NEW), WRONG, sizeof(WRONG), false));
	assert(sameFile("sdmc:/window/f.txt", OLD, sizeof(OLD)) &&
		   !exists("sdmc:/window/f.txt" SD_PUT_BACKUP) &&
		   !exists("sdmc:/window/f.txt" SD_PUT_TEMP));
	expectPrefix("put committed ok ", putPlain("/window/f.txt", NEW, sizeof(NEW)));
	assert(sameFile("sdmc:/window/f.txt", NEW, sizeof(NEW)) &&
		   !exists("sdmc:/window/f.txt" SD_PUT_BACKUP) &&
		   !exists("sdmc:/window/f.txt" SD_PUT_TEMP));
}

int main(void) {
#if THREEDROP_TEST_VERBS
	assert(sizeof(VERBS) / sizeof(VERBS[0]) == 12); // the release set and `bench`
#else
	assert(sizeof(VERBS) / sizeof(VERBS[0]) == 11); // the release set; bench is not in it
#endif
	out = malloc(OUT_MAX);
	signal(SIGALRM, unanswered);
	gApp.installedTitleVersion = 52; // as main.c caches it at boot
	writeFile(KEY_PATH, "abcd-efgh-2345\n", 15);
	assert(R_SUCCEEDED(authInit()) && authLoadKey());
	const char *gzipCase = getenv("GZ_GET_CASE");
	if (gzipCase) {
		gzipPipeline((unsigned)strtoul(gzipCase, NULL, 10));
		TEST_PASS();
		return 0;
	}
	unitListMissing(); // runs first, while nothing has borrowed tier 1 yet
	for (unsigned i = 0; i < 8; i++) {
		gzipPipeline(i);
	}
	intake();
	replyNeverCut();
	spend();
	pathGate();
	files();
	unicodePaths();
	getVerb();
	putVerb();
	putSiblings();
	gzipPuts();
	systemWord();
	bootFirm();
	getRanges();
	putRanges();
	putUnsized();
	hashVerb();
	sdIoTimes();
	clockStepBack();
	ciaTicketTitle();
	ciaAlignment();
	putCardFailures();
	queuedBehind();
	homeAndPower();
	cardGone();
	publishWindow();
	gzipPutWaits();
	gzipPutDeadline();
	gzipNoMemory();
	gzipGetCancels();
	getReaderStalls();
	screenshots();
	installCard();
	logLines();
	logTimes();
	cutReadsUnlogged();
	logCap();
	totalMs();
	hashFailures();
	ringLadders();
	digestSpelling();
	listingUnits();
	archiveReads();
	archiveWrites();
	archiveTree();
	archiveFlush();
	ringSessions();
	titles();
	layeredFs();
	layeredFsFolder();
	titleWords();
	seeds();
	stranded();
	stats();
	busyServe();
#if THREEDROP_TEST_VERBS
	benchVerb();
	benchFscallsMoving();
#endif
	TEST_PASS();
	return 0;
}

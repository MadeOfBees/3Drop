// SPDX-License-Identifier: GPL-3.0-or-later
// Reply keys are only appended, never renamed, because tools/bench and kept results read them
// by name.
#include "bench.h"

#include "app.h"
#include "auth.h"	  // NONCE_HEX, nonceSpend: a set spends one
#include "cmdFs.h"	  // argIdle
#include "discover.h" // discoverFormat
#include "fs.h"		  // fsArchive
#include "knobs.h"
#include "net.h"
#include "ring.h"	// ringDone, ringTier1
#include "sdXfer.h" // sdPutDigests
#include "stats.h"
#include "transfer.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h> // MIN, MAX
#include <sys/socket.h>
#include <unistd.h>

Knobs gKnobs;

static bool argNumber(const VerbContext *c, size_t i, u64 lo, u64 hi, u64 def, u64 *out) {
	*out = def;
	return i >= c->n || (parseU64(c->field[i], out) && *out >= lo && *out <= hi);
}

static void refuseArgs(VerbContext *c, const char *sub) {
	reply(c, OUTCOME_REJECTED, "grammar", 0, " %s", sub);
}

#define VALUE_TEXT_MAX 12 // an int in decimal, sign and NUL

static const char *valueText(char out[VALUE_TEXT_MAX], int value) {
	if (value < 0) {
		return "-";
	}
	snprintf(out, VALUE_TEXT_MAX, "%d", value);
	return out;
}

// ---- Knobs: bench <knob> [value], where a knob is a KNOBS row ----------------------------------

// A knob lives as long as its experiment and goes with the verdict.
typedef struct {
	const char *name;
	int *stored;
	int def, hi;
	int option;				// a listener option, or 0
	int (*effective)(void); // for a knob without an option, returns the value the code acts on
} Knob;

static int sendSizeEffective(void) {
	return (int)effectiveSendSize();
}
static int noHashEffective(void) {
	return !sdPutDigests();
}

static const Knob KNOBS[] = {
	{"rcvbuf", &gKnobs.rcvBuf, LISTENER_WINDOW_BYTES, LISTENER_WINDOW_MAX, SO_RCVBUF, NULL},
	// The largest value swept. Larger values are untried.
	{"sndbuf", &gKnobs.sndBuf, LISTENER_SEND_BUFFER, 262144, SO_SNDBUF, NULL},
	{"sendsize", &gKnobs.sendSize, SEND_CHUNK_SIZE, SEND_CHUNK_SIZE, 0, sendSizeEffective},
	// For measuring a put's ceiling without its digest, which prices the digest.
	{"nohash", &gKnobs.noHash, 0, 1, 0, noHashEffective},
	// When set, discovery's sockets are closed from the next idle pass and the multicast group is
	// left, so an arm with it set prices holding them open. Nothing is answered while a request
	// runs, set or not.
	{"nodisc", &gKnobs.noDisc, 0, 1, 0, NULL},
};

static int optionRead(int sock, int option) {
	int value = -1;
	socklen_t size = sizeof(value);
	return sock >= 0 && getsockopt(sock, SOL_SOCKET, option, &value, &size) == 0 ? value : -1;
}

// Returns what the knob took, never what was asked, because SOC accepts options it does not apply.
// A knob with neither is a flag, and its code reads the stored value.
static int knobApplied(const Knob *k) {
	return k->option	  ? optionRead(gListener, k->option)
		   : k->effective ? k->effective()
						  : *k->stored;
}

static bool relisten(void) {
	if (gListener >= 0) {
		close(gListener);
	}
	gListener = openListener(DEFAULT_PORT);
	return gListener >= 0;
}

// If a value is refused or misread, the previous one is put back, so the stored value never lies.
static void benchKnob(VerbContext *c, const Knob *k) {
	char applied[VALUE_TEXT_MAX];
	u64 asked = 0;
	if (c->n == 4) {
		if (!argNumber(c, 3, 0, (u64)k->hi, 0, &asked)) {
			reply(c, OUTCOME_REJECTED, "range", 0, " %s applied=%s", k->name,
				  valueText(applied, knobApplied(k)));
			return;
		}
		int before = *k->stored;
		*k->stored = (int)asked;
		if ((k->option && !relisten()) || knobApplied(k) != knobOr((int)asked, k->def)) {
			*k->stored = before;
			if (k->option) {
				(void)relisten();
			}
			reply(c, OUTCOME_REJECTED, "refused", 0, " %s applied=%s asked=%llu", k->name,
				  valueText(applied, knobApplied(k)), (unsigned long long)asked);
			return;
		}
	}
	char client[VALUE_TEXT_MAX];
	// The client value comes from the request's own socket. It shows whether an accepted socket
	// takes the listener's buffer (net.h: it ignores SO_SNDBUF and reads back 8192), and it is
	// read off the wire, never inferred. On a set, that socket predates the new listener
	// (relisten() above), so read the knob again on a new connection to see one accepted under it.
	reply(c, OUTCOME_COMMITTED, "ok", 0, " %s applied=%s default=%d%s%s", k->name,
		  valueText(applied, knobApplied(k)), k->def, k->option ? " client=" : "",
		  k->option ? valueText(client, optionRead(c->client, k->option)) : "");
}

// ---- The pattern tx sends and file writes ------------------------------------------------------

// It is tx's period and file's default chunk, so the card floor is written in 1048576-byte chunks.
#define PATTERN_BYTES (1024 * 1024)

// The pattern is not zeros, because a zero fast path on the way would flatter the numbers. It is
// kept in tier 1, never in a static (a large one starves the drain ring), and is written on every
// use, since any transfer overwrites it.
static const unsigned char *argPattern(VerbContext *c, u32 size) {
	if (!argIdle(c)) {
		return NULL;
	}
	unsigned char *bytes = ringTier1();
	if (!bytes) {
		replyFail(c, "ring", OUT_OF_MEMORY);
		return NULL;
	}
	for (u32 i = 0; i < size; i++) {
		bytes[i] = (unsigned char)(i * 31u + 7u);
	}
	return bytes;
}

// ---- rx, tx: the receive and send ceilings -----------------------------------------------------

static bool discard(void *context, const unsigned char *buffer, size_t size) {
	(void)context;
	(void)buffer;
	(void)size;
	return true;
}

// idle_ms here, wait_ms in an `mb` row and recv_idle_us/send_idle_us in `stats` are one
// quantity, the time in EAGAIN waits. A wait on a full ring is ring_acquire_us in `stats` and in
// none of them.
static void pumpFields(char *out, size_t size, u64 bytes, u64 ms, PhaseKind busy, PhaseKind idle) {
	snprintf(out, size, " bytes=%llu ms=%llu busy_ms=%llu idle_ms=%llu calls=%lu eagain=%lu",
			 (unsigned long long)bytes, (unsigned long long)ms,
			 (unsigned long long)(gStats.phaseUs[busy] / 1000),
			 (unsigned long long)(gStats.phaseUs[idle] / 1000), (unsigned long)gStats.phaseN[busy],
			 (unsigned long)gStats.phaseN[idle]);
}

// It runs on the request connection, under the production listener's options. It reads the air of
// the moment, not the price of a receive knob (a null sink rewards what the real sink cannot
// drain), and it is not an upper bound on an install. Its window opens once the request is
// dispatched, and the body is already streaming behind it. Up to one receive window (rcvbuf, 49152
// by default) is in SOC's buffer by then and is read without its air time, so the rate runs high
// by size / (size - window), which is +0.3% at 16 units and +5% at 1. Compare receive-buffer knobs
// at 32 units or more. The 1-256 cap is the verb's grammar, not a limit of the console (the body
// is discarded).
static void benchRx(VerbContext *c) {
	char fields[160];
	u64 units;
	TransferSession session;
	if (!argNumber(c, 3, 1, 256, 0, &units)) {
		refuseArgs(c, "rx");
		return;
	}
	if (!argIdle(c)) { // its stats would replace those of the stranded transfer
		return;
	}
	statsStart(c);
	transferSessionInit(&session, c->client, units << 20, NULL, false);
	u64 start = uptimeMs();
	bool ok = transferReceive(&session, units << 20, discard, NULL);
	pumpFields(fields, sizeof(fields), session.acceptedSize, uptimeMs() - start, PHASE_RECV,
			   PHASE_RECV_IDLE);
	reply(c, ok ? OUTCOME_COMMITTED : OUTCOME_REJECTED,
		  ok ? "ok" : transferFailDetail(session.failure), session.failure, " rx%s", fields);
}

// tx replies in get's frame, so the host reads it with get's own stream reader. Its window closes
// when the last send() returns, with up to one send buffer (sndbuf, 65536 by default, 262144 at
// most) still queued. The rate runs high by size / (size - buffer), which is +0.2% at 32 units and
// +0.8% at sndbuf 262144, so below 32 units a sweep of sndbuf favors the larger by construction.
// The 1-128 cap is the verb's grammar, not a limit of the console (the pattern repeats every MiB).
static void benchTx(VerbContext *c) {
	char fields[160];
	u64 units;
	TransferSession session;
	if (!argNumber(c, 3, 1, 128, 0, &units)) {
		refuseArgs(c, "tx");
		return;
	}
	TransferBytes bytes = {argPattern(c, PATTERN_BYTES), PATTERN_BYTES, units << 20, 0};
	if (!bytes.base) {
		return;
	}
	statsStart(c);
	if (!REPLY_OK(c, " tx bytes=%llu", (unsigned long long)bytes.size)) {
		return; // no body behind a header that did not go out whole
	}
	transferSessionInit(&session, c->client, bytes.size, NULL, false);
	u64 start = uptimeMs();
	bool ok = transferSend(&session, transferBytes, &bytes);
	pumpFields(fields, sizeof(fields), session.acceptedSize, uptimeMs() - start, PHASE_SEND,
			   PHASE_SEND_IDLE);
	replyTrailer(c, ok ? OUTCOME_COMMITTED : OUTCOME_REJECTED,
				 ok ? "ok" : transferFailDetail(session.failure), session.failure, " tx%s", fields);
}

// ---- file: the card without AM -----------------------------------------------------------------

// main makes APP_PATH at boot, so the bench needs no directory of its own.
#define FILE_PATH APP_PATH "/filebench.tmp"

typedef struct {
	u32 n;
	u64 firstMs; // kept separately because a first write allocates the file's clusters
	u64 steadyMs, steadyMaxMs;
} CallTimes;

static void callTimed(CallTimes *t, u64 ms) {
	if (t->n++ == 0) {
		t->firstMs = ms;
	} else {
		t->steadyMs += ms;
		t->steadyMaxMs = MAX(t->steadyMaxMs, ms);
	}
}

typedef struct {
	u64 done, readDone;
	u64 preallocMs, flushMs;
	CallTimes write, read;
} FileBench;

// Times the card and FS alone, giving a floor under AM's writes and, with `read`, the rate a
// digest over the card's bytes would read them at.
static Result fileBench(const unsigned char *data, u64 size, bool prealloc, u32 chunk, bool flush,
						bool read, FileBench *out) {
	FS_Path path = fsMakePath(PATH_ASCII, FILE_PATH);
	Handle handle = 0;
	memset(out, 0, sizeof(*out));
	FSUSER_DeleteFile(fsArchive(), path); // fresh clusters, not reused ones
	Result r = FSUSER_OpenFile(&handle, fsArchive(), path,
							   FS_OPEN_WRITE | FS_OPEN_CREATE | (read ? FS_OPEN_READ : 0), 0);
	if (R_FAILED(r)) {
		return r;
	}
	if (prealloc) {
		u64 t0 = uptimeMs();
		r = FSFILE_SetSize(handle, size);
		out->preallocMs = uptimeMs() - t0;
	}
	while (R_SUCCEEDED(r) && out->done < size) {
		u32 n = (u32)MIN(size - out->done, (u64)chunk), written = 0;
		u64 t0 = uptimeMs();
		r = FSFILE_Write(handle, &written, out->done, data, n, flush ? FS_WRITE_FLUSH : 0);
		callTimed(&out->write, uptimeMs() - t0);
		r = shortIo(r, written, n);
		out->done += written;
	}
	if (R_SUCCEEDED(r)) {
		u64 t0 = uptimeMs();
		r = FSFILE_Flush(handle);
		out->flushMs = uptimeMs() - t0;
	}
	// Read it back in chunks into tier 1, over the pattern the writes used.
	if (R_SUCCEEDED(r) && read) {
		unsigned char *into = ringTier1();
		while (R_SUCCEEDED(r) && out->readDone < out->done) {
			u32 n = (u32)MIN(out->done - out->readDone, (u64)chunk), got = 0;
			u64 t0 = uptimeMs();
			r = FSFILE_Read(handle, &got, out->readDone, into, n);
			callTimed(&out->read, uptimeMs() - t0);
			r = shortIo(r, got, n);
			out->readDone += got;
		}
	}
	FSFILE_Close(handle);
	FSUSER_DeleteFile(fsArchive(), path);
	return r;
}

// 12288-byte chunks are AM's own write size; the ring's chunks are up to 4 units. Its times are
// whole milliseconds (uptimeMs), so their sums are right to about a millisecond, but a single call
// under one millisecond reads 0 or 1. That makes first_ms and steady_max_ms say little at AM's
// chunk (the card floor's 1 MiB is fine). The 1-1024 cap is the verb's grammar (blocks has a
// reason, because tier 1 holds the pattern).
static void benchFile(VerbContext *c) {
	u64 units, prealloc, blocks, flush, read;
	FileBench f;
	if (!argNumber(c, 3, 1, 1024, 0, &units) || !argNumber(c, 4, 0, 1, 0, &prealloc) ||
		!argNumber(c, 5, 1, RING_TIER1_BYTES >> 10, PATTERN_BYTES >> 10, &blocks) ||
		!argNumber(c, 6, 0, 1, 0, &flush) || !argNumber(c, 7, 0, 1, 0, &read)) {
		refuseArgs(c, "file");
		return;
	}
	u64 size = units << 20, chunk = blocks << 10;
	const unsigned char *data = argPattern(c, (u32)chunk);
	if (!data) {
		return;
	}
	Result r = fileBench(data, size, prealloc, (u32)chunk, flush, read, &f);
	reply(c, R_SUCCEEDED(r) ? OUTCOME_COMMITTED : OUTCOME_REJECTED,
		  R_SUCCEEDED(r) ? "ok" : "file-io", r,
		  " file bytes=%llu chunk=%lu writes=%lu prealloc_ms=%llu first_ms=%llu steady_ms=%llu "
		  "steady_max_ms=%llu flush_ms=%llu bytes_done=%llu reads=%lu read_first_ms=%llu "
		  "read_steady_ms=%llu read_steady_max_ms=%llu read_done=%llu",
		  (unsigned long long)size, (unsigned long)chunk, (unsigned long)f.write.n,
		  (unsigned long long)f.preallocMs, (unsigned long long)f.write.firstMs,
		  (unsigned long long)f.write.steadyMs, (unsigned long long)f.write.steadyMaxMs,
		  (unsigned long long)f.flushMs, (unsigned long long)f.done, (unsigned long)f.read.n,
		  (unsigned long long)f.read.firstMs, (unsigned long long)f.read.steadyMs,
		  (unsigned long long)f.read.steadyMaxMs, (unsigned long long)f.readDone);
}

// ---- mb: the per-megabyte curve ----------------------------------------------------------------

// 4096 rows of decimal megabytes cover 4.096 GB of wire bytes. Past that, rows go unrecorded,
// though `stats` mb= counts them (a 4.29 GB FAT32 file would lose its last 199).
#define MB_ROWS 4096
static struct {
	u32 wallMs; // since the first body byte
	u32 waitMs; // cumulative
	u32 eagain; // stored cumulative, printed per row (benchMb)
	// The ring's frontier in kB. For a put it is the number of bytes the worker has written. For
	// a get it is the pump's position, which is up to a staging buffer ahead of the wire and a
	// lower bound on what the read worker has read. For an install it is AM's import position.
	// It is 0 without a ring.
	u32 sdKb;
} mbRows[MB_ROWS];

void benchMegabyte(u64 nowMs) {
	u32 i = gStats.mbCount - 1; // statsMegabyte has counted this megabyte
	if (i < MB_ROWS) {
		mbRows[i].wallMs = (u32)(nowMs - gStats.firstByteMs);
		mbRows[i].waitMs =
			(u32)((gStats.phaseUs[PHASE_RECV_IDLE] + gStats.phaseUs[PHASE_SEND_IDLE]) / 1000);
		mbRows[i].eagain = gStats.phaseN[PHASE_RECV_IDLE] + gStats.phaseN[PHASE_SEND_IDLE];
		mbRows[i].sdKb = gApp.uiSunkProgress ? (u32)(ringDone() / 1000) : 0;
	}
}

// The rows belong to that request by construction, because statsReset zeroes mbCount.
// Keyed on wire megabytes, a ring-full stall is one long row, too coarse for AM's shape inside it.
// wall_ms, wait_ms and sd_kb print running totals; eagain prints the row's own count. wait_ms is
// the EAGAIN waits alone. The wait on a full ring (ring_acquire_us in `stats`) is not in it, so it
// reads near 0 on a put the ring limits while the wire sits idle.
static void benchMb(VerbContext *c) {
	if (!argIdle(c)) { // the listing's digest is taken in tier 1
		return;
	}
	u32 n = MIN(gStats.mbCount, (u32)MB_ROWS);
	listingBegin(c, " mb req=%s dir=%s rows=%lu cols=wall_ms,wait_ms,eagain,sd_kb",
				 gStats.request[0] ? gStats.request : "-", gStats.phaseN[PHASE_SEND] ? "tx" : "rx",
				 (unsigned long)n);
	for (u32 i = 0; i < n; i++) {
		char line[48];
		int len = snprintf(line, sizeof(line), "%lu %lu %lu %lu\n", (unsigned long)mbRows[i].wallMs,
						   (unsigned long)mbRows[i].waitMs,
						   (unsigned long)(mbRows[i].eagain - (i ? mbRows[i - 1].eagain : 0)),
						   (unsigned long)mbRows[i].sdKb);
		listingLine(line, (size_t)len);
	}
	listingEnd(OUTCOME_COMMITTED, "ok", 0);
}

// ---- mem ---------------------------------------------------------------------------------------

// Every page is written because an untouched malloc succeeds against memory that is not there.
// Tier 1 is taken first, as the first transfer would.
static void benchMem(VerbContext *c) {
	u32 tier1 = ringTier1() ? RING_TIER1_BYTES : 0, touched = 0;
	for (u32 units = envGetHeapSize() >> 20; units > 0 && touched == 0; units--) {
		volatile unsigned char *p = malloc((size_t)units << 20);
		if (p) {
			for (size_t at = 0; at < (size_t)units << 20; at += 4096) {
				p[at] = 1;
			}
			free((void *)p);
			touched = units << 20;
		}
	}
	REPLY_OK(c, " mem lin_heap=%lu lin_free=%lu heap=%lu heap_touched_max=%lu tier1=%lu",
			 (unsigned long)envGetLinearHeapSize(), (unsigned long)linearSpaceFree(),
			 (unsigned long)envGetHeapSize(), (unsigned long)touched, (unsigned long)tier1);
}

// ---- led ---------------------------------------------------------------------------------------

static struct {
	Led shown;
	u64 shownMs, gapMaxMs;
	unsigned writes, failed;
} ledTrace;

void benchLedWritten(Led led, Result result, u64 nowMs) {
	if (R_FAILED(result)) {
		ledTrace.failed++;
		return;
	}
	if (ledTrace.shown.rgb) {
		ledTrace.gapMaxMs = MAX(ledTrace.gapMaxMs, nowMs - ledTrace.shownMs);
	}
	ledTrace.shown = led;
	ledTrace.shownMs = nowMs;
	ledTrace.writes++;
}

// gap_max_ms is the longest a lit pattern went unwritten, from its write to the next write (to
// black too) or to now. A gap that starts at a black write is skipped. A lit pattern goes dark
// about 15.4 s after its write, so a longer gap_max_ms is a dark LED.
static void benchLed(VerbContext *c) {
	u8 rgb[3];
	char now[8] = "-";
	if (R_SUCCEEDED(ledRead(rgb))) {
		snprintf(now, sizeof(now), "%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
	}
	u64 gap = ledTrace.gapMaxMs;
	if (ledTrace.shown.rgb) {
		gap = MAX(gap, uptimeMs() - ledTrace.shownMs);
	}
	// The window starts over only once its reply has gone out. The host asks again for a reply
	// that did not go out, and the retry must not report the window since the lost read.
	if (REPLY_OK(c, " led rgb=%s shown=%06lx breathes=%d gap_max_ms=%llu writes=%u failed=%u", now,
				 (unsigned long)ledTrace.shown.rgb, ledTrace.shown.breathes,
				 (unsigned long long)gap, ledTrace.writes, ledTrace.failed)) {
		ledTrace.gapMaxMs = 0;
		ledTrace.writes = ledTrace.failed = 0;
	}
}

// ---- fscalls: the file steps of the last spent change ------------------------------------------

static struct {
	char request[33];
	u64 ticks[FS_STEPS];
	u32 n[FS_STEPS];
} fsSteps;

void benchFsBegin(const VerbContext *c) {
	if (gApp.requestSpent) {
		memset(&fsSteps, 0, sizeof(fsSteps));
		snprintf(fsSteps.request, sizeof(fsSteps.request), "%s", c->id);
	}
}

void benchFsStep(FsStep step, u64 *at) {
	u64 now = svcGetSystemTick();
	fsSteps.ticks[step] += now - *at;
	fsSteps.n[step]++;
	*at = now;
}

// Prints each step's time in microseconds, or `-` for a step the change never took. `drop` is the
// delete of a leftover backup, which three verbs make three ways (see FsStep).
static void benchFsCalls(VerbContext *c) {
	static const char *const NAMES[FS_STEPS] = {
		[FS_ORPHAN] = "orphan", [FS_OPEN] = "open",		[FS_SETSIZE] = "setsize",
		[FS_FLUSH] = "flush",	[FS_CLOSE] = "close",	[FS_PUBLISH] = "publish",
		[FS_ASIDE] = "aside",	[FS_SWAP] = "swap",		[FS_DROP] = "drop",
		[FS_DELETE] = "delete", [FS_RENAME] = "rename", [FS_LOG] = "log"};
	char text[FS_STEPS * 32] = "", us[24];
	for (int i = 0; i < FS_STEPS; i++) {
		if (fsSteps.n[i]) {
			snprintf(us, sizeof(us), "%llu",
					 (unsigned long long)((double)fsSteps.ticks[i] * 1000.0 / CPU_TICKS_PER_MSEC));
		} else {
			snprintf(us, sizeof(us), "-");
		}
		(void)appendf(text, sizeof(text), " %s_us=%s", NAMES[i], us);
	}
	REPLY_OK(c, " fscalls req=%s%s", fsSteps.request[0] ? fsSteps.request : "-", text);
}

// ---- capture, state, and the dispatch ----------------------------------------------------------

// It perturbs throughput, so it is part of a series' label.
static void benchCapture(VerbContext *c) {
	if (c->n == 4) {
		bool on = strcmp(c->field[3], "on") == 0;
		if (!on && strcmp(c->field[3], "off") != 0) {
			refuseArgs(c, "capture");
			return;
		}
		benchCaptureSet(on);
	}
	CaptureState s = benchCaptureState();
	REPLY_OK(c, " capture capture=%d frames=%u held=%u dropped=%u slots=%u", s.armed, s.frames,
			 s.held, s.dropped, s.slots);
}

// Reports every knob's applied value by name, so that a sweep's coverage and a series'
// binding can be checked.
static void benchState(VerbContext *c) {
	// Per knob: a space, a name of up to 11 characters, "=" and a value, 24 in all.
	char knobs[sizeof(KNOBS) / sizeof(KNOBS[0]) * 24] = "", applied[VALUE_TEXT_MAX];
	for (size_t i = 0; i < sizeof(KNOBS) / sizeof(KNOBS[0]); i++) {
		(void)appendf(knobs, sizeof(knobs), " %s=%s", KNOBS[i].name,
					  valueText(applied, knobApplied(&KNOBS[i])));
	}
	REPLY_OK(c, " state%s capture=%d mb_rows=%d", knobs, benchCaptureState().armed, MB_ROWS);
}

// Reports the counters for the name and for the probe, so a gate sees a lookup reach the console,
// not a host's cache.
static void benchDisc(VerbContext *c) {
	char text[256] = "";
	discoverFormat(text, sizeof(text));
	REPLY_OK(c, "%s", text);
}

typedef struct {
	const char *name;
	u8 minFields, maxFields;
	void (*run)(VerbContext *c);
} Sub;

static const Sub SUBS[] = {
	{"rx", 4, 4, benchRx},			 {"tx", 4, 4, benchTx},	  {"file", 4, 8, benchFile},
	{"mb", 3, 3, benchMb},			 {"mem", 3, 3, benchMem}, {"capture", 3, 4, benchCapture},
	{"led", 3, 3, benchLed},		 {"hw", 3, 3, benchHw},	  {"disc", 3, 3, benchDisc},
	{"fscalls", 3, 3, benchFsCalls},
};

// ---- A set spends a nonce ----------------------------------------------------------------------

static const Knob *knobNamed(const char *name) {
	for (size_t i = 0; i < sizeof(KNOBS) / sizeof(KNOBS[0]); i++) {
		if (strcmp(name, KNOBS[i].name) == 0) {
			return &KNOBS[i];
		}
	}
	return NULL;
}

// Checks a set's own grammar before its nonce is spent, so a malformed one keeps its nonce. The
// grammar is a knob and a value, or `capture on|off`. A value past a knob's range is for the knob
// to refuse, after the spend.
static bool setGrammar(const VerbContext *c) {
	if (c->n != 5) {
		return false;
	}
	if (strcmp(c->field[3], "capture") == 0) {
		return strcmp(c->field[4], "on") == 0 || strcmp(c->field[4], "off") == 0;
	}
	return knobNamed(c->field[3]) != NULL;
}

// bench <id> <nonce> <knob|capture> [value]: a set spends the nonce as every change does, so a
// captured request cannot set it again, even after a relaunch, until the key changes.
// Returns false with its refusal sent, which is either grammar (nothing is spent) or challenge.
// Returns true with the fields shifted, so the knob and capture code below runs on a set as before.
static bool benchSet(VerbContext *c) {
	const Knob *knob = c->n > 3 ? knobNamed(c->field[3]) : NULL;
	bool capture = c->n > 3 && strcmp(c->field[3], "capture") == 0;
	if (!setGrammar(c)) {
		if (knob || capture) {
			refuseArgs(c, c->field[3]); // its own name is echoed, as a read's is
		} else {
			replyFail(c, "grammar", 0); // the sub is not echoed
		}
		return false;
	}
	if (!nonceSpend(c->field[2])) { // spent, expired or never minted
		replyFail(c, "challenge", 0);
		return false;
	}
	gApp.requestSpent = true; // the log line and the `stats op` receipt follow, as for a mutation
	// Dispatch began the record before this spend, so the set's log write would join the last
	// change's. Beginning the record again makes `fscalls` show the set's own.
	benchFsBegin(c);
	c->field[2] = c->field[3];
	c->field[3] = c->field[4];
	c->n = 4;
	return true;
}

void benchRun(VerbContext *c) {
	if (c->n == 2) {
		benchState(c);
		return;
	}
	// No sub's name is NONCE_HEX long, so a third field that long is a set's nonce.
	if (strlen(c->field[2]) == NONCE_HEX) {
		if (!benchSet(c)) {
			return;
		}
	} else if (c->n == 4 && (knobNamed(c->field[2]) || strcmp(c->field[2], "capture") == 0)) {
		// A knob or `capture` with a value and no nonce, the old form, would be replayable. Only a
		// set that names a nonce changes anything, so this one is refused as a spent one is.
		replyFail(c, "challenge", 0);
		return;
	}
	for (size_t i = 0; i < sizeof(KNOBS) / sizeof(KNOBS[0]); i++) {
		if (strcmp(c->field[2], KNOBS[i].name) == 0 && c->n <= 4) {
			benchKnob(c, &KNOBS[i]);
			return;
		}
	}
	for (size_t i = 0; i < sizeof(SUBS) / sizeof(SUBS[0]); i++) {
		const Sub *s = &SUBS[i];
		if (strcmp(c->field[2], s->name) == 0 && c->n >= s->minFields && c->n <= s->maxFields) {
			s->run(c);
			return;
		}
	}
	replyFail(c, "grammar", 0); // the sub is not echoed
}

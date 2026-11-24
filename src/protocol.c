// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.h"

#include "auth.h"
#include "fs.h" // appendLog
#include "hash.h"
#include "net.h"
#include "path.h" // SD_PATH_MAX
#include "ring.h" // ringTier1
#include "stats.h"

#include <mbedtls/constant_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h> // MIN
#include <unistd.h>

// ---- Replies --------------------------------------------------------------------------------

static char receipt[REPLY_MAX] = "op none";

const char *protocolLastOp(void) {
	return receipt;
}

static bool sendSigned(int client, const char *line, const uint8_t mac[MAC_SIZE]) {
	char text[REPLY_MAX + MAC_HEX + 2];
	char hex[MAC_HEX + 1];
	hexEncode(hex, mac, MAC_SIZE);
	snprintf(text, sizeof(text), "%s\n%s\n", line, hex);
	return sendStr(client, text);
}

// This runs for every line a request sends. A transfer's last line ends its total_ms, and a spent
// request's last line is its log line.
static void epilogue(VerbContext *c, const char *detail, Result result) {
	if (c->timed) {
		statsFinish();
	}
	c->detail = detail;
	c->result = result;
}

static const char *outcomeWord(const Verb *verb, Outcome outcome) {
	static const char *const WORDS[] = {"rejected", "committed", "unknown"};
	return outcome == OUTCOME_COMMITTED && !verb->mutation ? "ok" : WORDS[outcome];
}

static bool replyv(VerbContext *c, Outcome outcome, const char *detail, Result result,
				   const char *fields, va_list args) {
	epilogue(c, detail, result);
	const char *nonce = nonceLive();
	c->header[0] = '\0';
	appendf(c->header, sizeof(c->header), "response %s %s proto=%d ", c->id, nonce ? nonce : "-",
			PROTOCOL_EPOCH);
	size_t prefix = strlen(c->header);
	bool fits = appendf(c->header, sizeof(c->header), "%s %s %s %08lx", c->verb->name,
						outcomeWord(c->verb, outcome), detail, (unsigned long)(u32)result) &&
				vappendf(c->header, sizeof(c->header), fields, args);
	if (!fits) {
		// A truncated reply would still carry a valid MAC, so answer that it did not fit instead.
		appendLog("response-too-long", 0, 0);
		c->header[prefix] = '\0';
		appendf(c->header, sizeof(c->header), "error rejected response-too-long 00000000");
	}
	if (gApp.requestSpent) { // a refusal changed nothing, so it leaves the receipt
		snprintf(receipt, sizeof(receipt), "op %s %s", c->id, c->header + prefix);
	}
	uint8_t mac[MAC_SIZE];
	macResponse(mac, c->request, c->header);
	return sendSigned(c->client, c->header, mac) && fits;
}

bool reply(VerbContext *c, Outcome outcome, const char *detail, Result result, const char *fields,
		   ...) {
	va_list args;
	va_start(args, fields);
	bool sent = replyv(c, outcome, detail, result, fields, args);
	va_end(args);
	return sent;
}

bool replyFail(VerbContext *c, const char *detail, Result result) {
	reply(c, OUTCOME_REJECTED, detail, result, "");
	return false;
}

bool replyRefusal(VerbContext *c, const char *refusal, Result why) {
	return !refusal || replyFail(c, refusal, why);
}

void replyTrailer(VerbContext *c, Outcome outcome, const char *detail, Result result,
				  const char *fields, ...) {
	char trailer[TRAILER_MAX] = "";
	va_list args;
	epilogue(c, detail, result);
	va_start(args, fields);
	bool fits = appendf(trailer, sizeof(trailer), "trailer %s %s %s %s %08lx", c->id, c->verb->name,
						outcomeWord(c->verb, outcome), detail, (unsigned long)(u32)result) &&
				vappendf(trailer, sizeof(trailer), fields, args);
	va_end(args);
	if (!fits) { // with no trailer the host refuses the body, so it fails closed
		appendLog("trailer-too-long", 0, 0);
		return;
	}
	uint8_t mac[MAC_SIZE];
	macTrailer(mac, c->request, c->header, trailer);
	(void)sendSigned(c->client, trailer, mac);
}

// A listing is sent from buf, never from linear memory, because a send from linear memory was
// truncated on hardware. The digest's unit is in tier 1.
static struct {
	VerbContext *c;
	char buf[2048]; // one unexplained reset occurred with a larger listing; never re-tested
	size_t len;
	u8 *unit;
	u32 unitLen, count;
	Result hashed;
	bool sent; // false once refused, cut or ended, so no line reaches tier 1 or the socket
} listing;

bool listingBegin(VerbContext *c, const char *fields, ...) {
	va_list args;
	listing.c = c;
	listing.len = listing.unitLen = listing.count = 0;
	listing.hashed = 0;
	listing.sent = false;
	listing.unit = ringTier1();
	if (!listing.unit) {
		return replyFail(c, "ring", OUT_OF_MEMORY);
	}
	digestStart();
	va_start(args, fields);
	listing.sent = replyv(c, OUTCOME_COMMITTED, "ok", 0, fields, args);
	va_end(args);
	return listing.sent;
}

static void listingFlush(void) {
	listing.sent =
		listing.sent && sendBytes(listing.c->client, (const u8 *)listing.buf, listing.len);
	listing.len = 0;
}

// A line may straddle two units.
static void listingHash(const char *text, size_t n) {
	while (n > 0 && R_SUCCEEDED(listing.hashed)) {
		size_t take = MIN(DIGEST_UNIT - listing.unitLen, n);
		memcpy(listing.unit + listing.unitLen, text, take);
		listing.unitLen += take;
		text += take;
		n -= take;
		if (listing.unitLen == DIGEST_UNIT) {
			listing.hashed = digestUnits(listing.unit, DIGEST_UNIT);
			listing.unitLen = 0;
		}
	}
}

bool listingLine(const char *line, size_t n) {
	if (!listing.sent) {
		return false;
	}
	if (listing.len + n > sizeof(listing.buf)) {
		listingFlush();
	}
	memcpy(listing.buf + listing.len, line, n);
	listing.len += n;
	listing.count++;
	listingHash(line, n);
	return listing.sent && R_SUCCEEDED(listing.hashed);
}

// A digest that failed is `-`, so the host cannot check what it got. A walk that failed first keeps
// its own word.
void listingEnd(Outcome outcome, const char *detail, Result result) {
	u8 digest[32];
	char text[DIGEST_TEXT_SIZE] = "-";
	listingFlush();
	if (!listing.sent) {
		return;
	}
	digestUnits(listing.unit, listing.unitLen);
	Result r = digestFinish(digest);
	if (R_SUCCEEDED(r)) {
		digestText(text, digest);
	} else if (outcome != OUTCOME_REJECTED) {
		outcome = OUTCOME_REJECTED;
		detail = "hash";
		result = r;
	}
	replyTrailer(listing.c, outcome, detail, result, " %lu %s", (unsigned long)listing.count, text);
	listing.sent = false;
}

// A body that did not commit has no digest: `-`.
void bodyTrailer(VerbContext *c, const Operation *op, const u8 digest[32], u64 wireBytes) {
	char text[DIGEST_TEXT_SIZE] = "-";
	if (op->outcome == OUTCOME_COMMITTED) {
		digestText(text, digest);
	}
	replyTrailer(c, op->outcome, op->detail, op->result, " %s %llu", text,
				 (unsigned long long)wireBytes);
}

// ---- Dispatch -------------------------------------------------------------------------------

// Only canonical lines are accepted, so the MAC over the line stays one-to-one with the fields a
// verb acts on. The fields before a fault still count, so a request whose id survived gets a signed
// `grammar` reply.
static size_t splitFields(char *line, char *field[MAX_FIELDS], bool *canonical) {
	size_t n = 0;
	*canonical = true;
	for (char *token; (token = strsep(&line, " ")) != NULL;) {
		bool ok = *token != '\0' && n < MAX_FIELDS;
		for (const char *p = token; ok && *p; p++) {
			ok = (unsigned char)*p > 0x20 && (unsigned char)*p < 0x7F;
		}
		if (!ok) {
			*canonical = false;
			break;
		}
		field[n++] = token;
	}
	return n;
}

static void dispatch(VerbContext *c, char *line, const Verb *verbs, size_t count) {
	char *field[MAX_FIELDS];
	bool canonical;
	u8 id[REQUEST_ID_SIZE];
	c->field = field;
	c->n = splitFields(line, field, &canonical);
	// Without a request id there is no signed reply to build.
	if (c->n < 2 || !hexDecode(id, field[1], sizeof(id))) {
		sendStr(c->client, "ERR malformed\n");
		return;
	}
	c->id = field[1];
	const Verb *v = verbs;
	while (canonical && v < verbs + count && strcmp(v->name, field[0]) != 0) {
		v++;
	}
	if (!canonical || v == verbs + count || c->n < v->minFields || c->n > v->maxFields) {
		static const Verb ERROR_VERB = {.name = "error"};
		c->verb = &ERROR_VERB;
		replyFail(c, canonical && c->n == 2 ? "unsupported" : "grammar", 0);
		return;
	}
	c->verb = v;
	if (c->busy > v->whileBusy) {
		// Refused before the check and the spend, so the sender keeps its nonce.
		replyFail(c, "busy", 0);
		return;
	}
	if (v->check && !v->check(c)) {
		return; // refused before the spend, so the sender keeps its nonce
	}
	if (v->mutation && !nonceSpend(field[2])) {
		replyFail(c, "challenge", 0);
		return;
	}
	gApp.requestSpent = v->mutation;
	BENCH(benchFsBegin(c));
	v->run(c);
	if (gApp.requestSpent) { // after the last line, so no total_ms holds the card write
		char event[32], tail[STATS_TAIL_MAX];
		snprintf(event, sizeof(event), "%s-%s", v->name, c->detail);
		statsLogTail(tail, sizeof(tail), c->timed);
		BENCH(u64 fsAt = svcGetSystemTick());
		appendLogTail(event, c->result, c->timed ? gApp.wireBytes : 0, tail);
		BENCH(benchFsStep(FS_LOG, &fsAt));
	}
}

// The widest request names two SD paths, `rename <id> <nonce> hex:<from> hex:<to>`, each of
// SD_PATH_MAX - 1 bytes at two hex digits a byte.
#define HEX_PATH_MAX (sizeof("hex:") - 1 + 2 * (SD_PATH_MAX - 1))
_Static_assert(REQUEST_LINE_MAX >= sizeof("rename ") + 2 * REQUEST_ID_SIZE + 1 + NONCE_HEX + 1 +
									   2 * HEX_PATH_MAX + 1,
			   "a rename of two of the longest SD paths must fit the request line (and its NUL)");
// A request must fit in one TCP segment (MSS ~1440), because a request's tail in a second segment
// may wait out a delayed ACK.
_Static_assert(REQUEST_LINE_MAX + MAC_HEX + 1 <= 1400, "a request and its MAC fit one TCP segment");

bool protocolServe(int client, const Verb *verbs, size_t count, After *after, Busy busy) {
	// Static because one request is served at a time and the main stack is small.
	static char request[REQUEST_LINE_MAX + MAC_HEX + 1], line[REQUEST_LINE_MAX];
	char *macHex = NULL;
	uint8_t mac[MAC_SIZE], expected[MAC_SIZE];
	bool authentic = true, served = false;
	u64 start = uptimeMs();
	gApp.requestSpent = false; // until dispatch spends this request's nonce
	const bool busyRequest = busy != NOT_BUSY;
	// A wait serves a busy request, so the keys its scans see are that wait's (net.h).
	keepKeys(busyRequest);
	if (!setNonblocking(client)) {
		close(client);
		return true;
	}
	const u64 allowanceMs = busyRequest ? BUSY_PREAUTH_INPUT_TIMEOUT_MS : PREAUTH_INPUT_TIMEOUT_MS;
	LinesRead got = readRequestLines(client, request, sizeof(request), &macHex, allowanceMs);
	if (got == LINES_OK && strlen(request) >= REQUEST_LINE_MAX) {
		got = LINES_TOO_LONG;
	}
	if (got == LINES_TOO_LONG || (got == LINES_OK && !hexDecode(mac, macHex, sizeof(mac)))) {
		// This is a framing fault, not a wrong key. Say so, unsigned, so the host does not blame
		// the key.
		sendStr(client, "ERR malformed\n");
	} else if (got == LINES_OK && !(macRequest(expected, request) &&
									mbedtls_ct_memcmp(expected, mac, MAC_SIZE) == 0)) {
		// There is no artificial delay, because it would not slow offline guessing of a captured
		// MAC and would let an unauthenticated peer hold the one-client listener longer. PBKDF2 is
		// the control.
		sendStr(client, "ERR auth\n");
		authentic = false;
	} else if (got == LINES_OK) {
		VerbContext c = {.client = client, .request = request, .after = after, .busy = busy};
		memcpy(line, request, sizeof(line));
		dispatch(&c, line, verbs, count);
		served = true;
	}
	// A busy request that was left unanswered has no reply tail to keep, and the wait it holds
	// cannot spare the time to drain.
	if (busyRequest && got == LINES_GONE) {
		close(client);
	} else {
		gracefulClose(client);
	}
	// A host waits that long for a reply, so a change queued behind this one may belong to a tool
	// that gave up.
	if (served && uptimeMs() - start > HOST_REPLY_WAIT_MS) {
		nonceExpireBefore(start);
	}
	return authentic;
}

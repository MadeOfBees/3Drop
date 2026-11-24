// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTOCOL_H
#define PROTOCOL_H

// The wire grammar's host twin is tools/wire.sh: change one, change both.

// Every signed reply names its protocol epoch after its nonce (`response <id> <nonce> proto=<n>`),
// so a host learns at every exchange whether it may go on. Raise it only for a change an older host
// would misread: a request or reply field that means something else, moves or goes away. A new
// reply key or a new optional verb leaves it unchanged. The update bootstrap is frozen in every
// epoch: the key's derivation, the MAC domains, that header and its nonce, `stats <id> op` and
// its `ver=`, the receipt, and `put <id> <nonce> self <size> <prefix> <digest>` (testWire.sh
// pins them).
#define PROTOCOL_EPOCH 1

#include "codec.h"
#include "result.h"

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#define REQUEST_LINE_MAX 1152 // request line plus NUL, sized for two SD paths in hex (protocol.c)
#define MAX_FIELDS 12		  // the widest request, a batch CIA put, has 10
#define REQUEST_ID_SIZE 16	  // random bytes, sent as 32 lowercase hex characters
#define REPLY_MAX 1544		  // transfer `stats` is the widest reply (testDispatch.c prices it)
#define TRAILER_MAX 256		  // a trailer and its MAC fit inside tools/wire.sh WIRE_TRAILER_WINDOW

// The jump itself is already set in libctru's chainloader, so main.c only has to leave.
typedef enum {
	AFTER_NOTHING,
	AFTER_RELAUNCH,
	AFTER_SELF_UPDATED,
	AFTER_LAUNCH,
} After;
// The deliver argument for the relaunch. Without it, a `run self` restart looks like a
// self-update restart.
#define RUN_SELF_MARKER "3drop:run self"

// What holds the console while a request is served. The main loop serves while nothing holds the
// console. The QR flow serves from its screens (main.c's serveBusy): the scanner or one of its
// questions, where nothing is lent. A download serves nothing. A verb names the busiest state it
// is still answered in (Verb.whileBusy), and a request in a busier state is refused `busy`,
// before its check and its nonce.
typedef enum {
	NOT_BUSY,
	BUSY_AT_SCREEN,
} Busy;

typedef struct Verb Verb;

typedef struct {
	int client;
	const char *request; // verbatim, because the MAC binds it
	const char *id;
	char **field;
	size_t n;
	const Verb *verb;
	Busy busy;
	// statsStart ran, so `stats` describes the request, and its last line ends it.
	bool timed;
	// The last line's word (a literal) and Result, for a spent request's log line.
	const char *detail;
	Result result;
	char header[REPLY_MAX]; // the last reply line sent, which a trailer's MAC binds
	After *after;
} VerbContext;

// check refuses with no side effects, so a refused mutation keeps its nonce. No handler spends a
// nonce except `bench` in a bench build, where a set spends its own after its grammar is checked
// (src/bench/bench.c).
struct Verb {
	const char *name;
	u8 minFields, maxFields; // verb and id included
	bool mutation;
	bool (*check)(VerbContext *c);
	void (*run)(VerbContext *c);
	Busy whileBusy; // NOT_BUSY means the verb is refused whenever the console is busy
};

// Returns false when the MAC did not verify.
bool protocolServe(int client, const Verb *verbs, size_t count, After *after, Busy busy);

// A field whose value the console could not tell is `-`, never left out. A title mutation names
// its title on every outcome after the spend, and a file put names the bytes it committed; nothing
// else is echoed. The receipt is kept before the first byte goes out, so a lost reply can still be
// read back.
bool reply(VerbContext *c, Outcome outcome, const char *detail, Result result, const char *fields,
		   ...) __attribute__((format(printf, 5, 6)));
#define REPLY_OK(c, ...) reply((c), OUTCOME_COMMITTED, "ok", 0, __VA_ARGS__)
// Always false, so a check refuses with `return replyFail(...)`.
bool replyFail(VerbContext *c, const char *detail, Result result);
// A check's last step. A refusal word is replied with `why` and the call returns false; NULL
// sends nothing and returns true.
bool replyRefusal(VerbContext *c, const char *refusal, Result why);
void replyTrailer(VerbContext *c, Outcome outcome, const char *detail, Result result,
				  const char *fields, ...) __attribute__((format(printf, 5, 6)));

// A body the socket did not take whole gets no trailer, so the host never takes it for complete.
// Its digest is taken in tier 1, so the verb refuses `stranded` first.
bool listingBegin(VerbContext *c, const char *fields, ...) __attribute__((format(printf, 2, 3)));
bool listingLine(const char *line, size_t n);
void listingEnd(Outcome outcome, const char *detail, Result result);

// The wire length rides in the trailer because a gzip body's wire length is known only once it
// is deflated.
void bodyTrailer(VerbContext *c, const Operation *op, const u8 digest[32], u64 wireBytes);

const char *protocolLastOp(void);

#endif // PROTOCOL_H

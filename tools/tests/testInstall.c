// cc: -DTHREEDROP_TEST_VERBS=1 src/bench/bench.c src/bench/capture.c src/bench/ledRead.c
// cc: src/titles.c src/fs.c src/hash.c src/stats.c src/protocol.c src/codec.c src/cmdFs.c
// cc: src/path.c src/sdXfer.c src/screenshot.c src/sysinfo.c src/hbldr.c src/led.c -lz
// cc: src/bench/hw.c src/layeredFs.c
// The install route through the real put verb, with its writer on a real thread. The test covers
// every route's and failure's reply, AM's byte stream, the `new` ticket rule and a stranded writer,
// then uninstall.
#include "fakes.h"

#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "auth.h"
#define APP_VERSION "test" // stands in for tools/build.sh's definition, which no test runs
#include "cmd.c"
#include "install.c"
#include "ring.c"

// threadJoin returns the svc timeout (0x09401BFE, which passes R_SUCCEEDED) while the writer runs.
// The writer's next write waits out heldPolls of its joins.
static pthread_t writer;
static volatile bool writerDone;
static volatile unsigned heldPolls;
static Result joinError;
static bool noThread;	 // the writer's thread cannot be made
static unsigned threads; // made and not yet freed; a joined writer is freed before the next is made
static void *runWriter(void *arg) {
	worker(arg);
	writerDone = true;
	return NULL;
}
Thread threadCreate(ThreadFunc entry, void *arg, size_t stack, int prio, int core, bool detach) {
	assert(threads == 0);
	writerDone = false;
	if (noThread || pthread_create(&writer, NULL, runWriter, arg) != 0) {
		return NULL;
	}
	threads++;
	return (Thread)&writer;
}
Result threadJoin(Thread thread, u64 timeoutNs) {
	usleep(200);
	if (heldPolls != 0) { // read first, so the worker's one store, from 0, is never lost
		heldPolls--;
	}
	return !writerDone ? (Result)0x09401BFE : joinError ? joinError : pthread_join(writer, NULL);
}
void threadFree(Thread thread) {
	assert(threads == 1);
	threads--;
}
void *linearMemAlign(size_t size, size_t alignment) { // adds 1048576 poisoned bytes past the end
	return memset(memalign(alignment, size + RING_UNIT), 0xEE, size + RING_UNIT);
}

static u8 *wire;
static volatile u64 wireAt, amAt; // AM has the first amAt bytes of the CIA
static u64 recvFailAt = UINT64_MAX;
// With holdAtEdges the units across tier 1's end and the wrap are ready when the writer looks, so
// only the ring's own rule keeps a write from straddling an edge.
static bool holdAtEdges;
static bool atEdge(u64 at) {
	return holdAtEdges && (at == 23 * RING_UNIT || at == 39 * RING_UNIT);
}
// With holdInUnit the pump stops inside the first unit, past content start + 16, for up to a second
// or until AM holds that much. amInUnit is what AM held when the pump went on.
static volatile bool holdInUnit;
static volatile u64 amInUnit;
bool transferReceive(TransferSession *s, u64 n, TransferSink sink, void *context) {
	unsigned char *p;
	for (size_t k, end = wireAt + n; wireAt < end; wireAt += k) {
		if (wireAt >= recvFailAt || !ringAcquire(&p, &k)) {
			s->failure = TRANSFER_CANCELED;
			return false;
		}
		while (atEdge(wireAt) && amAt < wireAt) {
			usleep(100);
		}
		// The early write comes at once; the 10 s maximum is there for a loaded host, not a slow
		// worker.
		if (holdInUnit && wireAt > 2064 && wireAt < RING_UNIT) {
			for (int i = 0; i < 100000 && amAt < 2064; i++) {
				usleep(100);
			}
			amInUnit = amAt;
			holdInUnit = false;
		}
		k = k < 65536 ? k : 65536; // 0 while the ring is full
		k = k < end - wireAt ? k : end - wireAt;
		k = k < recvFailAt - wireAt ? k : recvFailAt - wireAt; // the wire is cut exactly there
		memcpy(p, wire + wireAt, k);
		if (k && !ringCommit(k)) {
			return false;
		}
	}
	return true;
}
bool transferSend(TransferSession *s, TransferSource source, void *context) {
	return false; // no body goes out here
}
bool transferRingAcquire(TransferSession *s, unsigned char **buffer, size_t *capacity) {
	return false; // no gzip put here
}
bool transferPoll(TransferSession *s) {
	return false; // nor a gzip get
}
bool transferBytes(void *context, unsigned char *dst, size_t cap, size_t *got, bool *end) {
	return false;
}
void transferWaitTick(void *session) {} // no screen here
static CiaInfo parsed;
bool ciaParse(const u8 *prefix, u64 prefixSize, u64 ciaSize, CiaInfo *out) {
	*out = parsed;
	out->contentSize = ciaSize - prefixSize;
	return true;
}
// The prefix size the put()s use, 4 past a 16-byte boundary as a real one is. Content 0 starts at
// 2048.
#define PREFIX 2020
static u64 ciaTotal;
// The nth write (1-based) fails, moves a byte fewer, or moves nothing. Every case except the
// failure is reported as success.
static int failWrite, shortWrite, emptyWrite;
static unsigned homeOpenWrites; // writes into AM made while HOME was allowed
static volatile bool stuck;
static Result writeResult;
static u64 writeMs;	 // each write takes this long on the fake clock
static u64 earlyEnd; // the early call ends here, one block into the first content
Result FSFILE_Write(Handle f, u32 *written, u64 offset, const void *buf, u32 size, u32 flags) {
	do { // a slow writer, so the ring runs ahead of it
		usleep(1000);
	} while (stuck || heldPolls);
	__atomic_add_fetch(&fake.nowMs, writeMs, __ATOMIC_SEQ_CST);
	// Writes must arrive in order and byte for byte, at most 4194304 bytes each, and each must end
	// on a unit or at the CIA's end (AM short-writes), except for the early call.
	u64 end = offset + size;
	assert(offset == amAt && size <= 4 * RING_UNIT &&
		   (end % RING_UNIT == 0 || end == ciaTotal || end == earlyEnd));
	if (failWrite && --failWrite == 0) {
		return writeResult;
	}
	assert(memcmp(buf, wire + offset, size) == 0);
	homeOpenWrites += !fake.homeBlocked;
	fake.freeClusters -= offset == 0; // the title's first cluster
	*written = size - (shortWrite && --shortWrite == 0);
	*written = emptyWrite && --emptyWrite == 0 ? 0 : *written;
	amAt += *written;
	while (atEdge(amAt) && wireAt < amAt + 4 * RING_UNIT) {
		usleep(100);
	}
	return 0;
}

static const u64 APP = 0x000400000F800100ULL;
static const char ID[] = "0123456789abcdef0123456789abcdef";
static After after;
static char *call(const char *line) {
	static char buf[512];
	char *fields[MAX_FIELDS] = {0};
	size_t n = 0;
	snprintf(buf, sizeof(buf), "%s", line);
	for (char *t = strtok(buf, " "); t && n < MAX_FIELDS; t = strtok(NULL, " ")) {
		fields[n++] = t;
	}
	VerbContext c = {.client = 3, .request = line, .id = ID, .field = fields, .n = n};
	c.after = &after;
	for (c.verb = VERBS; strcmp(c.verb->name, fields[0]) != 0; c.verb++) {
	}
	fake.replies = 0;
	after = AFTER_NOTHING;
	if (!c.verb->check || c.verb->check(&c)) {
		gApp.requestSpent = c.verb->mutation; // as the dispatcher would
		c.verb->run(&c);
	}
	// Otherwise fake.reply is still the last call's, and a verb that answered nothing would pass as
	// if it had answered.
	assert(fake.replies >= 1);
	return strchr(fake.reply + strlen("response ") + strlen(ID) + 1 + NONCE_HEX + 1, ' ') + 1;
}

static const char *putPrefix(const char *intent, u64 titleID, u16 version, u64 size, u64 prefix,
							 bool goodDigest) {
	char line[512], digest[DIGEST_TEXT_SIZE];
	snprintf(digest, sizeof(digest), "%s", refDigestText(wire, prefix));
	digest[2] = goodDigest ? digest[2] : digest[2] == '0' ? '1' : '0';
	parsed = (CiaInfo){.titleID = titleID, .version = version};
	fake.titleID = titleID;
	fake.amCalls = fake.amMutations = fake.starts = fake.overwriteStarts = 0;
	fake.ticketDeletes = fake.cancels = 0;
	wireAt = amAt = 0;
	ciaTotal = size;
	earlyEnd = prefix < 2048 ? 2064 : 0;
	snprintf(line, sizeof(line), "put %s %064d %s %llu %llu %s", ID, 0, intent,
			 (unsigned long long)size, (unsigned long long)prefix, digest);
	char *reply = call(line) + strlen("put ");
	if (strncmp(reply, "committed", 9) == 0) { // AM now reports this version
		fake.installed = true;
		fake.version = version;
	}
	return reply;
}

static const char *put(const char *intent, u64 titleID, u16 version, u64 size, bool goodDigest) {
	return putPrefix(intent, titleID, version, size, PREFIX, goodDigest);
}

static void pin(const char *want, const char *got) {
	if (strncmp(got, want, strlen(want)) != 0) {
		fprintf(stderr, "testInstall: `%s`, want `%s`\n", got, want);
		abort();
	}
}

// A put refused before AM started changes nothing in AM.
static void refusedBeforeAm(const char *want, const char *got) {
	pin(want, got);
	assert(fake.amMutations == 0);
}

static void routes(void) {
	const u64 self = fake.selfTitleID;
	homeOpenWrites = 0;
	pin("committed ok 00000000 000400000f800100 2 new", put("new", APP, 2, 4096, true));
	assert(amAt == ciaTotal && fake.starts == 1 && fake.ticketDeletes == 0);
	assert(after == AFTER_NOTHING);
	// HOME is blocked for exactly the import, because closing HOME with a live import crashed the
	// console.
	assert(homeOpenWrites == 0 && !fake.homeBlocked);
	// Its account, the one bench.sh scores, shows tier 1 alone, one AM start and finish, the early
	// call and the rest of the write, and the space the gate and the commit saw.
	const char *stats = call("stats 0123456789abcdef0123456789abcdef");
	assert(
		strstr(stats, " ring_cap=25165824 ") && strstr(stats, " join=1 ") &&
		strstr(stats, " sd_needed=32768 sd_free_before=34359738368 sd_free_after=34359705600 ") &&
		strstr(stats, " am_start_n=1 ") && strstr(stats, " am_write_n=2 ") &&
		strstr(stats, " am_finish_n=1 ") && strstr(stats, " req=0123456789abcdef0123456789abcdef"));
	// A CIA of one unit has no write after the first, so there is no slowest one.
	assert(strstr(stats, " am_write_max_ms=- "));
	pin("committed ok 00000000 000400000f800100 3 update", put("update", APP, 3, 4096, true));
	assert(amAt == ciaTotal && fake.overwriteStarts == 1);
	// AM's overwrite start accepts an equal version and changes nothing, so the put is refused and
	// never `committed`.
	refusedBeforeAm("rejected not-newer 00000000 000400000f800100 3 update",
					put("update", APP, 3, 4096, true));
	fake.chainload = 0;
	pin("committed ok 00000000 0004000003d37200 4 self", put("self", self, 4, 4096, true));
	assert(amAt == ciaTotal && after == AFTER_SELF_UPDATED && fake.chainload == self);
	refusedBeforeAm("rejected route 00000000 000400000f800100 4 self",
					put("self", APP, 4, 4096, true));
	refusedBeforeAm("rejected exists 00000000 000400000f800100 2 new",
					put("new", APP, 2, 4096, true));
	// A refusal frees the ring's tier 2, which a CIA past tier 1 armed.
	refusedBeforeAm("rejected exists 00000000 000400000f800100 2 new",
					put("new", APP, 2, 30u << 20, true));
	assert(!ring.tier[1]);
	refusedBeforeAm("rejected not-newer 00000000 000400000f800100 1 update",
					put("update", APP, 1, 4096, true));
	fake.installed = false;
	// An `update` of a title AM does not hold is refused as such, not run as an install.
	refusedBeforeAm("rejected notfound ", put("update", APP, 3, 4096, true));
	fake.pendingID = APP; // an unfinished import of it
	refusedBeforeAm("rejected exists-pending 00000000 000400000f800100 2 new",
					put("new", APP, 2, 4096, true));
	fake.pendingID = 0;
	fake.pendingInfoResult = (Result)0xD8A08004; // AM cannot say whether an import is pending
	refusedBeforeAm("rejected am-info d8a08004 000400000f800100 2 new",
					put("new", APP, 2, 4096, true));
	fake.pendingInfoResult = 0;
	fake.freeClusters = 0;
	refusedBeforeAm("rejected space d963f80e 000400000f800100 2 new",
					put("new", APP, 2, 4096, true));
	// A card with nothing free is a measured 0; no install ended, so nothing was measured after.
	assert(strstr(call("stats 0123456789abcdef0123456789abcdef"),
				  " sd_needed=32768 sd_free_before=0 sd_free_after=- "));
	fake.freeClusters = 0x100000;
	refusedBeforeAm("rejected digest d963f80a - - new", put("new", APP, 2, 4096, false));
	fake.startResult = (Result)0xD8A08009;
	pin("rejected am-start d8a08009 000400000f800100 2 new", put("new", APP, 2, 4096, true));
	assert(!fake.homeBlocked); // no import was opened, so HOME is allowed again
	fake.startResult = 0;
	// `stats self` counts the `new` imports AM started, not an overwrite's and not a refused start.
	assert(strstr(call("stats 0123456789abcdef0123456789abcdef self"), " new_n=1 lid="));
}

// A CIA that came by link names no route, so installOpen picks one (routePick) and the same gates
// run. This is qr.c's install: the two halves around the pump that installCia has, with the
// console's own pick. The title AM holds is the scripted one.
static unsigned waits;
static u64 writtenAtLastWait;
static void waitCounted(void *context) {
	waits++;
	writtenAtLastWait = ringDone();
	transferWaitTick(context);
}

static const char *qrInstall(u64 titleID, u16 version, const char **route) {
	TransferSession session = {0};
	InstallRun run = {0};
	Operation op = OP_REJECTED;
	parsed = (CiaInfo){.titleID = titleID, .version = version};
	fake.titleID = titleID;
	fake.amCalls = fake.amMutations = fake.starts = fake.overwriteStarts = 0;
	fake.ticketDeletes = fake.cancels = 0;
	wireAt = amAt = 0;
	ciaTotal = 4096;
	earlyEnd = 2064; // the early call, on every route
	assert(ringArm(ciaTotal, RING_TO_HANDLE, true, 0));
	assert(transferReceive(&session, PREFIX, NULL, NULL));
	osTickCounterStart(&run.gap); // when the prefix is in, as qr.c and installCia do
	gStats.phaseN[PHASE_PREFIX_GAP] = 0;
	waits = 0;
	bool opened = installOpen(&run, ciaTotal, PREFIX, &op);
	if (opened) {
		bool received = run.started && transferReceive(&session, ciaTotal - PREFIX, NULL, NULL);
		installClose(&run, received, "net", TRANSFER_RECV_FAILED, waitCounted, &session, &op);
	}
	// The screen is told it is committing once the writer has drained, before AM's commit runs. The
	// last call saw every byte written (the calls made while it drained saw fewer).
	assert(op.outcome != OUTCOME_COMMITTED || (waits >= 1 && writtenAtLastWait == ciaTotal));
	*route = run.route ? run.route->name : "none";
	// An open that got as far as AM's start measured the gap between the prefix and the body; one
	// that was refused before did not.
	assert(gStats.phaseN[PHASE_PREFIX_GAP] == (opened ? 1u : 0u));
	if (op.outcome == OUTCOME_COMMITTED) { // AM now reports this version
		fake.installed = true;
		fake.version = version;
	}
	return op.detail;
}

static void qrPin(const char *wantRoute, const char *wantDetail, u64 titleID, u16 version) {
	const char *route;
	const char *detail = qrInstall(titleID, version, &route);
	if (strcmp(route, wantRoute) != 0 || strcmp(detail, wantDetail) != 0) {
		fprintf(stderr, "testInstall: link install via `%s` says `%s`, want `%s` via `%s`\n", route,
				detail, wantDetail, wantRoute);
		abort();
	}
}

static void qrRoutes(void) {
	const u64 self = fake.selfTitleID;
	// A title AM does not hold is `new`. Once AM holds it, the same CIA at a newer version is an
	// `update`. The console decides, with the same gates a computer's install has.
	fake.installed = false;
	fake.importTicket = false;
	qrPin("new", "ok", APP, 2);
	assert(amAt == ciaTotal && fake.starts == 1 && fake.overwriteStarts == 0 && !fake.homeBlocked);
	qrPin("update", "ok", APP, 3);
	assert(amAt == ciaTotal && fake.starts == 0 && fake.overwriteStarts == 1);
	// An equal or older version over an installed one is refused before AM is touched.
	qrPin("update", "not-newer", APP, 3);
	assert(fake.amMutations == 0 && amAt == 0);
	qrPin("update", "not-newer", APP, 1);
	assert(fake.amMutations == 0);
	// 3Drop's own id is `self`, with the same version rule, installed or not.
	fake.installed = true;
	fake.version = 3;
	qrPin("self", "ok", self, 4);
	assert(fake.overwriteStarts == 1);
	qrPin("self", "not-newer", self, 4);
	assert(fake.amMutations == 0);
	// A title outside the managed set is `new` whatever AM holds, and refused before AM is asked.
	fake.installed = true;
	qrPin("new", "unmanaged", 0x0004001000021300ULL, 2);
	assert(fake.amCalls == 0);
	// When AM cannot say what it holds, whether or not it has the title, the route is `new`, and
	// the gate then refuses it in AM's words.
	fake.installed = true;
	fake.infoResult = (Result)0xD8A08004;
	qrPin("new", "am-info", APP, 2);
	assert(fake.amMutations == 0);
	fake.infoResult = 0;
	fake.installed = false;
	fake.pendingInfoResult = (Result)0xD8A08004;
	qrPin("new", "am-info", APP, 2);
	assert(fake.amMutations == 0);
	fake.pendingInfoResult = 0;
	// An unfinished import of it, and a full card, are refused as a computer's install is.
	fake.installed = false;
	fake.pendingID = APP;
	qrPin("new", "exists-pending", APP, 2);
	assert(fake.amMutations == 0);
	fake.pendingID = 0;
	fake.freeClusters = 0;
	qrPin("new", "space", APP, 2);
	assert(fake.amMutations == 0);
	fake.freeClusters = 0x100000;
	// When AM will not start the import, the install is refused, HOME is allowed again and the ring
	// is released.
	fake.startResult = (Result)0xD8A08009;
	qrPin("new", "am-start", APP, 2);
	assert(!fake.homeBlocked && !ring.tier[1]);
	fake.startResult = 0;
	fake.installed = false;
}

// A stop the user asked for (qr.c's B or START, recorded through installCancel) cancels the import
// wherever it lands before AM's commit: before the close, or while the screen says committing. The
// Operation alone carries it, because no global says the install came by link.
static Operation *canceling;
static void waitCancels(void *context) {
	waitCounted(context);
	if (ring.thread == NULL) { // joined, so this is the look before AM_FinishCiaInstall
		installCancel(canceling);
	}
}

static void qrCancels(void) {
	// The cases are: before the close (qr.c's look after the download's last), the same with a body
	// that also failed (the cancel's word wins, because the user asked for it), at the commit's
	// look, and with the whole body in while the writer is still on its first call. In that last
	// case only the cancel stops it, since qr.c records one without stopping the ring.
	for (int mode = 0; mode < 4; mode++) {
		bool committing = mode == 2, failed = mode == 1, behind = mode == 3;
		TransferSession session = {0};
		InstallRun run = {0};
		Operation op = OP_REJECTED;
		parsed = (CiaInfo){.titleID = APP, .version = 2};
		fake.titleID = APP;
		fake.installed = fake.importTicket = false;
		fake.ticketID = 0;
		fake.starts = fake.cancels = fake.ticketDeletes = 0;
		wireAt = amAt = 0;
		ciaTotal = behind ? 6u * RING_UNIT : 4096;
		earlyEnd = 2064;
		gApp.viaUrl = false;
		assert(ringArm(ciaTotal, RING_TO_HANDLE, true, 0));
		assert(transferReceive(&session, PREFIX, NULL, NULL));
		osTickCounterStart(&run.gap);
		assert(installOpen(&run, ciaTotal, PREFIX, &op));
		heldPolls = behind ? 50 : 0; // the writer's first call waits for the close's polls
		assert(run.started && transferReceive(&session, ciaTotal - PREFIX, NULL, NULL));
		canceling = &op;
		if (!committing) {
			installCancel(&op);
		}
		installClose(&run, !failed, "net", TRANSFER_RECV_FAILED,
					 committing ? waitCancels : waitCounted, &session, &op);
		// The writer is joined. A stop it saw before its first write left the hold uncounted, and
		// the cases after would wait out polls nobody makes.
		heldPolls = 0;
		assert(op.outcome == OUTCOME_REJECTED && strcmp(op.detail, "canceled") == 0 &&
			   op.result == TRANSFER_CANCELED);
		// Canceled and never finished, so the `new` leaves no title and drops the ticket it
		// brought.
		assert(fake.starts == 1 && fake.cancels == 1 && fake.ticketDeletes == 1 && !fake.installed);
		assert(!fake.homeBlocked && !ringStranded());
		// Stopped at once, as testQr's stopAtClose has it, not drained, so AM never held the CIA.
		assert(!behind || amAt < ciaTotal);
	}
}

// The drift guard again, through a link: every title the verdict file lists is refused or taken
// there in the same words as through the put verb, installed or not.
static void qrInstallable(void) {
	FILE *f = fopen(TESTS_DIR "/titleVerdicts.txt", "r");
	char line[256], word[16];
	unsigned long long id;
	unsigned rows = 0;
	assert(f);
	while (fgets(line, sizeof(line), f)) {
		if (line[0] != '#') {
			assert(sscanf(line, "%16llx %15s", &id, word) == 2);
			const char *want = strcmp(word, "ok") == 0 ? "ok" : word;
			for (int update = 0; update < 2; update++) {
				fake.installed = update; // AM holds v2 for the update, whatever the new did
				fake.version = 2;
				const char *route;
				const char *got = qrInstall((u64)id, (u16)(2 + update), &route);
				if (strcmp(got, want) != 0) {
					fprintf(stderr, "testInstall: link install of %016llx says `%s`, want `%s`\n",
							id, got, want);
					abort();
				}
				// Nothing is changed for a refusal; a title outside the managed set is not even
				// asked about (the pick reads AM for the others, which changes nothing).
				assert(strcmp(want, "ok") == 0 || fake.amMutations == 0);
				assert(strcmp(want, "unmanaged") != 0 || fake.amCalls == 0);
			}
			rows++;
		}
	}
	fclose(f);
	assert(rows > 20);
	fake.titleID = APP;
	fake.installed = false;
}

static Operation uninstalled; // the Operation of the last uninstall
static const char *uninstall(u64 titleID, bool ticket) {
	TitleFacts facts;
	Result why;
	fake.titleDeletes = fake.pendingDeletes = fake.ticketDeletes = fake.amMutations = 0;
	fake.titleDeleted = fake.pendingDeleted = fake.ticketDeleted = 0;
	const char *refusal = titleCheck(titleID, false, true, &facts, &why);
	if (refusal) {
		return refusal;
	}
	titleUninstall(titleID, &facts, ticket, &uninstalled);
	return uninstalled.detail;
}

static bool listed(u64 titleID) {
	char hex[17];
	snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)titleID);
	fake.bodyLen = 0;
	fake.body[0] = '\0';
	pin("ls ok ok ", call("ls 0123456789abcdef0123456789abcdef title:"));
	return strstr(fake.body, hex) != NULL;
}

// Install, `ls title:` and uninstall all take a title, or all refuse it before any AM call.
static void takes(u64 titleID, const char *refusal) {
	char want[32];
	snprintf(want, sizeof(want), "rejected %s ", refusal ? refusal : "");
	for (int update = 0; update < 2; update++) {
		fake.installed = update; // AM holds v2 for the update, whatever the new did
		fake.version = 2;
		const char *got = put(update ? "update" : "new", titleID, 2 + update, 4096, true);
		if (refusal) {
			refusedBeforeAm(want, got);
			assert(fake.amCalls == 0);
		} else {
			pin("committed ok ", got);
		}
	}
	fake.installed = true; // AM lists it either way
	assert(listed(titleID) == !refusal);
	fake.amCalls = 0;
	pin(refusal ? refusal : "ok", uninstall(titleID, false));
	assert(!refusal || fake.amCalls == 0);
}

// The drift guard on titleInstallable: every allowed high word with any one bit flipped is refused.
static void installable(void) {
	FILE *f = fopen(TESTS_DIR "/titleVerdicts.txt", "r");
	char line[256], word[16];
	unsigned long long id;
	unsigned rows = 0;
	assert(f);
	while (fgets(line, sizeof(line), f)) {
		if (line[0] != '#') {
			int fields = sscanf(line, "%16llx %15s", &id, word);
			assert(fields == 2);
			takes((u64)id, strcmp(word, "ok") == 0 ? NULL : word);
			rows++;
		}
	}
	fclose(f);
	assert(rows > 20);
	static const u32 HIGH[] = {0x00040000, 0x0004000E, 0x0004008C};
	for (size_t h = 0; h < sizeof(HIGH) / sizeof(HIGH[0]); h++) {
		for (u32 bit = 0; bit < 32; bit++) {
			takes((u64)(HIGH[h] ^ 1u << bit) << 32 | 0x0F800100, "unmanaged");
		}
	}
	fake.titleID = APP;
	fake.installed = false;
}

static const char *logSince(void) {
	static char text[4096];
	FILE *f = fopen(LOG_PATH, "r");
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	text[n] = '\0';
	if (f) {
		fclose(f);
	}
	remove(LOG_PATH);
	return text;
}

static u32 ticketsHeld(void) {
	u32 n = 0;
	assert(R_SUCCEEDED(AM_GetTicketCount(&n)));
	return n;
}

// A `new` that did not commit deletes the ticket its pipe committed, unless one existed before or
// AM would not cancel the import. An update never touches a ticket. Cancel's and Finish's failures
// are reported, never retried, and a ticket that would not go is `unknown ticket`.
static void aborts(void) {
	// Each route's abort logs its ticket. A fresh `new` drops it, a `new` over a held ticket keeps
	// it, and an update logs nothing.
	static const char *const TICKET_LINE[] = {"ticket-dropped result=0x00000000 bytes=1",
											  "ticket-kept result=0x00000000 bytes=1", NULL};
	for (int route = 0; route < 3; route++) { // new, new over an existing ticket, update
		const char *intent = route == 2 ? "update" : "new";
		fake.installed = route == 2;
		fake.version = 1;
		fake.ticketID = route == 1 ? APP : 0;
		fake.importTicket = false; // no ticket left by an earlier import either
		failWrite = 1;
		writeResult = (Result)0xD8E0806A;
		u32 held = ticketsHeld();
		logSince();
		pin("rejected am-write d8e0806a ", put(intent, APP, 2, 3u << 20, true));
		assert(fake.cancels == 1 && fake.ticketDeletes == (route == 0));
		const char *log = logSince();
		assert(TICKET_LINE[route] ? strstr(log, TICKET_LINE[route]) != NULL
								  : strstr(log, "ticket-") == NULL);
		assert(route != 0 || ticketsHeld() == held); // the fresh `new` left no ticket behind
		recvFailAt = 1u << 20;
		pin("rejected canceled d963f803 ", put(intent, APP, 2, 3u << 20, true));
		assert(fake.cancels == 1 && fake.ticketDeletes == (route == 0));
		recvFailAt = UINT64_MAX;
		failWrite = 1;
		fake.cancelResult = (Result)0xC8A04566; // unknown, not refused: an import AM did not cancel
		logSince();								// may yet commit, so the ticket it brought stays
		pin("unknown am-cancel c8a04566 ", put(intent, APP, 2, 4096, true));
		assert(fake.cancels == 1 && fake.ticketDeletes == 0);
		const char *kept = logSince();
		assert(route != 0 || strstr(kept, "ticket-kept result=0xC8A04566 bytes=0") != NULL);
		fake.cancelResult = 0;
		fake.importTicket = false;
		fake.finishResult = (Result)0xD8E08025;
		pin("unknown am-finish d8e08025 ", put(intent, APP, 2, 4096, true));
		assert(fake.cancels == 0 && fake.ticketDeletes == (route == 0));
		fake.finishResult = 0;
	}
	fake.installed = fake.importTicket = false; // no ticket before the `new`s below
	fake.ticketID = 0;
	// A `new` that did not commit and whose ticket AM will not delete is never a clean `rejected`,
	// because the console may now hold a ticket it did not have (`unknown ticket`, as an uninstall
	// says).
	fake.ticketResult = (Result)0xD8A08004;
	failWrite = 1;
	logSince();
	pin("unknown ticket d8a08004 ", put("new", APP, 2, 3u << 20, true));
	assert(fake.cancels == 1 && fake.ticketDeletes == 1 && fake.importTicket);
	assert(strstr(logSince(), "ticket-dropped result=0xD8A08004 bytes=1"));
	fake.ticketResult = 0;
	fake.importTicket = false;
	// A patch's or DLC's ticket stays whatever became of its import, because a DLC's ticket is its
	// purchase.
	failWrite = 1;
	logSince();
	pin("rejected am-write d8e0806a ", put("new", 0x0004000E00F80100ULL, 2, 3u << 20, true));
	assert(fake.cancels == 1 && fake.ticketDeletes == 0 && fake.importTicket);
	assert(strstr(logSince(), "ticket-kept result=0x00000000 bytes=0"));
	fake.importTicket = false;
	// AM taking a byte fewer than asked, and saying nothing, fails the import as SD_SHORT_IO.
	shortWrite = 1;
	pin("rejected am-write d963f80b ", put("new", APP, 2, 3u << 20, true));
	assert(fake.cancels == 1 && fake.ticketDeletes == 1);
	// A failed Finish that left the title installed committed it, so its ticket stays, and the log
	// says why (the Finish's Result).
	fake.finishResult = (Result)0xD8E08025;
	fake.finishInstalls = true;
	logSince();
	pin("unknown am-finish d8e08025 ", put("new", APP, 2, 4096, true));
	assert(fake.installed && fake.ticketDeletes == 0);
	assert(strstr(logSince(), "ticket-kept result=0xD8E08025 bytes=0"));
	fake.finishResult = 0;
	fake.finishInstalls = fake.installed = false;
	// AMNET cannot count the title's tickets before a `new`, so a ticket is taken to be held (its
	// eShop license, perhaps), and the canceled import deletes none.
	fake.numTicketsResult = (Result)0xD8A08004;
	failWrite = 1;
	logSince();
	pin("rejected am-write d8e0806a ", put("new", APP, 2, 3u << 20, true));
	assert(fake.cancels == 1 && fake.ticketDeletes == 0);
	assert(strstr(logSince(), "ticket-kept result=0xD8A08004 bytes=0"));
	fake.numTicketsResult = 0;
	// A failed Finish after which AM cannot say what it holds keeps the ticket, because a delete is
	// never made on a guess.
	import.titleID = APP;
	import.overwrite = import.hadTicket = false;
	fake.ticketDeletes = 0;
	fake.finishResult = (Result)0xD8E08025;
	fake.infoResult = (Result)0xD8A08004;
	Result ticket;
	assert(importEnd(true, &ticket) == fake.finishResult && fake.ticketDeletes == 0 && ticket == 0);
	fake.infoResult = 0;
	// It also keeps the ticket after a failed Finish that left the import pending, because AM's
	// cleanup or another tool may commit that import.
	fake.pendingID = fake.ticketID = APP;
	logSince();
	assert(importEnd(true, &ticket) == fake.finishResult && fake.ticketDeletes == 0);
	assert(strstr(logSince(), "ticket-kept result=0xD8E08025 bytes=0"));
	fake.finishResult = 0;
	fake.pendingID = fake.ticketID = 0;
}

// A writer's failure beats its pump's: past both tiers the pump waits on a writer that failed, so
// both fail. A writer that never started is reported as `writer`, with no ring account in `stats`.
// AM had started either way, so the import is canceled.
static void ladder(void) {
	fake.installed = false;
	failWrite = 1;
	writeResult = (Result)0xD8E0806A;
	pin("rejected am-write d8e0806a ", put("new", APP, 2, 45u << 20, true));
	assert(fake.cancels == 1);
	noThread = true;
	pin("rejected writer d963f811 000400000f800100 2 new", put("new", APP, 2, 30u << 20, true));
	noThread = false;
	assert(fake.cancels == 1 && !fake.homeBlocked && !ring.tier[1]);
	assert(
		strstr(call("stats 0123456789abcdef0123456789abcdef"), " ring_cap=- ring_high=- join=- "));
}

// Past tier 1 the ring takes tier 2 and wraps; tier 2 is freed once, after the join. The idle
// clock pauses for a full ring, never for a residue, and not past the stall bound.
static void ring2(void) {
	fake.installed = false;
	holdAtEdges = true; // no write straddles the end of tier 1 or the wrap
	pin("committed ok 00000000 000400000f800100 2 new", put("new", APP, 2, 57u << 20, true));
	holdAtEdges = false;
	assert(amAt == ciaTotal && !ring.tier[1] && gStats.ringCap == 40u << 20);
	ringArm(64u << 20, RING_TO_HANDLE, true, 0);
	ringCommit(300u << 10);
	assert(!ringBusy());
	ring.parked = true; // the last acquire found every unit taken
	u64 parkedMs = fake.nowMs;
	// Healthy AM calls take minutes, so under ten is not a stall.
	fake.nowMs = parkedMs + 10 * 60000 - 1;
	assert(ringBusy());
	fake.nowMs = parkedMs + WORKER_STALL_MS;
	assert(!ringBusy());
	ring.progressMs = nowMs() + 36; // stamped by the worker just after the pump read the clock
	assert(ringBusy());
	ringRelease();
	fake.installed = false;
}

// The first call, through content start + 16, goes while the pump is still inside the first unit,
// on every route, and the unit's own write picks up where that call stopped. first_write_ms covers
// both (each 10 ms write reads 9 on the fake tick). A body that ends before content start + 16 ends
// the wait, and a content start past the first unit gets no early call.
static void earlyFirstCall(void) {
	fake.installed = false;
	holdInUnit = true;
	writeMs = 10;
	pin("committed ok 00000000 000400000f800100 2 new", put("new", APP, 2, 3u << 20, true));
	writeMs = 0;
	assert(amInUnit == 2064 && amAt == ciaTotal);
	assert(strstr(call("stats 0123456789abcdef0123456789abcdef"), " first_write_ms=18 "));
	holdInUnit = true;
	pin("committed ok 00000000 000400000f800100 3 update", put("update", APP, 3, 3u << 20, true));
	assert(amInUnit == 2064 && amAt == ciaTotal);
	holdInUnit = true;
	pin("committed ok 00000000 0004000003d37200 4 self",
		put("self", fake.selfTitleID, 4, 3u << 20, true));
	assert(amInUnit == 2064 && amAt == ciaTotal);
	fake.installed = false;
	recvFailAt = 2048;
	pin("rejected canceled d963f803 000400000f800100 2 new", put("new", APP, 2, 3u << 20, true));
	recvFailAt = UINT64_MAX;
	assert(amAt == 0 && fake.cancels == 1 && !ringStranded());
	// An early call that AM refuses, takes none of, or takes only part of, fails the import at
	// once, even past the ring, where the pump would otherwise wait on a worker that has gone. The
	// import's ticket is deleted.
	writeResult = (Result)0xD8E0806A;
	for (int how = 0; how < 3; how++) {
		fake.importTicket = false;
		failWrite = how == 0;
		emptyWrite = how == 1;
		shortWrite = how == 2;
		pin(how ? "rejected am-write d963f80b " : "rejected am-write d8e0806a ",
			put("new", APP, 2, 45u << 20, true));
		assert(amAt == (how == 2 ? 2063 : 0) && fake.cancels == 1 && fake.ticketDeletes == 1 &&
			   !ringStranded());
	}
	// With a body cut after the early call, the early call may have gone through. The import is
	// canceled and the worker joined.
	fake.importTicket = false;
	recvFailAt = 4096;
	pin("rejected canceled d963f803 000400000f800100 2 new", put("new", APP, 2, 3u << 20, true));
	recvFailAt = UINT64_MAX;
	assert((amAt == 0 || amAt == 2064) && fake.cancels == 1 && !ringStranded());
	// With content start + 16 past the first unit there is no early call, so the unit's write never
	// falls short of it.
	fake.installed = false;
	pin("committed ok 00000000 000400000f800100 2 new",
		putPrefix("new", APP, 2, 3u << 20, RING_UNIT - 40, true));
	assert(amAt == ciaTotal);
	fake.installed = false;
}

// A call under a whole millisecond is a measured 0, not a missing value. The cases are the slowest
// write after the first of a CIA of more than one unit, and the free bytes once the install took
// the last cluster.
static void zeroTimes(void) {
	fake.installed = false;
	pin("committed ok 00000000 000400000f800100 2 new", put("new", APP, 2, 9u << 20, true));
	const char *stats = call("stats 0123456789abcdef0123456789abcdef");
	assert(gStats.phaseN[PHASE_AM_WRITE] > 2); // writes after the first
	assert(strstr(stats, " first_write_ms=0 ") && strstr(stats, " am_write_max_ms=0 "));
	fake.installed = false;
	fake.freeClusters = 1; // the install takes the last one
	pin("committed ok 00000000 000400000f800100 2 new", put("new", APP, 2, 4096, true));
	assert(fake.freeClusters == 0);
	assert(strstr(call("stats 0123456789abcdef0123456789abcdef"),
				  " sd_free_before=32768 sd_free_after=0 "));
	fake.freeClusters = 0x100000;
	fake.installed = false;
}

// JOINED is reached only when threadJoin() returns 0. A stranded writer frees and cancels nothing,
// and installs are refused, until the idle loop sees the writer return. Then the import is
// canceled.
static void strand(void) {
	for (int stuckWriter = 0; stuckWriter < 2; stuckWriter++) {
		fake.importTicket = false; // no ticket before this `new`
		stuck = stuckWriter;
		joinError = stuckWriter ? 0 : (Result)0xD8E007F7;
		fake.stepMs = 20000; // the stall bound passes in a few polls
		pin("unknown stranded 00000000 000400000f800100 2 new",
			put("new", APP, 2, 30u << 20, true));
		assert(fake.cancels == 0 && ring.tier[1] && after == AFTER_NOTHING); // tier 2 kept
		assert(fake.homeBlocked); // its import is still open
		ringPoll();
		assert(ringStranded() && fake.cancels == 0);
		stuck = false;
		joinError = 0;
		while (ringStranded()) {
			ringPoll();
		}
		assert(fake.cancels == 1 && !ring.tier[1] && fake.ticketDeletes == 1 && !fake.homeBlocked);
		fake.stepMs = 0;
	}
}

// Uninstall deletes the pending import, then the title, then the ticket, which goes only when asked
// and once both of the others went. AM's failures are never read as "nothing there".
static void uninstalls(void) {
	fake.importTicket = false;
	// With the title installed and its ticket held, but the ticket not asked for, the title goes
	// and the ticket stays.
	fake.installed = true;
	fake.ticketID = APP;
	pin("ok", uninstall(APP, false));
	assert(fake.titleDeletes == 1 && fake.ticketDeletes == 0);
	assert(fake.pendingDeleted == APP && fake.titleDeleted == APP); // each delete names the title
	// An unfinished `new` (exists-pending until this), with its ticket: both go.
	fake.pendingID = APP;
	pin("ok", uninstall(APP, true));
	assert(fake.pendingDeletes == 1 && fake.titleDeletes == 0 && fake.ticketDeletes == 1);
	// Installed, with an unfinished import of it too, and its ticket: all three go.
	fake.installed = true;
	fake.pendingID = fake.ticketID = APP;
	pin("ok", uninstall(APP, true));
	assert(fake.pendingDeletes == 1 && fake.titleDeletes == 1 && fake.ticketDeletes == 1);
	assert(fake.pendingDeleted == APP && fake.titleDeleted == APP && fake.ticketDeleted == APP);
	// AM refuses the pending import's delete, so nothing after it is tried.
	fake.installed = true;
	fake.ticketID = APP;
	fake.deletePendingResult = (Result)0xD8A08006;
	pin("am-delete", uninstall(APP, true));
	assert(fake.titleDeletes == 0 && fake.ticketDeletes == 0);
	fake.deletePendingResult = 0;
	// AM refuses the title's delete, so the ticket is not tried, and nothing changed.
	fake.installed = true;
	fake.deleteTitleResult = (Result)0xD8A08006;
	pin("am-delete", uninstall(APP, true));
	assert(fake.ticketDeletes == 0 && uninstalled.outcome == OUTCOME_REJECTED);
	// After a pending import of it went, what AM holds has changed, so the result is never
	// `rejected`.
	fake.pendingID = APP;
	pin("am-delete", uninstall(APP, true));
	assert(fake.pendingDeletes == 1 && fake.ticketDeletes == 0 &&
		   uninstalled.outcome == OUTCOME_UNKNOWN);
	fake.deleteTitleResult = 0;
	// The title went and the ticket did not, so the result is never `ok`.
	fake.ticketResult = (Result)0xD8A08004;
	pin("ticket", uninstall(APP, true));
	assert(uninstalled.outcome == OUTCOME_UNKNOWN && uninstalled.result == fake.ticketResult &&
		   fake.titleDeletes == 1);
	// With no ticket held (importEnd dropped the pipe's), there is nothing to delete, and the
	// result is `ok`.
	fake.installed = true;
	fake.ticketID = 0;
	pin("ok", uninstall(APP, true));
	assert(fake.titleDeletes == 1 && fake.ticketDeletes == 0);
	fake.ticketResult = 0;
	// AMNET cannot count its tickets, but the delete that was asked for is still tried.
	fake.installed = true;
	fake.ticketID = APP;
	fake.numTicketsResult = (Result)0xD8A08004;
	pin("ok", uninstall(APP, true));
	assert(fake.ticketDeletes == 1 && fake.ticketDeleted == APP);
	fake.numTicketsResult = 0;
	// The only AM_DeleteTicket call refuses a title outside the managed set, even when a ticket is
	// held, because deleting a system ticket bricks the console and a DLC's ticket is its purchase.
	static const u64 NOT_MANAGED[] = {0x0004001000021300ULL, 0x0004000E00175E00ULL,
									  0x0004008C00030002ULL};
	for (size_t i = 0; i < sizeof(NOT_MANAGED) / sizeof(NOT_MANAGED[0]); i++) {
		u32 held;
		fake.ticketID = NOT_MANAGED[i];
		fake.ticketDeletes = 0;
		assert(titleDropTicket(fake.ticketID, &held) == 0 && fake.ticketDeletes == 0 && held == 0);
	}
	// `stats self` reports AM's health, with `-` for anything AM did not answer. The X key is one
	// DoCleanup.
	fake.pending = 2;
	fake.needsCleanup = 1;
	assert(strstr(call("stats 0123456789abcdef0123456789abcdef self"),
				  " am_pending=2 am_cleanup=1 am_rc=00000000 "));
	fake.cleanupResult = (Result)0xD8A08004;
	assert(strstr(call("stats 0123456789abcdef0123456789abcdef self"),
				  " am_pending=2 am_cleanup=- am_rc=d8a08004 "));
	fake.cleanupResult = 0;
	assert(R_SUCCEEDED(amCleanup()) && fake.cleanups == 1);
	// The SD title database is created only where none exists, because `true` would replace every
	// title's record.
	titleDbStartup();
	assert(fake.titleDbInits == 1 && fake.titleDbOverwrites == 0);
}

int main(void) {
	wire = malloc(57u << 20);
	for (u32 i = 0; i < 57u << 20; i++) {
		wire[i] = (u8)(i * 131 + 7);
	}
	routes();
	qrRoutes();
	qrCancels();
	installable();
	qrInstallable();
	aborts();
	ladder();
	ring2();
	earlyFirstCall();
	zeroTimes();
	strand();
	uninstalls();
	TEST_PASS();
	return 0;
}

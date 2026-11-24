// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd.h"

#include "app.h"
#include "cia.h"
#include "cmdFs.h"
#include "fs.h"
#include "hash.h"
#include "hbldr.h"
#include "install.h"
#include "knobs.h" // BENCH
#include "layeredFs.h"
#include "led.h"
#include "ring.h"
#include "screenshot.h"
#include "stats.h"
#include "sysinfo.h"
#include "titles.h"
#include "transfer.h"

#include <3ds.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

// Holds values passed from a mutation's check to its run. It is static because one request is
// served at a time.
static struct {
	bool batched, ticket, seedHeld;
	int screen; // 0 top, 1 bottom
	After after;
	u64 titleID;	// 0 when no title is named; for a 3dsx run, Luma's placeholder title
	Target path;	// used only when running a 3dsx
	char stdio[18]; // its 3dslink argument
	TitleFacts facts;
	const Route *route;
	u64 size, prefixSize, batch[3];
	u8 digest[32], seed[16];
} arg;

// A title has one spelling, `title:<id>`, which is never a path, because a path starts with `/`,
// `hex:` or `sdmc:`.
static bool isTitle(const char *field) {
	return strncmp(field, "title:", strlen("title:")) == 0;
}

static bool argTitle(VerbContext *c, const char *field, u64 *titleID) {
	if (!isTitle(field) || !parseTid(field + strlen("title:"), titleID)) {
		return replyFail(c, "grammar", 0);
	}
	return true;
}

// The id's own refusal word needs no AM, so it comes before `stranded`, as it does for a path's
// gate.
static bool argTitleTaken(VerbContext *c, u64 titleID, bool applicationOnly) {
	return replyRefusal(c, titleRefusal(titleID, applicationOnly), 0);
}

// ---- stats <id> [xfer|self|op] --------------------------------------------------------------

// A version the console could not determine is `-`, as is every field it could not determine
// (protocol.h).
static const char *versionText(char out[12], int version) {
	if (version < 0) {
		return "-";
	}
	snprintf(out, 12, "%d", version);
	return out;
}

static void countText(char out[12], Result r, u32 n, Result *first) {
	if (R_SUCCEEDED(r)) {
		snprintf(out, 12, "%lu", (unsigned long)n);
	} else if (*first == 0) {
		*first = r;
	}
}

// What the console is doing: a QR flow holds it, a transfer is stranded, or neither.
static const char *stateText(const VerbContext *c) {
	return c->busy != NOT_BUSY ? "qr" : ringStranded() ? "stranded" : "ready";
}

// This is the only section that asks AM, the card or PTM, and it asks even while stranded, because
// it is the diagnostic. It does not ask while busy, because the QR flow serves it on the thread
// that reads B and a call that blocked would freeze B. While busy, its answers are `-` and the
// version is the one cached at boot, as `op` has it. am_tickets counts system tickets too. up_ms is
// the console's uptime, since the system tick counts from boot; AM's state lives in Process9, which
// only a boot resets.
static void statsSelf(VerbContext *c) {
	static const char *const LID[] = {
		[LID_UNKNOWN] = "-", [LID_OPEN] = "open", [LID_CLOSED] = "closed"};
	char version[12], pending[12] = "-", cleanup[12] = "-", titles[12] = "-", tickets[12] = "-",
					  space[80] = "free=- cluster=- clusters=-", console[256];
	int installed = gApp.installedTitleVersion;
	u64 freeBytes;
	u32 cluster, clusters, pendingTitles = 0, titleCount = 0, ticketCount = 0;
	u8 needsCleanup = 0;
	Result amRc = 0, r;
	if (c->busy == NOT_BUSY) {
		installed = selfTitleVersion();
		// Make each call first. The order in which arguments are evaluated is unspecified, so a
		// count could be read before AM wrote it.
		r = AM_GetNumPendingTitles(&pendingTitles, MEDIATYPE_SD);
		countText(pending, r, pendingTitles, &amRc);
		r = AM_NeedsCleanup(&needsCleanup, MEDIATYPE_SD);
		countText(cleanup, r, needsCleanup, &amRc);
		r = AM_GetTitleCount(MEDIATYPE_SD, &titleCount);
		countText(titles, r, titleCount, &amRc);
		r = AM_GetTicketCount(&ticketCount);
		countText(tickets, r, ticketCount, &amRc);
		if (R_SUCCEEDED(sdFreeSpace(&freeBytes, &cluster, &clusters))) {
			snprintf(space, sizeof(space), "free=%llu cluster=%lu clusters=%lu",
					 (unsigned long long)freeBytes, (unsigned long)cluster,
					 (unsigned long)clusters);
		}
	}
	sysinfoFormat(console, sizeof(console));
	REPLY_OK(c,
			 " ver=%s tid=%016llx state=%s am_pending=%s am_cleanup=%s am_rc=%08lx %s am_titles=%s "
			 "am_tickets=%s%s up_ms=%llu new_n=%lu lid=%s",
			 versionText(version, installed), SELF_TITLE_ID, stateText(c), pending, cleanup,
			 (unsigned long)(u32)amRc, space, titles, tickets, console,
			 (unsigned long long)uptimeMs(), (unsigned long)installNewStarts(), LID[lidState()]);
}

// `xfer` never calls AM or the card; `op` is also how a host gets its first nonce.
static void cmdStats(VerbContext *c) {
	const char *section = c->n > 2 ? c->field[2] : "xfer";
	if (strcmp(section, "xfer") == 0) {
		char text[STATS_TEXT_MAX];
		statsFormat(text, sizeof(text));
		REPLY_OK(c, "%s", text);
	} else if (strcmp(section, "self") == 0) {
		statsSelf(c);
	} else if (strcmp(section, "op") == 0) {
		// The version cached at boot is included, so a relaunch's proof doesn't ask AM (see stats
		// self). The release name is only shown. The receipt comes last because the host reads it
		// to the end.
		char version[12];
		REPLY_OK(c, " ver=%s rel=%s %s", versionText(version, gApp.installedTitleVersion),
				 APP_VERSION, protocolLastOp());
	} else {
		replyFail(c, "grammar", 0);
	}
}

// ---- ls <id> <path|title:|title:<id>> [d|t|dt|r|rt] -----------------------------------------

static bool listTitle(u64 titleID, const AM_TitleInfo *info) {
	char line[64];
	int n = info ? snprintf(line, sizeof(line), "t %u %llu %016llx\n", info->version,
							(unsigned long long)info->size, (unsigned long long)titleID)
				 : snprintf(line, sizeof(line), "t - - %016llx\n", (unsigned long long)titleID);
	return listingLine(line, (size_t)n);
}

static void lsTitles(VerbContext *c) {
	if (!listingBegin(c, "")) {
		return;
	}
	Result r = titleEnumerate(listTitle);
	listingEnd(R_FAILED(r) ? OUTCOME_REJECTED : OUTCOME_COMMITTED, R_FAILED(r) ? "am-list" : "ok",
			   r);
}

static void lsTitle(VerbContext *c, u64 titleID) {
	TitleFacts t;
	Result why;
	const char *refusal = titleCheck(titleID, false, false, &t, &why);
	if (replyRefusal(c, refusal, why)) {
		AM_TitleInfo info = {.titleID = titleID, .size = t.size, .version = t.version};
		listingBegin(c, "");
		listTitle(titleID, &info);
		listingEnd(OUTCOME_COMMITTED, "ok", 0);
	}
}

static void cmdLs(VerbContext *c) {
	// "" is first because it is the only word a title takes.
	static const char *const FLAGS[] = {"", "d", "t", "dt", "r", "rt"};
	const char *target = c->field[2];
	bool title = isTitle(target), all = strcmp(target, "title:") == 0, named = title && !all;
	u64 titleID = 0;
	int f = argWord(c, 3, FLAGS, title ? 1 : sizeof(FLAGS) / sizeof(FLAGS[0]));
	if (f < 0 || (named && !(argTitle(c, target, &titleID) && argTitleTaken(c, titleID, false)))) {
		return;
	}
	if (!argIdle(c)) { // the listing's digest is taken in tier 1, and `title:` asks AM
		return;
	}
	if (!title) {
		lsPath(c, FLAGS[f]);
	} else if (all) {
		lsTitles(c);
	} else {
		lsTitle(c, titleID);
	}
}

// ---- put ------------------------------------------------------------------------------------
// put <id> <nonce> <new|update|self> <size> <prefix-size> <prefix-digest>
//   [<index> <total> <installed>]
// put <id> <nonce> <path> <size> <digest>
//   [gzip <wire-length> | range <offset> <length> <kept> | system]
// (a range that comes before the file's last one may give its size as `-`)

static bool checkPutCia(VerbContext *c) {
	char **f = c->field;
	arg.batched = c->n == 10;
	bool ok = (c->n == 7 || arg.batched) && parseU64(f[4], &arg.size) &&
			  parseU64(f[5], &arg.prefixSize) && arg.prefixSize != 0 && arg.prefixSize < arg.size &&
			  arg.prefixSize <= CIA_MAX_PREFIX_SIZE;
	for (size_t i = 0; ok && arg.batched && i < 3; i++) {
		ok = parseU64(f[7 + i], &arg.batch[i]);
	}
	if (!ok || (arg.batched && (arg.batch[0] == 0 || arg.batch[0] > arg.batch[1] ||
								arg.batch[1] > 9999 || arg.batch[2] >= arg.batch[0]))) {
		return replyFail(c, "grammar", 0);
	}
	return argDigest(c, f[6], arg.digest) && argIdle(c); // the CIA streams through tier 1 into AM
}

static bool checkPut(VerbContext *c) {
	arg.route = routeFind(c->field[3]);
	return arg.route ? checkPutCia(c) : checkPutFile(c);
}

static void runPutCia(VerbContext *c) {
	Operation *op = &gApp.last; // the screen follows the install as it happens
	gApp.batchActive = arg.batched;
	gApp.batchIndex = (u32)arg.batch[0];
	gApp.batchTotal = (u32)arg.batch[1];
	gApp.batchInstalled = (u32)arg.batch[2];
	appSetStatus(SERVER_RECEIVING);
	installCia(c->client, arg.route, arg.size, arg.prefixSize, arg.digest, op);
	bool committed = appInstallDone(op);
	char titleID[17] = "-", version[12];
	if (op->titleID != 0) {
		snprintf(titleID, sizeof(titleID), "%016llx", (unsigned long long)op->titleID);
	}
	reply(c, op->outcome, op->detail, op->result, " %s %s %s", titleID,
		  versionText(version, op->version), arg.route->name);
	if (committed && arg.route->self) {
		aptSetChainloaderToSelf();
		*c->after = AFTER_SELF_UPDATED;
	}
}

static void runPut(VerbContext *c) {
	statsStart(c); // `stats` describes this request from here on
	if (arg.route) {
		runPutCia(c);
	} else {
		runPutFile(c);
	}
}

// ---- delete <id> <nonce> <path> [r|system] | delete <id> <nonce> title:<id> [ticket] ---------

static bool checkDelete(VerbContext *c) {
	// These are each word's position in the tables below.
	enum { FLAG_NONE, FLAG_TREE, FLAG_SYSTEM, FLAG_TICKET = 1 };
	static const char *const PATH_FLAGS[] = {
		[FLAG_NONE] = "", [FLAG_TREE] = "r", [FLAG_SYSTEM] = "system"};
	static const char *const TITLE_FLAGS[] = {[FLAG_NONE] = "", [FLAG_TICKET] = "ticket"};
	const char *target = c->field[3];
	bool title = isTitle(target);
	int flag = title ? argWord(c, 4, TITLE_FLAGS, sizeof(TITLE_FLAGS) / sizeof(TITLE_FLAGS[0]))
					 : argWord(c, 4, PATH_FLAGS, sizeof(PATH_FLAGS) / sizeof(PATH_FLAGS[0]));
	Result why = 0;
	arg.titleID = 0;
	arg.ticket = title && flag == FLAG_TICKET;
	if (flag < 0) {
		return false;
	}
	if (!title) {
		return checkDeletePath(c, flag == FLAG_TREE, flag == FLAG_SYSTEM);
	}
	if (!argTitle(c, target, &arg.titleID) || !argTitleTaken(c, arg.titleID, arg.ticket) ||
		!argIdle(c)) { // the title check asks AM
		return false;
	}
	const char *refusal = titleCheck(arg.titleID, arg.ticket, true, &arg.facts, &why);
	return replyRefusal(c, refusal, why);
}

static void runDelete(VerbContext *c) {
	if (arg.titleID != 0) {
		Operation op;
		titleUninstall(arg.titleID, &arg.facts, arg.ticket, &op);
		reply(c, op.outcome, op.detail, op.result, " %016llx", (unsigned long long)arg.titleID);
	} else {
		runDeletePath(c);
	}
}

// ---- run <id> <nonce> self | title:<id> [game] | <path.3dsx> [stdio] ----------------------------

// libctru strips `<hex ip>_3DSLINK_` from argv, and link3dsStdio() connects back to that host on
// port 17491, so the argument names the requester.
static bool check3dsx(VerbContext *c) {
	const char *name;
	bool isDir;
	u64 size;
	Result why;
	TitleFacts facts;
	struct sockaddr_in peer;
	socklen_t peerSize = sizeof(peer);
	if (!argPath(c, 3, &arg.path, true)) { // a launch reads the file
		return false;
	}
	name = strrchr(arg.path.path, '.');
	if (!name || strcasecmp(name, ".3dsx") != 0) {
		return replyFail(c, "path", 0);
	}
	if (R_FAILED(why = fsStat(fsArchive(), arg.path.path, &isDir, &size)) || isDir) {
		return replyFail(c, "notfound", why);
	}
	arg.titleID = LUMA_SHARED_CONFIG->selectedHbldr3dsxTid;
	if (!LUMA_SHARED_CONFIG->useHbldr || !hbldrPresent()) {
		return replyFail(c, "hbldr", 0);
	}
	if (titleCheck(arg.titleID, true, false, &facts, &why)) {
		return replyFail(c, "hbldr-title", why);
	}
	if (c->n == 5) {
		if (getpeername(c->client, (struct sockaddr *)&peer, &peerSize) != 0) {
			return replyFail(c, "stdio", 0);
		}
		snprintf(arg.stdio, sizeof(arg.stdio), "%08lx_3DSLINK_",
				 (unsigned long)peer.sin_addr.s_addr);
	}
	return true;
}

// A title with a romfs folder is launched only when the user calls it a game (layeredFs.h).
static bool checkRun(VerbContext *c) {
	enum { WORD_NONE, WORD_STDIO, WORD_GAME };
	static const char *const OPTIONS[] = {
		[WORD_NONE] = "", [WORD_STDIO] = "stdio", [WORD_GAME] = "game"};
	const char *target = c->field[3];
	Result why = 0;
	arg.titleID = 0;
	arg.after = AFTER_RELAUNCH;
	arg.path.path[0] = arg.stdio[0] = '\0';
	bool self = strcmp(target, "self") == 0, title = isTitle(target);
	int word = argWord(c, 4, OPTIONS, sizeof(OPTIONS) / sizeof(OPTIONS[0]));
	if (word < 0 || (title && !argTitle(c, target, &arg.titleID))) {
		return false;
	}
	// stdio is only for a 3dsx, and game is only for a title.
	if ((word == WORD_STDIO && (self || title)) || (word == WORD_GAME && !title)) {
		return replyFail(c, "grammar", 0);
	}
	if (title && arg.titleID != SELF_TITLE_ID && !argTitleTaken(c, arg.titleID, true)) {
		return false;
	}
	if (!argIdle(c)) { // exiting would leave what the stranded worker holds (an import) open
		return false;
	}
	if (!self && !title) {
		arg.after = AFTER_LAUNCH;
		return check3dsx(c);
	}
	if (self || arg.titleID == SELF_TITLE_ID) {
		arg.titleID = 0;
		return true;
	}
	arg.after = AFTER_LAUNCH;
	const char *refusal = titleCheck(arg.titleID, true, false, &arg.facts, &why);
	if (!refusal && word != WORD_GAME) {
		refusal = layeredFsRefusal(arg.titleID, &why);
	}
	return replyRefusal(c, refusal, why);
}

static void runRun(VerbContext *c) {
	if (arg.after == AFTER_RELAUNCH) {
		REPLY_OK(c, " self");
		// This uses libctru's flag-2 self-chainload, because a flag-0 explicit-title jump loses the
		// restart signal that startup reads.
		aptSetChainloaderArgs(RUN_SELF_MARKER, sizeof(RUN_SELF_MARKER), NULL);
		aptSetChainloaderToSelf();
	} else if (arg.path.path[0] == '\0') {
		REPLY_OK(c, " %016llx", (unsigned long long)arg.titleID);
		aptSetChainloader(arg.titleID, MEDIATYPE_SD);
	} else {
		// When the 3dsx exits, Luma's spent loader runs hbmenu instead of 3Drop, so remote access
		// ends here.
		Result r = hbldrArm(arg.path.path, arg.stdio[0] ? arg.stdio : NULL);
		if (R_FAILED(r)) {
			reply(c, OUTCOME_REJECTED, "hbldr", r, " 3dsx %016llx",
				  (unsigned long long)arg.titleID);
			return; // nothing is armed, so 3Drop stays
		}
		REPLY_OK(c, " 3dsx %016llx", (unsigned long long)arg.titleID);
		aptSetChainloader(arg.titleID, MEDIATYPE_SD);
	}
	*c->after = arg.after;
}

// ---- seed <id> <nonce> title:<id> <seed> -------------------------------------------------------

static bool checkSeed(VerbContext *c) {
	u8 stored[16];
	TitleFacts facts;
	Result why;
	if (!argTitle(c, c->field[3], &arg.titleID)) {
		return false;
	}
	if (!hexDecode(arg.seed, c->field[4], sizeof(arg.seed))) {
		return replyFail(c, "grammar", 0);
	}
	if (!argTitleTaken(c, arg.titleID, true) || !argIdle(c)) { // the title check asks AM
		return false;
	}
	const char *refusal = titleCheck(arg.titleID, true, false, &facts, &why);
	if (!refusal) {
		why = seedGet(arg.titleID, stored);
		arg.seedHeld = R_SUCCEEDED(why);
		if (!arg.seedHeld && why != SEED_ABSENT) {
			refusal = "seed";
		} else if (arg.seedHeld && memcmp(stored, arg.seed, sizeof(stored)) != 0) {
			refusal = "exists"; // the held seed may be the one the title boots with
		}
	}
	return replyRefusal(c, refusal, why);
}

static void runSeed(VerbContext *c) {
	Result r = arg.seedHeld ? 0 : seedAdd(arg.titleID, arg.seed);
	if (R_FAILED(r)) {
		reply(c, OUTCOME_REJECTED, "seed", r, " %016llx", (unsigned long long)arg.titleID);
	} else {
		REPLY_OK(c, " %016llx %s", (unsigned long long)arg.titleID,
				 arg.seedHeld ? "present" : "added");
	}
}

// ---- screenshot <id> <nonce> <top|bottom> ------------------------------------------------------
// screenshot is a mutation, so a captured request cannot be replayed to watch the screen.

static bool checkScreenshot(VerbContext *c) {
	static const char *const SCREENS[] = {"top", "bottom"};
	arg.screen = argWord(c, 3, SCREENS, 2);
	return arg.screen >= 0 && argIdle(c); // the BMP is made in tier 1
}

static void runScreenshot(VerbContext *c) {
	Frame frame;
	u8 *bmp = ringTier1(), digest[32];
	statsStart(c);
	if (!bmp) {
		replyFail(c, "ring", OUT_OF_MEMORY);
		return;
	}
	u32 size = frameGrab(&frame) ? bmpEncode(bmp, &frame, arg.screen) : 0;
	if (size == 0) {
		replyFail(c, "capture", CAPTURE_FAILED);
		return;
	}
	Result r = digestOf(bmp, size, digest);
	if (R_FAILED(r)) {
		replyFail(c, "hash", r);
		return;
	}
	if (!REPLY_OK(c, " %lu raw", (unsigned long)size)) {
		return;
	}
	TransferSession session;
	TransferBytes bytes = {bmp, size, size, 0};
	Operation op = OP_REJECTED;
	transferSessionInit(&session, c->client, size, NULL, false);
	if (transferSend(&session, transferBytes, &bytes)) {
		opSet(&op, OUTCOME_COMMITTED, "ok", 0);
	} else {
		transferFailOp(&op, &session);
	}
	bodyTrailer(c, &op, digest, session.acceptedSize);
}

// ---- The verb table -------------------------------------------------------------------------
// tools/tests/fakeDevice.sh greps these rows, so the fake takes the same arities as the device.
// The last field is the busiest state in which a row is still answered (protocol.h). At a QR
// screen, only `stats` and `screenshot` are answered.
static const Verb VERBS[] = {
	{"stats", 2, 3, false, NULL, cmdStats, BUSY_AT_SCREEN},
	{"ls", 3, 4, false, NULL, cmdLs, NOT_BUSY},
	{"get", 3, 6, false, NULL, cmdGet, NOT_BUSY},
	{"hash", 3, 3, false, NULL, cmdHash, NOT_BUSY},
	{"screenshot", 4, 4, true, checkScreenshot, runScreenshot, BUSY_AT_SCREEN},
	{"put", 6, 10, true, checkPut, runPut, NOT_BUSY},
	{"delete", 4, 5, true, checkDelete, runDelete, NOT_BUSY},
	{"mkdir", 4, 4, true, checkMkdir, runMkdir, NOT_BUSY},
	{"rename", 5, 5, true, checkRename, runRename, NOT_BUSY},
	{"run", 4, 5, true, checkRun, runRun, NOT_BUSY},
	{"seed", 5, 5, true, checkSeed, runSeed, NOT_BUSY},
	BENCH({"bench", 2, 8, false, NULL, benchRun, NOT_BUSY})};

bool cmdServe(int client, After *after) {
	return protocolServe(client, VERBS, sizeof(VERBS) / sizeof(VERBS[0]), after, NOT_BUSY);
}

void cmdServeBusy(int client, Busy busy) {
	After none = AFTER_NOTHING; // no verb served here writes it, and nothing reads it
	protocolServe(client, VERBS, sizeof(VERBS) / sizeof(VERBS[0]), &none, busy);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// There is no whole-CIA digest on the wire, because AM checks every content against the TMD as it
// writes it, and the request MAC covers the TMD through the prefix digest. A CIA that comes by URL
// has no host to sign it, so its integrity comes from the TLS to its server (certs.c) and AM's own
// checks against the TMD. Nothing is gated on AM's state, because no install deletes before it
// starts. So a refused AM start loses nothing and is itself the probe.
#include "install.h"

#include "app.h"
#include "cia.h"
#include "fs.h"
#include "hash.h"
#include "ring.h"
#include "stats.h"
#include "titles.h"
#include "transfer.h"
#include "ui.h"

#include <3ds.h>
#include <string.h>

static const Route ROUTES[] = {
	{"new", false, false}, {"update", false, true}, {"self", true, true}};

const Route *routeFind(const char *intent) {
	for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
		if (strcmp(ROUTES[i].name, intent) == 0) {
			return &ROUTES[i];
		}
	}
	return NULL;
}

static struct {
	Handle am;
	u64 titleID;
	bool overwrite;
	// Always true for an overwrite, because the title and its ticket were there before.
	bool hadTicket;
	Result ticketQuery;
	u32 tickets;
} import;
static u32 newStarts;

u32 installNewStarts(void) {
	return newStarts;
}

// A HOME close with a live overwrite handle crashed the system on hardware.
static void homeBlock(bool block) {
	aptSetHomeAllowed(!block);
	if (block) {
		appSetStatus(SERVER_INSTALLING);
	}
}

// Finish and Cancel take the handle whatever they return, so it is never retried or closed. AM
// commits a `new`'s ticket before its TMD, so a `new` that surely did not commit deletes the ticket
// unless one was there before. The log line is the only record of a ticket the console keeps. An
// import that AM would not cancel, or left pending, may yet commit, so it is not proof that the
// import did not commit. *ticket is set to the delete's failure when the ticket could not be shown
// gone, and to 0 otherwise.
static Result importEnd(bool commit, Result *ticket) {
	TitleFacts t;
	Result r = commit ? AM_FinishCiaInstall(import.am) : AM_CancelCIAInstall(import.am);
	import.am = 0;
	*ticket = 0;
	bool committed = commit && R_SUCCEEDED(r);
	if (!import.overwrite && !committed) {
		u32 held;
		if (import.hadTicket || !titleManaged(import.titleID)) { // a DLC's ticket is its purchase
			appendLog("ticket-kept", import.ticketQuery, import.tickets);
		} else if (R_FAILED(r) && (!commit || R_FAILED(titleRead(import.titleID, &t)) ||
								   t.installed || t.pending)) {
			appendLog("ticket-kept", r, 0);
		} else {
			*ticket = titleDropTicket(import.titleID, &held);
			appendLog("ticket-dropped", *ticket, held);
		}
	}
	homeBlock(false);
	return r;
}

static void importAbandon(const RingStats *stats) {
	Result ticket;
	appendLog("stranded-cancel", importEnd(false, &ticket), stats->written);
}

// The nonce is spent and a refusal must leave the console as it was, so nothing here mutates AM.
static const char *installGate(const Route *route, const CiaInfo *cia, Result *why) {
	TitleFacts t;
	u64 id = cia->titleID;
	if (!titleInstallable(id)) {
		return "unmanaged";
	}
	if ((id == SELF_TITLE_ID) != route->self) {
		return "route"; // 3Drop's own id goes as `self`, and nothing else does
	}
	const char *refusal = route->self ? NULL : titleRefusal(id, false);
	if (refusal) {
		return refusal;
	}
	if (R_FAILED(*why = titleRead(id, &t))) {
		return "am-info";
	}
	if (!route->overwrite && t.pending) {
		return "exists-pending";
	}
	if (!route->overwrite && t.installed) {
		return "exists";
	}
	if (route->overwrite && !t.installed) {
		*why = TITLE_ABSENT;
		return "notfound";
	}
	if (route->overwrite && cia->version <= t.version) {
		return "not-newer";
	}
	u64 freeBytes;
	u32 cluster, clusters;
	*why = sdFreeSpace(&freeBytes, &cluster, &clusters);
	if (R_FAILED(*why)) {
		return "space";
	}
	u64 need = cia->contentSize / cluster + (cia->contentSize % cluster != 0); // in clusters
	gStats.sdNeeded = need * cluster;
	gStats.sdFreeBefore = freeBytes;
	if (need > freeBytes / cluster) {
		*why = SD_NO_SPACE;
		return "space";
	}
	return NULL;
}

static void refuse(Operation *op, const char *detail, Result why) {
	ringRelease();
	opSet(op, OUTCOME_REJECTED, detail, why);
}

// Picks the route for a CIA whose sender does not name one, the way tools/drop.sh does: 3Drop's own
// id is `self`, a title AM holds is an `update`, and the rest are `new`. A title AM cannot be asked
// about goes as `new`, so installGate says why in its own words.
const Route *routePick(const CiaInfo *cia) {
	TitleFacts t;
	if (cia->titleID == SELF_TITLE_ID) {
		return routeFind("self");
	}
	bool installed =
		titleInstallable(cia->titleID) && R_SUCCEEDED(titleRead(cia->titleID, &t)) && t.installed;
	return routeFind(installed ? "update" : "new");
}

bool installOpen(InstallRun *run, u64 ciaSize, u64 prefixSize, Operation *op) {
	CiaInfo cia;
	Result why = 0;
	TickCounter tick;
	if (!ciaParse(ringTier1(), prefixSize, ciaSize, &cia)) {
		refuse(op, "metadata", STAGE_METADATA);
		return false;
	}
	op->titleID = import.titleID = cia.titleID;
	op->version = cia.version;
	if (!run->route) {
		run->route = routePick(&cia);
	}
	const Route *route = run->route;
	const char *detail = installGate(route, &cia, &why);
	if (detail) {
		refuse(op, detail, why);
		return false;
	}
	// A failed count counts as a ticket that existed, so nothing is deleted on a guess.
	import.overwrite = route->overwrite;
	import.tickets = 0;
	import.ticketQuery = route->overwrite ? 0 : titleTicketCount(cia.titleID, &import.tickets);
	import.hadTicket = route->overwrite || R_FAILED(import.ticketQuery) || import.tickets != 0;
	homeBlock(true);
	osTickCounterStart(&tick);
	Result r = route->overwrite ? AM_StartCiaInstallOverwrite(&import.am, MEDIATYPE_SD)
								: AM_StartCiaInstall(MEDIATYPE_SD, &import.am);
	phaseSince(PHASE_AM_START, &tick);
	if (R_FAILED(r)) {
		homeBlock(false);
		refuse(op, "am-start", r);
		return false;
	}
	newStarts += !route->overwrite;
	// AM holds the call that carries the first content's first block (TMDFinish, ContentBegin), so
	// that call goes as soon as its bytes are in and the hold overlaps the body's wire time. The
	// overlap was measured on a `new`; on an update, a call AM took short cancels it (shortIo).
	ringEarly(ciaAlign(prefixSize) + 16); // the TMD ends the prefix
	// No other AM call until the import ends, because AM calls during a live import slowed it.
	run->started = ringStart(import.am, false, importAbandon);
	phaseSince(PHASE_PREFIX_GAP, &run->gap);
	return true;
}

void installClose(InstallRun *run, bool received, const char *receiveDetail, Result receiveWhy,
				  void (*waiting)(void *context), void *context, Operation *op) {
	RingStats writer;
	Result r, ticket, why = 0;
	TickCounter tick;
	const char *detail = NULL;
	if (run->started && !ringBodyEnd(received && !installCanceled(op), PHASE_AM_WRITE, waiting,
									 context, &writer, op)) {
		// When stranded inside AM, HOME stays blocked, because quitting would leave AM's import
		// pipe held.
		return;
	}
	ringRelease();
	if (!run->started) {
		detail = "writer"; // not `ring`, because AM has started and this refusal cancels an import
		why = OUT_OF_MEMORY;
	} else if (writer.failure) {
		detail = "am-write";
		why = writer.failure;
	} else if (!received) {
		detail = receiveDetail;
		why = receiveWhy;
	}
	if (!detail && !installCanceled(op)) {
		waiting(context); // the screen says committing while AM_FinishCiaInstall runs
	}
	if (installCanceled(op)) {
		detail = op->detail;
		why = op->result;
	}
	if (detail) {
		r = importEnd(false, &ticket);
		if (R_FAILED(r)) {
			// An import that AM would not cancel may yet commit, so the outcome is unknown, not
			// refused.
			opSet(op, OUTCOME_UNKNOWN, "am-cancel", r);
		} else if (R_FAILED(ticket)) { // the ticket this import brought may still be held
			opSet(op, OUTCOME_UNKNOWN, "ticket", ticket);
		} else {
			opSet(op, OUTCOME_REJECTED, detail, why);
		}
		return;
	}
	osTickCounterStart(&tick);
	r = importEnd(true, &ticket);
	phaseSince(PHASE_AM_FINISH, &tick);
	if (R_FAILED(r)) {
		opSet(op, OUTCOME_UNKNOWN, "am-finish", r);
		return;
	}
	u64 freeAfter;
	u32 cluster, clusters;
	if (R_SUCCEEDED(sdFreeSpace(&freeAfter, &cluster, &clusters))) {
		gStats.sdFreeAfter = freeAfter;
		gStats.sdFreeAfterKnown = true;
	}
	opSet(op, OUTCOME_COMMITTED, "ok", 0);
}

void installCia(int client, const Route *route, u64 ciaSize, u64 prefixSize,
				const u8 prefixDigest[32], Operation *op) {
	TransferSession session;
	InstallRun run = {.route = route};
	u8 digest[32];
	*op = OP_REJECTED;
	if (!ringArm(ciaSize, RING_TO_HANDLE, true, 0)) {
		opSet(op, OUTCOME_REJECTED, "ring", OUT_OF_MEMORY);
		return;
	}
	transferSessionInit(&session, client, ciaSize, uiProgressTick, true);
	if (!transferReceive(&session, prefixSize, NULL, NULL)) {
		refuse(op, transferFailDetail(session.failure), session.failure);
		return;
	}
	osTickCounterStart(&run.gap);
	Result r = digestOf(ringTier1(), (u32)prefixSize, digest);
	if (R_FAILED(r)) {
		refuse(op, "hash", r);
		return;
	}
	if (!digestEqual(digest, prefixDigest)) {
		refuse(op, "digest", STAGE_DIGEST);
		return;
	}
	if (!installOpen(&run, ciaSize, prefixSize, op)) {
		return;
	}
	bool received = run.started && transferReceive(&session, ciaSize - prefixSize, NULL, NULL);
	installClose(&run, received, transferFailDetail(session.failure), session.failure,
				 transferWaitTick, &session, op);
}

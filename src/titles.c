// SPDX-License-Identifier: GPL-3.0-or-later
// Titles through AM (titles.h): what is installed, which titles 3Drop may touch, and uninstalling.
// An uninstall removes a pending import first, then the title, then its ticket when asked.
#include "titles.h"

#include "fs.h"

#include <3ds.h>
#include <stdlib.h>
#include <sys/param.h> // MIN

Result titleRead(u64 titleID, TitleFacts *facts) {
	AM_TitleInfo info = {0};
	AM_PendingTitleInfo pending;
	Result r = AM_GetTitleInfo(MEDIATYPE_SD, 1, &titleID, &info);
	*facts = (TitleFacts){.installed = R_SUCCEEDED(r), .version = info.version, .size = info.size};
	if (r == TITLE_ABSENT) {
		r = AM_GetPendingTitleInfo(1, MEDIATYPE_SD, &titleID, &pending);
		facts->pending = R_SUCCEEDED(r);
		r = r == TITLE_ABSENT ? 0 : r;
	}
	return R_FAILED(r) ? r : 0;
}

const char *titleCheck(u64 titleID, bool applicationOnly, bool pending, TitleFacts *facts,
					   Result *why) {
	const char *refusal = titleRefusal(titleID, applicationOnly);
	*why = 0;
	if (refusal) {
		return refusal;
	}
	if (R_FAILED(*why = titleRead(titleID, facts))) {
		return "am-info";
	}
	if (facts->installed || (pending && facts->pending)) {
		return NULL;
	}
	*why = TITLE_ABSENT;
	return "notfound";
}

int selfTitleVersion(void) {
	TitleFacts self;
	return R_SUCCEEDED(titleRead(SELF_TITLE_ID, &self)) && self.installed ? self.version : -1;
}

// One AM_GetTitleInfo asks for TITLE_CHUNK titles. libctru sends the ids and the infos as mapped
// buffers and 3dbrew gives the call a count with no cap (AM:GetProgramInfos), so a chunk is
// 3Drop's own bound on the stack and on what one failure costs. AM answers a chunk with one
// result. When that fails, each of its titles is asked for alone, so a title AM has no info for
// is still listed.
#define TITLE_CHUNK 32u

Result titleEnumerate(bool (*each)(u64 titleID, const AM_TitleInfo *info)) {
	u32 count = 0, read = 0, n = 0;
	u64 *ids = NULL;
	Result r = AM_GetTitleCount(MEDIATYPE_SD, &count);
	if (R_SUCCEEDED(r)) {
		ids = calloc(count + 1, sizeof(*ids));
		r = ids ? AM_GetTitleList(&read, MEDIATYPE_SD, count, ids) : OUT_OF_MEMORY;
	}
	for (u32 i = 0; R_SUCCEEDED(r) && i < read; i++) {
		if (titleInstallable(ids[i]) && titleUid(ids[i]) != titleUid(SELF_TITLE_ID)) {
			ids[n++] = ids[i];
		}
	}
	bool more = true;
	for (u32 from = 0; more && from < n; from += TITLE_CHUNK) {
		AM_TitleInfo info[TITLE_CHUNK];
		u32 take = MIN(n - from, TITLE_CHUNK);
		bool batched = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, take, &ids[from], info));
		for (u32 i = 0; more && i < take; i++) {
			u64 *id = &ids[from + i];
			bool known = batched || R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, id, &info[i]));
			more = each(*id, known ? &info[i] : NULL);
		}
	}
	free(ids);
	return r;
}

// AM_DeleteUserTitle refuses system titles. The outcome words are the ones in titles.h.
void titleUninstall(u64 titleID, const TitleFacts *facts, bool dropTicket, Operation *op) {
	*op = OP_REJECTED;
	op->titleID = titleID;
	Result r = AM_DeletePendingTitle(MEDIATYPE_SD, titleID);
	bool changed = R_SUCCEEDED(r); // TITLE_ABSENT means none was pending
	r = r == TITLE_ABSENT ? 0 : r;
	if (R_SUCCEEDED(r) && facts->installed) {
		r = AM_DeleteUserTitle(MEDIATYPE_SD, titleID);
	}
	if (R_FAILED(r)) {
		opSet(op, changed ? OUTCOME_UNKNOWN : OUTCOME_REJECTED, "am-delete", r);
		return;
	}
	u32 held;
	r = dropTicket ? titleDropTicket(titleID, &held) : 0;
	if (R_FAILED(r)) {
		opSet(op, OUTCOME_UNKNOWN, "ticket", r);
		return;
	}
	opSet(op, OUTCOME_COMMITTED, "ok", 0);
}

// AM answers TITLE_ABSENT, not 0, when there is no ticket, and libctru copies out a count anyway.
Result titleTicketCount(u64 titleID, u32 *count) {
	Result r = AMNET_GetTitleNumTickets(count, titleID);
	if (R_FAILED(r)) {
		*count = 0;
	}
	return r == TITLE_ABSENT ? 0 : r;
}

Result titleDropTicket(u64 titleID, u32 *held) {
	*held = 0;
	if (!titleManaged(titleID)) {
		return 0;
	}
	// If none is held, there is no `unknown ticket` for one that is already gone. If the count
	// fails, the delete is still tried.
	if (R_SUCCEEDED(titleTicketCount(titleID, held)) && *held == 0) {
		return 0;
	}
	Result r = AM_DeleteTicket(titleID);
	if (R_FAILED(r)) {
		appendLog("ticket-delete-failed", r, 0);
	}
	return r;
}

// This runs only when the user asks (X), because it deletes finished, uncommitted imports
// media-wide, whoever made them. An unfinished import (`exists-pending`) is not one of them;
// uninstalling its title clears it.
Result amCleanup(void) {
	Result r = AM_DoCleanup(MEDIATYPE_SD);
	appendLog("am-cleanup", r, 0);
	return r;
}

// Creates the SD title database if missing (false means never over an existing one). It runs once
// only, because the availability query is a full reload of AM's databases.
void titleDbStartup(void) {
	bool available = false;
	(void)AM_InitializeExternalTitleDatabase(false);
	Result r = AM_QueryAvailableExternalTitleDatabase(&available);
	if (R_FAILED(r) || !available) {
		appendLog("title-db-unavailable", r, 0);
	}
}

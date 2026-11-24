// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TITLES_H
#define TITLES_H

#include "result.h"

#include <3ds.h>
#include <stdbool.h>

// UIDs below 0x300 are system titles, and UIDs above 0xFFFFF are New 3DS system
// titles (3dbrew Titles).
static inline bool titleUidIsApplication(u32 uid) {
	return uid >= 0x300 && uid <= 0xFFFFF;
}
static inline u32 titleUid(u64 titleID) {
	return (u32)(titleID >> 8) & 0xFFFFFF;
}
// The only titles 3Drop will delete the ticket of, launch or write into, because
// AM_DeleteTicket has no guard and deleting a system ticket bricks the console.
// Variation 0x02 is left out because FBI installs it to NAND.
static inline bool titleManaged(u64 titleID) {
	return (titleID >> 32) == 0x00040000 && (titleID & 0xFF) != 0x02;
}
// NS layers a patch onto any title with the same low word, so a patch or DLC needs
// an application's id.
static inline bool titleInstallable(u64 titleID) {
	bool application = titleUidIsApplication(titleUid(titleID));
	switch (titleID >> 32) {
	case 0x00040000:
		return titleManaged(titleID);
	case 0x0004000E: // a patch
		return application && titleManaged(0x0004000000000000ULL | (u32)titleID);
	case 0x0004008C: // DLC: the low byte is its index, not a variation
		return application;
	default:
		return false;
	}
}
// SELF_TITLE_ID, 3Drop's own title, comes from the build; the Makefile reads it from
// res/buildCIA.rsf. These words are the one title vocabulary, used on every verb that
// names a title: `unmanaged` means a title 3Drop never touches, `self` means any
// title of 3Drop's uid (NS would launch its patch in 3Drop's place), and
// `not-application` means a patch or DLC where a verb takes an application only (it
// has no code, save, seed or LayeredFS of its own, and its ticket is its purchase).
static inline const char *titleRefusal(u64 titleID, bool applicationOnly) {
	if (!titleInstallable(titleID)) {
		return "unmanaged";
	}
	if (titleUid(titleID) == titleUid(SELF_TITLE_ID)) {
		return "self";
	}
	return applicationOnly && !titleManaged(titleID) ? "not-application" : NULL;
}
// 0xD8A083FA is matched exactly. Any other failure is AM's, and is never read as absent.
#define TITLE_ABSENT ((Result)MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, RM_AM, RD_NOT_FOUND))
int selfTitleVersion(void); // -1 when AM cannot say
typedef struct {
	bool installed, pending;
	u16 version;
	u64 size;
} TitleFacts;
Result titleRead(u64 titleID, TitleFacts *facts);
// Checks titleRefusal's word first, then AM's: `am-info` when AM cannot say,
// `notfound` (TITLE_ABSENT) when the title is not installed. With `pending`, an
// unfinished import is as good as installed.
const char *titleCheck(u64 titleID, bool applicationOnly, bool pending, TitleFacts *facts,
					   Result *why);
// A title AM gives no info for is passed to `each` with NULL, never skipped, because a listing
// must not hide it. If `each` returns false, the walk stops, so a listing whose host is gone
// asks AM nothing more, except at most the rest of the chunk it is in (it asks in chunks).
Result titleEnumerate(bool (*each)(u64 titleID, const AM_TitleInfo *info));
// Removes an unfinished import of the title first, then the title (`facts` from titleCheck),
// then its ticket when `dropTicket` asks, which the caller allows for an application only,
// because a retail title's ticket is its eShop license. The outcome is COMMITTED when all of
// it went, and `unknown` once something changed, with the detail `ticket` when the title
// went and AM could not show the ticket gone, or `am-delete` when the title's delete fails
// after a pending import was removed. `rejected am-delete` means nothing changed.
void titleUninstall(u64 titleID, const TitleFacts *facts, bool dropTicket, Operation *op);
Result titleTicketCount(u64 titleID, u32 *count);
// This is the only AM_DeleteTicket call, because a wrong ticket delete can brick the console.
Result titleDropTicket(u64 titleID, u32 *held);

Result amCleanup(void);
void titleDbStartup(void);

#endif // TITLES_H

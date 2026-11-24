// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef APP_H
#define APP_H

#include "led.h" // Led
#include "result.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#define DEFAULT_PORT 1337

// Every timer uses the console's uptime as its clock. osGetTime follows the system clock, which
// steps (a 36 ms step back was measured), and a step would end a deadline early or stretch it.
static inline u64 ticksMs(u64 ticks) {
	return ticks / (SYSCLOCK_ARM11 / 1000);
}
static inline u64 uptimeMs(void) {
	return ticksMs(svcGetSystemTick());
}

typedef enum {
	SERVER_STARTING, // no listener yet, and the network is still coming up
	SERVER_NEEDS_KEY,
	SERVER_READY,
	SERVER_LISTEN_FAILED, // never set; appStatus() reports it while the listener is lost
	SERVER_AUTH_FAILED,
	SERVER_RECEIVING,
	SERVER_INSTALLING,
	SERVER_INSTALLED,
	SERVER_INSTALL_FAILED,
	SERVER_FILE,	 // a `get` or file `put`
	SERVER_STRANDED, // never set; appStatus() reports it while a ring worker is stranded
	SERVER_FATAL,	 // a service would not start, so nothing will listen
	SERVER_STATUS_COUNT,
} ServerStatus;

static inline bool statusIsTransfer(ServerStatus status) {
	return status == SERVER_RECEIVING || status == SERVER_INSTALLING || status == SERVER_FILE;
}

// True for the statuses appSetIdle can leave, which show the status screen with no card on it.
static inline bool statusIsIdle(ServerStatus status) {
	return status == SERVER_STARTING || status == SERVER_NEEDS_KEY || status == SERVER_READY;
}

// Status changes go through appSetStatus(), so the LED's hold follows them. They come
// from main.c's listener and keys, a request's verb (cmd.c, cmdFs.c, install.c), the
// QR flow (qr.c), and appInstalledShown(), which takes INSTALLED back to idle.
typedef struct {
	ServerStatus status;
	// -1 when AM has not been asked yet (main.c) or cannot say; the screen shows ?
	int installedTitleVersion;
	// True when the current request is a mutation that spent its nonce
	// (protocolServe). For such a request, its reply is kept as the receipt for
	// `stats op`, its line is logged, and a send cut short is logged (net.h).
	bool requestSpent;

	// The fields in this group belong to a single request and are cleared by appTransferStart.
	u64 wireBytes;
	u64 wireTotal; // 0 when no transfer is running or its size is unknown
	bool viaUrl; // the transfer is a download from a scanned link (qr.c), not a computer's request
	// The bar follows the ring's frontier because received bytes run ahead of the
	// sink, and the wire bytes of a gzip body are not the file's bytes.
	u64 uiWrittenBytes;
	bool uiSunkProgress;
	bool fileSending, fileGzip;
	u64 inflatedBytes; // a gzip put's file bytes off the wire so far; its rate may not pass this
	char fileName[48];
	bool batchActive;
	u32 batchIndex;
	u32 batchTotal;
	u32 batchInstalled;

	// The last install, as it runs and then as its card. Only the next install replaces it.
	Operation last;
	u64 lastBytes; // what it took off the wire
	// True when the last install came from a scanned link. viaUrl belongs to a single transfer
	// and a later transfer clears it, but the card's wording must not change when that happens.
	bool lastViaUrl;
} App;

extern App gApp;

// A request that moves bytes starts here, so that no count, file name or batch position on
// screen is left over from an earlier request. The install card does not belong to a request.
static inline void appTransferStart(void) {
	gApp.wireBytes = gApp.wireTotal = gApp.uiWrittenBytes = gApp.inflatedBytes = 0;
	gApp.uiSunkProgress = gApp.fileSending = gApp.fileGzip = gApp.batchActive = gApp.viaUrl = false;
	gApp.fileName[0] = '\0';
	gApp.batchIndex = gApp.batchTotal = gApp.batchInstalled = 0;
}

ServerStatus appStatus(void);
void appSetStatus(ServerStatus status);
// Called when an install ends, whether it came from a computer or a link. It records
// the bytes the card shows, and sets INSTALLED only for COMMITTED (UNKNOWN shows as
// a failure, with its own wording). Returns true if the install committed.
static inline bool appInstallDone(const Operation *op) {
	bool committed = op->outcome == OUTCOME_COMMITTED;
	gApp.lastBytes = gApp.wireBytes;
	gApp.lastViaUrl = gApp.viaUrl;
	appSetStatus(committed ? SERVER_INSTALLED : SERVER_INSTALL_FAILED);
	return committed;
}
// Called when a card is dismissed or an install's card gives way. Sets READY once this process
// has listened; before that, STARTING, or NEEDS_KEY if there is no key. A scanned link installs
// with no key or listener, so the status can't simply go back to READY when a card goes away.
void appSetIdle(void);
// INSTALLED gives way to the idle status once its card is drawn. ui.c's
// uiCompletionTransition calls this right after, and holds the card for a second
// itself. Returns true if it was INSTALLED.
static inline bool appInstalledShown(void) {
	if (gApp.status != SERVER_INSTALLED) {
		return false;
	}
	appSetIdle();
	return true;
}
// An install's outcome is held, with the lid closed, until the lid opens or a status
// that is not idle replaces it. On a console that never listened, the card gives way to
// STARTING or NEEDS_KEY, not READY, and the color is kept through them as through
// READY. The hold is never stored in gApp.status, so the screen still reads that
// status; a lost listener or a stranded worker shows over it.
Led appLed(void);
// main.c calls these on listener events. While the listener is lost, the status and
// hold underneath wait for it.
void appListenerUp(void);
void appListenerDown(int err); // the errno of a failed bring-up or of a dead listener
// After a self-update, the green that the old process left lit becomes this process's hold.
void appStatusInit(bool selfUpdated);
#define STATUS_TICK_MS 250 // appStatusTick throttles itself to this, so call it at any rate
// Every main-thread wait that can outlast LED_REFRESH_MS must call this, or the LED goes dark
// while 3Drop is still running. Main thread only, because led.c and app.c take no locks.
void appStatusTick(void);
// The pattern outlives the process, so exit leaves the LED off. On a self-update,
// the green is written again so that it lasts a fresh ~15.4 s across the relaunch.
void appStatusShutdown(bool selfUpdating);

// HOME is blocked while an import is live (homeBlock). A POWER press then waits too,
// because aptMainLoop would act on it and exit with the import open, which is the
// crash the block prevents.
static inline bool appRunning(void) {
	return aptIsHomeAllowed() ? aptMainLoop() : !aptShouldClose();
}

// Returns 0 for an unknown total, and 100 when done is at or past total.
static inline u32 percentOf(u64 done, u64 total) {
	if (total == 0) {
		return 0;
	}
	if (done >= total) {
		return 100;
	}
	return (u32)((done * 100ULL) / total);
}
static inline u64 transferDoneBytes(const App *app) {
	return app->uiSunkProgress ? app->uiWrittenBytes : app->wireBytes;
}

#endif // APP_H

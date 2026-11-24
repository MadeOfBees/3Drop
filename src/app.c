// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"

#include "auth.h"  // authKeyLoaded
#include "knobs.h" // BENCH
#include "ring.h"  // ringStranded

#include <errno.h> // ENETDOWN

// It has no lock because only the main thread touches it, one client at a time. The first screens
// draw before main.c asks AM for its version, and 0 would read as a real version.
App gApp = {.installedTitleVersion = -1};

// The color of the LED for each status, and of its twin dot on the screen. The headline's hue comes
// from ui.c's own table, which differs on purpose (READY is green there).
#define LED_PURPLE 0xFF00FF
#define LED_BLUE 0x0000FF
#define LED_GREEN 0x00FF00
#define LED_RED 0xFF0000
static const Led LEDS[] = {
	[SERVER_STARTING] = {LED_BLUE, true},		[SERVER_NEEDS_KEY] = {LED_RED, false},
	[SERVER_READY] = {LED_PURPLE, true},		[SERVER_LISTEN_FAILED] = {LED_RED, false},
	[SERVER_AUTH_FAILED] = {LED_RED, false},	[SERVER_RECEIVING] = {LED_BLUE, true},
	[SERVER_INSTALLING] = {LED_BLUE, true},		[SERVER_INSTALLED] = {LED_GREEN, false},
	[SERVER_INSTALL_FAILED] = {LED_RED, false}, [SERVER_FILE] = {LED_BLUE, true},
	[SERVER_STRANDED] = {LED_BLUE, true},		[SERVER_FATAL] = {LED_RED, false},
};
_Static_assert(sizeof(LEDS) / sizeof(LEDS[0]) == SERVER_STATUS_COUNT, "a status has no LED row");

// Nothing is shown at start because the pattern register (0x2D) is write-only, so a pattern 3Drop
// didn't write can be neither saved nor restored.
static struct {
	bool ready, lidClosed;
	const Led *held;
	Led shown;
	u64 polledMs, writtenMs;
} indicator;

void appSetStatus(ServerStatus status) {
	gApp.status = status;
	if (status == SERVER_INSTALLED || status == SERVER_INSTALL_FAILED) {
		indicator.held = indicator.lidClosed ? &LEDS[status] : NULL;
	} else if (!statusIsIdle(status)) {
		indicator.held = NULL;
	}
}

static struct { bool listened, lost; } listener;

void appListenerUp(void) {
	listener.listened = true;
	listener.lost = false;
	if (gApp.status == SERVER_STARTING) {
		appSetStatus(SERVER_READY);
	}
}

// Before the first listen, a network still coming up is a start, not a failure. The boot path's
// errno arrives negated.
void appListenerDown(int err) {
	listener.lost = listener.listened || (err != ENETDOWN && err != -ENETDOWN);
}

// READY claims a listener has come up, and STARTING claims a key is loaded.
void appSetIdle(void) {
	appSetStatus(listener.listened ? SERVER_READY
				 : authKeyLoaded() ? SERVER_STARTING
								   : SERVER_NEEDS_KEY);
}

// Unreachable outranks stranded because, with both, a host cannot even ask `stats`.
ServerStatus appStatus(void) {
	return listener.lost ? SERVER_LISTEN_FAILED : ringStranded() ? SERVER_STRANDED : gApp.status;
}

Led appLed(void) {
	if (!indicator.ready) {
		return LED_OFF;
	}
	ServerStatus status = appStatus();
	return indicator.held && status == gApp.status ? *indicator.held : LEDS[status];
}

void appStatusInit(bool selfUpdated) {
	ledInit();
	indicator.ready = true;
	if (selfUpdated) {
		indicator.shown = LEDS[SERVER_INSTALLED];
		indicator.held = &LEDS[SERVER_INSTALLED];
	}
}

void appStatusTick(void) {
	u64 now = uptimeMs();
	if (now - indicator.polledMs < STATUS_TICK_MS) {
		return;
	}
	indicator.polledMs = now;
	Lid lid = lidState();
	if (lid == LID_UNKNOWN) {
		return; // an unknown lid authorizes no write
	}
	indicator.lidClosed = lid == LID_CLOSED;
	if (!indicator.lidClosed) {
		indicator.held = NULL; // opening the lid ends the hold
	}
	Led want = indicator.lidClosed ? appLed() : LED_OFF;
	if (want.rgb == indicator.shown.rgb && want.breathes == indicator.shown.breathes &&
		(!want.rgb || now - indicator.writtenMs < LED_REFRESH_MS)) {
		return; // the LED already shows it, and it is not yet due for a refresh
	}
	Result written = ledWrite(want);
	BENCH(benchLedWritten(want, written, now));
	// A failed write changes nothing, so the next poll writes again.
	if (R_SUCCEEDED(written)) {
		indicator.shown = want;
		indicator.writtenMs = now;
	}
}

void appStatusShutdown(bool selfUpdating) {
	Led last = selfUpdating && indicator.lidClosed ? appLed() : LED_OFF;
	if (last.rgb || indicator.shown.rgb) {
		(void)ledWrite(last);
	}
	ledExit();
}

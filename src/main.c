// SPDX-License-Identifier: GPL-3.0-or-later
#include <3ds.h>

#include "app.h"
#include "auth.h"
#include "cmd.h"
#include "discover.h"
#include "fs.h"
#include "net.h"
#include "qr.h"
#include "ring.h"
#include "stats.h"
#include "sysinfo.h"
#include "titles.h"
#include "ui.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h> // snprintf
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// libctru's weak hook, run before main(). It stops a self-update with the lid closed from
// suspending at startup.
void userAppInit(void) {
	aptSetSleepAllowed(false);
}

static After after;

// libctru does not wrap APT:GetStartupArgument (0x00510080). RESTART is set after libctru's own
// self-chainload, never after a HOME close or a power cycle.
static bool startupWasRestart(void) {
	u8 parameter[0x300] = {0};
	u32 *staticBuffers = getThreadStaticBuffers();
	const u32 saved[2] = {staticBuffers[0], staticBuffers[1]};
	staticBuffers[0] = IPC_Desc_StaticBuffer(sizeof(parameter), 0);
	staticBuffers[1] = (u32)(uintptr_t)parameter;
	u32 cmdbuf[16] = {IPC_MakeHeader(0x51, 2, 0), sizeof(parameter), 1}; // type 1: RESTART
	Result result = aptSendCommand(cmdbuf);
	staticBuffers[0] = saved[0];
	staticBuffers[1] = saved[1];
	return R_SUCCEEDED(result) && (cmdbuf[2] & 0xFF) != 0;
}

// On a console that drops the deliver argument, a `run self` looks like a self-update.
static bool startupWasSelfUpdate(void) {
	char argument[sizeof(RUN_SELF_MARKER)] = "";
	bool received = false;
	u64 sender;
	return startupWasRestart() &&
		   !(R_SUCCEEDED(
				 APT_ReceiveDeliverArg(argument, sizeof(argument), NULL, &sender, &received)) &&
			 received && memcmp(argument, RUN_SELF_MARKER, sizeof(argument)) == 0);
}

typedef struct {
	u32 listenerAcceptErrors;
	// net.c's one buffer; it holds "unknown" until Wi-Fi associates. getIPText() rewrites it in
	// place and returns the same buffer, so a refresh reaches the screen at its next draw and there
	// is no copy to update.
	const char *ipText;
	bool regenChordLatched;
	u64 nextListenRetryMs;
	u64 listenRetries;
} Serve;

typedef enum {
	SERVE_INPUT_SERVE,
	SERVE_INPUT_NEXT_FRAME,
	SERVE_INPUT_QUIT,
} ServeInput;

// If the listener fails to open, the screen says so; unless failEvent is NULL, its errno is also
// logged under that event.
static bool listenerOpen(const char *failEvent) {
	gListener = bringNetworkUp();
	if (gListener >= 0) {
		appListenerUp();
		return true;
	}
	appListenerDown(errno);
	if (failEvent) {
		appendLog(failEvent, errno, 0); // a socket errno, logged as is
	}
	return false;
}

static void serveListenerRecovery(Serve *s) {
	if (gListener < 0 && authKeyLoaded() && uptimeMs() >= s->nextListenRetryMs) {
		s->nextListenRetryMs = uptimeMs() + 1000;
		s->listenRetries++;
		if (!listenerOpen(NULL)) {
			return;
		}
		// Attempts during this outage run one a second, so their count says how long the console
		// was unreachable.
		appendLog("listen-recovered", 0, s->listenRetries);
		s->listenRetries = 0;
		s->ipText = getIPText();
		drawScreen(s->ipText);
	}
}

static void serveTicks(void) {
	closeParked(false);
	ringPoll();
	appStatusTick();
	uiIdleAnimationTick();
}

// A self-update or a relaunch is due, as set in `after`. This closes the listener and says so on
// the screen; the caller then ends the loop.
static void leaveForAfter(void) {
	close(gListener);
	gListener = -1;
	static const UiNotice NOTICE[] = {[AFTER_RELAUNCH] = UI_RUN_SELF,
									  [AFTER_SELF_UPDATED] = UI_SELF_UPDATED,
									  [AFTER_LAUNCH] = UI_LAUNCH};
	uiNotice(NOTICE[after], NULL);
}

static ServeInput serveInput(Serve *s) {
	// Never exit past a stranded worker. A START that the last request took (net.h) is read every
	// turn, so while a worker is stranded it is dropped, as the loop's own press is.
	bool requestStart = idleStartTaken();
	hidScanInput();
	if (((hidKeysDown() & KEY_START) || requestStart) && !ringStranded()) {
		// START only dismisses an install failure because quitting would leave the console
		// unreachable.
		if (appStatus() == SERVER_INSTALL_FAILED) {
			appSetIdle();
			drawScreen(s->ipText);
			return SERVE_INPUT_NEXT_FRAME;
		}
		return SERVE_INPUT_QUIT;
	}

	// Test each key on its own, because KEY_UP covers D-pad up and circle-pad up, and a mask test
	// would demand both.
	u32 held = hidKeysHeld();
	if ((held & KEY_UP) && (held & KEY_SELECT) && (held & KEY_B)) {
		if (!s->regenChordLatched) {
			s->regenChordLatched = true;
			confirmAndRegenKey();
			discoverClose(); // the next tick answers to the new key's name
			drawScreen(s->ipText);
		}
	} else {
		s->regenChordLatched = false;
	}

	// X runs AM's media-wide cleanup of leftover imports (titles.c). Its outcome shows for a
	// second, so a press is never silent. A success also dismisses a failed card, as START does.
	if ((hidKeysDown() & KEY_X) && !ringStranded()) {
		Result r = amCleanup();
		if (R_SUCCEEDED(r)) {
			appSetIdle();
			uiNotice(UI_CLEANUP_DONE, NULL);
		} else {
			char code[12];
			snprintf(code, sizeof(code), "0x%08lX", (unsigned long)(u32)r);
			uiNotice(UI_CLEANUP_FAILED, code);
		}
	}

	// Y scans a QR code and installs what its links name. This runs on this thread, so only
	// serveBusy answers until it is over. Its outcome is shown the same way as an install from a
	// computer.
	if ((hidKeysDown() & KEY_Y) && !ringStranded()) {
		qrRun(&after);
		if (after != AFTER_NOTHING) {
			leaveForAfter();
			return SERVE_INPUT_QUIT;
		}
		uiCompletionTransition();
		return SERVE_INPUT_NEXT_FRAME;
	}
	return SERVE_INPUT_SERVE;
}

// The listener's lifecycle. listenerOpen brings it up, at boot and then once a second while it is
// down (serveListenerRecovery). serveAccept counts the accept errors that mean it died and takes it
// down (appListenerDown, discoverClose). app.c turns those two edges into LISTEN_FAILED and back.
// An idle accept also runs discovery and the keepalive retry, and a client is served to completion.
static bool serveAccept(Serve *s) {
	if (gListener >= 0) {
		int client = accept(gListener, NULL, NULL);
		if (client < 0) {
			if (listenerAcceptFatal(errno)) {
				if (++s->listenerAcceptErrors >= LISTENER_ACCEPT_FATAL_LIMIT) {
					appListenerDown(errno);
					appendLog("listen-dead", errno, s->listenerAcceptErrors);
					close(gListener);
					gListener = -1;
					s->listenerAcceptErrors = 0;
					discoverClose(); // a receive-only socket never learns that Wi-Fi went down
				}
			} else {
				// Only consecutive errors count, because stray ones must not add up to a rebuild.
				s->listenerAcceptErrors = 0;
				// Nothing is waiting, so no answer falls between a handshake and its request. A new
				// address reaches the screen at the idle tick, after any install card's hold.
				if (discoverTick()) {
					s->ipText = getIPText();
				}
				retryNetworkKeepalive(); // asks NDM only while no request waits
			}
		} else {
			s->listenerAcceptErrors = 0;
			// The last reply's parked socket is retired before this command, because a long command
			// runs no expiry tick and the socket would stay open through it.
			closeParked(true);
			if (!cmdServe(client, &after)) {
				appSetStatus(SERVER_AUTH_FAILED);
			}
			discoverDrain();
			if (after != AFTER_NOTHING) {
				leaveForAfter();
				return false;
			}
			uiCompletionTransition();
		}
	}
	return true;
}

// serveBusy accepts at most once a frame, whatever wakes it.
#define BUSY_ACCEPT_MS 16

// The QR flow (qrRun) holds the main thread, so each of its waits calls this, and so does every
// question it asks (ui.c's uiAsk, through the tick it is given). Each call does one non-blocking
// accept, at most once a frame, and serves the client in the busy mode (cmd.h). So at a screen,
// `stats` and `screenshot` show what the console is stuck on, and every other verb is refused
// `busy`. While a download runs nothing is served, because a socket call or a request served here
// can hold this thread past a short key press; a B press has been lost this way. Nothing the main
// loop does around a request happens here: there is no `after` (cmdServeBusy has none to give), no
// AUTH_FAILED (a wrong MAC would replace the status the flow runs on), and no discovery.
void serveBusy(void) {
	static u64 nextMs;
	u64 now = uptimeMs();
	if (gListener < 0 || qrDownloading() || now < nextMs) {
		return;
	}
	nextMs = now + BUSY_ACCEPT_MS;
	int client = accept(gListener, NULL, NULL);
	if (client < 0) {
		return; // no client waiting; the main loop finds a dead listener once the flow is over
	}
	// As serveAccept does, the last reply's socket is closed before this client is served.
	closeParked(true);
	// A screenshot resets the transfer's counters and stats, which belong to the flow while it
	// runs, so they are saved and put back. No thread writes them at a screen.
	const App flow = gApp;
	const InstallStats stats = gStats;
	cmdServeBusy(client, BUSY_AT_SCREEN);
	gApp = flow;
	gStats = stats;
}

// A service that fails to start leaves nothing to listen on. The log says which one, and the screen
// and, with the lid closed, the LED say so in red until START.
static int bootFatal(const char *event, Result result, const char *detail) {
	appendLog(event, result, 0);
	appStatusInit(false);
	appSetStatus(SERVER_FATAL);
	uiNotice(UI_FATAL, detail);
	appStatusShutdown(false);
	uiExit();
	gfxExit();
	return 1;
}

int main(void) {
	bool selfUpdateRestart = startupWasSelfUpdate();

	gfxInitDefault();
	// Done first, so even an early renderer failure is logged. A failure here is left to whatever
	// needs the directory, which can say what went wrong.
	fsMkdirs(fsArchive(), APP_PATH);
	sysinfoInit(); // the model, which picks the UI's width
	if (!uiInit(APP_VERSION)) {
		appendLog("ui-init-failed", 0, 0);
		gfxExit();
		return 1;
	}
	// PBKDF2 takes seconds, and without a frame the boot looks hung.
	uiNotice(UI_STARTUP, NULL);

	Result r = authInit();
	if (R_FAILED(r)) {
		return bootFatal("auth-init-failed", r, "Failed to start the pairing-key service.");
	}
	authLoadKey();
	if (R_FAILED(r = amInit())) {
		return bootFatal("am-init-failed", r,
						 "Failed to initialize AM.\nRun 3Drop as an installed CIA.");
	}

	gApp.installedTitleVersion = selfTitleVersion();
	titleDbStartup(); // before the listener exists

	if (R_FAILED(r = initNetwork())) {
		amExit();
		return bootFatal("soc-init-failed", r, "Failed to initialize network.");
	}

	initNetworkKeepalive();
	appStatusInit(selfUpdateRestart);

	Serve s = {0};
	s.ipText = getIPText();
	if (authKeyLoaded()) {
		listenerOpen("listen-failed"); // logged because the screen is not readable from the host
	} else {
		appSetStatus(SERVER_NEEDS_KEY);
	}

	drawScreen(s.ipText);

	while (appRunning()) {
		netWait(gListener);
		serveListenerRecovery(&s);
		serveTicks();
		ServeInput decision = serveInput(&s);
		if (decision == SERVE_INPUT_QUIT) {
			break;
		}
		if (decision == SERVE_INPUT_NEXT_FRAME) {
			continue;
		}
		if (!serveAccept(&s)) {
			break;
		}
	}

	if (gListener >= 0) {
		close(gListener);
	}
	discoverClose();
	closeParked(true);
	// Another title must not inherit 3Drop's LED, but a self-update's green LED belongs to the new
	// 3Drop.
	appStatusShutdown(after == AFTER_SELF_UPDATED);

	// Sleep stays refused for a chainload because, with the lid closed, APT would suspend before
	// the new process starts.
	unlockNetworkKeepalive();
	qrExit(); // shuts down libcurl and sslc before the sockets under them
	ndmuExit();
	exitNetwork();
	if (after == AFTER_NOTHING) {
		aptSetSleepAllowed(true);
	}
	amExit();
	authExit();
	uiExit();
	gfxExit();
	return 0;
}

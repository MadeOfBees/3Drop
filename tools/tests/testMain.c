// cc: src/app.c src/led.c src/ui.c src/auth.c src/codec.c src/sysinfo.c src/stats.c src/urls.c
// The real main.c, with the console's services and its network faked: a service that will not start
// is reported in the log and in red, and then the loop's own rules are tested, keys and requests
// included.
#include "fakes.h"
#include "ui.h" // declared before the renames below, so the real ones stay reachable

#include <citro2d.h>

#define APP_VERSION "test" // the real value comes from tools/build.sh, which no test runs

// What citro2d was asked to draw: the strings ui.c parsed since the frame began, a unit separator
// on each side of each, so a whole line can be asked for as well as a part of one.
#define SEP "\x1f"
static char frame[4096];
// A5 is drawn inside uiInit, where the loop's notice hook cannot see it, so its frame is kept when
// the next one begins.
static char o2dsFrame[sizeof(frame)];
bool C3D_FrameBegin(u8 flags) {
	if (strstr(frame, "Original 2DS detected")) {
		snprintf(o2dsFrame, sizeof(o2dsFrame), "%s", frame);
	}
	snprintf(frame, sizeof(frame), SEP);
	return true;
}
const char *C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *str) {
	text->width = 0.0f;
	text->str = str;
	if ((size_t)buf <= 256) { // ui.c's row-measuring buffer: not drawn
		return str + strlen(str);
	}
	size_t used = strlen(frame);
	snprintf(frame + used, sizeof(frame) - used, "%s" SEP, str);
	return str + strlen(str);
}
static bool drew(const char *shot, const char *part) {
	return strstr(shot, part) != NULL;
}
static bool drewLine(const char *shot, const char *whole) {
	char line[512];
	snprintf(line, sizeof(line), SEP "%s" SEP, whole);
	return drew(shot, line);
}

// What the loop shows when it leaves, how often it gives a card its transition, and whether the
// boot is a self-update. main.c calls these three by name, and each is seen here on its way to the
// real one. The key chord's prompt is counted but not run (testUi and testLed run it). A
// transition's own frame, and the status it leaves, are kept.
static int noticed = -1;
static char noticedDetail[64]; // its detail, "" for none
// A1, the first card the loop shows
static char startupFrame[sizeof(frame)];
static unsigned transitions;
static void noticeShown(UiNotice notice, const char *detail) {
	noticed = (int)notice;
	snprintf(noticedDetail, sizeof(noticedDetail), "%s", detail ? detail : "");
	uiNotice(notice, detail);
	if (notice == UI_STARTUP) {
		snprintf(startupFrame, sizeof(startupFrame), "%s", frame);
	}
}
static char card[sizeof(frame)];
static ServerStatus statusAfterCard;
static void transitionGiven(void) {
	transitions++;
	uiCompletionTransition();
	memcpy(card, frame, sizeof(card));
	statusAfterCard = gApp.status;
}
static int selfUpdated = -1; // appStatusInit's argument
static void statusInitSeen(bool updated) {
	selfUpdated = updated;
	appStatusInit(updated);
}
static unsigned regenAsks;
static void regenAsked(void) {
	regenAsks++;
}
#define uiNotice noticeShown
#define uiCompletionTransition transitionGiven
#define appStatusInit statusInitSeen
#define confirmAndRegenKey regenAsked
#define main threedropMain
#include "main.c"
#undef main
#undef confirmAndRegenKey
#undef appStatusInit
#undef uiNotice
#undef uiCompletionTransition

#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>

void gfxInitDefault(void) {}
void gfxExit(void) {}
// Each service starts, except the one whose Result a test sets.
static Result psResult, amResult, socResult;
Result psInit(void) {
	return psResult;
}
Result amInit(void) {
	return amResult;
}
void amExit(void) {}
int selfTitleVersion(void) {
	return 1;
}
static u8 model = CFG_MODEL_3DSXL;
Result CFGU_GetSystemModel(u8 *out) {
	*out = model;
	return 0;
}
void titleDbStartup(void) {}
static unsigned cleanups;
static Result cleanupResult = (Result)0xD8A08004; // AM refuses unless a test says otherwise
Result amCleanup(void) {
	cleanups++;
	return cleanupResult;
}
static bool stranded;
bool ringStranded(void) {
	return stranded;
}
Result initNetwork(void) {
	return socResult;
}
void exitNetwork(void) {}
// main.c opens the ndm:u session before any bring-up asks NDM for the lock, and closes it after.
static bool keepaliveOpen;
void initNetworkKeepalive(void) {
	keepaliveOpen = true;
}
void ndmuExit(void) {
	keepaliveOpen = false;
}
// While netFails lasts, a bring-up fails (EADDRINUSE). After that it listens on 127.0.0.1, where
// accept() returns EAGAIN, as it does for an idle listener.
static int netFails;
int bringNetworkUp(void) {
	assert(keepaliveOpen);
	if (netFails > 0) {
		netFails--;
		errno = EADDRINUSE;
		return -1;
	}
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	int s = socket(AF_INET, SOCK_STREAM, 0);
	assert(s >= 0 && bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(s, 1) == 0 &&
		   fcntl(s, F_SETFL, O_NONBLOCK) == 0);
	return s;
}
// Records the listen-* events that were logged, each as `<event>:<bytes>` and space-separated, and
// the Result of the last listen-failed.
static char listened[256];
static Result failedWith;
void appendLog(const char *event, Result result, u64 bytes) {
	size_t n = strlen(listened);
	if (strncmp(event, "listen-", strlen("listen-")) == 0) {
		snprintf(listened + n, sizeof(listened) - n, "%s%s:%llu", n ? " " : "", event,
				 (unsigned long long)bytes);
		if (strcmp(event, "listen-failed") == 0) {
			failedWith = result;
		}
	}
	fakeLog(event, result, bytes);
}
const char *getIPText(void) {
	return "192.0.2.50";
}
void netWait(int fd) {}
// net.c parks a reply's socket until the next client is accepted (closeParked(true)) or
// PARKED_CLOSE_MS has passed (the tick, closeParked(false)); cmdServe parks its client here.
static int parked = -1;
static u64 parkedAtMs;
void closeParked(bool now) {
	if (parked >= 0 && (now || uptimeMs() - parkedAtMs >= PARKED_CLOSE_MS)) {
		parked = -1;
	}
}
// main.c lets the lock go before ndmuExit closes the ndm:u session it goes through.
void unlockNetworkKeepalive(void) {
	assert(keepaliveOpen);
}
static unsigned keepaliveTries;
void retryNetworkKeepalive(void) {
	keepaliveTries++;
}
void ringPoll(void) {}
// The keys held on each turn of the loop, set by holds(): one mask per turn, with the last one
// staying in effect; if none is scripted, none is held.
static u32 heldScript[8], keysHeld;
static unsigned heldCount, heldTurn;
static void holds(unsigned count, ...) {
	va_list masks;
	va_start(masks, count);
	for (heldCount = 0; heldCount < count; heldCount++) {
		heldScript[heldCount] = va_arg(masks, u32);
	}
	va_end(masks);
	heldTurn = 0;
}
u32 hidKeysHeld(void) {
	return keysHeld;
}
static unsigned sleepAllowed; // aptSetSleepAllowed(true): 3Drop is leaving for good
void aptSetSleepAllowed(bool allowed) {
	sleepAllowed += allowed;
}
// The QR scan (qr.c) as a script: it counts its calls, and can end in 3Drop's own install or in a
// link's outcome. The outcome is filed the way oneLink files it: the transfer starts clean, the
// Operation is the outcome, and appInstallDone turns it into a status. One press, one scan.
static unsigned qrCalls;
static bool qrInstalledSelf, qrAsks;
static const Operation *qrOutcome;
static u32 qrAnswer;
void qrRun(After *after) {
	qrCalls++;
	if (qrAsks) { // a question, with the key that answers it already down, as the flow asks one
		fake.keysDown = KEY_A;
		qrAnswer = uiAsk(UI_QR_CONFIRM, "x", KEY_A | KEY_B, serveBusy);
	}
	if (qrInstalledSelf) {
		*after = AFTER_SELF_UPDATED;
	}
	if (qrOutcome) {
		appTransferStart();
		gApp.viaUrl = true;
		gApp.wireBytes = 4096;
		gApp.last = *qrOutcome;
		appInstallDone(&gApp.last);
		fake.keysDown = 0;
	}
}
static unsigned qrExits; // once per boot that reached the loop, however it ends
void qrExit(void) {
	qrExits++;
}
static bool downloadHolds; // a download thread runs, as qrDownloading says
bool qrDownloading(void) {
	return downloadHolds;
}
static u32 staticBuffers[2];
u32 *getThreadStaticBuffers(void) {
	return staticBuffers;
}
// What APT reports about the launch: an ordinary launch unless startupRestart is set (3Drop
// chainloaded itself), with startupDelivered, if any, as the argument it was given.
static bool startupRestart;
static const char *startupDelivered;
Result aptSendCommand(u32 *cmdbuf) {
	cmdbuf[2] = startupRestart;
	return 0;
}
Result APT_ReceiveDeliverArg(void *param, size_t size, void *hmac, u64 *sender, bool *received) {
	*received = startupDelivered != NULL;
	if (startupDelivered) {
		snprintf(param, size, "%s", startupDelivered);
	}
	return 0;
}

// The loop runs `turns` turns, each a step of turnMs on the clock. The fatal screen's wait runs
// while APT does, and the loop is never reached.
static unsigned turns;
static u64 turnMs;
bool aptMainLoop(void) {
	fake.nowMs += turnMs;
	keysHeld = heldCount ? heldScript[heldTurn < heldCount ? heldTurn : heldCount - 1] : 0;
	heldTurn++;
	return turns > 0 && turns--;
}
// accept() answers a boot's calls from acceptScript, one letter per call: C is a client, F fails as
// a dead listener's accept does; past its end, or on any other letter, it fails as an idle
// listener's accept does.
#define CLIENT 9
static const char *acceptScript;
int accept(int fd, struct sockaddr *address, socklen_t *length) {
	char next = acceptScript && *acceptScript ? *acceptScript++ : 'E';
	if (next == 'C') {
		return CLIENT;
	}
	errno = next == 'F' ? EBADF : EAGAIN;
	return -1;
}
// A request: cmdServe returns servedOk, as a MAC that verifies or not, and sets after to
// servedAfter. Only a client that accept() handed out reaches it, so a boot that failed served no
// request. It moves the clock on by servedMs, then parks its client as the reply does; openAtStart
// counts the requests that began with the last reply's socket still open.
static bool servedOk;
static After servedAfter;
static unsigned served, openAtStart;
static u64 servedMs;
// servedStart: the request's scan took a START on the idle screen, which net.c's idleStartTaken
// hands to the loop once.
static bool servedStart, startTaken;
bool idleStartTaken(void) {
	bool taken = startTaken;
	startTaken = false;
	return taken;
}
bool cmdServe(int client, After *after) {
	assert(client == CLIENT);
	served++;
	openAtStart += parked >= 0;
	*after = servedAfter;
	fake.nowMs += servedMs;
	parked = client;
	parkedAtMs = uptimeMs();
	startTaken = servedStart;
	return servedOk;
}
// A request served while the QR flow holds the thread (serveBusy) is counted, along with the state
// it was served in. It writes over the flow's counters and stats as a screenshot does, and the flow
// must find them as it left them.
static unsigned busyServed;
static Busy busyState;
void cmdServeBusy(int client, Busy busy) {
	assert(client == CLIENT);
	busyServed++;
	busyState = busy;
	openAtStart += parked >= 0;
	gApp.wireBytes += 288054;
	gApp.viaUrl = false;
	gApp.batchTotal = 0;
	statsReset("shot");
	parked = client;
	parkedAtMs = uptimeMs();
}

// The LED as the MCU takes it: the last lit color, and whether the last write was lit.
static u32 litRgb;
static bool lastLit;
Result MCUHWC_SetInfoLedPattern(const InfoLedPattern *pattern) {
	u32 rgb = (u32)pattern->redPattern[0] << 16 | (u32)pattern->greenPattern[0] << 8 |
			  pattern->bluePattern[0];
	litRgb = rgb ? rgb : litRgb;
	lastLit = rgb != 0;
	return 0;
}

// A service that will not start leaves nothing listening: one log line names it with its Result,
// and the screen and (with the lid closed) the LED show red until START; 3Drop then leaves the LED
// dark.
static void fatal(Result *fails, const char *event) {
	*fails = (Result)0xC8A0C7F8; // any failure
	fake.keysDown = KEY_START;
	fake.shell = 0; // the lid closed
	litRgb = 0;
	turns = 5;			 // the fatal screen's wait runs while APT does
	fake.nowMs += 60000; // a boot a minute after the last: app.c's statics outlive a test's process
	assert(threedropMain() == 1);
	assert(strcmp(fake.log, event) == 0 && fake.logResult == *fails);
	assert(litRgb == 0xFF0000 && !lastLit);
	*fails = 0;
	fake.keysDown = 0;
}

// A boot of `count` turns, with the fake clock a minute on and the key on the card.
static void boot(unsigned count) {
	FILE *key = fopen(KEY_PATH, "w");
	assert(key && fputs("abcd-efgh-2345\n", key) >= 0 && fclose(key) == 0);
	turns = count;
	listened[0] = '\0';
	fake.nowMs += 60000;
	assert(threedropMain() == 0);
}

// A boot of `turns` turns, with the clock stepping `turnMs` each turn, whose first `fails`
// bring-ups fail. `logged` is the listen-* events it makes, and `left` is the number of failures it
// did not use up.
typedef struct {
	int fails;
	unsigned turns;
	u64 turnMs;
	const char *logged;
	int left;
} Listening;

static void expectListened(const char *want) {
	if (strcmp(listened, want) != 0) {
		fprintf(stderr, "listen events: got '%s', want '%s'\n", listened, want);
		assert(!"the listener's events");
	}
}

// A listener that will not open at boot is logged with its errno, but a failed retry is not logged.
// A retry comes once a second, not once a frame, and the retry that opens the listener logs the
// attempts it took.
static void listener(Listening l) {
	gApp.status = SERVER_STARTING;
	netFails = l.fails;
	turnMs = l.turnMs;
	failedWith = 0;
	boot(l.turns);
	turnMs = 0;
	expectListened(l.logged);
	assert(netFails == l.left);
	assert(failedWith == (l.fails > 0 ? (Result)EADDRINUSE : 0));
}

// START never quits while a worker is stranded, because that would leave the console unreachable,
// and X does not run AM's cleanup under it either. With no stranded worker, START quits at once and
// X cleans up each turn.
static void strandedKeys(void) {
	FILE *key = fopen(KEY_PATH, "w");
	assert(key && fputs("abcd-efgh-2345\n", key) >= 0 && fclose(key) == 0);
	stranded = true;
	fake.keysDown = KEY_START | KEY_X;
	turns = 3;
	cleanups = 0;
	assert(threedropMain() == 0 && turns == 0 && cleanups == 0);
	stranded = false;
	turns = 3;
	assert(threedropMain() == 0 && turns == 2);
	fake.keysDown = KEY_X;
	turns = 2;
	noticed = -1;
	assert(threedropMain() == 0 && cleanups == 2);
	// X is never silent: AM's refusal shows with its code, a success shows as done (testUi holds
	// each up for a second).
	assert(noticed == UI_CLEANUP_FAILED && strcmp(noticedDetail, "0xD8A08004") == 0);
	cleanupResult = 0;
	gApp.status = SERVER_INSTALL_FAILED; // a failed card, which a cleanup dismisses
	turns = 1;
	assert(threedropMain() == 0 && cleanups == 3 && noticed == UI_CLEANUP_DONE);
	assert(noticedDetail[0] == '\0' && gApp.status == SERVER_READY);
	cleanupResult = (Result)0xD8A08004;
	fake.keysDown = 0;
}

// Y scans a QR code the way X cleans up: once per turn while it is held, and never while a worker
// is stranded (it may still hold the ring and an import). A scan that installed 3Drop itself ends
// the loop as a computer's self-install does, and the console stays awake for the chainload; any
// other scan ends where it began.
static void qrKey(void) {
	FILE *key = fopen(KEY_PATH, "w");
	assert(key && fputs("abcd-efgh-2345\n", key) >= 0 && fclose(key) == 0);
	fake.keysDown = KEY_Y;
	stranded = true;
	turns = 3;
	qrCalls = 0;
	assert(threedropMain() == 0 && turns == 0 && qrCalls == 0);
	stranded = false;
	turns = 3;
	transitions = 0;
	assert(threedropMain() == 0 && turns == 0 && qrCalls == 3);
	assert(transitions == 3); // each scan's result card gives way as a request's does
	// Not Y, not a scan: the other keys never start one.
	fake.keysDown = KEY_X | KEY_A | KEY_B;
	turns = 2;
	qrCalls = 0;
	assert(threedropMain() == 0 && qrCalls == 0);

	fake.keysDown = KEY_Y;
	qrInstalledSelf = true;
	after = AFTER_NOTHING;
	turns = 5;
	qrCalls = 0;
	sleepAllowed = 0;
	unsigned exits = qrExits;
	transitions = 0;
	noticed = -1;
	assert(threedropMain() == 0 && qrCalls == 1 && turns == 4); // the first turn was the last
	assert(after == AFTER_SELF_UPDATED && gListener == -1 && sleepAllowed == 0);
	assert(noticed == UI_SELF_UPDATED && transitions == 0); // the relaunch card stays to the jump
	assert(qrExits == exits + 1); // libcurl and sslc stop on the way out, a relaunch's too
	qrInstalledSelf = false;
	after = AFTER_NOTHING;
	fake.keysDown = 0;
	turns = 1;
	assert(threedropMain() == 0 && sleepAllowed == 1); // an ordinary exit lets it sleep again
	assert(qrExits == exits + 2);
}

// A boot with no key, whose first turn presses Y and the scan ends in `last`, followed by `count`
// turns of 600 ms. No key can be made, so nothing listens either.
static void keylessBoot(const Operation *last, unsigned count) {
	qrOutcome = last;
	fake.keysDown = KEY_Y;
	fake.nowMs += 60000;
	turns = count;
	turnMs = 600;
	assert(threedropMain() == 0);
	turnMs = 0;
}

#define SCANNED_TITLE 0x000400000F800100ULL
// The first screens draw before main.c asks AM for the title's version, so they show `?`, as the
// meta line's G1 says, and never `build 0`. These are A5 (an Original 2DS's notice, drawn inside
// uiInit) and A1. Run this first, because the version is cached for the rest of the process.
static void startupScreens(void) {
	model = CFG_MODEL_2DS;
	fake.psResult = (Result)0xC8A0C7F8; // no key can be made: a short boot
	remove(KEY_PATH);
	fake.keysDown = KEY_START;
	fake.nowMs += 60000;
	turns = 5;
	assert(threedropMain() == 0 && gApp.installedTitleVersion == 1);
	assert(drew(o2dsFrame, "build ?") && !drew(o2dsFrame, "build 0"));
	assert(drew(startupFrame, "Starting up") && drew(startupFrame, "build ?") &&
		   !drew(startupFrame, "build 0"));
	model = CFG_MODEL_3DSXL;
	fake.keysDown = 0;
}
// Y scans with no key and no listener (main.c guards it with the stranded worker alone). Each card
// a link install ends on, drawn by the real ui.c, says nothing about a listener, and the status
// after the installed card is the keyless one, not READY. Run this before the cases that listen,
// because auth.c's key and app.c's listener outlive a boot.
static void keylessScan(void) {
	static const struct {
		Operation last;
		const char *title, *guide, *keys;
	} CARDS[] = {
		{{OUTCOME_COMMITTED, "ok", 0, SCANNED_TITLE, 2},
		 "Install complete",
		 "Title installed and verified.",
		 "START  exit"},
		{{OUTCOME_REJECTED, "canceled", TRANSFER_CANCELED, SCANNED_TITLE, 2},
		 "Canceled",
		 "This CIA was not installed.",
		 "START  dismiss"},
		{{OUTCOME_UNKNOWN, "ticket", (Result)0xD8A08004, SCANNED_TITLE, 2},
		 "Canceled, ticket may remain",
		 "Delete Ticket.",
		 "START  dismiss"},
		{{OUTCOME_UNKNOWN, "am-cancel", (Result)0xC8A04566, SCANNED_TITLE, 2},
		 "Install unconfirmed",
		 "Look in HOME before you scan it again.",
		 "START  dismiss"},
		{{OUTCOME_REJECTED, "am-write", (Result)0xD8E0806A, SCANNED_TITLE, 2},
		 "Install failed",
		 "Check the link and the free space, then scan again.",
		 "START  dismiss"},
	};
	fake.psResult = (Result)0xC8A0C7F8; // no key can be made
	remove(KEY_PATH);
	assert(!authKeyLoaded() && gListener == -1);
	for (size_t i = 0; i < sizeof(CARDS) / sizeof(CARDS[0]); i++) {
		keylessBoot(&CARDS[i].last, 2);
		bool installed = CARDS[i].last.outcome == OUTCOME_COMMITTED;
		bool isCard = drewLine(card, CARDS[i].title) && drew(card, CARDS[i].guide) &&
					  drewLine(card, CARDS[i].keys);
		// No card promises what nothing here provides.
		bool promises = drew(card, "istening") || drew(card, "another sender");
		if (!isCard || promises) {
			fprintf(stderr, "keyless scan, `%s`: %s\n", CARDS[i].last.detail, card);
			assert(!"the card of a scan with no listener");
		}
		// A failed card stays for START. The installed one gives way to the status of a console
		// that never listened, and is held a second all the same: 600 ms on, it is the frame.
		assert(statusAfterCard == (installed ? SERVER_NEEDS_KEY : SERVER_INSTALL_FAILED));
		assert(gApp.status == statusAfterCard && strcmp(frame, card) == 0);
	}
	// A second later, the status screen takes the installed card's place, and it says there is no
	// key rather than that it listens.
	keylessBoot(&CARDS[0].last, 3);
	assert(drewLine(frame, "No pairing key") && !drew(frame, "Listening"));
	fake.psResult = 0;
	qrOutcome = NULL;
	fake.keysDown = 0;
}

// START over a failed install's card dismisses it and does not quit, which would leave the result
// unread; the press that finds the card gone quits.
static void startDismisses(void) {
	fake.keysDown = KEY_START;
	gApp.status = SERVER_INSTALL_FAILED;
	boot(3);
	assert(turns == 1);
	fake.keysDown = 0;
}

// The key chord (D-pad up, SELECT and B, all held) asks to regenerate the key once per hold, not
// once per frame; two of the three keys ask nothing.
static void regenChord(void) {
	static const u32 CHORD = KEY_UP | KEY_SELECT | KEY_B;
	regenAsks = 0;
	holds(4, CHORD, CHORD, 0u, CHORD);
	boot(4);
	assert(regenAsks == 2);
	regenAsks = 0;
	holds(3, KEY_UP | KEY_SELECT, KEY_UP | KEY_B, KEY_SELECT | KEY_B);
	boot(3);
	assert(regenAsks == 0);
	holds(0);
}

// A boot with one turn for each call of accept() that `script` answers.
static void accepts(const char *script) {
	acceptScript = script;
	gApp.status = SERVER_STARTING;
	boot((unsigned)strlen(script));
	acceptScript = NULL;
}

// If accept() fails as a dead listener's does LISTENER_ACCEPT_FATAL_LIMIT times in a row, the
// listener is closed and logged with the count; an idle listener's error or a client in between
// starts the count over.
static void deadListener(void) {
	enum { LIMIT = LISTENER_ACCEPT_FATAL_LIMIT };
	static const char FATALS[] = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"; // more than the limit
	char run[3 * LIMIT], dead[32];
	servedOk = true;
	snprintf(run, sizeof(run), "%.*s", LIMIT, FATALS);
	snprintf(dead, sizeof(dead), "listen-dead:%d", LIMIT);
	accepts(run);
	expectListened(dead);
	snprintf(run, sizeof(run), "%.*sE%.*s", LIMIT - 1, FATALS, LIMIT - 1, FATALS);
	accepts(run);
	expectListened("");
	snprintf(run, sizeof(run), "%.*sC%.*s", LIMIT - 1, FATALS, LIMIT - 1, FATALS);
	accepts(run);
	expectListened("");
}

// A request that fails its MAC puts the wrong-key message on the screen. One that leaves something
// to do ends the loop for it: the listener closes, the screen says what is next, and 3Drop leaves.
static void requests(void) {
	served = 0;
	servedOk = false;
	accepts("C");
	assert(served == 1 && gApp.status == SERVER_AUTH_FAILED);
	servedOk = true;
	accepts("C");
	assert(served == 2 && gApp.status == SERVER_READY);
	static const struct {
		After after;
		UiNotice notice;
	} LEAVES[] = {{AFTER_RELAUNCH, UI_RUN_SELF},
				  {AFTER_SELF_UPDATED, UI_SELF_UPDATED},
				  {AFTER_LAUNCH, UI_LAUNCH}};
	for (size_t i = 0; i < sizeof(LEAVES) / sizeof(LEAVES[0]); i++) {
		servedAfter = LEAVES[i].after;
		sleepAllowed = 0;
		transitions = 0;
		noticed = -1;
		unsigned exits = qrExits;
		accepts("CEEEE");
		assert(turns == 4 && noticed == (int)LEAVES[i].notice && transitions == 0);
		assert(gListener == -1 && sleepAllowed == 0 && qrExits == exits + 1);
		servedAfter = after = AFTER_NOTHING;
	}
}

// A START that a request took on the idle screen quits at the next turn, as the loop's own press
// does. While a worker is stranded the START is dropped, not kept, so once the worker is free
// nothing quits.
static void requestStart(void) {
	servedOk = true;
	servedStart = true;
	accepts("CEE");
	assert(turns == 1);
	stranded = true;
	accepts("CEE");
	assert(turns == 0);
	stranded = false;
	servedStart = false;
	accepts("EEE");
	assert(turns == 0);
}

// A reply's socket stays parked until the next client is accepted and closes then, before that
// client's command runs. A command that runs for a minute ticks nothing, so a socket left to the
// tick would stay open through it. It does not close sooner, because the reply's tail needs it:
// neither the end of its own command nor a turn with no client closes it.
static void parkedReply(void) {
	Serve s = {0};
	served = openAtStart = 0;
	servedOk = true;
	servedMs = 60000; // past PARKED_CLOSE_MS
	// A client, a turn with none, a client. accept() is the fake's, so any listener will do.
	gListener = 100;
	acceptScript = "CEC";
	assert(serveAccept(&s) && parked == CLIENT);
	assert(serveAccept(&s) && parked == CLIENT);
	assert(serveAccept(&s) && served == 2 && openAtStart == 0 && parked == CLIENT);
	gListener = -1;
	acceptScript = NULL;
	servedMs = 0;
}

// While the QR flow holds the thread, its waits call serveBusy, which does one accept per call and
// no more than one per frame. A client is served at the screen the flow is at, after the last
// reply's socket is closed, and what it wrote over the flow's counters and stats is put back. While
// a download runs, nothing is asked of the listener and the last reply's socket stays. With no
// listener, nobody connecting or a failing accept, nothing changes.
static void busyServing(void) {
	gListener = 100; // accept() is the fake's, so any listener will do
	busyServed = openAtStart = 0;
	parked = -1;
	fake.nowMs += 1000;
	gApp.wireBytes = 1234;
	gApp.viaUrl = true;
	gApp.batchTotal = 3;
	statsReset("qr");
	acceptScript = "CCCCCC";
	serveBusy();
	assert(busyServed == 1 && busyState == BUSY_AT_SCREEN && strlen(acceptScript) == 5);
	assert(gApp.wireBytes == 1234 && gApp.viaUrl && gApp.batchTotal == 3);
	assert(strcmp(gStats.request, "qr") == 0 && parked == CLIENT);
	serveBusy(); // the same frame: nothing is asked of the listener
	assert(busyServed == 1 && strlen(acceptScript) == 5);
	fake.nowMs += BUSY_ACCEPT_MS - 1;
	serveBusy();
	assert(busyServed == 1 && strlen(acceptScript) == 5);
	fake.nowMs += 1; // the next frame, with a download running: no accept, the socket stays
	downloadHolds = true;
	serveBusy();
	assert(busyServed == 1 && strlen(acceptScript) == 5 && parked == CLIENT);
	downloadHolds = false;
	fake.nowMs += BUSY_ACCEPT_MS;
	serveBusy(); // the download is over: the client that waited is served
	assert(busyServed == 2 && busyState == BUSY_AT_SCREEN && strlen(acceptScript) == 4);
	assert(openAtStart == 0); // the first client's socket was closed before the second's request
	fake.nowMs += BUSY_ACCEPT_MS;
	acceptScript = "F"; // a dead listener's error is the main loop's to count, after the flow
	serveBusy();
	fake.nowMs += BUSY_ACCEPT_MS;
	acceptScript = "E"; // nobody connecting
	serveBusy();
	fake.nowMs += BUSY_ACCEPT_MS;
	gListener = -1; // no listener
	acceptScript = "C";
	serveBusy();
	assert(busyServed == 2 && strlen(acceptScript) == 1);
	gListener = -1;
	acceptScript = NULL;
}

// A question's wait calls it too: a client that connects while ui.c holds one is answered there,
// and the key that answers the question still ends it.
static void busyAtQuestion(void) {
	busyServed = qrAnswer = 0;
	qrAsks = true;
	fake.keysDown = KEY_Y;
	acceptScript = "C";
	gApp.status = SERVER_STARTING;
	boot(3);
	acceptScript = NULL;
	qrAsks = false;
	fake.keysDown = 0;
	assert(busyServed == 1 && qrAnswer == KEY_A && busyState == BUSY_AT_SCREEN);
}

// A restart that was not given the `run self` marker is a self-update; no other launch is.
static void startupArguments(void) {
	static const struct {
		bool restart;
		const char *delivered;
		bool selfUpdate;
	} BOOTS[] = {{false, NULL, false},
				 {true, NULL, true},
				 {true, RUN_SELF_MARKER, false},
				 {true, "another argument", true},
				 {false, RUN_SELF_MARKER, false}};
	for (size_t i = 0; i < sizeof(BOOTS) / sizeof(BOOTS[0]); i++) {
		startupRestart = BOOTS[i].restart;
		startupDelivered = BOOTS[i].delivered;
		selfUpdated = -1;
		boot(1);
		assert(selfUpdated == BOOTS[i].selfUpdate);
	}
	startupRestart = false;
	startupDelivered = NULL;
}

int main(void) {
	fake.nowMs = 100000;
	fake.rendererFails = true; // logged too, though nothing can show it
	assert(threedropMain() == 1 && strcmp(fake.log, "ui-init-failed") == 0);
	fake.rendererFails = false;
	startupScreens();
	keylessScan();
	fatal(&psResult, "auth-init-failed");
	fatal(&amResult, "am-init-failed");
	fatal(&socResult, "soc-init-failed");
	listener((Listening){.turns = 1, .logged = ""});
	assert(keepaliveTries == 1); // the lock is asked for on the idle pass, which has no request
	static const Listening RUNS[] = {
		{.fails = 2, .turns = 1, .logged = "listen-failed:0"},
		{.fails = 1, .turns = 1, .logged = "listen-failed:0 listen-recovered:1"},
		// No second has passed: one retry of the two there could be.
		{.fails = 3, .turns = 3, .logged = "listen-failed:0", .left = 1},
		// A second a turn: the retry that opens it is the second, and says so.
		{.fails = 2, .turns = 2, .turnMs = 1000, .logged = "listen-failed:0 listen-recovered:2"},
	};
	for (size_t i = 0; i < sizeof(RUNS) / sizeof(RUNS[0]); i++) {
		listener(RUNS[i]);
	}
	strandedKeys();
	qrKey();
	startDismisses();
	regenChord();
	deadListener();
	requests();
	requestStart();
	parkedReply();
	busyServing();
	busyAtQuestion();
	startupArguments();
	TEST_PASS();
	return 0;
}

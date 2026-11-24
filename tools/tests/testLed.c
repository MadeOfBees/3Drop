// cc: src/codec.c src/transfer.c src/ring.c src/install.c src/titles.c src/fs.c src/path.c
// cc: src/hash.c src/stats.c src/protocol.c src/cmdFs.c src/sdXfer.c src/urls.c -lz
// cc: src/layeredFs.c
// Tests the LED as the MCU receives it, against mcu::HWC, srv and ptm:u fakes that follow libctru
// rather than led.c's own assumptions. The long waits run the real install, ls and save put on a
// fake clock. It also covers the on-screen dot that follows the LED, the stranded and
// listener-lost screens, and the key screens giving way after 30 s.
#include "fakes.h"

#include "app.c"
#include "led.c"
#include "ui.c"

#include "cia.h"
#include "cmdFs.h"
#include "install.h"
#include "sdXfer.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/param.h> // MAX
#include <sys/stat.h>
#include <unistd.h>

// srv's policy is per-thread and blocking by default, and mcu::HWC takes one session. While it is
// held, a non-blocking open answers 0xD0401834, and a blocking one (which is counted) would wait
// forever.
static bool nonBlocking, portFull;
static unsigned blockingOpens;
void srvSetBlockingPolicy(bool value) {
	nonBlocking = value;
}
// A counted session, as libctru's: a failed open rolls its count back.
static int mcuRefs;
static Result patternResult;
Result mcuHwcInit(void) {
	if (mcuRefs++ > 0) {
		return 0;
	}
	blockingOpens += !nonBlocking;
	if (portFull) {
		mcuRefs--;
		return (Result)0xD0401834;
	}
	return 0;
}
void mcuHwcExit(void) {
	assert(mcuRefs > 0); // below zero, every later init would answer 0 with no session
	mcuRefs--;
}
typedef struct {
	InfoLedPattern pattern;
	u64 atMs;
	Result result;
} Write;
static Write writes[4096];
static unsigned nWrites;
Result MCUHWC_SetInfoLedPattern(const InfoLedPattern *pattern) {
	assert(mcuRefs > 0 && !nonBlocking && nWrites < sizeof(writes) / sizeof(writes[0]));
	writes[nWrites++] = (Write){*pattern, fake.nowMs, patternResult};
	return patternResult;
}

static u32 sample(const InfoLedPattern *p, int i) {
	return (u32)p->redPattern[i] << 16 | (u32)p->greenPattern[i] << 8 | p->bluePattern[i];
}
static bool lit(const InfoLedPattern *p) {
	for (int i = 0; i < 32; i++) {
		if (sample(p, i) != 0) {
			return true;
		}
	}
	return false;
}
static const InfoLedPattern *lastTaken(unsigned since) {
	for (unsigned i = nWrites; i > since; i--) {
		if (writes[i - 1].result == 0) {
			return &writes[i - 1].pattern;
		}
	}
	return NULL;
}
static unsigned litWrites(unsigned since) {
	unsigned n = 0;
	for (unsigned i = since; i < nWrites; i++) {
		n += lit(&writes[i].pattern);
	}
	return n;
}

#define FRAME_MS 16
static void ticksFor(u64 ms) {
	for (u64 end = fake.nowMs + ms; fake.nowMs < end; fake.nowMs += FRAME_MS) {
		appStatusTick();
	}
}
static void launch(bool selfUpdated) {
	memset(&indicator, 0, sizeof(indicator));
	memset(&listener, 0, sizeof(listener)); // a new process has never listened
	gApp.status = SERVER_STARTING;
	appStatusInit(selfUpdated);
}

enum { OPEN = 1, CLOSED = 0 }; // PTMU_GetShellState's answer
#define POLL (STATUS_TICK_MS + FRAME_MS)

// Every lit pattern is the liveness canary: played once over the longest delay and ending dark,
// so it goes dark unless rewritten; smoothing 0 would be blink mode.
static void canary(const InfoLedPattern *p) {
	assert(p && p->delay == 0xFF && p->loopDelay == 0xFF && p->smoothing >= 1);
	assert(sample(p, 31) == 0);
}
static void patternShape(void) {
	fake.shell = CLOSED;
	appSetStatus(SERVER_READY);
	ticksFor(POLL);
	const InfoLedPattern *p = lastTaken(0);
	canary(p);
	for (int i = 0; i < 31; i++) {
		assert(sample(p, i) == ((u32)ledBreath(i) << 16 | ledBreath(i))); // purple at the level
	}
	appSetStatus(SERVER_INSTALL_FAILED);
	ticksFor(POLL);
	p = lastTaken(0);
	canary(p);
	for (int i = 0; i < 31; i++) {
		assert(sample(p, i) == 0xFF0000);
	}
	fake.shell = OPEN;
	ticksFor(POLL);
	p = lastTaken(0);
	assert(p && !lit(p));
}

// mcu::HWC opens non-blocking and the policy is back before any pattern IPC; a port another
// process holds costs no pattern IPC and no exit, and the next poll writes again.
static void nonBlockingOpen(void) {
	fake.shell = CLOSED;
	ticksFor(POLL);
	assert(lit(lastTaken(0)));
	unsigned from = nWrites;
	portFull = true;
	fake.shell = OPEN;
	ticksFor(2000);
	assert(nWrites == from && mcuRefs == 0);
	portFull = false;
	ticksFor(POLL);
	assert(nWrites == from + 1 && !lit(lastTaken(from)));
	assert(blockingOpens == 0 && !nonBlocking);
}

// Nothing lit with the lid open; closing lights it and opening clears it, each within one poll.
static void lidGate(void) {
	unsigned from = nWrites;
	fake.shell = OPEN;
	appSetStatus(SERVER_RECEIVING);
	ticksFor(10000);
	assert(litWrites(from) == 0);
	fake.shell = CLOSED;
	ticksFor(POLL);
	assert(lastTaken(from) && sample(lastTaken(from), 0) == 0x0000FF);
	from = nWrites;
	fake.shell = OPEN;
	ticksFor(POLL);
	assert(lastTaken(from) && !lit(lastTaken(from)));
	from = nWrites;
	ticksFor(10000);
	assert(nWrites == from);
}

// At most one write per STATUS_TICK_MS however the status and lid flap, and a lit pattern
// rewritten every LED_REFRESH_MS, well inside its lit life.
static void cadence(void) {
	unsigned from = nWrites;
	for (u64 t = 0; t < 60000; t += 20, fake.nowMs += 20) {
		if (t % 100 == 0) {
			appSetStatus(t / 100 % 2 ? SERVER_READY : SERVER_AUTH_FAILED);
		}
		fake.shell = t / 7000 % 2 ? OPEN : CLOSED;
		appStatusTick();
	}
	assert(nWrites > from + 10);
	for (unsigned i = from + 1; i < nWrites; i++) {
		assert(writes[i].atMs - writes[i - 1].atMs >= STATUS_TICK_MS);
	}
	fake.shell = CLOSED;
	appSetStatus(SERVER_INSTALLING);
	ticksFor(POLL);
	assert(sample(lastTaken(0), 0) == 0x0000FF);
	from = nWrites;
	ticksFor(60000);
	// 8 samples, from the MCU's timing (delay 0xFF: 15937.5 ms for all 32), not LED_SAMPLE_MS.
	const u64 refreshMs = 8 * 0xFF * 1000 / 16 / 32;
	assert(nWrites - from >= 60000 / (refreshMs + POLL));
	for (unsigned i = from + 1; i < nWrites; i++) {
		u64 gap = writes[i].atMs - writes[i - 1].atMs;
		assert(gap >= LED_REFRESH_MS && gap <= refreshMs + POLL);
	}
}

// A refused off is written again until it lands; a lid PTM will not report authorizes no write.
static void failures(void) {
	fake.shell = CLOSED;
	ticksFor(POLL);
	assert(lit(lastTaken(0)));
	patternResult = (Result)0xD8E0806A;
	fake.shell = OPEN;
	ticksFor(2000);
	assert(lit(lastTaken(0)) && !lit(&writes[nWrites - 1].pattern));
	patternResult = 0;
	ticksFor(POLL);
	assert(!lit(lastTaken(0)));
	fake.shell = CLOSED;
	ticksFor(POLL);
	fake.shellResult = (Result)0xD8E0806A;
	unsigned from = nWrites;
	appSetStatus(SERVER_INSTALL_FAILED);
	ticksFor(5000);
	assert(nWrites == from);
	fake.shellResult = 0;
	ticksFor(POLL);
	assert(sample(lastTaken(from), 0) == 0xFF0000);
}

// Idle, busy, success and failure never share a look, and every status lights the LED.
static void table(void) {
	static const Led WANT[] = {
		[SERVER_STARTING] = {0x0000FF, true}, // a relaunch's bring-up, not a failure
		[SERVER_NEEDS_KEY] = {0xFF0000, false},		[SERVER_READY] = {0xFF00FF, true},
		[SERVER_LISTEN_FAILED] = {0xFF0000, false}, [SERVER_AUTH_FAILED] = {0xFF0000, false},
		[SERVER_RECEIVING] = {0x0000FF, true},		[SERVER_INSTALLING] = {0x0000FF, true},
		[SERVER_INSTALLED] = {0x00FF00, false},		[SERVER_INSTALL_FAILED] = {0xFF0000, false},
		[SERVER_FILE] = {0x0000FF, true},			[SERVER_STRANDED] = {0x0000FF, true},
		[SERVER_FATAL] = {0xFF0000, false},
	};
	fake.shell = CLOSED;
	for (unsigned s = 0; s < SERVER_STATUS_COUNT; s++) {
		appSetStatus(SERVER_RECEIVING); // no outcome held
		appSetStatus((ServerStatus)s);
		ticksFor(POLL);
		const InfoLedPattern *p = lastTaken(0);
		assert(sample(p, 0) == WANT[s].rgb && (sample(p, 1) != sample(p, 0)) == WANT[s].breathes);
	}
}

// With the lid closed, an install's outcome is held past READY until a transfer or failure ends it.
static void hold(void) {
	appListenerUp(); // an install's card gives way to READY only on a console that has listened
	fake.shell = CLOSED;
	appSetStatus(SERVER_RECEIVING);
	ticksFor(POLL);
	appSetStatus(SERVER_INSTALLED);
	uiCompletionTransition();
	assert(gApp.status == SERVER_READY);
	ticksFor(60000);
	assert(sample(lastTaken(0), 0) == 0x00FF00);
	uiCompletionTransition(); // a request that leaves READY
	ticksFor(POLL);
	assert(sample(lastTaken(0), 0) == 0x00FF00);
	fake.shell = OPEN;
	ticksFor(POLL);
	assert(!lit(lastTaken(0)));
	fake.shell = CLOSED;
	ticksFor(POLL);
	assert(sample(lastTaken(0), 0) == 0xFF00FF); // opened once: READY's own color

	static const ServerStatus NEXT[] = {SERVER_RECEIVING, SERVER_FILE, SERVER_AUTH_FAILED};
	for (size_t i = 0; i < sizeof(NEXT) / sizeof(NEXT[0]); i++) {
		appSetStatus(SERVER_INSTALL_FAILED);
		appSetStatus(SERVER_READY); // START dismisses the card
		ticksFor(10000);
		assert(sample(lastTaken(0), 0) == 0xFF0000);
		appSetStatus(NEXT[i]);
		appSetStatus(SERVER_READY);
		ticksFor(POLL);
		assert(sample(lastTaken(0), 0) == 0xFF00FF);
	}
	// An outcome with the lid open holds nothing: closing it later shows READY.
	fake.shell = OPEN;
	ticksFor(POLL);
	appSetStatus(SERVER_INSTALLED);
	uiCompletionTransition();
	fake.shell = CLOSED;
	ticksFor(POLL);
	assert(sample(lastTaken(0), 0) == 0xFF00FF);
}

// Exit clears what this session lit; a lid-closed self-update leaves its green, written however
// recent the last write, for the relaunch to hold; a session that never lit writes nothing.
static void exits(void) {
	fake.shell = CLOSED;
	appSetStatus(SERVER_RECEIVING);
	appSetStatus(SERVER_READY);
	ticksFor(POLL);
	unsigned from = nWrites;
	appStatusShutdown(false);
	assert(nWrites == from + 1 && !lit(lastTaken(from)));

	launch(false);
	appSetStatus(SERVER_RECEIVING);
	ticksFor(POLL);
	appSetStatus(SERVER_INSTALLED);
	fake.nowMs += 10;
	from = nWrites;
	appStatusShutdown(true);
	assert(nWrites == from + 1 && writes[from].atMs == fake.nowMs);
	assert(sample(lastTaken(from), 0) == 0x00FF00);
	fake.nowMs += 3000; // the relaunch, whose first lid read PTM refuses
	fake.shellResult = (Result)0xD8E0806A;
	launch(true);
	from = nWrites;
	ticksFor(POLL);
	fake.shellResult = 0;
	ticksFor(POLL);
	assert(nWrites == from + 1 && sample(lastTaken(from), 0) == 0x00FF00); // not STARTING's blue
	appSetStatus(SERVER_READY);
	ticksFor(30000);
	assert(sample(lastTaken(0), 0) == 0x00FF00);
	fake.shell = OPEN;
	ticksFor(POLL);
	assert(!lit(lastTaken(0)));
	appStatusShutdown(false);

	fake.nowMs += 3000;
	launch(true); // the lid was opened during the relaunch
	from = nWrites;
	ticksFor(POLL);
	assert(nWrites == from + 1 && !lit(lastTaken(from)));
	appStatusShutdown(true);
	assert(nWrites == from + 1);

	fake.nowMs += 3000;
	launch(false);
	appSetStatus(SERVER_READY);
	ticksFor(10000);
	appStatusShutdown(false);
	assert(nWrites == from + 1);
}

// ---- The dot: the LED's twin on screen -------------------------------------------------

static u32 dotDrawn;
static unsigned frames;
bool C3D_FrameBegin(u8 flags) {
	dotDrawn = 0;
	frames++;
	return true;
}
void C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 color) {
	if (x == 388.0f && y == 12.0f) {
		dotDrawn = color;
	}
}
static u32 onScreen(Led led) {
	return RGB(led.rgb >> 16 & 0xFF, led.rgb >> 8 & 0xFF, led.rgb & 0xFF) & 0xFFFFFF;
}

// The dot is appLed()'s color for every status, lid open or closed, held or not.
static void dotColour(void) {
	for (unsigned s = 0; s < SERVER_STATUS_COUNT; s++) {
		for (int held = 0; held < 2; held++) {
			for (int closed = 0; closed < 2; closed++) {
				fake.shell = CLOSED;
				appSetStatus(SERVER_RECEIVING);
				ticksFor(POLL);
				if (held) {
					appSetStatus(SERVER_INSTALLED);
				}
				fake.shell = closed ? CLOSED : OPEN;
				appSetStatus((ServerStatus)s);
				ticksFor(POLL);
				drawScreen(NULL);
				assert(dotDrawn && (dotDrawn & 0xFFFFFF) == onScreen(appLed()));
				assert(!closed || sample(lastTaken(0), 0) == appLed().rgb);
			}
		}
	}
}

// The dot breathes the LED's levels at the LED's rate, and a steady status's dot is steady (the
// LED's own samples are patternShape's).
static void dotBreath(void) {
	fake.shell = CLOSED;
	appSetStatus(SERVER_READY);
	ticksFor(POLL);
	static const ServerStatus IDLE[] = {SERVER_READY, SERVER_STARTING};
	for (size_t i = 0; i < sizeof(IDLE) / sizeof(IDLE[0]); i++) {
		appSetStatus(IDLE[i]);
		fake.nowMs = (fake.nowMs / (4 * LED_SAMPLE_MS) + 1) * 4 * LED_SAMPLE_MS; // a breath begins
		drawScreen(NULL);
		for (unsigned k = 0; k < 8; k++, fake.nowMs += IDLE_TICK_MS) {
			nextIdleMs = fake.nowMs;
			uiIdleAnimationTick();
			assert(dotDrawn >> 24 == ledBreath(k / 2));
		}
	}
	// A request that changes nothing draws nothing, nor does the idle tick on a steady screen.
	appSetStatus(SERVER_READY);
	drawScreen(NULL);
	unsigned before = frames;
	fake.nowMs += LED_SAMPLE_MS;
	uiCompletionTransition();
	assert(frames == before);
	appSetStatus(SERVER_AUTH_FAILED);
	drawScreen(NULL);
	before = frames;
	for (unsigned k = 0; k < 8; k++, fake.nowMs += IDLE_TICK_MS) {
		nextIdleMs = fake.nowMs;
		uiIdleAnimationTick();
	}
	assert(frames == before);

	// Mid-transfer the breath alone draws no frame: it rides the frames progress draws anyway.
	appSetStatus(SERVER_RECEIVING);
	gApp.wireTotal = 1000;
	gApp.wireBytes = 0;
	uiProgressTick();
	before = frames;
	for (unsigned k = 0; k < 8; k++, fake.nowMs += IDLE_TICK_MS) {
		uiProgressTick();
	}
	assert(frames == before);
	gApp.wireBytes = 500;
	uiProgressTick();
	assert(frames == before + 1 && dotDrawn >> 24 == ledBreath(fake.nowMs / LED_SAMPLE_MS));

	appSetStatus(SERVER_INSTALL_FAILED);
	ticksFor(POLL);
	for (unsigned k = 0; k < 8; k++, fake.nowMs += IDLE_TICK_MS) {
		drawScreen(NULL);
		assert(dotDrawn >> 24 == 0xFF);
	}
}

// Over a notice the dot stays appLed()'s, never the notice's hue, and breathes while a prompt
// waits; before appStatusInit there is no dot and no LED write.
static void beforeInit(void) {
	fake.keysDown = KEY_START;
	uiNotice(UI_STARTUP, NULL);
	assert(dotDrawn == 0);
	uiNotice(UI_FATAL, "detail");
	assert(dotDrawn == 0 && nWrites == 0);
	fake.keysDown = 0;
}
static void dotNotices(void) {
	fake.shell = CLOSED;
	appSetStatus(SERVER_RECEIVING); // no outcome held: READY breathes
	appSetStatus(SERVER_READY);
	ticksFor(POLL);
	fake.keysDown = 0xFFFFFFFF; // the notices that wait for a key get one
	for (unsigned n = UI_STARTUP; n < UI_SCREEN_COUNT; n++) {
		uiNotice((UiNotice)n, NULL);
		assert((dotDrawn & 0xFFFFFF) == onScreen(appLed()));
	}
	fake.keysDown = 0;
	u32 first = dotDrawn;
	unsigned before = frames;
	(void)waitKeys(KEY_A, 2000 / FRAME_MS, NULL); // a prompt left 2 s: a breath, a frame per step
	assert(frames >= before + 3 && frames <= before + 5);
	assert((dotDrawn & 0xFFFFFF) == (first & 0xFFFFFF));
}

// ---- Coverage: the LED through every long main-thread wait ------------------------------

void gspWaitForVBlank(void) {
	fake.nowMs += FRAME_MS;
}
// Only the main thread's waits pass fake time: the worker's early call spins on the pump, so its
// sleeps would run the clock ahead whenever the main thread is off the CPU.
static pthread_t mainThread;
void svcSleepThread(s64 ns) {
	if (pthread_equal(pthread_self(), mainThread)) {
		__atomic_add_fetch(&fake.nowMs, (u64)ns / 1000000, __ATOMIC_SEQ_CST);
	} else {
		usleep(100);
	}
}

// A join takes its timeout of fake time and answers the svc timeout (0x09401BFE, which R_SUCCEEDs)
// while the worker runs; one AM has let go finishes inside the join, as on the console.
static pthread_t worker;
static volatile bool workerDone;
static ThreadFunc workerEntry;
static void *runWorker(void *arg) {
	workerEntry(arg);
	__atomic_store_n(&workerDone, true, __ATOMIC_SEQ_CST);
	return NULL;
}
Thread threadCreate(ThreadFunc entry, void *arg, size_t stack, int prio, int core, bool detach) {
	workerEntry = entry;
	workerDone = false;
	return pthread_create(&worker, NULL, runWorker, arg) == 0 ? (Thread)&worker : NULL;
}
static u64 amUntilMs;
Result threadJoin(Thread thread, u64 timeoutNs) {
	if (!__atomic_load_n(&workerDone, __ATOMIC_SEQ_CST)) {
		__atomic_add_fetch(&fake.nowMs, timeoutNs / 1000000, __ATOMIC_SEQ_CST);
		while (fake.nowMs >= amUntilMs && !__atomic_load_n(&workerDone, __ATOMIC_SEQ_CST)) {
			usleep(100);
		}
	}
	return workerDone ? pthread_join(worker, NULL) : (Result)0x09401BFE;
}
void threadFree(Thread thread) {}

// AM holds its first write until amUntilMs and commits in FINISH_MS, longer than the console takes.
#define AM_HANDLE 0x55 // fakes.c's AM_StartCiaInstall
#define FINISH_MS 5000
static u64 unitMs;
static bool committingShown; // what the screen said while AM committed
Result FSFILE_Write(Handle file, u32 *written, u64 offset, const void *buffer, u32 size,
					u32 flags) {
	if (file == AM_HANDLE) {
		while (__atomic_load_n(&fake.nowMs, __ATOMIC_SEQ_CST) < amUntilMs) {
			usleep(100);
		}
		*written = size;
		return 0;
	}
	fake.nowMs += unitMs;
	ssize_t n = pwrite((int)file - 1, buffer, size, (off_t)offset);
	*written = n > 0 ? (u32)n : 0;
	return n == (ssize_t)size ? 0 : (Result)0xC86044D2;
}
Result AM_FinishCiaInstall(Handle cia) {
	committingShown = shown.committing && strcmp(shown.title, "Finalizing") == 0;
	fake.nowMs += FINISH_MS;
	return 0;
}
Result FSUSER_ControlArchive(FS_Archive archive, u32 action, void *input, u32 inputSize,
							 void *output, u32 outputSize) {
	fake.nowMs += 250;
	return (Result)0xC8804478;
}

static u8 *body;
static size_t bodyLen, bodyAt;
ssize_t recv(int fd, void *buffer, size_t len, int flags) {
	size_t n = MIN(bodyLen - bodyAt, len);
	memcpy(buffer, body + bodyAt, n);
	bodyAt += n;
	return (ssize_t)n;
}
static void wire(size_t size) {
	free(body);
	body = calloc(1, size);
	bodyLen = size;
	bodyAt = 0;
}
bool ciaParse(const u8 *prefix, u64 prefixSize, u64 ciaSize, CiaInfo *out) {
	*out = (CiaInfo){.titleID = fake.titleID, .version = 1, .contentSize = ciaSize - prefixSize};
	return true;
}

static u64 longestUnwritten(u64 startMs) {
	u64 longest = 0, at = 0;
	for (unsigned i = 0; i < nWrites; i++) {
		if (writes[i].result == 0) {
			if (writes[i].atMs > startMs && at) {
				longest = MAX(longest, writes[i].atMs - at);
			}
			at = writes[i].atMs;
		}
	}
	return MAX(longest, fake.nowMs - at);
}

static u64 install(size_t size, u64 holdMs) {
	u8 prefixDigest[32];
	Operation op;
	wire(size);
	refDigest(body, 2048, prefixDigest);
	fake.installed = false;
	amUntilMs = fake.nowMs + holdMs;
	u64 start = fake.nowMs;
	appSetStatus(SERVER_RECEIVING);
	installCia(3, routeFind("new"), bodyLen, 2048, prefixDigest, &op);
	assert(op.outcome == OUTCOME_COMMITTED);
	appSetStatus(SERVER_INSTALLED);
	uiCompletionTransition();
	ticksFor(POLL);
	return fake.nowMs - start;
}

// A request that sets no status keeps the held outcome.
static void stillHeld(void) {
	ticksFor(POLL);
	assert(sample(lastTaken(0), 0) == 0x00FF00);
}

// With the lid closed, the LED is rewritten within every refresh through each long wait; AM's
// commit, one IPC, adds FINISH_MS.
static void ledThroughWaits(void) {
	fake.shell = CLOSED;
	appSetStatus(SERVER_READY);
	ticksFor(POLL);

	u64 start = fake.nowMs;
	assert(install(4 << 20, 30000) >= 30000 + FINISH_MS);
	assert(committingShown); // the commit is on screen while AM does it
	assert(longestUnwritten(start) <= LED_REFRESH_MS + POLL + FINISH_MS);
	assert(sample(lastTaken(0), 0) == 0x00FF00);
	start = fake.nowMs;
	assert(install(48 << 20, 30000) >= 30000 + FINISH_MS);
	assert(longestUnwritten(start) <= LED_REFRESH_MS + POLL + FINISH_MS);
	Operation op;

	mkdir("sdmc:/walk", 0755);
	for (int i = 0; i < 600; i++) {
		char name[32];
		snprintf(name, sizeof(name), "sdmc:/walk/%03d", i);
		fclose(fopen(name, "w"));
	}
	static const Verb LS = {"ls", 3, 4, false, NULL, NULL, NOT_BUSY};
	char *field[] = {"ls", "0123456789abcdef0123456789abcdef", "/walk", "t"};
	VerbContext ls = {.client = 3, .request = "ls", .id = field[1], .field = field, .n = 4};
	ls.verb = &LS;
	start = fake.nowMs;
	lsPath(&ls, "t");
	assert(fake.nowMs - start >= 120000); // the walk's deadline
	assert(longestUnwritten(start) <= LED_REFRESH_MS + POLL);
	stillHeld();
	static const Verb DELETE = {"delete", 4, 5, true, NULL, NULL, NOT_BUSY};
	char *delField[] = {"delete", field[1], "0", "/walk/000"};
	VerbContext del = {.client = 3, .request = "delete", .id = field[1], .field = delField, .n = 4};
	del.verb = &DELETE;
	assert(checkDeletePath(&del, false, false));
	runDeletePath(&del);
	stillHeld();

	Target save;
	mkdir("save:000400000f800100", 0755);
	assert(targetParse("save:000400000f800100/big.bin", &save));
	wire(24 << 20);
	SdPut put = {.size = bodyLen, .wireSize = bodyLen};
	refDigest(body, bodyLen, put.digest);
	unitMs = 600;
	start = fake.nowMs;
	archivePut(3, &save, &put, &op);
	assert(fake.nowMs - start >= 24 * unitMs);
	assert(longestUnwritten(start) <= LED_REFRESH_MS + unitMs); // a tick per write
	unitMs = 0;

	start = fake.nowMs;
	assert(waitKeys(KEY_A, 60000 / FRAME_MS, NULL) == 0);
	assert(longestUnwritten(start) <= LED_REFRESH_MS + POLL);
}

// Stranded: busy blue over the held red, no key hint, and the LED written all through the join.
static void stranded(void) {
	fake.shell = CLOSED;
	appSetStatus(SERVER_READY);
	ticksFor(POLL);
	u8 prefixDigest[32];
	wire(4 << 20);
	refDigest(body, 2048, prefixDigest);
	amUntilMs = UINT64_MAX;
	u64 start = fake.nowMs;
	Operation op;
	appSetStatus(SERVER_RECEIVING);
	installCia(3, routeFind("new"), bodyLen, 2048, prefixDigest, &op);
	appSetStatus(SERVER_INSTALL_FAILED); // runPutCia: the outcome was not a commit
	assert(strcmp(op.detail, "stranded") == 0 && ringStranded());
	assert(fake.nowMs - start >= 180000 && longestUnwritten(start) <= LED_REFRESH_MS + POLL);
	ticksFor(POLL);
	assert(appStatus() == SERVER_STRANDED && appLed().rgb == 0x0000FF && appLed().breathes);
	drawScreen(NULL);
	assert(shown.screen == SERVER_STRANDED && !shown.keys[0] && shown.dots);
	assert(strstr(shown.guide, "HOME is held"));
	assert((dotDrawn & 0xFFFFFF) == onScreen(appLed()));
	appListenerUp();
	appListenerDown(ENETDOWN); // unreachable outranks stranded
	assert(appStatus() == SERVER_LISTEN_FAILED && appLed().rgb == 0xFF0000);
	drawScreen(NULL);
	assert(shown.screen == SERVER_LISTEN_FAILED && !shown.keys[0] && shown.dots); // a wait
	appListenerUp();
	assert(appStatus() == SERVER_STRANDED);
	amUntilMs = 0;
	while (ringStranded()) {
		ringPoll();
	}
	ticksFor(POLL);
	assert(appStatus() == SERVER_INSTALL_FAILED && sample(lastTaken(0), 0) == 0xFF0000);
}

// The key as auth.c keeps it: a regen loads one.
static bool keyLoaded = true;
bool authKeyLoaded(void) {
	return keyLoaded;
}
static bool keysGoAfterRegen; // nobody answers the result screen
bool authRegenerateKey(void) {
	fake.keysDown = keysGoAfterRegen ? 0 : fake.keysDown;
	return keyLoaded = true;
}
static void regenerate(void) {
	fake.keysDown = KEY_A; // the confirmation, then any key past the result
	confirmAndRegenKey();
	fake.keysDown = 0;
}

// Nothing is served while a key screen waits. Unanswered, the prompt gives way as B, and the result
// screen does too, each within its 30 s (a wait without end trips the alarm).
static void unansweredKeyScreens(void) {
	keyLoaded = false;
	u64 t0 = fake.nowMs;
	alarm(10);
	confirmAndRegenKey();
	assert(!keyLoaded && fake.nowMs - t0 <= (UI_KEY_WAIT_FRAMES + 1) * FRAME_MS); // + its notice's
	keysGoAfterRegen = true;
	fake.keysDown = KEY_A;
	t0 = fake.nowMs;
	confirmAndRegenKey();
	alarm(0);
	assert(keyLoaded && fake.nowMs - t0 <= (UI_KEY_WAIT_FRAMES + 4) * FRAME_MS);
	keysGoAfterRegen = false;
}

// A console that never listened gives an install's card way to NEEDS_KEY (keyless) or STARTING,
// not READY. With the lid closed the outcome's color is held through them as through READY.
static void idleHold(void) {
	static const struct {
		bool key;
		ServerStatus idle;
	} CASES[] = {{false, SERVER_NEEDS_KEY}, {true, SERVER_STARTING}};
	for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
		launch(false);
		keyLoaded = CASES[i].key;
		fake.shell = CLOSED;
		appSetStatus(SERVER_RECEIVING);
		ticksFor(POLL);
		appSetStatus(SERVER_INSTALLED);
		uiCompletionTransition();
		assert(gApp.status == CASES[i].idle);
		ticksFor(60000);
		assert(sample(lastTaken(0), 0) == 0x00FF00);
		fake.shell = OPEN;
		ticksFor(POLL);
		assert(!lit(lastTaken(0)));
		fake.shell = CLOSED;
	}
	// A failed install's red, its card dismissed into STARTING's blue (NEEDS_KEY's own is red).
	appSetStatus(SERVER_RECEIVING);
	ticksFor(POLL); // the lid is seen closed again
	appSetStatus(SERVER_INSTALL_FAILED);
	appSetIdle(); // START dismisses the card
	assert(gApp.status == SERVER_STARTING);
	ticksFor(10000);
	assert(sample(lastTaken(0), 0) == 0xFF0000);
	keyLoaded = true;
	appListenerUp();
}

static void retriesFail(int err, u32 rgb) {
	unsigned from = nWrites;
	for (int retry = 0; retry < 120; retry++) { // one a second, as main.c retries
		appListenerDown(err);
		ticksFor(1000);
	}
	assert(nWrites > from);
	for (unsigned i = from; i < nWrites; i++) {
		assert(sample(&writes[i].pattern, 0) == rgb);
	}
}

// A lost listener is steady red on the LED and the dot, over the held outcome, and both come back
// with it; before the first listen a network coming up is busy blue however long, any other error
// red at once; X and a regen leave no READY to claim a listener that is not there.
static void lostListener(void) {
	launch(true); // a lid-closed self-update's relaunch holds its green
	fake.shell = CLOSED;
	appListenerDown(EADDRINUSE);
	ticksFor(POLL);
	assert(appStatus() == SERVER_LISTEN_FAILED && sample(lastTaken(0), 0) == 0xFF0000);
	appListenerDown(-ENETDOWN); // the boot path's sign
	ticksFor(POLL);
	assert(appStatus() == SERVER_STARTING && sample(lastTaken(0), 0) == 0x00FF00);

	launch(false);
	appListenerDown(-ENETDOWN);
	ticksFor(POLL);
	assert(appStatus() == SERVER_STARTING && sample(lastTaken(0), 0) == 0x0000FF);
	retriesFail(ENETDOWN, 0x0000FF);
	appListenerDown(EADDRINUSE);
	ticksFor(POLL);
	assert(appStatus() == SERVER_LISTEN_FAILED && sample(lastTaken(0), 0) == 0xFF0000);
	assert(sample(lastTaken(0), 1) == 0xFF0000);
	appListenerDown(-ENETDOWN);
	ticksFor(POLL);
	assert(appStatus() == SERVER_STARTING && sample(lastTaken(0), 0) == 0x0000FF);
	appSetIdle(); // X
	assert(gApp.status == SERVER_STARTING);
	keyLoaded = false;
	appSetStatus(SERVER_NEEDS_KEY);
	appSetIdle();
	assert(gApp.status == SERVER_NEEDS_KEY);
	regenerate();
	assert(keyLoaded && gApp.status == SERVER_STARTING);
	appListenerUp();
	ticksFor(POLL);
	assert(gApp.status == SERVER_READY && sample(lastTaken(0), 0) == 0xFF00FF);

	appSetStatus(SERVER_RECEIVING);
	appSetStatus(SERVER_INSTALLED);
	uiCompletionTransition();
	ticksFor(POLL);
	assert(sample(lastTaken(0), 0) == 0x00FF00);
	appListenerDown(ENETDOWN);
	unsigned from = nWrites;
	ticksFor(POLL);
	assert(nWrites == from + 1 && sample(lastTaken(from), 0) == 0xFF0000);
	assert(sample(lastTaken(from), 1) == 0xFF0000);
	holdUntilMs = 0;		 // the card's hold is over
	nextIdleMs = fake.nowMs; // the idle tick draws it: main.c needs no draw call
	uiIdleAnimationTick();
	assert(shown.screen == SERVER_LISTEN_FAILED && shown.keys[0]);
	assert((dotDrawn & 0xFFFFFF) == onScreen(appLed()));
	retriesFail(ENETDOWN, 0xFF0000);
	appListenerUp();
	ticksFor(POLL);
	assert(gApp.status == SERVER_READY && sample(lastTaken(0), 0) == 0x00FF00);

	appListenerDown(EBADF); // once it has listened, any errno
	assert(appStatus() == SERVER_LISTEN_FAILED);
	appListenerUp();
	static const ServerStatus UNDER[] = {SERVER_INSTALL_FAILED, SERVER_AUTH_FAILED};
	for (size_t i = 0; i < sizeof(UNDER) / sizeof(UNDER[0]); i++) {
		appSetStatus(UNDER[i]);
		appListenerDown(ENETDOWN);
		assert(appStatus() == SERVER_LISTEN_FAILED);
		appListenerUp();
		assert(appStatus() == UNDER[i]);
	}
	regenerate();
	assert(gApp.status == SERVER_READY);
}

int main(void) {
	mainThread = pthread_self();
	fake.stepMs = 0;
	fake.nowMs = 1000000;
	assert(uiInit("v"));
	beforeInit();
	launch(false);
	patternShape();
	nonBlockingOpen();
	lidGate();
	cadence();
	failures();
	table();
	hold();
	ledThroughWaits();
	stranded();
	lostListener();
	unansweredKeyScreens();
	dotColour();
	dotBreath();
	dotNotices();
	idleHold();
	exits();
	assert(mcuRefs == 0 && blockingOpens == 0);
	TEST_PASS();
	return 0;
}

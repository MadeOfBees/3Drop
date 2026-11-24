// cc: src/hash.c src/titles.c
// cc: src/certs.c src/urls.c src/codec.c src/fs.c
// cc: -lmbedx509
// Tests src/qr.c, the Y key's flow, against a scripted camera, screen, download thread, ring
// and install. They cover what a scan shows and refuses, the confirm screen, the order a link
// goes through (policy, the certificate list, the download's two stops, the install and its
// outcome), the three answers to the failed-check prompt, a batch, B at every point, and
// 3Drop's own install leaving as a computer's does. The download thread is a coroutine that
// svcSleepThread steps, so the script decides every interleaving. `testQrStrict` builds the
// same file with plain http turned off.
#include "fakes.h"

#include "caRoots.h"	   // PEM_OK's two real certificates
#define APP_VERSION "test" // the real one comes from tools/build.sh, which no test runs
#include "ring.h"
void stopCounted(void);
#define ringStop stopCounted // qr.c's calls are counted; ring.c below keeps the real name
#include "qr.c"
#undef ringStop

#include <assert.h>
#include <curl/curl.h> // CURLE_*: the codes a download's refusal carries
#include <mbedtls/x509.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

// The drain regression uses the real ring and installClose, not the scripted commit below.
u64 drainDone(void);
void drainRelease(void);
#define ringArm drainArm
#define ringDone drainDone
#define ringRelease drainRelease
#define ringStranded drainStranded
#include "ring.c"
#define installOpen drainOpen
#define installClose drainClose
#include "install.c"
#undef installClose
#undef installOpen
#undef ringStranded
#undef ringRelease
#undef ringDone
#undef ringArm
void drainStatsReset(const char *request);
#define statsReset drainStatsReset
#define statsFinish drainStatsFinish
#include "stats.c"
#undef statsFinish
#undef statsReset

// A body of 9 units and a few bytes: more than two of the writer's calls (WRITE_UNITS each).
#define DRAIN_BODY ((2 * WRITE_UNITS + 1) * (u64)RING_UNIT + 4096)
static bool realDrain, joinPower, closing, keyAtJoin;
// joinsPending: timed joins that time out before the real one; the first presses the key.
static unsigned joinsPending, pendingJoins, amFinishes, writeCalls, ringStops;
static u64 writtenBytes; // the bytes of the AM write calls that returned
void stopCounted(void) {
	ringStops++;
	ringStop();
}
// A live import blocks HOME, so a POWER press only reaches the app as APT's close request.
bool aptShouldClose(void) {
	return closing;
}
Result threadJoin(Thread thread, u64 timeoutNs) {
	assert(realDrain && timeoutNs == 250000000ULL);
	if (joinsPending) {
		assert(downloadStage(&dl) == DL_FINISHED && dl.ok && ring.thread == thread);
		// The key comes while the writer is inside its first AM call (the card is held), so what it
		// does next is the one thing left to decide.
		for (unsigned spin = 0; spin < 200000 && !__atomic_load_n(&writeCalls, __ATOMIC_SEQ_CST);
			 spin++) {
			usleep(100);
		}
		assert(__atomic_load_n(&writeCalls, __ATOMIC_SEQ_CST) == 1);
		if (pendingJoins++ == 0) {
			closing = joinPower; // stays true at every look after
			fake.keysDown = joinPower ? 0 : KEY_B;
		}
		joinsPending--;
		return 0x09401BFE;
	}
	fake.cardHeld = false;
	return pthread_join(*(pthread_t *)thread, NULL);
}
Result FSFILE_Write(Handle file, u32 *written, u64 offset, const void *buffer, u32 size,
					u32 flags) {
	assert(realDrain && file == 0x55);
	__atomic_add_fetch(&writeCalls, 1, __ATOMIC_SEQ_CST);
	while (fake.cardHeld) {
		usleep(100);
	}
	static u64 next; // the writer is the only caller, and an import's calls run in order
	assert(offset == 0 || offset == next);
	next = offset + size;
	__atomic_add_fetch(&writtenBytes, size, __ATOMIC_SEQ_CST);
	*written = size;
	return 0;
}
Result AM_FinishCiaInstall(Handle cia) {
	assert(realDrain && cia == 0x55);
	amFinishes++;
	return 0;
}

#define PEM_OK "## Bundle\n" CA_ROOTS
#define PEM_OLD "## Old\n-----BEGIN CERTIFICATE-----\nOLD1\n-----END CERTIFICATE-----\n"
#define CIA_SIZE 40000u
#define CIA_PREFIX 3000u
#define TITLE 0x0004000000ABC100ULL

// ---- The camera ----
static struct {
	Result openResult, failure;
	ScanStep steps[8];
	unsigned stepCount, at, polls, opens, closes, pressBAt;
	u32 pressedKey;
	const char *payload;
} cam;
static u16 frameMark[4];
Result scanOpen(void) {
	cam.opens += cam.openResult == 0; // a failed open has closed itself
	return cam.openResult;
}
void scanClose(void) {
	cam.closes++;
}
ScanStep scanPoll(u64 waitNs) {
	cam.polls++;
	assert(cam.polls < 200);					 // a scan that nothing ends is a hang, not a pass
	assert(waitNs > 0 && waitNs <= 33000000ULL); // never longer than a frame
	if (cam.pressBAt && cam.polls == cam.pressBAt) {
		fake.keysDown = cam.pressedKey ? cam.pressedKey : KEY_B;
	}
	return cam.at < cam.stepCount ? cam.steps[cam.at++] : SCAN_IDLE;
}
const u16 *scanFrame(void) {
	return frameMark;
}
const u8 *scanPayload(size_t *length) {
	*length = strlen(cam.payload);
	return (const u8 *)cam.payload;
}
Result scanResult(void) {
	return cam.failure;
}
// A key is down for one read, as libctru's hidKeysDown is for one scan.
u32 hidKeysDown(void) {
	u32 keys = fake.keysDown;
	fake.keysDown = 0;
	return keys;
}

// ---- The screen ----
typedef struct {
	UiNotice screen;
	u32 mask;
	char text[600];
	void (*tick)(void);
} Asked;
static struct {
	Asked asked[16];
	unsigned askedCount;
	u32 answers[16];
	unsigned answerCount, answerAt;
	UiNotice notices[16];
	unsigned noticeCount, scanOpens, scanCloses, frames, progressTicks;
	const u16 *lastFrame;
	bool picture;	// uiScanOpen's answer
	bool sawDone;	// a progress tick that read the ring's count
	bool sawPrefix; // a progress tick that showed the prefix's bytes
} screen;
static void answer(u32 key) {
	screen.answers[screen.answerCount++] = key;
}
u32 uiAsk(UiNotice which, const char *detail, u32 mask, void (*tick)(void)) {
	assert(screen.askedCount < 16 && screen.answerAt < screen.answerCount); // no unplanned prompt
	assert(tick == serveBusy); // every question lets a client in, as the scanner and the waits do
	Asked *a = &screen.asked[screen.askedCount++];
	a->screen = which;
	a->mask = mask;
	a->tick = tick;
	snprintf(a->text, sizeof(a->text), "%s", detail ? detail : "");
	u32 key = screen.answers[screen.answerAt++];
	assert(key == 0 || (key & mask) != 0); // only a key the screen offers (0: the app is closing)
	return key;
}
void uiNotice(UiNotice which, const char *detail) {
	(void)detail;
	assert(screen.noticeCount < 16);
	screen.notices[screen.noticeCount++] = which;
}
bool uiScanOpen(void) {
	screen.scanOpens++;
	return screen.picture;
}
void uiScanClose(void) {
	screen.scanCloses++;
}
void uiScanFrame(const u16 *frame) {
	screen.frames++;
	screen.lastFrame = frame;
}
void uiProgressTick(void) {
	screen.progressTicks++;
	screen.sawDone |= gApp.uiWrittenBytes == 1234;	  // ringDone's value, as progress() reads it
	screen.sawPrefix |= gApp.wireBytes == CIA_PREFIX; // the bytes in when the import opened
}
// This stands in for main.c's serveBusy, which answers a client. Here each call is counted, along
// with whether a download thread was running at the time. stopRequested makes one call for each of
// the LED's ticks below, so the two counts agree.
static unsigned busyTicks, busyTicksDownloading;
void serveBusy(void) {
	busyTicks++;
	busyTicksDownloading += qrDownloading();
}
// The LED's keep-alive and the status, which a test reads for what the person would see.
static unsigned statusTicks, statusCount;
static ServerStatus statuses[8];
void appStatusTick(void) {
	statusTicks++;
}
void appSetStatus(ServerStatus status) {
	assert(statusCount < 8);
	statuses[statusCount++] = status;
	gApp.status = status;
}
static bool noticed(UiNotice which) {
	for (unsigned i = 0; i < screen.noticeCount; i++) {
		if (screen.notices[i] == which) {
			return true;
		}
	}
	return false;
}

// ---- The ring, stats and the install ----
static unsigned installCloses; // counted below, read by ringStranded
static struct {
	bool armOk, stranded, strandedAfterFirst;
	unsigned arms, releases;
	u64 armTotal, done;
} scripted;
bool ringArm(u64 total, RingDir dir, bool overflow, u64 offset) {
	assert(dir == RING_TO_HANDLE && overflow && offset == 0);
	scripted.arms++;
	scripted.armTotal = total;
	return realDrain ? drainArm(total, dir, overflow, offset) : scripted.armOk;
}
void ringRelease(void) {
	scripted.releases++;
	if (realDrain) {
		drainRelease();
	}
}
bool ringStranded(void) {
	return scripted.stranded || (scripted.strandedAfterFirst && installCloses >= 1);
}
u64 ringDone(void) {
	return realDrain ? drainDone() : scripted.done;
}
static struct {
	unsigned resets, finishes;
	char request[16];
} stats;
void statsReset(const char *request) {
	stats.resets++;
	snprintf(stats.request, sizeof(stats.request), "%s", request);
}
void statsFinish(void) {
	stats.finishes++;
}

static const Route R_NEW = {"new", false, false}, R_UPDATE = {"update", false, true},
				   R_SELF = {"self", true, true};
typedef struct {
	// What the caller must have set before downloadStart.
	DlSink sink;
	const char *url;
	bool verify;
	// Where its CAs come from: the bundle shipped in the app, in memory, or (not set) the card's
	// file.
	bool shipped;
	// The thread: size 0 ends it before the headers, prefix 0 before the prefix; detail is its
	// refusal at the end (NULL: it fetched every byte).
	u64 size, prefix;
	const char *detail;
	Result why;
	long http, verifyResult;
	DlTls tls;
	const char *file; // a file sink's body
	// Main's install at the prefix.
	bool opens, started, commitUnknown;
	bool noThread; // downloadStart cannot make the thread
	const char *refusal;
	Result refusalWhy;
	const Route *route;
	int version;
	// A key goes down (or the power button, which closes the app) at this step of the thread (0:
	// none). The main thread sees it at its next look, and the thread's next step is the
	// cancellation.
	unsigned keyAfter;
	u32 key;
	bool power;
	// The tail the real worker has: its transfer returned (ok with every byte in, or with its
	// refusal marked) but DL_FINISHED is published a step later, so a B that main reads in
	// between is the key edge the worker never sees.
	bool slowFinish;
} Course;
static Course courses[8];
static unsigned courseCount, coursesRun, pumps, installOpens, downloadInits, downloadExits, cancels,
	joins;
static Result initResult;
static const Course *current;
static Download *active;
// a slowFinish course's tail: the bytes are in, but the state is not yet DL_FINISHED
static bool performDone;
static struct {
	bool received;
	const char *detail;
	Result why;
} closed;

// A course for the next download. A CIA's `verify` is its own flag. A list is never fetched
// unchecked, so for a list `verify` says what it is checked against: true means the card's
// list, false means the bundle shipped in the app.
static Course *add(DlSink sink, const char *url, bool verify) {
	assert(courseCount < 8);
	Course *c = &courses[courseCount++];
	*c = (Course){.sink = sink,
				  .url = url,
				  .verify = verify || sink == DL_SINK_FILE,
				  .shipped = sink == DL_SINK_FILE && !verify};
	if (sink == DL_SINK_CIA) {
		c->size = CIA_SIZE;
		c->prefix = CIA_PREFIX;
		c->opens = c->started = true;
		c->route = &R_NEW;
		c->version = 3;
	} else {
		c->size = 1;
		c->file = PEM_OK;
	}
	return c;
}

Result downloadInit(void) {
	downloadInits++;
	return initResult;
}
void downloadExit(void) {
	downloadExits++;
}
bool downloadTlsUp(void) { // the real one's rule: up when the init the flow made succeeded
	return downloadInits > 0 && initResult == 0;
}
// libcurl's URL parser belongs to the real library, which no host test links (testDownload
// scripts its URL API). These stand in for it, working from what the test links name: the host
// after any login, up to the path, with a port kept. A site that does not fit is refused, as the
// real one refuses it.
static unsigned siteCalls, pathCalls;
bool downloadSite(const char *url, char *out, size_t cap) {
	siteCalls++;
	const char *host = strstr(url, "://") + 3; // urlsParse gave every link a scheme
	size_t length = strcspn(host, "/?#");	   // where libcurl ends the authority
	const char *login = memchr(host, '@', length);
	if (login) {
		length -= (size_t)(login + 1 - host);
		host = login + 1;
	}
	if (strstr(url, "unreadable") || length == 0 || length >= cap) {
		return false;
	}
	memcpy(out, host, length);
	out[length] = '\0';
	return true;
}
// This one gives the link's path as written, up to a query or a fragment ("/" when the link
// names none), and whether a query follows it. That is what libcurl answers for the links these
// tests hold, which have no dot segments to resolve (testDownload scripts those). A link with
// "badpath" in it is one whose path it cannot read.
bool downloadPath(const char *url, char *out, size_t cap, bool *query) {
	pathCalls++;
	const char *rest = strstr(url, "://") + 3;
	rest += strcspn(rest, "/?#"); // past the login and the host
	bool slash = *rest == '/';
	size_t length = slash ? strcspn(rest, "?#") : 1;
	*query = (slash ? rest[length] : *rest) == '?';
	if (strstr(url, "badpath") || length >= cap) {
		*query = false;
		return false;
	}
	memcpy(out, slash ? rest : "/", length);
	out[length] = '\0';
	return true;
}
bool downloadStart(Download *d) {
	assert(coursesRun < courseCount); // no unplanned download
	current = &courses[coursesRun++];
	assert(d->sink == current->sink && strcmp(d->url, current->url) == 0);
	assert(d->verify == current->verify);
	// The bundle in memory is the app's own bytes, whole, and nothing else is ever handed over
	// that way.
	assert((d->caBlob != NULL) == current->shipped);
	if (current->shipped) {
		size_t length;
		assert(d->caBlob == certsShipped(&length) && d->caBlobLen == length && length > 0);
	}
	assert(strcmp(d->userAgent, "3Drop/test") == 0);
	assert(d->sink == DL_SINK_CIA || strcmp(d->path, CACERT_TEMP) == 0);
	// The real one resets everything on the thread's side, including the last download's cancel,
	// because qr.c hands it the same Download again.
	memset(&d->state, 0, sizeof(*d) - offsetof(Download, state));
	if (current->noThread) { // the real one's account of it
		d->detail = "thread";
		d->why = DOWNLOAD_NO_THREAD;
		d->state = DL_FINISHED;
		return false;
	}
	active = d;
	pumps = 0;
	performDone = false;
	return true;
}
void downloadAnswer(Download *d, bool go) { // the real one's rule: only a waiting stop is answered
	int state = d->state, stage = state & DL_STAGE_MASK;
	if ((stage == DL_SIZED || stage == DL_PREFIXED) && (state >> 8) == DL_WAITING) {
		d->state = stage | (go ? DL_GO : DL_STOP) << 8;
	}
}
void downloadCancel(Download *d) {
	cancels++;
	d->cancel = true;
}
void downloadJoin(Download *d) {
	assert(d == active && downloadStage(d) == DL_FINISHED); // joined once, after the end
	joins++;
	active = NULL;
	if (keyAtJoin) { // after the download's last look, before the close's first
		fake.keysDown = KEY_B;
	}
}
static void end(Download *d, const char *detail, Result why) {
	d->detail = detail;
	d->why = why;
	d->ok = detail == NULL;
	if (detail == NULL || strcmp(detail, "canceled") != 0) {
		d->http = current->http;
		d->verifyResult = current->verifyResult;
		d->tls = current->tls;
	}
	if (d->sink == DL_SINK_FILE && detail) {
		remove(d->path); // half a file is not a bundle
	}
	d->state = DL_FINISHED;
}
static void endCourse(Download *d) {
	end(d, current->detail, current->why);
}
// One step of the thread, taken when the main thread sleeps.
static unsigned sleeps;
void svcSleepThread(s64 ns) {
	assert(++sleeps < 400); // a thread nobody answers waits forever, and a hang is a failure here
	Download *d = active;
	// serveBusy serves nothing while a download thread runs, whether for the list or for a link.
	assert(!d || qrDownloading());
	if (!d || downloadStage(d) == DL_FINISHED) {
		return;
	}
	pumps++;
	if (current->keyAfter && pumps == current->keyAfter) {
		fake.keysDown = current->power ? 0 : current->key ? current->key : KEY_B;
		fake.powerPressed = current->power;
	}
	if (d->cancel && !performDone) { // a worker whose transfer has returned does not look again
		end(d, "canceled", TRANSFER_CANCELED);
		return;
	}
	if (performDone) { // the tail's stores, a step after the transfer returned
		endCourse(d);
		return;
	}
	int state = d->state, stage = state & DL_STAGE_MASK, given = state >> 8;
	if (stage == DL_RUNNING && d->sink == DL_SINK_FILE) { // a file: connected, then its bytes
		FILE *f = fopen(d->path, "wb");
		assert(f && fputs(current->file, f) >= 0 && fclose(f) == 0);
		d->received = strlen(current->file);
		if (current->slowFinish) {
			performDone = true;
		} else {
			endCourse(d);
		}
	} else if (stage == DL_RUNNING) { // a CIA's body, in one step
		if (current->slowFinish) {
			d->received = d->size;
			performDone = true;
		} else {
			endCourse(d);
		}
	} else if (stage == DL_STARTING && d->sink == DL_SINK_FILE) {
		d->state = DL_RUNNING;
	} else if (stage == DL_STARTING) {
		if (current->size == 0) {
			endCourse(d);
		} else {
			d->size = current->size;
			d->state = DL_SIZED;
		}
	} else if (stage == DL_SIZED && given != DL_WAITING) {
		if (given == DL_STOP) {
			end(d, "stopped", 0);
		} else if (current->prefix == 0) {
			endCourse(d);
		} else {
			d->prefixSize = d->received = current->prefix;
			d->state = DL_PREFIXED;
		}
	} else if (stage == DL_PREFIXED && given != DL_WAITING) {
		if (given == DL_STOP) {
			end(d, "stopped", 0);
		} else {
			d->received = current->detail ? current->prefix + 100 : d->size;
			d->state = DL_RUNNING;
		}
	}
}
bool installOpen(InstallRun *run, u64 ciaSize, u64 prefixSize, Operation *op) {
	installOpens++;
	assert(ciaSize == current->size && prefixSize == current->prefix);
	assert(scripted.arms >= 1 && scripted.armTotal == ciaSize); // armed for Content-Length
	assert(run->route == NULL);		 // the console picks the route and no link names one
	assert(run->gap.reference != 0); // the prefix gap's clock started when the prefix was in
	op->titleID = TITLE;
	op->version = current->version;
	if (!current->opens) {
		ringRelease();
		opSet(op, OUTCOME_REJECTED, current->refusal, current->refusalWhy);
		return false;
	}
	run->route = current->route;
	run->started = current->started;
	if (realDrain) {
		import.am = 0x55;
		import.overwrite = true;
		aptSetHomeAllowed(false);
		for (u64 left = ciaSize; left;) { // the whole body is buffered before the writer starts
			unsigned char *unit;
			size_t room;
			assert(ringAcquire(&unit, &room) && room > 0 && room <= left);
			assert(ringCommit(room));
			left -= room;
		}
		run->started = ringStart(import.am, false, NULL);
		assert(run->started);
	}
	return true;
}
void installClose(InstallRun *run, bool received, const char *receiveDetail, Result receiveWhy,
				  void (*waiting)(void *context), void *context, Operation *op) {
	installCloses++;
	closed.received = received;
	closed.detail = receiveDetail;
	closed.why = receiveWhy;
	if (realDrain) {
		drainClose(run, received, receiveDetail, receiveWhy, waiting, context, op);
		return;
	}
	if (!run->started) {
		opSet(op, OUTCOME_REJECTED, "writer", OUT_OF_MEMORY);
	} else if (!received) {
		opSet(op, OUTCOME_REJECTED, receiveDetail, receiveWhy);
	} else {
		unsigned ticks = statusTicks;
		scripted.done = 777;
		waiting(
			context); // the screen says committing, the LED is kept alive, the count is the ring's
		assert(statusTicks == ticks + 1 && gApp.uiWrittenBytes == 777);
		if (op->result == TRANSFER_CANCELED) {
			return;
		}
		opSet(op, current->commitUnknown ? OUTCOME_UNKNOWN : OUTCOME_COMMITTED,
			  current->commitUnknown ? "am-finish" : "ok", current->commitUnknown ? 0xD8A08009 : 0);
	}
}

// ---- The card ----
static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}
static char *slurp(const char *path) {
	static char text[4096];
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	text[n] = '\0';
	if (f) {
		fclose(f);
	}
	return f ? text : NULL;
}
static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static void bundleOn(const char *text) {
	mkdir("sdmc:/config", 0755);
	mkdir("sdmc:/config/ssl", 0755);
	put(CACERT_PATH, text);
}
static void bundleOff(void) {
	remove(CACERT_PATH);
	remove(CACERT_TEMP);
	remove("sdmc:" CACERT_ASIDE_FS);
}
static char *logText(void) {
	char *text = slurp(LOG_PATH);
	return text ? text : "";
}
static bool logged(const char *event, Result result, u64 bytes) {
	char line[96];
	snprintf(line, sizeof(line), "%s result=0x%08lX bytes=%llu", event, (unsigned long)(u32)result,
			 (unsigned long long)bytes);
	return strstr(logText(), line) != NULL;
}
static unsigned logLines(void) {
	unsigned n = 0;
	for (const char *p = logText(); *p; p++) {
		n += *p == '\n';
	}
	return n;
}

// ---- A scan, a run ----
static void reset(void) {
	qrExit();
	realDrain = joinPower = closing = keyAtJoin = false;
	joinsPending = pendingJoins = amFinishes = writeCalls = ringStops = 0;
	writtenBytes = 0;
	siteCalls = pathCalls = 0;
	memset(&cam, 0, sizeof(cam));
	memset(&screen, 0, sizeof(screen));
	memset(&scripted, 0, sizeof(scripted));
	memset(&stats, 0, sizeof(stats));
	memset(&closed, 0, sizeof(closed));
	memset(courses, 0, sizeof(courses));
	courseCount = coursesRun = pumps = installOpens = installCloses = sleeps = 0;
	downloadInits = downloadExits = cancels = joins = statusTicks = statusCount = 0;
	busyTicks = busyTicksDownloading = 0;
	scripted.done = 1234;
	initResult = 0;
	current = NULL;
	active = NULL;
	memset(&gApp, 0, sizeof(gApp));
	gApp.last = OP_REJECTED;
	fake.keysDown = 0;
	fake.stepMs = 50;
	fake.chainload = 0;
	fake.powerPressed = false;
	scripted.armOk = true;
	screen.picture = true;
	bundleOff();
	remove(LOG_PATH);
}
// A camera that sees `text` after two frames.
static void code(const char *text) {
	cam.steps[0] = SCAN_IDLE;
	cam.steps[1] = SCAN_FRAME;
	cam.steps[2] = SCAN_FOUND;
	cam.stepCount = 3;
	cam.payload = text;
}
static After play(void) {
	After a = AFTER_NOTHING;
	qrRun(&a);
	return a;
}
static const char *prompt(unsigned i) {
	assert(i < screen.askedCount);
	return screen.asked[i].text;
}
static void balanced(void) { // the camera and its picture are closed as often as opened
	// A client is let in at every look, wherever the flow waits.
	assert(busyTicks == statusTicks);
	assert(cam.closes == cam.opens && screen.scanCloses == screen.scanOpens);
	assert(joins == coursesRun && active == NULL); // every thread joined
}

// ---- The scan ----
static void scanning(void) {
	// A camera that will not start is reported with its Result; nothing else is opened or fetched.
	reset();
	cam.openResult = 0xC8A04001;
	answer(KEY_A);
	assert(play() == AFTER_NOTHING);
	assert(screen.askedCount == 1 && screen.asked[0].screen == UI_QR_REFUSED);
	assert(screen.asked[0].mask == 0xFFFFFFFFu); // any key goes on
	assert(strstr(prompt(0), "The camera could not be started.") &&
		   strstr(prompt(0), "0xC8A04001"));
	assert(cam.opens == 0 && cam.closes == 0 && screen.scanOpens == 0 && downloadInits == 0);

	// B ends the scan, as do START and the app being closed; nothing is asked or fetched.
	for (int how = 0; how < 3; how++) {
		reset();
		cam.pressBAt = 3;
		cam.pressedKey = how == 1 ? KEY_START : KEY_B;
		if (how == 2) {
			cam.pressBAt = 0;
			fake.powerPressed = true; // aptMainLoop answers false: HOME or POWER
		}
		assert(play() == AFTER_NOTHING);
		assert(screen.askedCount == 0 && coursesRun == 0 && downloadInits == 0);
		assert(cam.opens == 1 && screen.scanOpens == 1 && noticed(UI_QR_SCAN));
		balanced();
		assert(how == 2 || cam.polls == 3); // heard within the poll it came in
		// The scanner lets a client in at every look, one before each poll and the one that heard
		// the key, with nothing lent, so a screenshot of it is safe.
		assert(busyTicks == cam.polls + 1 && busyTicksDownloading == 0);
	}

	// The picture follows every frame but not the idle polls; if it did not open, there is none.
	for (int picture = 1; picture >= 0; picture--) {
		reset();
		screen.picture = picture;
		code("https://a.example/x.cia\n");
		answer(KEY_B); // not installed here
		assert(play() == AFTER_NOTHING);
		assert(screen.frames == (picture ? 2u : 0u)); // a frame, then the one with the code
		assert(!picture || screen.lastFrame == frameMark);
		assert(cam.polls == 3 && screen.askedCount == 1 && screen.asked[0].screen == UI_QR_CONFIRM);
		balanced();
	}

	// A camera that breaks mid-scan.
	reset();
	cam.steps[0] = SCAN_FRAME;
	cam.steps[1] = SCAN_BROKEN;
	cam.stepCount = 2;
	cam.failure = 0xC8A04002;
	cam.payload = "";
	answer(KEY_A);
	assert(play() == AFTER_NOTHING);
	assert(screen.askedCount == 1 && screen.asked[0].screen == UI_QR_REFUSED);
	assert(strstr(prompt(0), "The camera stopped.") && strstr(prompt(0), "0xC8A04002"));
	balanced();

	// A code that decodes but is not a list of links is reported, in words about what is wrong,
	// and ends the scan, because the next frame would read the same code.
	static const struct {
		const char *payload;
		UrlsVerdict verdict;
	} BAD[] = {
		{"hello world", URLS_BAD},
		{"ftp://h/a.cia", URLS_BAD},
		{"", URLS_EMPTY},
		{"https://h/1\nhttps://h/2\nhttps://h/3\nhttps://h/4\nhttps://h/5\nhttps://h/6\nhttps://h/"
		 "7\n"
		 "https://h/8\nhttps://h/9\nhttps://h/10\nhttps://h/11\nhttps://h/12\nhttps://h/13\n"
		 "https://h/14\nhttps://h/15\nhttps://h/16\nhttps://h/17\n",
		 URLS_TOO_MANY},
	};
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		reset();
		code(BAD[i].payload);
		answer(KEY_A);
		assert(play() == AFTER_NOTHING);
		assert(screen.askedCount == 1 && screen.asked[0].screen == UI_QR_REFUSED);
		assert(strcmp(prompt(0), urlsWhy(BAD[i].verdict)) == 0); // no Result line: nothing failed
		assert(cam.polls == 3 && downloadInits == 0 && coursesRun == 0);
		balanced();
	}
}

// ---- The confirm screen ----
static const char RELEASE[] = "/MadeOfBees/3Drop/releases/download/v1/3drop.cia";
// The test path of `length` characters: "/MadeOfBees/3Drop/releases/download/" (36), digits
// counting 0 to 9 from there, and "/3drop.cia" (10), so a cut that drops or shifts one character
// shows.
static const char *longPath(size_t length) {
	static char path[200];
	size_t at = (size_t)snprintf(path, sizeof(path), "/MadeOfBees/3Drop/releases/download/");
	for (; at + 10 < length; at++) {
		path[at] = (char)('0' + (at - 36) % 10);
	}
	snprintf(path + at, sizeof(path) - at, "/3drop.cia");
	assert(strlen(path) == length);
	return path;
}
// The test link: github.com, then `path`, then `after` (a query, a fragment). Four buffers in
// turn, so a call can name two links.
static const char *github(const char *path, const char *after) {
	static char link[4][400];
	static unsigned next;
	char *out = link[next++ % 4];
	snprintf(out, sizeof(link[0]), "https://github.com%s%s", path, after);
	return out;
}
// The first three rows of every long path: its start is whole whatever the cut after it.
#define LONG_HEAD                                                                                  \
	"/MadeOfBees/3Drop/releases/\n"                                                                \
	"download/012345678901234567890\n"                                                             \
	"123456789012345678901234567890\n"

static void confirming(void) {
	// One https link: its site, then its path, whole, with nothing about encryption, and a plain A
	// or B to answer.
	reset();
	code("https://a.example/x.cia\n");
	answer(KEY_B);
	play();
	assert(screen.asked[0].screen == UI_QR_CONFIRM && screen.asked[0].mask == (KEY_A | KEY_B));
	assert(strcmp(prompt(0), "1  a.example\n/x.cia\n") == 0 && siteCalls == 1 && pathCalls == 1);

	// What the feature is for: a GitHub release link. The host does not name anyone, so the site
	// gets a row of its own and the owner, the repository and the file follow in rows of 30. That
	// makes three rows, so there is no scrolling and no Up/Down. Beside another repository's link
	// on the same host, the two differ where the person looks.
	reset();
	code(github(RELEASE, ""));
	answer(KEY_B);
	play();
	assert(strcmp(prompt(0),
				  "1  github.com\n/MadeOfBees/3Drop/releases/\ndownload/v1/3drop.cia\n") == 0);
	assert(screen.asked[0].mask == (KEY_A | KEY_B) && rowTotal == 3);
	char other[400];
	snprintf(other, sizeof(other), "%s\n%s", github(RELEASE, ""),
			 github("/someone-else/3drop/releases/download/v1/3drop.cia", ""));
	reset();
	code(other);
	answer(KEY_B);
	play();
	assert(strcmp(prompt(0),
				  "1  github.com\n/MadeOfBees/3Drop/releases/\ndownload/v1/3drop.cia\n"
				  "2  github.com\n/someone-else/3drop/releases/\ndownload/v1/3drop.cia\n") == 0);
	assert(screen.asked[0].mask == (KEY_A | KEY_B) && rowTotal == 6);
	// B (and a key that is neither, and the app closing) fetches nothing and starts neither
	// library.
	assert(downloadInits == 0 && coursesRun == 0 && stats.resets == 0);
	reset();
	code("https://a.example/x.cia\n");
	answer(0);
	play();
	assert(downloadInits == 0 && coursesRun == 0);
	reset();
	code("https://a.example/x.cia\n");
	answer(KEY_A | KEY_B); // both: the cancel wins
	play();
	assert(downloadInits == 0 && coursesRun == 0);

	// Two links, one plain http (written as one) and one whose site takes two rows (the scheme in
	// capitals): each by number, site and path, a long site cut into rows and never cut off, the
	// mark on the plain one, and what plain http means on the bottom screen (after the \f).
	char host45[64], payload[1024], want[1024];
	snprintf(host45, sizeof(host45), "%045d", 0);
	snprintf(payload, sizeof(payload), "HTTPS://%s/x\nhttp://host/5\n", host45);
	reset();
	code(payload);
	answer(KEY_B);
	play();
	snprintf(want, sizeof(want),
			 "1  %.27s\n%s\n/x\n2  http://host\n/5\n"
			 "\f\nhttp:// is not encrypted: someone on the network can replace the file.",
			 host45, host45 + 27);
	assert(strcmp(prompt(0), want) == 0 && screen.asked[0].mask == (KEY_A | KEY_B));
	// The same with every link https: no note.
	reset();
	code("https://1/a\nhttps://2/b\nhttps://3/c\n");
	answer(KEY_B);
	play();
	assert(strcmp(prompt(0), "1  1\n/a\n2  2\n/b\n3  3\n/c\n") == 0);
	// A port is part of the site; plain http carries its mark whatever the scheme was written as.
	reset();
	code("http://host:8000/a.cia");
	answer(KEY_B);
	play();
	assert(strstr(prompt(0), "1  http://host:8000\n/a.cia\n\f") &&
		   strstr(prompt(0), "not encrypted"));
	// A link with no scheme is https, not plain: no mark and no note, and the row is the same site.
	reset();
	code("host:8000/a.cia");
	answer(KEY_B);
	play();
	assert(strcmp(prompt(0), "1  host:8000\n/a.cia\n") == 0);

	// What could fool a person in the text of a link is not what is shown; libcurl's reading of
	// the site is. So with a login before the host, or a long prefix that pushes the host past the
	// width the old screen cut at, the screen still names the host the download will go to.
	char prefixed[256], tricks[512];
	snprintf(prefixed, sizeof(prefixed), "https://%0*d@evil.example/x.cia", 90, 0);
	snprintf(tricks, sizeof(tricks), "https://good.example@evil.example/x.cia\n%s\n", prefixed);
	reset();
	code(tricks);
	answer(KEY_B);
	play();
	assert(strcmp(prompt(0), "1  evil.example\n/x.cia\n2  evil.example\n/x.cia\n") == 0);
	assert(!strstr(prompt(0), "good.example") && siteCalls == 2 && pathCalls == 2);
	// The path follows the true host, not the name the login claims: github.com is what the link
	// says before the @, evil.example is where it goes, and the person reads that, then the path.
	reset();
	code(github("@evil.example", RELEASE)); // https://github.com@evil.example/MadeOfBees/...
	answer(KEY_B);
	play();
	assert(strcmp(prompt(0),
				  "1  evil.example\n/MadeOfBees/3Drop/releases/\ndownload/v1/3drop.cia\n") == 0);
	assert(!strstr(prompt(0), "github.com") && rowTotal == 3);

	// A path of up to 120 characters is whole, in rows of 30; a longer one is its first 90 and its
	// last 27 around "...", so the owner at one end and the file name at the other both show.
	// 120 is whole, 121 is the first that is cut, 130 is cut further in.
	static const struct {
		size_t length;
		const char *last; // the rows after LONG_HEAD
	} LONG[] = {
		{120, "12345678901234567890123/\n3drop.cia\n"},
		{121, "123...89012345678901234/\n3drop.cia\n"},
		{130, "123...78901234567890123/\n3drop.cia\n"},
	};
	for (size_t i = 0; i < sizeof(LONG) / sizeof(LONG[0]); i++) {
		reset();
		code(github(longPath(LONG[i].length), ""));
		answer(KEY_B);
		play();
		snprintf(want, sizeof(want), "1  github.com\n" LONG_HEAD "%s", LONG[i].last);
		assert(strcmp(prompt(0), want) == 0 && rowTotal == 6 &&
			   screen.asked[0].mask == (KEY_A | KEY_B));
	}

	// The query is not shown: "?..." ends the path instead, so the person knows the link has more.
	// A fragment is neither shown nor marked, a bare host still has its path "/", and the mark
	// comes after a path of exactly 120 (it is not part of the 120) and after a cut one.
	static const struct {
		const char *link;
		const char *rows;
		size_t rowCount;
	} QUERIES[] = {
		{"https://github.com/MadeOfBees/3Drop/releases/download/v1/3drop.cia?token=SECRET&x=1#frag",
		 "1  github.com\n/MadeOfBees/3Drop/releases/\ndownload/v1/3drop.cia?...\n", 3},
		{"https://github.com/MadeOfBees/3Drop/releases/download/v1/3drop.cia#frag",
		 "1  github.com\n/MadeOfBees/3Drop/releases/\ndownload/v1/3drop.cia\n", 3},
		{"https://github.com?token=SECRET", "1  github.com\n/?...\n", 2},
		{"https://github.com/a?b#c?d", "1  github.com\n/a?...\n", 2},
		{"https://github.com/a?", "1  github.com\n/a?...\n", 2},
		{"https://github.com/a#c?d", "1  github.com\n/a\n", 2},
	};
	for (size_t i = 0; i < sizeof(QUERIES) / sizeof(QUERIES[0]); i++) {
		reset();
		code(QUERIES[i].link);
		answer(KEY_B);
		play();
		assert(strcmp(prompt(0), QUERIES[i].rows) == 0 && rowTotal == QUERIES[i].rowCount);
		assert(!strstr(prompt(0), "SECRET") && !strstr(prompt(0), "token") &&
			   !strstr(prompt(0), "frag"));
	}
	static const struct {
		size_t length;
		const char *last; // the rows after LONG_HEAD
	} MARKED[] = {
		{120, "12345678901234567890123/\n3drop.cia?...\n"},
		{130, "123...78901234567890123/\n3drop.cia?...\n"},
	};
	for (size_t i = 0; i < sizeof(MARKED) / sizeof(MARKED[0]); i++) {
		reset();
		code(github(longPath(MARKED[i].length), "?a=b"));
		answer(KEY_B);
		play();
		snprintf(want, sizeof(want), "1  github.com\n" LONG_HEAD "%s", MARKED[i].last);
		assert(strcmp(prompt(0), want) == 0 && rowTotal == 6);
	}

	// A path breaks after a slash where a row of 30 would cut a word, when a row of its own holds
	// that word (UT-07, UT-14). A longer word is cut from its first character, as it always was.
	// A cut at a slash, or just before one, cuts no word. A site is not a path, so the slashes of
	// http:// stay where they are.
#define B10 "bbbbbbbbbb"
	static const struct {
		const char *path, *rows;
		size_t rowCount;
	} SLASHES[] = {
		{"/Universal-Team/Universal-Updater/releases/download/v3.4.1/Universal-Updater.cia",
		 "1  github.com\n/Universal-Team/\nUniversal-Updater/releases/\ndownload/v3.4.1/\n"
		 "Universal-Updater.cia\n",
		 5},
		{"/a/" B10 B10 B10 B10, "1  github.com\n/a/" B10 B10 "bbbbbbb\n" B10 "bbb\n", 3},
		{"/a/" B10 B10 B10, "1  github.com\n/a/\n" B10 B10 B10 "\n", 3},
		{"/a/" B10 B10 B10 "b", "1  github.com\n/a/" B10 B10 "bbbbbbb\nbbbb\n", 3},
		{"/" B10 B10 "bbbbbbbb/bbbb", "1  github.com\n/" B10 B10 "bbbbbbbb/\nbbbb\n", 3},
		{"/" B10 B10 "bbbbbbbbb/bb", "1  github.com\n/" B10 B10 "bbbbbbbbb\n/bb\n", 3},
	};
	for (size_t i = 0; i < sizeof(SLASHES) / sizeof(SLASHES[0]); i++) {
		reset();
		code(github(SLASHES[i].path, ""));
		answer(KEY_B);
		play();
		if (strcmp(prompt(0), SLASHES[i].rows) != 0 || rowTotal != SLASHES[i].rowCount) {
			fprintf(stderr, "testQr: path %zu: `%s`\n", i, prompt(0));
			assert(!"a path's rows");
		}
	}
#undef B10
	// The slashes of a plain http site's scheme stay where they are, so a host of 28 characters is
	// cut as ever, at 30, not after the "http://" the path rule would break at.
#define Z10 "0000000000"
	reset();
	code("http://" Z10 Z10 "00000000/x\n");
	answer(KEY_B);
	play();
	assert(strstr(prompt(0), "1  http://" Z10 Z10 "\n00000000\n/x\n\f") == prompt(0));
#undef Z10

	// A site that cannot be shown whole is not a link to agree to, and neither is a link libcurl
	// cannot read, so the code is refused, naming the link, before anything is asked or started.
	// 120 characters of site fit (a row more than four), 121 do not.
	char host120[160], host121[160];
	snprintf(host120, sizeof(host120), "https://%0120d/x", 0);
	snprintf(host121, sizeof(host121), "https://%0121d/x", 0);
	reset();
	code(host120);
	answer(KEY_B);
	play();
	assert(screen.askedCount == 1 && screen.asked[0].screen == UI_QR_CONFIRM);
	// Five rows: four full ones and "1  " + 120 - 120 = 3; then the path "/x".
	assert(strlen(prompt(0)) == 4 * 31 + 4 + strlen("/x\n"));
	static const char SITE_WHY[] = "its site cannot be read, or is too long.";
	const struct {
		const char *payload, *why;
		int link, links;
	} refusals[] = {
		{host121, SITE_WHY, 1, 1},
		{"https://a/1\nhttps://unreadable/2\nhttps://c/3", SITE_WHY, 2, 3},
		{"https://a/1\nhttps://b/2\nhttps://c/badpath", "its path cannot be read.", 3, 3},
	};
	for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
		reset();
		code(refusals[i].payload);
		answer(KEY_A);
		assert(play() == AFTER_NOTHING);
		assert(screen.askedCount == 1 && screen.asked[0].screen == UI_QR_REFUSED);
		assert(screen.asked[0].mask == 0xFFFFFFFFu && coursesRun == 0 && downloadInits == 0);
		snprintf(want, sizeof(want), "Link %d of %d: %s", refusals[i].link, refusals[i].links,
				 refusals[i].why);
		assert(strcmp(prompt(0), want) == 0);
		balanced();
	}

	// When there are more rows than fit, Up and Down are asked for, and only then; they move the
	// window one row, stopping at the first and the last. The sites are read once, not at every
	// redraw. Four links of a site row and a path row each are eight rows; the window can start
	// on a path.
	reset();
	code("https://h1/a\nhttps://h2/a\nhttps://h3/a\nhttps://h4/a\n");
	static const u32 SCROLL[] = {KEY_UP, KEY_DOWN, KEY_DOWN, KEY_DOWN, KEY_UP, KEY_UP, KEY_B};
	static const char *const WINDOW[] = {
		"1  h1\n/a\n2  h2\n/a\n3  h3\n/a\nUp/Down: rows 1-6 of 8",
		"1  h1\n/a\n2  h2\n/a\n3  h3\n/a\nUp/Down: rows 1-6 of 8",
		"/a\n2  h2\n/a\n3  h3\n/a\n4  h4\nUp/Down: rows 2-7 of 8",
		"2  h2\n/a\n3  h3\n/a\n4  h4\n/a\nUp/Down: rows 3-8 of 8",
		"2  h2\n/a\n3  h3\n/a\n4  h4\n/a\nUp/Down: rows 3-8 of 8",
		"/a\n2  h2\n/a\n3  h3\n/a\n4  h4\nUp/Down: rows 2-7 of 8",
		"1  h1\n/a\n2  h2\n/a\n3  h3\n/a\nUp/Down: rows 1-6 of 8",
	};
	for (unsigned i = 0; i < 7; i++) {
		answer(SCROLL[i]);
	}
	play();
	assert(screen.askedCount == 7 && siteCalls == 4 && pathCalls == 4 && downloadInits == 0);
	assert(coursesRun == 0);
	for (unsigned i = 0; i < 7; i++) {
		assert(screen.asked[i].screen == UI_QR_CONFIRM);
		assert(screen.asked[i].mask == (KEY_A | KEY_B | KEY_UP | KEY_DOWN));
		assert(strcmp(prompt(i), WINDOW[i]) == 0);
	}
	// A after scrolling installs every link, not the rows on show: two links of a 100-character
	// site (four rows) and a path (one) take ten rows between them.
	char wide1[160], wide2[160], wide[400];
	snprintf(wide1, sizeof(wide1), "https://%0100d/1.cia", 1);
	snprintf(wide2, sizeof(wide2), "https://%0100d/2.cia", 2);
	snprintf(wide, sizeof(wide), "%s\n%s\n", wide1, wide2);
	reset();
	bundleOn(PEM_OLD);
	code(wide);
	answer(KEY_DOWN);
	answer(KEY_A);
	add(DL_SINK_CIA, wide1, true);
	add(DL_SINK_CIA, wide2, true);
	assert(play() == AFTER_NOTHING);
	assert(screen.askedCount == 2 && coursesRun == 2 && logLines() == 2);
	assert(strstr(prompt(0), "Up/Down: rows 1-6 of 10") && strstr(prompt(1), "rows 2-7 of 10"));

	// This is the most the screen is asked to hold: sixteen links, each with a 120-character
	// site and a path cut to 120 with its query's mark, one of them plain. That is five rows each
	// for the site and the path, 160 rows in all, every one inside the buffer (the last link's end
	// is where it should be), and the window inside the prompt.
	char many[URLS_MAX * 300], *at = many;
	for (int i = 0; i < URLS_MAX - 1; i++) {
		at += sprintf(at, "https://%0120d%s?q=1\n", i, longPath(130));
	}
	sprintf(at, "http://%0120d%s?q=1\n", 0, longPath(130));
	reset();
	code(many);
	answer(KEY_B);
	play();
	assert(strlen(prompt(0)) < 320);
	assert(rowTotal == 160 && strlen(rows) == 4126 && strcmp(rows + 4126 - 6, "a?...\n") == 0);
	assert(strstr(prompt(0), "Up/Down: rows 1-6 of 160\f\nhttp:// is not encrypted: someone on the "
							 "network can replace the file.") != NULL);
}

// ---- One link ----
static void scanAndInstall(const char *payload) {
	code(payload);
	answer(KEY_A);
}

static void oneHttps(void) {
	// An https link with the bundle on the card: one verified download, no bundle fetched, and the
	// install's outcome as a computer's would show it.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://a.example/x.cia");
	add(DL_SINK_CIA, "https://a.example/x.cia", true)->route = &R_UPDATE;
	fake.stepMs = 250;		 // a tick of progress at every look
	gApp.fileSending = true; // state from an earlier request, cleared when this transfer starts
	gApp.batchTotal = 9;
	snprintf(gApp.fileName, sizeof(gApp.fileName), "stale");
	assert(play() == AFTER_NOTHING);
	assert(screen.sawDone && screen.sawPrefix); // the bar follows the ring, and the wire's bytes
	assert(!gApp.fileSending && gApp.fileName[0] == '\0' && gApp.batchTotal == 1);
	assert(statusCount == 2 && statuses[0] == SERVER_RECEIVING && statuses[1] == SERVER_INSTALLED);
	assert(statusTicks >= pumps && statusTicks > 3); // the LED is kept alive at every look
	// A client is let in at each of them, and a download is reported as running only during its
	// waits: not at the scanner, the question or after the install.
	assert(busyTicks == statusTicks && busyTicksDownloading > 0);
	assert(busyTicksDownloading < busyTicks && !qrDownloading());
	assert(coursesRun == 1 && downloadInits == 1 && downloadExits == 0);
	assert(scripted.arms == 1 && scripted.armTotal == CIA_SIZE && scripted.releases == 0);
	assert(installOpens == 1 && installCloses == 1 && closed.received && closed.detail == NULL);
	assert(gApp.last.outcome == OUTCOME_COMMITTED && strcmp(gApp.last.detail, "ok") == 0);
	assert(gApp.last.titleID == TITLE && gApp.last.version == 3);
	assert(gApp.status == SERVER_INSTALLED && gApp.lastBytes == CIA_SIZE);
	assert(stats.resets == 1 && strcmp(stats.request, "qr") == 0 && stats.finishes == 1);
	assert(gApp.viaUrl && gApp.wireTotal == CIA_SIZE && gApp.uiSunkProgress);
	assert(!gApp.batchActive && gApp.batchIndex == 1 && gApp.batchTotal == 1);
	assert(logged("qr-ok", 0, CIA_SIZE) && logLines() == 1);
	assert(strcmp(slurp(CACERT_PATH), PEM_OLD) == 0); // the bundle is as it was
	assert(screen.askedCount == 1 && screen.progressTicks > 0);
	assert(fake.chainload == 0); // not 3Drop: no relaunch
	balanced();
	// libcurl and sslc stop with the app, once.
	qrExit();
	qrExit();
	assert(downloadExits == 1);

	// The bar is redrawn at most every UI_REDRAW_MIN_MS. A quick pass of looks asks for it at
	// the first look, at the start of the fetch, at the close's first look (qr.c's, before the
	// writer's join) and at the commit.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://a.example/x.cia");
	add(DL_SINK_CIA, "https://a.example/x.cia", true);
	fake.stepMs = 10;
	play();
	assert(screen.progressTicks == 4);

	// Plain http: no certificate list is fetched or read up front (it may never leave http), but
	// the download is told to check any https connection it is sent on to.
	reset();
	scanAndInstall("http://192.168.1.5:8000/a.cia");
	add(DL_SINK_CIA, "http://192.168.1.5:8000/a.cia", true);
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 1 && !exists(CACERT_PATH) && logged("qr-ok", 0, CIA_SIZE));
	assert(screen.askedCount == 1 && !noticed(UI_QR_BUNDLE));
	// A link with no scheme is https, so it goes the same way as the other https links: the list
	// is fetched for a check that runs, and the transfer is verified.
	reset();
	scanAndInstall("host/b.cia");
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, "https://host/b.cia", true);
	play();
	assert(coursesRun == 2 && noticed(UI_QR_BUNDLE) && logged("qr-ok", 0, CIA_SIZE));
	assert(strcmp(slurp(CACERT_PATH), PEM_OK) == 0);
}

static void certificateList(void) {
	// No list on the card: it is fetched first into the temp file, checked against the bundle
	// shipped in the app, and kept as the list. Then the download is checked against it.
	reset();
	scanAndInstall("https://a.example/x.cia");
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, "https://a.example/x.cia", true);
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 2 && noticed(UI_QR_BUNDLE));
	assert(strcmp(slurp(CACERT_PATH), PEM_OK) == 0 && !exists(CACERT_TEMP));
	assert(logged("qr-ok", 0, CIA_SIZE));
	balanced();

	// An empty file is not a list either, so it is fetched again.
	reset();
	bundleOn("");
	scanAndInstall("https://a.example/x.cia");
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, "https://a.example/x.cia", true);
	play();
	assert(coursesRun == 2 && strcmp(slurp(CACERT_PATH), PEM_OK) == 0);

	// When a fetch fails, the link is refused in the download's own word and Result (a network
	// that did not answer, a server that refused), not as a list the card could not save, and
	// nothing else is fetched.
	static const struct {
		const char *detail;
		Result why;
	} FAILS[] = {
		{"connect", DOWNLOAD_CURL(CURLE_COULDNT_CONNECT)},
		{"http", DOWNLOAD_HTTP(404)},
		{"too-big", DOWNLOAD_TOO_BIG},
	};
	for (size_t i = 0; i < sizeof(FAILS) / sizeof(FAILS[0]); i++) {
		reset();
		scanAndInstall("https://a.example/x.cia");
		Course *failing = add(DL_SINK_FILE, CACERT_URL, false);
		failing->detail = FAILS[i].detail;
		failing->why = FAILS[i].why;
		assert(play() == AFTER_NOTHING);
		assert(coursesRun == 1 && !exists(CACERT_PATH) && !exists(CACERT_TEMP));
		assert(gApp.last.outcome == OUTCOME_REJECTED);
		assert(strcmp(gApp.last.detail, FAILS[i].detail) == 0 && gApp.last.result == FAILS[i].why);
		assert(gApp.status == SERVER_INSTALL_FAILED && installOpens == 0 && scripted.arms == 0);
		char event[32], words[160];
		snprintf(event, sizeof(event), "qr-%s", FAILS[i].detail);
		assert(logged(event, FAILS[i].why, 0));
		assert(fetchWhy(words, sizeof(words), gApp.last.detail, gApp.last.result));
		assert(!strstr(words, "saved"));
		// The list's fetch is a download too: serveBusy served nothing through its waits (each
		// finds the flag up, as svcSleepThread checks) and serves the question after it again.
		assert(busyTicksDownloading > 0 && busyTicksDownloading < busyTicks && !qrDownloading());
	}

	// A page that is not a list (a captive portal's) is refused, and no list is kept.
	reset();
	scanAndInstall("https://a.example/x.cia");
	add(DL_SINK_FILE, CACERT_URL, false)->file = "<html>log in to the Wi-Fi</html>";
	play();
	assert(coursesRun == 1 && !exists(CACERT_PATH) && !exists(CACERT_TEMP));
	assert(strcmp(gApp.last.detail, "bundle") == 0 && gApp.last.result == STAGE_METADATA);
	assert(logged("qr-bundle", STAGE_METADATA, 0));

	// A whole list that the card will not take (the move into place fails): refused as "bundle"
	// with the card's own Result, not a page's.
	reset();
	scanAndInstall("https://a.example/x.cia");
	add(DL_SINK_FILE, CACERT_URL, false);
	fake.renameResult = (Result)0xC8804464;
	fake.renameFrom = "cacert.pem.tmp";
	play();
	fake.renameResult = 0;
	fake.renameFrom = NULL;
	assert(coursesRun == 1 && !exists(CACERT_PATH) && !exists(CACERT_TEMP));
	assert(strcmp(gApp.last.detail, "bundle") == 0 && gApp.last.result == (Result)0xC8804464);
	assert(logged("qr-bundle", (Result)0xC8804464, 0));

	// The card cannot hold the folder: the link is refused as "bundle", and nothing is fetched.
	reset();
	scanAndInstall("https://a.example/x.cia");
	fake.sdGone = 0xC8804478;
	play();
	fake.sdGone = 0;
	assert(coursesRun == 0 && strcmp(gApp.last.detail, "bundle") == 0);
	assert(gApp.last.result == DOWNLOAD_NO_FILE);

	// The list is read once per link, from the card, so the second link of a batch does not fetch
	// it again.
	reset();
	scanAndInstall("https://a.example/1.cia\nhttps://a.example/2.cia");
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, "https://a.example/1.cia", true);
	add(DL_SINK_CIA, "https://a.example/2.cia", true);
	play();
	assert(coursesRun == 3);
}

// A download that ends on a failed certificate check; mbedTLS's flags say how.
static Course *checkFails(const char *url, long flags) {
	Course *c = add(DL_SINK_CIA, url, true);
	c->size = c->prefix = 0;
	c->detail = "tls";
	c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
	c->tls = DL_TLS_CHECK;
	c->verifyResult = flags;
	return c;
}

// A fetch of the list that fails its check, with a B that main reads in the worker's tail. The
// transfer has returned with the check marked failed (the real classify's tls and verifyResult)
// but DL_FINISHED is not up yet, so the key's edge is gone when the worker ends. `onCard` says
// what the fetch ran against.
static void listCheckFailsAtB(bool onCard) {
	Course *c = add(DL_SINK_FILE, CACERT_URL, onCard);
	c->detail = "tls";
	c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
	c->tls = DL_TLS_CHECK;
	c->verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	c->slowFinish = true;
	c->keyAfter = 2; // the step that writes the body: main reads the B on its next look
}

// The first of two links fails its check and B answers. The prompt's top screen is `top`, and
// the cause follows for the bottom screen. None of a query's text is on it.
static void namesLink(const char *link, const char *top) {
	char two[1200], want[1200];
	snprintf(two, sizeof(two), "%s\nhttps://b.example/2.cia", link);
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(two);
	checkFails(link, MBEDTLS_X509_BADCERT_CN_MISMATCH);
	answer(KEY_B);
	play();
	snprintf(want, sizeof(want), "%s\f\n%s", top, tlsCauseWhy(TLS_CAUSE_HOST));
	assert(screen.askedCount == 2 && strcmp(prompt(1), want) == 0);
	assert(!strstr(prompt(1), "SECRET") && !strstr(prompt(1), "token"));
	// Checks what ui.c's body (320 bytes) must hold, and the whole text of the prompt.
	assert(strchr(prompt(1), '\f') - prompt(1) < 320 && strlen(prompt(1)) < 512);
}

static void failedCheck(void) {
	const char *url = "https://a.example/x.cia";

	// B: the link is refused as canceled, nothing else is fetched, and the prompt named the cause,
	// never a date (a flag the hook clears, if one got here, is not a cause).
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	answer(KEY_B);
	assert(play() == AFTER_NOTHING);
	assert(screen.askedCount == 2 && screen.asked[1].screen == UI_QR_CHECK);
	assert(screen.asked[1].mask == (KEY_A | KEY_Y | KEY_B));
	assert(strstr(prompt(1), tlsCauseWhy(TLS_CAUSE_ISSUER)) && !strstr(prompt(1), "date:"));
	assert(coursesRun == 1 && gApp.last.outcome == OUTCOME_REJECTED);
	assert(strcmp(gApp.last.detail, "canceled") == 0 && gApp.last.result == TRANSFER_CANCELED);
	assert(logged("qr-canceled", TRANSFER_CANCELED, 0) && logLines() == 1);
	assert(installOpens == 0 && scripted.arms == 0 && gApp.status == SERVER_INSTALL_FAILED);

	// Another site's certificate.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_CN_MISMATCH);
	answer(KEY_B);
	play();
	assert(strstr(prompt(1), tlsCauseWhy(TLS_CAUSE_HOST)) && !strstr(prompt(1), "date:"));

	// The app closing at the question is a B.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	answer(0);
	play();
	assert(coursesRun == 1 && strcmp(gApp.last.detail, "canceled") == 0);

	// Y: the same link again with no check, for this download only; the list is not touched.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_CIA, url, false);
	answer(KEY_Y);
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 2 && screen.askedCount == 2 && logged("qr-ok", 0, CIA_SIZE));
	assert(logLines() == 1 && strcmp(slurp(CACERT_PATH), PEM_OLD) == 0);
	assert(gApp.last.outcome == OUTCOME_COMMITTED && gApp.status == SERVER_INSTALLED);
	balanced();

	// Y skips the check of that download alone, so the next link of the batch is checked again.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://a.example/1.cia\nhttps://a.example/2.cia");
	checkFails("https://a.example/1.cia", MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_CIA, "https://a.example/1.cia", false);
	add(DL_SINK_CIA, "https://a.example/2.cia", true);
	answer(KEY_Y);
	play();
	assert(coursesRun == 3 && screen.askedCount == 2 && logLines() == 2);

	// A skipped download is never asked about again, whatever it ends on, because the question
	// would come round forever.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_CIA, url, false);
	courses[courseCount - 1].size = courses[courseCount - 1].prefix = 0;
	courses[courseCount - 1].detail = "tls";
	courses[courseCount - 1].tls = DL_TLS_CHECK;
	answer(KEY_Y);
	play();
	assert(coursesRun == 2 && screen.askedCount == 2 && strcmp(gApp.last.detail, "tls") == 0);

	// A skipped download that fails again for another reason is not asked about again.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	Course *c = add(DL_SINK_CIA, url, false);
	c->size = c->prefix = 0;
	c->detail = "connect";
	c->why = DOWNLOAD_CURL(CURLE_COULDNT_CONNECT);
	answer(KEY_Y);
	play();
	assert(coursesRun == 2 && screen.askedCount == 2);
	assert(strcmp(gApp.last.detail, "connect") == 0 && logged("qr-connect", c->why, 0));

	// A: a fresh list is fetched, and that fetch is checked, against the list on the card (the one
	// the failed check ran against); the link is then tried again, checked by the fresh list.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_FILE, CACERT_URL, true);
	add(DL_SINK_CIA, url, true);
	answer(KEY_A);
	play();
	assert(coursesRun == 3 && noticed(UI_QR_BUNDLE) && screen.askedCount == 2);
	assert(strcmp(slurp(CACERT_PATH), PEM_OK) == 0 && !exists(CACERT_TEMP));
	assert(logged("qr-ok", 0, CIA_SIZE) && logLines() == 1);

	// A, and the fresh list fails again: the question comes again, on the same screen, with the
	// cause as before.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_FILE, CACERT_URL, true);
	checkFails(url, MBEDTLS_X509_BADCERT_CN_MISMATCH);
	answer(KEY_A);
	answer(KEY_B);
	play();
	assert(coursesRun == 3 && screen.askedCount == 3 && screen.asked[2].screen == UI_QR_CHECK);
	assert(strstr(prompt(2), tlsCauseWhy(TLS_CAUSE_HOST)));
	assert(strcmp(gApp.last.detail, "canceled") == 0);

	// A, and the list cannot be fetched: that is reported, then the question stands. The old list
	// is kept.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	c = add(DL_SINK_FILE, CACERT_URL, true);
	c->detail = "connect";
	c->why = DOWNLOAD_CURL(CURLE_COULDNT_CONNECT);
	answer(KEY_A);
	answer(KEY_A); // the refusal, any key
	answer(KEY_B);
	play();
	assert(coursesRun == 2 && screen.askedCount == 4);
	assert(screen.asked[2].screen == UI_QR_REFUSED && screen.asked[3].screen == UI_QR_CHECK);
	assert(strstr(prompt(2), "The certificate list could not be fetched."));
	char hex[16];
	snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)(u32)DOWNLOAD_CURL(CURLE_COULDNT_CONNECT));
	assert(strstr(prompt(2), hex));
	assert(strcmp(slurp(CACERT_PATH), PEM_OLD) == 0 && strcmp(gApp.last.detail, "canceled") == 0);

	// A, and the fresh list fails the check both ways, against the list on the card and then
	// against the bundle shipped in the app: it replaces nothing and the card's list stands, the
	// person is told that (and not that the fetch failed for lack of a network), and the question
	// comes back. A second A runs the same two checked fetches again, never an unchecked one,
	// because there is no way from here to a list that neither of the two vouched for.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	for (int i = 0; i < 2; i++) {
		for (int onCard = 1; onCard >= 0; onCard--) {
			c = add(DL_SINK_FILE, CACERT_URL, onCard);
			c->detail = "tls";
			c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
			c->tls = DL_TLS_CHECK;
			c->verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
		}
		answer(KEY_A);
		answer(KEY_A); // the refusal, any key
	}
	answer(KEY_B);
	play();
	assert(coursesRun == 5 && screen.askedCount == 6 && gApp.last.result == TRANSFER_CANCELED);
	assert(screen.asked[2].screen == UI_QR_REFUSED && screen.asked[3].screen == UI_QR_CHECK);
	assert(screen.asked[4].screen == UI_QR_REFUSED && screen.asked[5].screen == UI_QR_CHECK);
	static const char NOT_VOUCHED[] = "failed the check against the certificates built into 3Drop, "
									  "so the card's list was not changed.";
	assert(strstr(prompt(2), NOT_VOUCHED) && strstr(prompt(4), NOT_VOUCHED));
	assert(!strstr(prompt(2), "could not be fetched"));
	snprintf(hex, sizeof(hex), "0x%08lX",
			 (unsigned long)(u32)DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION));
	assert(strstr(prompt(2), hex));
	assert(strcmp(slurp(CACERT_PATH), PEM_OLD) == 0 && !exists(CACERT_TEMP));
	assert(logLines() == 1 && logged("qr-canceled", TRANSFER_CANCELED, 0));

	// A, and what comes is a page or a cut file, not a list: nothing replaces the card's list, the
	// temp file is gone, and the failure is reported as a failed fetch (the check did not fail).
	static const char *const NOT_LISTS[] = {
		"<html>log in to the Wi-Fi</html>",
		PEM_OLD "-----BEGIN CERTIFICATE-----\nCUT",
	};
	for (int i = 0; i < 2; i++) {
		reset();
		bundleOn(PEM_OLD);
		scanAndInstall(url);
		checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
		add(DL_SINK_FILE, CACERT_URL, true)->file = NOT_LISTS[i];
		answer(KEY_A);
		answer(KEY_A); // the refusal, any key
		answer(KEY_B);
		play();
		assert(coursesRun == 2 && screen.askedCount == 4);
		assert(screen.asked[2].screen == UI_QR_REFUSED);
		assert(strstr(prompt(2), "The certificate list could not be fetched."));
		snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)(u32)STAGE_METADATA);
		assert(strstr(prompt(2), hex));
		assert(strcmp(slurp(CACERT_PATH), PEM_OLD) == 0 && !exists(CACERT_TEMP));
	}

	// A, and a whole list arrives that the card will not take (the move into place fails): the
	// Result shown is the card's, not a page's, and the old list is back.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_FILE, CACERT_URL, true);
	answer(KEY_A);
	answer(KEY_A); // the refusal, any key
	answer(KEY_B);
	fake.renameResult = (Result)0xC8804464;
	fake.renameFrom = "cacert.pem.tmp";
	play();
	fake.renameResult = 0;
	fake.renameFrom = NULL;
	assert(coursesRun == 2 && screen.askedCount == 4 && screen.asked[2].screen == UI_QR_REFUSED);
	assert(strstr(prompt(2), "The certificate list could not be fetched."));
	snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)0xC8804464);
	assert(strstr(prompt(2), hex));
	assert(strcmp(slurp(CACERT_PATH), PEM_OLD) == 0 && !exists(CACERT_TEMP));

	// A list on the card that cannot be read: the question says so (no cause, no date). The fresh
	// list is checked against the bundle shipped in the app (the fake holds the fetch to that), and
	// A replaces the unreadable one.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, 0)->tls = DL_TLS_BUNDLE;
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, url, true);
	answer(KEY_A);
	play();
	assert(strstr(prompt(1), "The certificate list on the card could not be read."));
	assert(!strstr(prompt(1), "date:") && !strstr(prompt(1), "signed by"));
	assert(coursesRun == 3 && logged("qr-ok", 0, CIA_SIZE));
	assert(strcmp(slurp(CACERT_PATH), PEM_OK) == 0 && !exists(CACERT_TEMP));
	// The same, and that fetch fails its check against the shipped bundle: the unreadable list is
	// left as it was, not deleted, and the person is told the check failed, not the network. The
	// check that failed was the fresh list's, so the question that comes back is the link's own,
	// still that of a list that cannot be read.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, 0)->tls = DL_TLS_BUNDLE;
	c = add(DL_SINK_FILE, CACERT_URL, false);
	c->detail = "tls";
	c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
	c->tls = DL_TLS_CHECK;
	c->verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	answer(KEY_A);
	answer(KEY_A); // the refusal, any key
	answer(KEY_B);
	play();
	assert(coursesRun == 2 && screen.askedCount == 4 && screen.asked[3].screen == UI_QR_CHECK);
	assert(strstr(prompt(2), NOT_VOUCHED));
	assert(!strstr(prompt(2), "could not be fetched"));
	assert(strcmp(slurp(CACERT_PATH), PEM_OLD) == 0);

	// A, and the list on the card reads but is too old to vouch for curl.se's current certificate:
	// the fresh list fails the check against it as the link did, and is fetched again, checked
	// against the bundle shipped in the app. It replaces the old list, and the link is tried again,
	// checked by it. This is the card that could not renew its own list.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	c = add(DL_SINK_FILE, CACERT_URL, true);
	c->detail = "tls";
	c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
	c->tls = DL_TLS_CHECK;
	c->verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, url, true);
	answer(KEY_A);
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 4 && noticed(UI_QR_BUNDLE) && screen.askedCount == 2);
	assert(strcmp(slurp(CACERT_PATH), PEM_OK) == 0 && !exists(CACERT_TEMP));
	assert(logged("qr-ok", 0, CIA_SIZE) && logLines() == 1);
	// A B while the second fetch runs publishes nothing, so the card's list stays.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	c = add(DL_SINK_FILE, CACERT_URL, true);
	c->detail = "tls";
	c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
	c->tls = DL_TLS_CHECK;
	add(DL_SINK_FILE, CACERT_URL, false)->keyAfter = 1;
	answer(KEY_A);
	answer(KEY_A); // the refusal, any key
	answer(KEY_B);
	play();
	assert(coursesRun == 3 && strcmp(slurp(CACERT_PATH), PEM_OLD) == 0 && !exists(CACERT_TEMP));
	assert(strcmp(gApp.last.detail, "canceled") == 0);
	// A B in the tail of that fetch against the card's list, after its check failed: still a B.
	// The shipped bundle is not tried (it would never see this B), nothing is published, the
	// refusal is the one a B has always had, not a failed check's, and the question stands.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	listCheckFailsAtB(true);
	// Nothing may follow it: no fetch against the shipped bundle, no second try of the link.
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, url, true);
	answer(KEY_A);
	answer(KEY_A); // the refusal, any key
	answer(KEY_B);
	play();
	assert(coursesRun == 2 && cancels == 1 && screen.askedCount == 4);
	assert(strcmp(slurp(CACERT_PATH), PEM_OLD) == 0 && !exists(CACERT_TEMP));
	assert(screen.asked[2].screen == UI_QR_REFUSED && screen.asked[3].screen == UI_QR_CHECK);
	assert(strstr(prompt(2), "The certificate list could not be fetched."));
	assert(!strstr(prompt(2), NOT_VOUCHED));
	assert(strcmp(gApp.last.detail, "canceled") == 0 && gApp.last.result == TRANSFER_CANCELED);
	assert(logged("qr-canceled", TRANSFER_CANCELED, 0));
	balanced();

	// A card with no list at all: the shipped bundle's check of the first fetch fails, and that
	// is reported the way a failed check is (with the cause), not as a connection that could not
	// be set up. Nothing is fetched after it, and no list is kept.
	reset();
	scanAndInstall(url);
	c = add(DL_SINK_FILE, CACERT_URL, false);
	c->detail = "tls";
	c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
	c->tls = DL_TLS_CHECK;
	c->verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	answer(KEY_A); // the refusal, any key
	play();
	assert(coursesRun == 1 && !exists(CACERT_PATH) && !exists(CACERT_TEMP));
	assert(screen.askedCount == 2 && screen.asked[1].screen == UI_QR_REFUSED);
	assert(strstr(prompt(1), "The certificate list failed its check."));
	assert(strstr(prompt(1), tlsCauseWhy(TLS_CAUSE_ISSUER)) && !strstr(prompt(1), "date: "));
	assert(strstr(prompt(1), "0x") && strcmp(gApp.last.detail, "tls") == 0);
	// Another cause is named as its own, with no date; and a first fetch that fails for lack of a
	// network is not called a failed check.
	reset();
	scanAndInstall(url);
	c = add(DL_SINK_FILE, CACERT_URL, false);
	c->detail = "tls";
	c->why = DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION);
	c->tls = DL_TLS_CHECK;
	c->verifyResult = MBEDTLS_X509_BADCERT_CN_MISMATCH;
	answer(KEY_A);
	play();
	assert(strstr(prompt(1), tlsCauseWhy(TLS_CAUSE_HOST)) && !strstr(prompt(1), "date:"));
	reset();
	scanAndInstall(url);
	c = add(DL_SINK_FILE, CACERT_URL, false);
	c->detail = "connect";
	c->why = DOWNLOAD_CURL(CURLE_COULDNT_CONNECT);
	play();
	assert(screen.askedCount == 1 && strcmp(gApp.last.detail, "connect") == 0);
	// And a B in the tail of the list's first fetch on a card with none, after the shipped
	// bundle's check failed: the link is canceled, and no refusal is shown for the check.
	reset();
	scanAndInstall(url);
	listCheckFailsAtB(false);
	// Nothing may follow it: the link is not tried, and a refusal is not shown (a key waits).
	add(DL_SINK_CIA, url, true);
	answer(KEY_A);
	play();
	assert(coursesRun == 1 && cancels == 1 && screen.askedCount == 1 && installOpens == 0);
	assert(!exists(CACERT_PATH) && !exists(CACERT_TEMP));
	assert(strcmp(gApp.last.detail, "canceled") == 0 && gApp.last.result == TRANSFER_CANCELED);
	assert(logged("qr-canceled", TRANSFER_CANCELED, 0));
	balanced();

	// A failed check with no list on the card for it to have run against (an http link sent on to
	// https): the fresh list is the first use's, checked against the shipped bundle.
	reset();
	scanAndInstall("http://h/1.cia");
	checkFails("http://h/1.cia", MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, "http://h/1.cia", true);
	answer(KEY_A);
	play();
	assert(coursesRun == 3 && strcmp(slurp(CACERT_PATH), PEM_OK) == 0);
	assert(logged("qr-ok", 0, CIA_SIZE) && screen.askedCount == 2);

	// A handshake that failed for another reason is not a check to overrule, so there is no
	// question.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	checkFails(url, 0)->tls = DL_TLS_SETUP;
	play();
	assert(screen.askedCount == 1 && coursesRun == 1);
	assert(strcmp(gApp.last.detail, "tls") == 0);
	assert(logged("qr-tls", DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION), 0));
	// A failure that is not TLS at all.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	c = add(DL_SINK_CIA, url, true);
	c->size = c->prefix = 0;
	c->detail = "http";
	c->why = DOWNLOAD_HTTP(404);
	c->http = 404;
	play();
	assert(screen.askedCount == 1 && coursesRun == 1 && logged("qr-http", DOWNLOAD_HTTP(404), 0));

	// An http link that is sent on to https is checked there as an https link is: no list is
	// fetched before it starts, a failed check asks the same A/Y/B, and a Y skips the check for
	// that link only.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("http://h/1.cia\nhttp://h/2.cia");
	checkFails("http://h/1.cia", MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	add(DL_SINK_CIA, "http://h/1.cia", false);
	add(DL_SINK_CIA, "http://h/2.cia", true);
	answer(KEY_Y);
	play();
	assert(coursesRun == 3 && screen.askedCount == 2 && screen.asked[1].screen == UI_QR_CHECK);
	assert(!noticed(UI_QR_BUNDLE) && logLines() == 2);
	// With no list on the card that hop cannot be checked: the transfer ends as an unreadable
	// list, since none was fetched up front, and the person is asked; A gets the list and the
	// link is tried again, checked.
	reset();
	scanAndInstall("http://h/1.cia");
	checkFails("http://h/1.cia", 0)->tls = DL_TLS_BUNDLE;
	add(DL_SINK_FILE, CACERT_URL, false);
	add(DL_SINK_CIA, "http://h/1.cia", true);
	answer(KEY_A);
	play();
	assert(coursesRun == 3 && noticed(UI_QR_BUNDLE) && screen.askedCount == 2);
	assert(strstr(prompt(1), "The certificate list on the card could not be read."));
	assert(strcmp(slurp(CACERT_PATH), PEM_OK) == 0 && logged("qr-ok", 0, CIA_SIZE));

	// The prompt names the link: its number among the links, its site (cut into rows when long,
	// the port with it) and its path, whole, on the top screen; the cause is the bottom screen's,
	// after the \f.
	char host45[64], second[128], two[256], named[512];
	snprintf(host45, sizeof(host45), "%045d", 0);
	snprintf(second, sizeof(second), "https://%s:8443/2.cia", host45);
	snprintf(two, sizeof(two), "https://a.example/1.cia\n%s", second);
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(two);
	add(DL_SINK_CIA, "https://a.example/1.cia", true);
	checkFails(second, MBEDTLS_X509_BADCERT_CN_MISMATCH);
	answer(KEY_B);
	play();
	snprintf(named, sizeof(named), "Link 2 of 2\n%.30s\n%s:8443\n/2.cia\n\f\n%s", host45,
			 host45 + 30, tlsCauseWhy(TLS_CAUSE_HOST));
	assert(screen.askedCount == 2 && strcmp(prompt(1), named) == 0);
	// A single link is named too.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://a.example/x.cia");
	checkFails("https://a.example/x.cia", MBEDTLS_X509_BADCERT_NOT_TRUSTED);
	answer(KEY_B);
	play();
	static const char HEAD[] = "Link 1 of 1\na.example\n/x.cia\n\f\n";
	assert(strncmp(prompt(1), HEAD, sizeof(HEAD) - 1) == 0);
	assert(strstr(prompt(1), tlsCauseWhy(TLS_CAUSE_ISSUER)));

	// The path is named on the prompt under the confirm screen's rules, after the site. The first
	// case is a GitHub release link, whole (behind a login the prompt does not name). The second is
	// a site of 120 characters with a path past 120, shown as its first 90, "...", its last 27, and
	// "?..." for its query; that is the longest top screen there is (267 bytes with two-digit
	// numbers; 265 here).
	char release[400], cut[400];
	snprintf(release, sizeof(release), "https://github.com@evil.example%s", RELEASE);
	namesLink(release,
			  "Link 1 of 2\nevil.example\n/MadeOfBees/3Drop/releases/\ndownload/v1/3drop.cia\n");
	char site120[160];
	snprintf(site120, sizeof(site120), "%0120d", 7);
	snprintf(cut, sizeof(cut), "https://%s%s?token=SECRET", site120, longPath(130));
	snprintf(named, sizeof(named),
			 "Link 1 of 2\n%.30s\n%.30s\n%.30s\n%.30s\n" LONG_HEAD
			 "123...78901234567890123/\n3drop.cia?...\n",
			 site120, site120 + 30, site120 + 60, site120 + 90);
	namesLink(cut, named);
	assert(strlen(named) == 265);
}

static void outcomes(void) {
	const char *url = "https://a.example/x.cia";
	// 3Drop itself is installed, the chainloader is set as the wire's self install sets it, and
	// the caller is told to leave. The rest of the batch is not fetched, and the log says how
	// many were left, as after a refusal.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://a.example/x.cia\nhttps://a.example/y.cia");
	add(DL_SINK_CIA, url, true)->route = &R_SELF;
	assert(play() == AFTER_SELF_UPDATED);
	assert(fake.chainload == fake.selfTitleID && coursesRun == 1 && logLines() == 2);
	assert(logged("qr-ok", 0, CIA_SIZE) && logged("qr-skipped", 0, 1));
	// An update of another title is not a relaunch, and a batch goes on to its next link.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://a.example/x.cia\nhttps://a.example/y.cia");
	add(DL_SINK_CIA, url, true)->route = &R_UPDATE;
	add(DL_SINK_CIA, "https://a.example/y.cia", true);
	assert(play() == AFTER_NOTHING && fake.chainload == 0 && coursesRun == 2);
	// A self install whose commit may or may not have landed is not a success: no relaunch.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	Course *c = add(DL_SINK_CIA, url, true);
	c->route = &R_SELF;
	c->commitUnknown = true;
	assert(play() == AFTER_NOTHING && fake.chainload == 0);
	assert(gApp.last.outcome == OUTCOME_UNKNOWN && gApp.status == SERVER_INSTALL_FAILED);
	assert(logged("qr-am-finish", 0xD8A08009, CIA_SIZE));

	// An install refusal: the download is stopped at the prefix, before a byte of the body, and
	// the refusal's own words stand, not the stop's.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	c = add(DL_SINK_CIA, url, true);
	c->opens = false;
	c->refusal = "not-newer";
	assert(play() == AFTER_NOTHING);
	assert(installOpens == 1 && installCloses == 0 && active == NULL);
	assert(gApp.last.outcome == OUTCOME_REJECTED && strcmp(gApp.last.detail, "not-newer") == 0);
	assert(gApp.last.titleID == TITLE && gApp.last.version == 3);
	assert(logged("qr-not-newer", 0, CIA_PREFIX) && gApp.status == SERVER_INSTALL_FAILED);
	assert(scripted.releases == 1); // the refusal let go of the ring once, not twice
	balanced();
	// The words a person reads for it, from the card.
	char words[160];
	assert(fetchWhy(words, sizeof(words), gApp.last.detail, gApp.last.result));
	assert(strstr(words, "already installed") && !strstr(words, "Raise"));

	// A title with an import that never finished: the refusal's word, and the way out the card
	// names. X on the idle screen runs AM's cleanup of imports that finished and were never
	// committed, which does not clear a pending one, so the card must send the person to
	// uninstall the title.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	c = add(DL_SINK_CIA, url, true);
	c->opens = false;
	c->refusal = "exists-pending";
	assert(play() == AFTER_NOTHING);
	assert(strcmp(gApp.last.detail, "exists-pending") == 0);
	assert(logged("qr-exists-pending", 0, CIA_PREFIX) && gApp.status == SERVER_INSTALL_FAILED);
	assert(fetchWhy(words, sizeof(words), gApp.last.detail, gApp.last.result));
	assert(strcmp(words, "An earlier install of this title did not finish. "
						 "Uninstall the title to clear it.") == 0);

	// An import that opened but whose writer did not start: the download is stopped, and the close
	// is told the body did not come.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	add(DL_SINK_CIA, url, true)->started = false;
	play();
	assert(installOpens == 1 && installCloses == 1 && !closed.received);
	assert(closed.detail && strcmp(closed.detail, "stopped") == 0);
	assert(strcmp(gApp.last.detail, "writer") == 0);

	// A ring that cannot be armed for the declared size: the download is stopped before its prefix.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	add(DL_SINK_CIA, url, true);
	scripted.armOk = false;
	play();
	assert(installOpens == 0 && installCloses == 0 && scripted.releases == 0);
	assert(strcmp(gApp.last.detail, "ring") == 0 && gApp.last.result == OUT_OF_MEMORY);
	assert(logged("qr-ring", OUT_OF_MEMORY, 0) && gApp.wireTotal == 0);

	// With no thread to download on, the failure is reported as such, with nothing to join and no
	// list or ring touched.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	add(DL_SINK_CIA, url, true)->noThread = true;
	assert(play() == AFTER_NOTHING);
	assert(scripted.arms == 0 && installOpens == 0 && joins == 0 && active == NULL);
	assert(strcmp(gApp.last.detail, "thread") == 0 && gApp.last.result == DOWNLOAD_NO_THREAD);
	assert(logged("qr-thread", DOWNLOAD_NO_THREAD, 0));
	reset();
	scanAndInstall(url); // and the list's fetch has none either
	add(DL_SINK_FILE, CACERT_URL, false)->noThread = true;
	play();
	assert(coursesRun == 1 && joins == 0 && !exists(CACERT_PATH));
	assert(strcmp(gApp.last.detail, "thread") == 0 && gApp.last.result == DOWNLOAD_NO_THREAD);

	// The transfer ends before the headers (no route to the server): the ring was never armed.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	c = add(DL_SINK_CIA, url, true);
	c->size = c->prefix = 0;
	c->detail = "connect";
	c->why = DOWNLOAD_CURL(CURLE_COULDNT_CONNECT);
	play();
	assert(scripted.arms == 0 && scripted.releases == 0 && installOpens == 0);
	assert(logged("qr-connect", c->why, 0));
	// After the headers and before the prefix: the armed ring is let go of, once.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	c = add(DL_SINK_CIA, url, true);
	c->prefix = 0;
	c->detail = "net";
	c->why = DOWNLOAD_CURL(CURLE_RECV_ERROR);
	play();
	assert(scripted.arms == 1 && scripted.releases == 1 && installOpens == 0);
	assert(logged("qr-net", c->why, 0));
	// After the import opened: the close takes the transfer's words as the failure's.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	c = add(DL_SINK_CIA, url, true);
	c->detail = "short";
	c->why = TRANSFER_SHORT_READ;
	play();
	assert(installCloses == 1 && !closed.received && strcmp(closed.detail, "short") == 0);
	// Releasing the ring is installClose's job.
	assert(closed.why == TRANSFER_SHORT_READ && scripted.releases == 0);
	assert(logged("qr-short", TRANSFER_SHORT_READ, CIA_PREFIX + 100));
}

// A finished download's body is all in the ring and the writer is inside its first AM call (the
// card is held). The key comes during the joins that time out: the writer ends after that call, so
// fewer bytes reach AM than the body holds; the import is canceled and never finished, the stop is
// sent to the writer once however many looks follow, and the next scan in this process installs a
// fresh body.
static void drainCancel(bool power) {
	const char *url = "https://a.example/x.cia";
	reset();
	realDrain = true;
	joinPower = power;
	joinsPending = power ? 2 : 1;
	fake.cancels = 0;
	fake.cardHeld = true;
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	add(DL_SINK_CIA, url, true)->size = DRAIN_BODY;
	play();
	assert(pendingJoins == (power ? 2u : 1u) && amFinishes == 0 && fake.cancels == 1);
	assert(gApp.last.result == TRANSFER_CANCELED && !fake.homeBlocked);
	assert(ring.thread == NULL && !drainStranded());
	assert(writeCalls == 1 && writtenBytes == (u64)WRITE_UNITS * RING_UNIT);
	assert(writtenBytes < DRAIN_BODY && ringStops == 1);
	// A second scan in this process can arm, join and commit a fresh ring.
	closing = false;
	cam.at = 0;
	answer(KEY_A);
	add(DL_SINK_CIA, url, true);
	play();
	assert(amFinishes == 1 && fake.cancels == 1 && gApp.last.outcome == OUTCOME_COMMITTED);
	assert(writeCalls == 2 && writtenBytes == (u64)WRITE_UNITS * RING_UNIT + CIA_SIZE);
	assert(ringStops == 1 && ring.thread == NULL && !fake.homeBlocked);
	balanced();
}

// The worker's tail: its transfer has returned (ok, every byte in) but DL_FINISHED is not published
// yet. Main's loop reads a B there, so the key's edge is gone by the close: what stops the commit
// is the operation's own cancel, and a list's fetch that ends the same way publishes nothing.
static void lateStop(void) {
	const char *url = "https://a.example/x.cia";
	// A download whose import is open: no Finish, one Cancel, and the next scan still installs.
	reset();
	realDrain = true;
	fake.cancels = 0;
	fake.cardHeld = false;
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	Course *c = add(DL_SINK_CIA, url, true);
	c->slowFinish = true;
	c->keyAfter = 4; // the step that takes the body in: main reads the B on its next look
	play();
	assert(cancels == 1 && gApp.last.outcome == OUTCOME_REJECTED);
	assert(strcmp(gApp.last.detail, "canceled") == 0 && gApp.last.result == TRANSFER_CANCELED);
	assert(amFinishes == 0 && fake.cancels == 1 && installCloses == 1);
	assert(ring.thread == NULL && !drainStranded() && !fake.homeBlocked);
	closing = false;
	cam.at = 0;
	answer(KEY_A);
	add(DL_SINK_CIA, url, true);
	play();
	assert(amFinishes == 1 && gApp.last.outcome == OUTCOME_COMMITTED);
	balanced();

	// The list's fetch, the same tail: no publish, no half list kept, and the link is a cancel.
	reset();
	bundleOn(PEM_OLD);
	remove(CACERT_PATH); // a list that is there is not fetched
	scanAndInstall(url);
	c = add(DL_SINK_FILE, CACERT_URL, false);
	c->slowFinish = true;
	c->keyAfter = 2;			 // the step that writes the body
	add(DL_SINK_CIA, url, true); // and nothing may reach link 1: the fetch was called off
	play();
	assert(!exists(CACERT_PATH) && !exists(CACERT_TEMP));
	assert(coursesRun == 1 && cancels == 1 && installOpens == 0);
	assert(strcmp(gApp.last.detail, "canceled") == 0 && gApp.last.result == TRANSFER_CANCELED);
	assert(logged("qr-canceled", TRANSFER_CANCELED, 0));
	balanced();
}

// The key comes after the download's last look and before the writer's join, at the close's first
// look (qr.c's own): the writer is told at once, so the buffered body never drains into an import
// that is canceled anyway, and nothing is finished.
static void stopAtClose(void) {
	const char *url = "https://a.example/x.cia";
	reset();
	realDrain = keyAtJoin = true;
	fake.cancels = 0;
	fake.cardHeld = true; // the writer waits inside its first AM call until the join
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	add(DL_SINK_CIA, url, true)->size = DRAIN_BODY;
	play();
	assert(amFinishes == 0 && fake.cancels == 1 && ringStops == 1);
	assert(strcmp(gApp.last.detail, "canceled") == 0 && gApp.last.result == TRANSFER_CANCELED);
	assert(writtenBytes <= (u64)WRITE_UNITS * RING_UNIT && writtenBytes < DRAIN_BODY);
	assert(ring.thread == NULL && !drainStranded() && !fake.homeBlocked);
	fake.keysDown = 0;
	balanced();
}

// B (and START, and the app closing) at each point of a download.
static void stopping(void) {
	const char *url = "https://a.example/x.cia";
	// B, START, and the power button (HOME, POWER: the app is closing), each at the three steps the
	// main thread can still stop: with the headers in the thread's hands, at its first stop (the
	// ring is armed, no import is open), and at its second (the import is open).
	static const struct {
		u32 key;
		bool power;
	} KEYS[] = {{KEY_B, false}, {KEY_START, false}, {0, true}};
	for (unsigned k = 0; k < 3; k++) {
		for (unsigned at = 1; at <= 3; at++) {
			reset();
			bundleOn(PEM_OLD);
			scanAndInstall(url);
			Course *c = add(DL_SINK_CIA, url, true);
			c->keyAfter = at;
			c->key = KEYS[k].key;
			c->power = KEYS[k].power;
			play();
			assert(gApp.last.outcome == OUTCOME_REJECTED);
			assert(strcmp(gApp.last.detail, "canceled") == 0 &&
				   gApp.last.result == TRANSFER_CANCELED);
			assert(cancels == 1 && joins == 1 && gApp.status == SERVER_INSTALL_FAILED);
			assert(logged("qr-canceled", TRANSFER_CANCELED, gApp.lastBytes));
			assert(installCloses ==
				   (at == 1 ? 0u : 1u)); // an open import is closed by installClose
			assert(scripted.releases == (at == 1 ? 1u : 0u)); // armed, never opened: let go of
			assert(closed.received == false || at == 1);
		}
	}
	// B after DL_FINISHED but before the commit must still cancel the import.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall(url);
	add(DL_SINK_CIA, url, true)->keyAfter = 4;
	play();
	assert(gApp.last.outcome == OUTCOME_REJECTED);
	assert(strcmp(gApp.last.detail, "canceled") == 0);
	assert(gApp.last.result == TRANSFER_CANCELED);

	// The real writer is pending after DL_FINISHED; B (or the app closing, which stays true at
	// every look) during its timed joins cancels, not commits, and stops the writer early.
	drainCancel(false);
	drainCancel(true);

	// A B read in the loop after the transfer returned, before DL_FINISHED went up: still a cancel.
	lateStop();
	// One read after the download's last look, at the close's first: the writer stops at once.
	stopAtClose();

	// B at the certificate list's fetch ends it and the link, as a cancel and not as a list that
	// could not be saved; a half list is not kept, and the old one stays.
	reset();
	bundleOn(PEM_OLD);
	remove(CACERT_PATH); // (a list that is there is not fetched)
	scanAndInstall(url);
	Course *c = add(DL_SINK_FILE, CACERT_URL, false);
	c->keyAfter = 1;
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 1 && cancels == 1 && !exists(CACERT_PATH) && !exists(CACERT_TEMP));
	assert(strcmp(gApp.last.detail, "canceled") == 0 && gApp.last.result == TRANSFER_CANCELED);
	assert(logged("qr-canceled", TRANSFER_CANCELED, 0));
}

static void batches(void) {
	// Three links, all installed, in the order the code lists them; the screen counts them.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://h/1.cia\nhttps://h/2.cia\nhttps://h/3.cia");
	add(DL_SINK_CIA, "https://h/1.cia", true);
	add(DL_SINK_CIA, "https://h/2.cia", true)->route = &R_UPDATE;
	add(DL_SINK_CIA, "https://h/3.cia", true);
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 3 && stats.resets == 3 && stats.finishes == 3);
	assert(gApp.batchActive && gApp.batchIndex == 3 && gApp.batchTotal == 3);
	assert(gApp.batchInstalled == 2);
	assert(logLines() == 3 && !strstr(logText(), "qr-skipped"));
	balanced();

	// The first refusal stops the rest: they are said to be skipped, with how many.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://h/1.cia\nhttps://h/2.cia\nhttps://h/3.cia\nhttps://h/4.cia");
	add(DL_SINK_CIA, "https://h/1.cia", true);
	Course *c = add(DL_SINK_CIA, "https://h/2.cia", true);
	c->opens = false;
	c->refusal = "exists";
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 2 && logged("qr-ok", 0, CIA_SIZE) && logged("qr-exists", 0, CIA_PREFIX));
	assert(logged("qr-skipped", 0, 2) && logLines() == 3);
	// The last link's refusal skips nothing.
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://h/1.cia\nhttps://h/2.cia");
	add(DL_SINK_CIA, "https://h/1.cia", true);
	c = add(DL_SINK_CIA, "https://h/2.cia", true);
	c->opens = false;
	c->refusal = "exists";
	play();
	assert(!strstr(logText(), "qr-skipped") && logLines() == 2);

	// A ring left holding a stranded worker (a close that could not join it) ends the batch before
	// the next link, with no refusal to log; one stranded before the first link starts none.
	reset();
	bundleOn(PEM_OLD);
	scripted.strandedAfterFirst = true;
	scanAndInstall("https://h/1.cia\nhttps://h/2.cia");
	add(DL_SINK_CIA, "https://h/1.cia", true);
	play();
	assert(coursesRun == 1 && logLines() == 1 && logged("qr-ok", 0, CIA_SIZE));
	reset();
	bundleOn(PEM_OLD);
	scripted.stranded = true;
	scanAndInstall("https://h/1.cia");
	play();
	assert(coursesRun == 0 && logLines() == 0);

	// The library that could not start says so once, and plain http goes on without it.
	reset();
	initResult = DOWNLOAD_NO_TLS;
	scanAndInstall("http://h/1.cia\nhttp://h/2.cia");
	add(DL_SINK_CIA, "http://h/1.cia", true);
	add(DL_SINK_CIA, "http://h/2.cia", true);
	play();
	assert(downloadInits == 1 && coursesRun == 2 && logged("qr-tls-init", DOWNLOAD_NO_TLS, 0));
	assert(logLines() == 3);
	// A second scan in the same process finds the libraries up; the app's exit stops them, once.
	code("http://h/3.cia");
	cam.at = 0;
	answer(KEY_A);
	add(DL_SINK_CIA, "http://h/3.cia", true);
	play();
	assert(downloadInits == 1 && coursesRun == 3 && downloadExits == 0);
	qrExit();
	qrExit();
	assert(downloadExits == 1);
	// And a console that never scanned has nothing to stop.
	reset();
	qrExit();
	assert(downloadExits == 0 && downloadInits == 0);

	// With sslc down and a link that needs https, the result is a setup failure in its own words,
	// before the list is fetched or a download started, and with no question a Y could skip. It
	// stops the rest.
	reset();
	initResult = DOWNLOAD_NO_TLS;
	scanAndInstall("https://h/1.cia\nhttp://h/2.cia");
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 0 && !exists(CACERT_PATH) && screen.askedCount == 1);
	assert(strcmp(gApp.last.detail, "no-tls") == 0 && gApp.last.result == DOWNLOAD_NO_TLS);
	assert(logged("qr-no-tls", DOWNLOAD_NO_TLS, 0) && logged("qr-skipped", 0, 1));
	char noTls[160];
	assert(fetchWhy(noTls, sizeof(noTls), "no-tls", DOWNLOAD_NO_TLS));
	assert(strstr(noTls, "SSL service") && strstr(noTls, "Plain http links still work"));
	// With a list on the card: the same, and nothing is downloaded.
	reset();
	initResult = DOWNLOAD_NO_TLS;
	bundleOn(PEM_OLD);
	scanAndInstall("https://h/1.cia");
	play();
	assert(coursesRun == 0 && strcmp(gApp.last.detail, "no-tls") == 0);
	assert(!strstr(logText(), "qr-skipped") && screen.askedCount == 1);
}

#if TLS_ALLOW_PLAIN_HTTP
int main(void) {
	scanning();
	confirming();
	oneHttps();
	certificateList();
	failedCheck();
	outcomes();
	stopping();
	batches();
	TEST_PASS();
	return 0;
}
#else
// The build with plain http off: a plain link is refused, as "plain-http" and before the network
// or the list is touched; an https link goes on as before.
int main(void) {
	// The rest of the file is for the build that allows plain http. The functions are named
	// here, so none is unused.
	void (*const unused[])(void) = {scanning,	 confirming, oneHttps, certificateList,
									failedCheck, outcomes,	 stopping, batches};
	(void)unused;
	reset();
	scanAndInstall("http://h/1.cia\nhttps://h/2.cia");
	assert(play() == AFTER_NOTHING);
	assert(coursesRun == 0 && !exists(CACERT_PATH));
	assert(strcmp(gApp.last.detail, "plain-http") == 0 && gApp.last.outcome == OUTCOME_REJECTED);
	assert(logged("qr-plain-http", 0, 0) && gApp.status == SERVER_INSTALL_FAILED);
	assert(logged("qr-skipped", 0, 1)); // a refusal stops the rest
	reset();
	bundleOn(PEM_OLD);
	scanAndInstall("https://h/2.cia");
	add(DL_SINK_CIA, "https://h/2.cia", true);
	play();
	assert(coursesRun == 1 && logged("qr-ok", 0, CIA_SIZE));
	// The words for it.
	char words[120];
	assert(fetchWhy(words, sizeof(words), "plain-http", 0) && strstr(words, "turned off"));
	TEST_PASS();
	return 0;
}
#endif

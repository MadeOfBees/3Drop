// cc: -Dmalloc=testMalloc -Dcalloc=testCalloc -Dfree=testFree src/scan.c src/quircLib.c -lm
// Tests src/scan.c and the quirc it links, against a scripted camera. It covers the sensor's
// settings and the order they go in, a QR code drawn into a 400x240 RGB565 frame and decoded
// through the grey conversion the device uses, the two buffers taking turns, a camera that drops a
// frame, every failure of the open, and a close that leaves nothing held.
#include "fakes.h"

#include "qrFixtures.h"
#include "scan.h"

#include <assert.h>
#include <stdlib.h>

// ---- Allocations: scan.c's and quirc's go through these, so a leak or a failed one shows ----
#undef malloc
#undef calloc
#undef free
// stdlib.h declared the renamed functions, so these declarations are libc's own.
void *malloc(size_t n);
void *calloc(size_t count, size_t n);
void free(void *p);
static long live;
static unsigned failIn; // the Nth allocation from now fails; 0 means none
void *testMalloc(size_t n) {
	if (failIn != 0 && --failIn == 0) {
		return NULL;
	}
	void *p = malloc(n);
	live += p != NULL;
	return p;
}
void *testCalloc(size_t count, size_t n) {
	if (failIn != 0 && --failIn == 0) {
		return NULL;
	}
	void *p = calloc(count, n);
	live += p != NULL;
	return p;
}
void testFree(void *p) {
	live -= p != NULL;
	free(p);
}

// ---- The camera: each call is written down, and the one named in failOn fails ----
#define TRANSFER_UNIT 0x2000
static char calls[1024];
static const char *failOn;
static const Result FAILURE = (Result)0xC8A04001;
// What a refused event call leaves in its out word. libctru writes the word out whatever CAM
// answered (cam.c:88-112), so it is not a handle, and closing it would close someone else's.
static const Handle POISON = 0x0BAD;
static Handle nextHandle, receivedHandle, failedHandle, closed[16];
static unsigned closes, sleeps, busyPolls;
static void *armedDst;
static bool frameReady, bufferError, busyForever;
static s64 lastWait;
static unsigned armed;

static Result step(const char *what) {
	size_t n = strlen(calls);
	snprintf(calls + n, sizeof(calls) - n, "%s ", what);
	return failOn && strcmp(failOn, what) == 0 ? FAILURE : 0;
}
Result camInit(void) {
	return step("camInit");
}
void camExit(void) {
	step("camExit");
}
Result CAMU_SetSize(u32 select, CAMU_Size size, CAMU_Context context) {
	assert(select == SELECT_OUT1 && size == SIZE_CTR_TOP_LCD && context == CONTEXT_A);
	return step("size");
}
Result CAMU_SetOutputFormat(u32 select, CAMU_OutputFormat format, CAMU_Context context) {
	assert(select == SELECT_OUT1 && format == OUTPUT_RGB_565 && context == CONTEXT_A);
	return step("format");
}
Result CAMU_SetFrameRate(u32 select, CAMU_FrameRate frameRate) {
	assert(select == SELECT_OUT1 && frameRate == FRAME_RATE_30);
	return step("rate");
}
Result CAMU_SetNoiseFilter(u32 select, bool noiseFilter) {
	assert(select == SELECT_OUT1 && noiseFilter);
	return step("noise");
}
Result CAMU_SetAutoExposure(u32 select, bool autoExposure) {
	assert(select == SELECT_OUT1 && autoExposure);
	return step("exposure");
}
Result CAMU_SetAutoWhiteBalance(u32 select, bool autoWhiteBalance) {
	assert(select == SELECT_OUT1 && autoWhiteBalance);
	return step("white");
}
Result CAMU_SetPhotoMode(u32 select, CAMU_PhotoMode photoMode) {
	assert(select == SELECT_OUT1 && photoMode == PHOTO_MODE_LETTER); // Universal Updater uses it
	return step("photo");
}
// A console's camera refuses Universal Updater's sharpness, 127, as out of range (0xE0E053FD), so
// scan.c asks for none. A call that came back fails the open here as it does on a console.
Result CAMU_SetSharpness(u32 select, s8 sharpness) {
	(void)select;
	step("sharp");
	return sharpness == 127 ? (Result)0xE0E053FD : 0;
}
Result CAMU_Activate(u32 select) {
	assert(select == SELECT_NONE || select == SELECT_OUT1); // the outer camera, or none
	return step(select == SELECT_NONE ? "off" : "on");
}
Result CAMU_GetBufferErrorInterruptEvent(Handle *event, u32 port) {
	assert(port == PORT_CAM1);
	Result r = step("errorEvent");
	failedHandle = r ? 0 : nextHandle++;
	*event = r ? POISON : failedHandle;
	return r;
}
Result CAMU_SetTrimming(u32 port, bool trimming) {
	assert(port == PORT_CAM1 && !trimming);
	return step("trim");
}
Result CAMU_GetMaxBytes(u32 *maxBytes, s16 width, s16 height) {
	assert(width == SCAN_WIDTH && height == SCAN_HEIGHT);
	*maxBytes = TRANSFER_UNIT;
	return step("maxBytes");
}
Result CAMU_SetTransferBytes(u32 port, u32 bytes, s16 width, s16 height) {
	assert(port == PORT_CAM1 && bytes == TRANSFER_UNIT && width == SCAN_WIDTH &&
		   height == SCAN_HEIGHT);
	return step("transfer");
}
Result CAMU_ClearBuffer(u32 port) {
	assert(port == PORT_CAM1);
	return step("clear");
}
Result CAMU_SetReceiving(Handle *event, void *dst, u32 port, u32 imageSize, s16 transferUnit) {
	assert(port == PORT_CAM1 && imageSize == SCAN_FRAME_BYTES && transferUnit == TRANSFER_UNIT);
	Result r = step("receive");
	if (r == 0) {
		*event = receivedHandle = nextHandle++;
		armedDst = dst;
		armed++;
	} else {
		*event = POISON;
	}
	return r;
}
Result CAMU_StartCapture(u32 port) {
	assert(port == PORT_CAM1);
	return step("start");
}
Result CAMU_StopCapture(u32 port) {
	assert(port == PORT_CAM1);
	return step("stop");
}
Result CAMU_IsBusy(bool *busy, u32 port) {
	assert(port == PORT_CAM1);
	*busy = busyForever;
	// Only the first poll is written down, because a stuck camera is asked a thousand times.
	return busyPolls++ == 0 ? step("busy") : 0;
}
// Waiting on a handle that signaled returns 0; anything else is libctru's time-out, which is a
// success code.
Result svcWaitSynchronization(Handle handle, s64 nanoseconds) {
	lastWait = nanoseconds;
	if (handle == receivedHandle && handle != 0 && frameReady) {
		return 0;
	}
	if (handle == failedHandle && handle != 0 && bufferError) {
		return 0;
	}
	return (Result)0x09401BFE;
}
Result svcCloseHandle(Handle handle) {
	assert(closes < sizeof(closed) / sizeof(closed[0]));
	closed[closes++] = handle;
	return 0;
}
void svcSleepThread(s64 ns) {
	assert(ns == 1000000); // a millisecond at a time, a thousand of them at most
	sleeps++;
}

static bool wasClosed(Handle handle) {
	for (unsigned i = 0; i < closes; i++) {
		if (closed[i] == handle) {
			return true;
		}
	}
	return false;
}

static void reset(void) {
	scanClose();
	calls[0] = '\0';
	failOn = NULL;
	nextHandle = 0x100;
	receivedHandle = failedHandle = 0;
	closes = sleeps = busyPolls = armed = 0;
	armedDst = NULL;
	frameReady = bufferError = busyForever = false;
	failIn = 0;
}

// The camera gives a frame: the armed buffer is written, and its receive event signals.
static void camera(const u16 *frame) {
	assert(armedDst && !frameReady);
	memcpy(armedDst, frame, SCAN_FRAME_BYTES);
	frameReady = true;
}
// Whatever the camera makes of the signaled receive, the next arming is a new event.
static ScanStep poll(void) {
	ScanStep step = scanPoll(7000000);
	frameReady = false;
	return step;
}

// ---- A scene: a lamp-lit wall, and a code printed on a pale sheet, as a phone would show one ----
typedef struct {
	int scale, left, top; // pixels to a module, and where the sheet's corner is
	bool mirror, invert;
	int noise; // each pixel's level moves by up to this much
} Scene;

static u16 rgb565(unsigned level) {
	return (u16)((level >> 3) << 11 | (level >> 2) << 5 | level >> 3);
}

static void render(u16 *frame, const char *const *rows, int modules, const Scene *scene) {
	unsigned seed = 20260102u;
	int sheet = (modules + 8) * scene->scale; // four modules of quiet zone all around
	for (int y = 0; y < SCAN_HEIGHT; y++) {
		for (int x = 0; x < SCAN_WIDTH; x++) {
			int level = 120 + x * 40 / SCAN_WIDTH + y * 20 / SCAN_HEIGHT; // the wall
			int sx = x - scene->left, sy = y - scene->top;
			if (rows && sx >= 0 && sy >= 0 && sx < sheet && sy < sheet) {
				int mx = sx / scene->scale - 4, my = sy / scene->scale - 4;
				bool dark = false;
				if (mx >= 0 && my >= 0 && mx < modules && my < modules) {
					dark = rows[my][scene->mirror ? modules - 1 - mx : mx] == '#';
				}
				dark = dark != scene->invert;
				level = dark ? 45 : 225;
			}
			seed = seed * 1664525u + 1013904223u;
			if (scene->noise) {
				level += (int)(seed >> 24) % (2 * scene->noise + 1) - scene->noise;
			}
			frame[y * SCAN_WIDTH + x] = rgb565((unsigned)(level < 0		? 0
														  : level > 255 ? 255
																		: level));
		}
	}
}

static u16 frame[SCAN_WIDTH * SCAN_HEIGHT];

// A second sheet on the wall: only its own square of the picture is taken over.
static void overlay(u16 *dst, const char *const *rows, int modules, const Scene *scene) {
	static u16 other[SCAN_WIDTH * SCAN_HEIGHT];
	render(other, rows, modules, scene);
	int sheet = (modules + 8) * scene->scale;
	for (int y = scene->top; y < scene->top + sheet && y < SCAN_HEIGHT; y++) {
		for (int x = scene->left; x < scene->left + sheet && x < SCAN_WIDTH; x++) {
			dst[y * SCAN_WIDTH + x] = other[y * SCAN_WIDTH + x];
		}
	}
}

// ---- The grey quirc is given ----
static void grey(void) {
	// The mean of the three channels at their own depths: 5 bits of red and blue and 6 of green,
	// each scaled to 8 (x8, x4, x8), as FBI's loop does it.
	static const struct {
		u16 pixel;
		u8 grey;
	} CASES[] = {
		{0x0000, 0},
		{0xFFFF, (31 * 8 + 63 * 4 + 31 * 8) / 3}, // 249
		{0xF800, 31 * 8 / 3},					  // red alone
		{0x07E0, 63 * 4 / 3},					  // green alone
		{0x001F, 31 * 8 / 3},					  // blue alone
		{0x8410, (16 * 8 + 32 * 4 + 16 * 8) / 3}, // middle grey: 128
		{0x0821, (1 * 8 + 1 * 4 + 1 * 8) / 3},	  // one step of each
	};
	u16 in[sizeof(CASES) / sizeof(CASES[0])];
	u8 out[sizeof(in) / sizeof(in[0])];
	for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
		in[i] = CASES[i].pixel;
	}
	memset(out, 0xEE, sizeof(out));
	scanGrey(out, in, sizeof(in) / sizeof(in[0]));
	for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
		assert(out[i] == CASES[i].grey);
	}
	// Exactly the pixels asked for are converted.
	memset(out, 0xEE, sizeof(out));
	scanGrey(out, in, 3);
	assert(out[3] == 0xEE);
}

// ---- The open: every setting, in an order that works, and every way it can fail ----
static void opens(void) {
	reset();
	assert(scanOpen() == 0);
	assert(strcmp(calls, "camInit size format rate noise exposure white photo on errorEvent "
						 "trim maxBytes transfer clear receive start ") == 0);
	assert(armed == 1 && armedDst);
	assert(scanResult() == 0);
	scanClose();

	// When any one step fails, the open fails with that step's Result, the camera is released, and
	// nothing allocated is left behind.
	static const char *const STEPS[] = {
		"camInit", "size",		 "format", "rate",	   "noise",	   "exposure", "white",	  "photo",
		"on",	   "errorEvent", "trim",   "maxBytes", "transfer", "clear",	   "receive", "start"};
	for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
		reset();
		long before = live;
		failOn = STEPS[i];
		assert(scanOpen() == FAILURE);
		assert(live == before);
		bool started = i > 0; // camInit failing started nothing to stop
		assert((strstr(calls, "camExit") != NULL) == started);
		assert((strstr(calls, "off") != NULL) == started);
		// A capture that never started is not stopped, and no call follows the failing one except
		// the camera's release.
		char tail[64];
		snprintf(tail, sizeof(tail), "%s%s", STEPS[i], started ? " off camExit " : " ");
		size_t callsLen = strlen(calls), tailLen = strlen(tail);
		assert(strstr(calls, "stop") == NULL);
		assert(callsLen >= tailLen && strcmp(calls + callsLen - tailLen, tail) == 0);
		assert(scanPoll(0) == SCAN_BROKEN);
		scanClose();				// twice
		assert(!wasClosed(POISON)); // the word a refused event call left is never closed
	}

	// Memory: if any one allocation fails, the open returns OUT_OF_MEMORY and the camera is not
	// even started.
	for (unsigned n = 1; n <= 8; n++) {
		reset();
		long before = live;
		failIn = n;
		Result r = scanOpen();
		if (failIn == 0) { // the Nth allocation was this open's own, and it failed
			assert(r == OUT_OF_MEMORY && live == before && strstr(calls, "camInit") == NULL);
		} else {
			assert(r == 0); // fewer allocations than n
			scanClose();
			assert(live == before);
		}
	}
	reset();
}

// ---- The decode: a code in a frame, through the conversion, to its payload ----
static void decodes(void) {
	static const struct {
		const char *const *rows;
		int modules;
		const char *payload;
	} CODES[] = {
		{QR_ONE, QR_ONE_MODULES, QR_ONE_PAYLOAD},
		{QR_TWO, QR_TWO_MODULES, QR_TWO_PAYLOAD},
		{QR_TEXT, QR_TEXT_MODULES, QR_TEXT_PAYLOAD},
	};
	static const Scene SCENES[] = {
		{5, 100, 15, false, false, 0}, // clean, mid frame
		{4, 12, 9, false, false, 0},   // small, to a corner
		{5, 150, 20, false, false, 9}, // a camera's noise
		{3, 200, 60, false, false, 4}, // far off, at three pixels a module
	};
	for (size_t c = 0; c < sizeof(CODES) / sizeof(CODES[0]); c++) {
		for (size_t s = 0; s < sizeof(SCENES) / sizeof(SCENES[0]); s++) {
			reset();
			assert(scanOpen() == 0);
			render(frame, CODES[c].rows, CODES[c].modules, &SCENES[s]);
			camera(frame);
			void *decoded = armedDst;
			assert(poll() == SCAN_FOUND);
			size_t length = 0;
			const u8 *payload = scanPayload(&length);
			assert(length == strlen(CODES[c].payload) &&
				   memcmp(payload, CODES[c].payload, length) == 0);
			// The frame in hand is the one the camera wrote, and the camera now writes the other.
			assert(scanFrame() != NULL && memcmp(scanFrame(), frame, SCAN_FRAME_BYTES) == 0);
			assert(armed == 2 && armedDst != decoded && scanFrame() == decoded);
			assert(lastWait == 7000000); // the wait asked for is the wait made
			scanClose();
		}
	}
}

// Two codes in a frame, one with its data inverted past any repair (its three corners are whole, so
// quirc finds it). Whichever quirc tries first, the one that decodes is the answer.
static void twoCodes(void) {
	char damaged[QR_ONE_MODULES][QR_ONE_MODULES + 1];
	const char *rows[QR_ONE_MODULES];
	for (int y = 0; y < QR_ONE_MODULES; y++) {
		for (int x = 0; x < QR_ONE_MODULES; x++) {
			bool corner = (x < 9 && y < 9) || (x >= QR_ONE_MODULES - 8 && y < 9) ||
						  (x < 9 && y >= QR_ONE_MODULES - 8);
			bool dark = QR_ONE[y][x] == '#';
			damaged[y][x] = dark != !corner ? '#' : '.';
		}
		damaged[y][QR_ONE_MODULES] = '\0';
		rows[y] = damaged[y];
	}
	for (int swap = 0; swap < 2; swap++) {
		reset();
		assert(scanOpen() == 0);
		const Scene bad = {4, swap ? 205 : 5, 40, false, false, 0};
		const Scene good = {4, swap ? 5 : 205, 40, false, false, 0};
		render(frame, rows, QR_ONE_MODULES, &bad);
		overlay(frame, QR_TWO, QR_TWO_MODULES, &good);
		camera(frame);
		assert(poll() == SCAN_FOUND);
		size_t length = 0;
		const u8 *payload = scanPayload(&length);
		assert(length == strlen(QR_TWO_PAYLOAD) && memcmp(payload, QR_TWO_PAYLOAD, length) == 0);
		scanClose();
	}
	reset();
}

static void noCode(void) {
	reset();
	assert(scanOpen() == 0);
	static const Scene PLAIN = {5, 100, 15, false, false, 0};
	static const Scene NOISY = {5, 100, 15, false, false, 30};
	size_t length = 99;
	// A wall; a wall with a camera's worst noise; a code cut off by the frame's edge; a code with
	// its dark and light swapped; and one read in a mirror. quirc reads none of them, and the
	// payload of a frame with no code is empty.
	render(frame, NULL, 0, &PLAIN);
	camera(frame);
	assert(poll() == SCAN_FRAME);
	assert(scanPayload(&length) != NULL && length == 0);
	render(frame, NULL, 0, &NOISY);
	camera(frame);
	assert(poll() == SCAN_FRAME);
	static const Scene CUT = {5, 280, 15, false, false, 0};
	render(frame, QR_ONE, QR_ONE_MODULES, &CUT);
	camera(frame);
	assert(poll() == SCAN_FRAME);
	static const Scene INVERTED = {5, 100, 15, false, true, 0};
	render(frame, QR_ONE, QR_ONE_MODULES, &INVERTED);
	camera(frame);
	assert(poll() == SCAN_FRAME);
	static const Scene MIRRORED = {5, 100, 15, true, false, 0};
	render(frame, QR_ONE, QR_ONE_MODULES, &MIRRORED);
	camera(frame);
	assert(poll() == SCAN_FRAME);
	assert(scanPayload(&length) != NULL && length == 0);
	// And the code on the next frame is read, which shows that nothing was left over from the
	// failures.
	static const Scene GOOD = {5, 100, 15, false, false, 0};
	render(frame, QR_ONE, QR_ONE_MODULES, &GOOD);
	camera(frame);
	assert(poll() == SCAN_FOUND);
	scanPayload(&length);
	assert(length == strlen(QR_ONE_PAYLOAD));
	// And that length does not carry over to the next frame: a frame with no code has none.
	render(frame, NULL, 0, &PLAIN);
	camera(frame);
	assert(poll() == SCAN_FRAME);
	assert(scanPayload(&length) != NULL && length == 0);
	scanClose();
	reset();
}

// ---- The two buffers take turns, and a frame is never read while the camera writes it ----
static void turns(void) {
	reset();
	assert(scanOpen() == 0);
	static const Scene SCENE = {5, 100, 15, false, false, 0};
	render(frame, QR_ONE, QR_ONE_MODULES, &SCENE);
	void *seen[6];
	for (int i = 0; i < 6; i++) {
		void *writing = armedDst;
		camera(frame);
		assert(poll() == SCAN_FOUND);
		seen[i] = writing;
		assert(scanFrame() == writing &&
			   armedDst != writing); // the camera never gets the one in hand
	}
	assert(seen[0] == seen[2] && seen[2] == seen[4] && seen[1] == seen[3] && seen[3] == seen[5] &&
		   seen[0] != seen[1]);
	assert(armed == 7); // one from the open, and one after each frame
	// Each frame's event is closed once it is taken, and no other is.
	assert(closes == 6);
	scanClose();
}

// ---- Nothing yet, a camera that drops a frame, and one that stops ----
static void quiet(void) {
	reset();
	assert(scanOpen() == 0);
	// If no frame arrives during the wait, the poll is idle and nothing is armed or closed.
	assert(scanPoll(1234567) == SCAN_IDLE && lastWait == 0 /* the error event's turn */);
	assert(armed == 1 && closes == 0);
	size_t length = 5;
	assert(scanPayload(&length) != NULL && length == 0);

	// A dropped frame: the buffer error event signals. The old receive is closed, the buffer is
	// cleared, a receive is armed again into the same buffer, and the capture starts again.
	void *dst = armedDst;
	Handle old = receivedHandle;
	calls[0] = '\0';
	bufferError = true;
	assert(poll() == SCAN_IDLE);
	assert(strcmp(calls, "clear receive start ") == 0);
	assert(wasClosed(old) && armed == 2 && armedDst == dst);
	assert(scanResult() == 0);
	// Frames come again after it.
	bufferError = false;
	static const Scene SCENE = {5, 100, 15, false, false, 0};
	render(frame, QR_ONE, QR_ONE_MODULES, &SCENE);
	camera(frame);
	assert(poll() == SCAN_FOUND);

	// Each step of the recovery failing ends the scan. It is broken with that step's Result, and
	// broken for good.
	static const char *const RECOVERY[] = {"clear", "receive", "start"};
	for (size_t i = 0; i < 3; i++) {
		bufferError = true;
		failOn = RECOVERY[i];
		assert(poll() == SCAN_BROKEN && scanResult() == FAILURE);
		failOn = NULL;
		assert(scanPoll(0) == SCAN_BROKEN);
		scanClose();
		assert(scanOpen() == 0);
	}
	bufferError = false;

	// If the re-arm after a frame fails, the frame is dropped and the scan is broken.
	render(frame, QR_ONE, QR_ONE_MODULES, &SCENE);
	camera(frame);
	failOn = "receive";
	assert(poll() == SCAN_BROKEN && scanResult() == FAILURE);
	failOn = NULL;
	assert(scanPoll(0) == SCAN_BROKEN);
	scanClose();
	assert(!wasClosed(POISON)); // the words refused receives left, in the recovery and the re-arm
	reset();
}

// ---- The close: the capture stopped, the camera off, every handle and buffer given back ----
static void closing(void) {
	reset();
	long before = live;
	assert(scanOpen() == 0);
	Handle received = receivedHandle, failed = failedHandle;
	assert(live > before);
	calls[0] = '\0';
	scanClose();
	assert(strcmp(calls, "stop busy clear off camExit ") == 0);
	assert(wasClosed(received) && wasClosed(failed) && live == before);
	// Closing twice, and polling after that, asks nothing more of the camera.
	calls[0] = '\0';
	scanClose();
	assert(calls[0] == '\0' && scanPoll(0) == SCAN_BROKEN);

	// A capture that is slow to stop is waited for, but not forever.
	reset();
	assert(scanOpen() == 0);
	busyForever = true;
	calls[0] = '\0';
	scanClose();
	assert(busyPolls == 1000 && sleeps == 1000);
	assert(strcmp(calls, "stop busy clear off camExit ") == 0); // and then it goes on
	busyForever = false;
	reset();
	assert(scanOpen() == 0);
	scanClose();
	assert(busyPolls == 1 && sleeps == 0);

	// The capture of a broken scan is stopped like any other, because a recovery that failed, or a
	// re-arm after a frame that failed, stops nothing itself. Then the camera is turned off and
	// everything is given back.
	reset();
	assert(scanOpen() == 0);
	bufferError = true;
	failOn = "clear";
	assert(poll() == SCAN_BROKEN);
	failOn = NULL;
	bufferError = false;
	calls[0] = '\0';
	scanClose();
	assert(strcmp(calls, "stop busy clear off camExit ") == 0 && live == before);
	reset();
	assert(scanOpen() == 0);
	camera(frame);
	failOn = "receive";
	assert(poll() == SCAN_BROKEN);
	failOn = NULL;
	calls[0] = '\0';
	scanClose();
	assert(strcmp(calls, "stop busy clear off camExit ") == 0 && live == before);
	reset();
}

int main(void) {
	grey();
	opens();
	decodes();
	noCode();
	twoCodes();
	turns();
	quiet();
	closing();
	TEST_PASS();
	return 0;
}

// cc: src/codec.c src/stats.c src/urls.c
// Tests src/ui.c against a recording citro2d. It covers the QR and link cards' copy, the sentences
// that name a remedy (`outcomes`), every screen's fit and printability, the install card and batch
// line across a later request, the redraw signature, the progress tick's cadence, the rate/ETA
// sampler, the BMP encoder's key blackout, and capture-on-change.
#include "fakes.h"
#include "stats.h"

#include "bench/capture.c"
#include "screenshot.c"
#include "ui.c"

#include <assert.h>
#include <malloc.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>

// Provides tier 1 the way ring.c lends it; nothing else of the ring runs here.
u8 *ringTier1(void) {
	static u8 *tier1;
	if (!tier1) {
		tier1 = memalign(0x1000, RING_TIER1_BYTES);
	}
	return tier1;
}
static bool strandedNow; // true when a worker never came back; the capture checks set it
bool ringStranded(void) {
	return strandedNow;
}

static unsigned glyphs, densest, syncFrames, tryFrames;
static char displayKey[KEY_MAX + 1];
const char *authDisplayKey(void) {
	return displayKey;
}
static bool gpuBusy;
static bool gpuRefuses; // refuses every frame, even one that waits
static unsigned regens;
bool authRegenerateKey(void) {
	regens++;
	return false;
}

// The text each draw was given, in the order of the frame. ui.c breaks a wrapped text into rows
// before it parses it, so these are the rows the person reads.
static char drawnRows[12][TEXT_MAX];
static float drawnScale[12];
static unsigned drawnCount;
static unsigned measures; // the rows ui.c parsed to measure, not to draw
// A string is as wide as ten pixels for each character of its longest line.
const char *C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *str) {
	glyphs += buf == textBuf ? (unsigned)strlen(str) : 0; // ui.c's measuring buffer draws nothing
	measures += buf != textBuf;
	size_t widest = 0;
	for (const char *line = str; line;) {
		size_t length = strcspn(line, "\n");
		widest = MAX(widest, length);
		line = line[length] ? line + length + 1 : NULL;
	}
	text->width = 10.0f * (float)widest;
	text->str = str;
	return str + strlen(str);
}
// Counts each draw that shows the pairing key, and each whose cell leaves the rows a capture blacks
// out. citro2d puts a cell's top at most 30 px (times the scale) above the baseline. A quarter cell
// below the baseline is allowed for descenders, because the system font's own metric is unmeasured.
static unsigned keyDraws, keyOutsideBand;
void C2D_DrawText(const C2D_Text *text, u32 flags, float x, float y, float depth, float scaleX,
				  float scaleY, ...) {
	assert(!(flags & C2D_WordWrap) || !measureBuf); // ui.c breaks rows while it can measure them
	if (drawnCount < sizeof(drawnRows) / sizeof(drawnRows[0])) {
		snprintf(drawnRows[drawnCount], sizeof(drawnRows[0]), "%s", text->str);
		drawnScale[drawnCount++] = scaleX;
	}
	if (displayKey[0] && strstr(text->str, displayKey)) {
		keyDraws++;
		keyOutsideBand += !(flags & C2D_AtBaseline) || y - 30.0f * scaleY < (float)UI_KEY_TOP ||
						  y + 7.5f * scaleY > (float)UI_KEY_END;
	}
}
// Records the scan's picture as the top screen draws it: how often, and what the texture held.
static unsigned imageDraws;
static C2D_Image lastImage;
static u16 texelAtBegin;
static bool textureAtBegin;
bool C3D_FrameBegin(u8 flags) {
	glyphs = 0;
	drawnCount = 0;
	tryFrames += flags == C3D_FRAME_NONBLOCK;
	syncFrames += flags == C3D_FRAME_SYNCDRAW;
	// What the GPU may still be reading when a frame begins must not be touched until it has
	// finished. Record the first texel as it is at this moment, and whether the scan's texture was
	// still there.
	texelAtBegin = previewTex.data ? ((u16 *)previewTex.data)[0] : 0;
	textureAtBegin = previewTex.data != NULL;
	return !gpuRefuses && (flags != C3D_FRAME_NONBLOCK || !gpuBusy);
}
bool C2D_DrawImageAt(C2D_Image img, float x, float y, float depth, const void *tint, float scaleX,
					 float scaleY) {
	imageDraws++;
	lastImage = img;
	assert(x == 0.0f && y == 0.0f && scaleX == 1.0f && scaleY == 1.0f);
	return true;
}
void C3D_FrameEnd(u8 flags) {
	densest = glyphs > densest ? glyphs : densest;
}

static u8 framebuffer[240 * 8 * 3]; // a screen up to eight columns wide: two, except for one frame
static u16 grabHeight = 2;
u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height) {
	*width = 240;
	*height = grabHeight;
	return framebuffer;
}
static void captureChecks(bool key) {
	Frame frame;
	u8 bmp[54 + 240 * 8];
	assert(frameGrab(&frame) && bmpEncode(bmp, &frame, 0) == sizeof(bmp));
	for (unsigned r = 0; r < 240; r++) {
		bool band = 239 - r >= UI_KEY_TOP && 239 - r < UI_KEY_END;
		assert(bmp[54 + r * 8] == (key && band ? 0 : 0xFF));
	}
}

// Checks every framebuffer shape: raw row r of column x is BMP row r, pixel x, with rows padded to
// 4 bytes.
static void bmpTruth(void) {
	static u8 fb[240 * 800 * 3], bmp[54 + 240 * 800 * 3];
	for (size_t i = 0; i < sizeof(fb); i++) {
		fb[i] = (u8)(i * 7 + i / 3);
	}
	static const u16 WIDTHS[] = {320, 400, 800};
	for (int w = 0; w < 3; w++) {
		Frame frame = {.fb = {fb, fb}, .w = {240, 240}, .h = {WIDTHS[w], WIDTHS[w]}};
		u32 stride = (WIDTHS[w] * 3u + 3) & ~3u, size = 54 + stride * 240;
		assert(bmpSize(&frame, 1) == size && bmpEncode(bmp, &frame, 1) == size);
		assert(bmp[0] == 'B' && bmp[1] == 'M' && bmp[2] == (u8)size && bmp[10] == 54 &&
			   bmp[18] == (u8)WIDTHS[w] && bmp[19] == WIDTHS[w] >> 8 && bmp[22] == 240 &&
			   bmp[28] == 24);
		for (u32 r = 0; r < 240; r += 97) {
			for (u32 x = 0; x < WIDTHS[w]; x += 131) {
				assert(memcmp(&bmp[54 + r * stride + x * 3], &fb[(x * 240 + r) * 3], 3) == 0);
			}
		}
	}
}
static int captured(const char *name) {
	char path[96];
	snprintf(path, sizeof(path), CAPTURE_DIR "/%s-top.bmp", name);
	FILE *file = fopen(path, "rb");
	int byte = file && fseek(file, 54, SEEK_SET) == 0 ? fgetc(file) : -1;
	if (file) {
		fclose(file);
	}
	return byte;
}

// Only printable ASCII and line breaks are allowed, because the system font draws nothing else as
// it looks.
static unsigned screenGlyphs(const Screen *s) {
	const char *drawn[] = {"3Drop", s->meta, s->title, s->hero, s->rate,
						   s->eta,	s->body, s->guide, s->keys};
	unsigned n = 0;
	for (size_t i = 0; i < sizeof(drawn) / sizeof(drawn[0]); i++) {
		for (const char *c = drawn[i]; *c; c++, n++) {
			assert(*c == '\n' || (*c >= ' ' && *c <= '~'));
		}
	}
	return n;
}

static Screen at(void) {
	Screen s;
	build(&s, gApp.status, NULL);
	return s;
}

// ---- The live rate, on the fake clock: ticks as in transfer.c and qr.c, every 200 ms ----
static u64 received, sinkCredit;
static unsigned wireDivisor; // a gzip wire carries fewer bytes than its file
static void startTransfer(ServerStatus status, u64 sunkBefore) {
	memset(&gApp, 0, sizeof(gApp));
	memset(&rateSampler, 0, sizeof(rateSampler));
	gApp.status = status;
	gApp.wireTotal = 100ULL * 1024 * 1024;
	gApp.uiSunkProgress = true;
	gApp.uiWrittenBytes = received = sunkBefore;
	sinkCredit = 0;
	wireDivisor = 1;
	uiProgressTick();
}
// Runs `ms` of ticks, `period` apart. The wire takes `wire` bytes a second and the sink writes up
// to `sink`, in whole ring units as a put's worker does and never past the wire. Without that sink,
// the count follows the file's bytes as they go, as a get's sender takes them. `check` runs after
// each tick.
static void flow(u64 ms, u64 period, u64 wire, u64 sink, void (*check)(void)) {
	for (u64 t = 0; t < ms; t += period) {
		fake.nowMs += period;
		received += wire * period / 1000;
		sinkCredit += sink * period / 1000;
		while (sinkCredit >= RING_UNIT && gApp.uiWrittenBytes + RING_UNIT <= received) {
			gApp.uiWrittenBytes += RING_UNIT;
			sinkCredit -= RING_UNIT;
		}
		sinkCredit = MIN(sinkCredit, RING_UNIT); // a sink held back by the wire banks nothing
		gApp.wireBytes = received / wireDivisor;
		gApp.inflatedBytes = received; // a gzip put's bytes, as its inflate makes them
		if (!gApp.uiSunkProgress) {
			gApp.uiWrittenBytes = received;
		}
		uiProgressTick();
		if (check) {
			check();
		}
	}
}
// Checks that the rate is at most the wire's 1.5 MB/s, and that the ETA follows from the figure
// shown.
static void atMost150(void) {
	assert(!rateSampler.rateValid || rateSampler.rateHundredths <= 150U);
	u64 left = gApp.wireTotal - gApp.uiWrittenBytes, perS = rateSampler.rateHundredths * 10000ULL;
	assert(rateSampler.etaSeconds == 0 ||
		   (rateSampler.etaSeconds * perS * 100ULL <= left * 105ULL &&
			(rateSampler.etaSeconds + 1ULL) * perS * 100ULL >= left * 95ULL));
}
static void near150(void) { // the rate is within 5%, and the ETA follows from the same figure
	assert(rateSampler.rateValid && rateSampler.rateHundredths >= 143U &&
		   rateSampler.rateHundredths <= 157U);
	u64 left = (gApp.wireTotal - gApp.uiWrittenBytes) / 1500000ULL;
	assert(rateSampler.etaSeconds * 100ULL >= left * 95ULL &&
		   rateSampler.etaSeconds * 100ULL <= left * 105ULL);
}

// Nothing shows until 1 MiB is sunk and 2 s have passed, even if a total is left over from before.
static void rateGates(void) {
	startTransfer(SERVER_FILE, 528579072ULL);
	flow(1800, 200, 4000000, 4000000, NULL);
	assert(gApp.uiWrittenBytes - 528579072ULL >= 6ULL * RING_UNIT && !rateSampler.rateValid &&
		   strcmp(at().rate, "--.-- MB/s") == 0);
	flow(200, 200, 4000000, 4000000, NULL);
	assert(rateSampler.rateValid);
	startTransfer(SERVER_FILE, 0);
	flow(3000, 200, 500000, 0, NULL);
	gApp.uiWrittenBytes = RING_UNIT - 1;
	flow(200, 200, 500000, 0, NULL);
	assert(!rateSampler.rateValid);
	gApp.uiWrittenBytes = RING_UNIT;
	flow(200, 200, 500000, 0, NULL);
	assert(rateSampler.rateValid);
}

// A steady transfer reads its own rate; a gzip get reads its file's bytes, not its wire's.
static void rateSteady(void) {
	startTransfer(SERVER_FILE, 0);
	flow(5000, 200, 1500000, 1500000, NULL);
	flow(30000, 200, 1500000, 1500000, near150);
	startTransfer(SERVER_FILE, 0);
	gApp.uiSunkProgress = false;
	gApp.fileSending = gApp.fileGzip = true;
	wireDivisor = 4;
	flow(5000, 200, 1500000, 0, NULL);
	flow(10000, 200, 1500000, 0, near150);
}

// A gzip put reads its file's bytes too, never above those inflated so far: a card that holds its
// first writes and then drains them at four times the wire's speed reads as the wire did.
static void rateGzipPut(void) {
	startTransfer(SERVER_FILE, 0);
	gApp.fileGzip = true;
	wireDivisor = 4;
	flow(8000, 200, 1500000, 0, atMost150);
	flow(20000, 200, 1500000, 6000000, atMost150);
	near150();
}

// AM holds its first writes while the wire runs, then drains the backlog at its own speed: the
// figure never reads above the wire, and settles on it.
static void rateHoldThenDrain(void) {
	startTransfer(SERVER_INSTALLING, 0);
	flow(8000, 200, 1500000, 0, atMost150);
	assert(strcmp(at().rate, "--.-- MB/s") == 0);
	flow(20000, 200, 1500000, 6000000, atMost150);
	near150();
}

// The window is the last 5 s, on the 250 ms ticks of a ring wait too. 5 s after a slowdown, the
// figure is the new rate alone.
static void rateWindow(void) {
	startTransfer(SERVER_FILE, 0);
	gApp.uiSunkProgress = false;
	flow(10000, 250, 1500000, 0, NULL);
	flow(5000, 250, 500000, 0, NULL);
	assert(rateSampler.rateValid && rateSampler.rateHundredths == 50U);
}

// UT-09: a transfer that stops reads "unknown" RATE_STALL_MS after its last sunk byte, on a put, an
// install and a QR download alike, wherever the stop falls between two figures.
static void rateStall(ServerStatus status, bool viaUrl) {
	for (u64 lead = 8000; lead < 9200; lead += 200) {
		startTransfer(status, 0);
		gApp.viaUrl = viaUrl;
		flow(lead, 200, 4000000, 4000000, NULL);
		sinkCredit = 0;
		gApp.uiWrittenBytes += RING_UNIT; // the last unit, then nothing
		received = MAX(received, gApp.uiWrittenBytes);
		flow(200, 200, 0, 0, NULL);
		flow(RATE_STALL_MS - 200, 200, 0, 0, NULL);
		assert(rateSampler.rateValid);
		flow(200, 200, 0, 0, NULL);
		Screen s = at();
		assert(strcmp(s.rate, "--.-- MB/s") == 0 && strcmp(s.eta, "--m--s left") == 0);
		flow(60000, 200, 0, 0, NULL);
		assert(!rateSampler.rateValid);
	}
}
static void rateStallPut(void) {
	rateStall(SERVER_FILE, false);
}
static void rateStallInstall(void) {
	rateStall(SERVER_INSTALLING, false);
}
static void rateStallQr(void) {
	rateStall(SERVER_INSTALLING, true);
}

// RATE_CASE=<name> runs that case alone. Each case fails on its own on the tree before the rule.
typedef struct {
	const char *name;
	void (*run)(void);
} RateCase;
static const RateCase RATE_CASES[] = {
	{"gates", rateGates},	   {"steady", rateSteady},		{"drain", rateHoldThenDrain},
	{"window", rateWindow},	   {"stall-put", rateStallPut}, {"stall-install", rateStallInstall},
	{"stall-qr", rateStallQr}, {"gzip-put", rateGzipPut},
};
static size_t rateRule(const char *only) {
	size_t ran = 0;
	for (size_t i = 0; i < sizeof(RATE_CASES) / sizeof(RATE_CASES[0]); i++) {
		if (!only || strcmp(only, RATE_CASES[i].name) == 0) {
			RATE_CASES[i].run();
			ran++;
		}
	}
	return ran;
}

#define CHANGES(change)                                                                            \
	do {                                                                                           \
		Screen before = at();                                                                      \
		change;                                                                                    \
		Screen after = at();                                                                       \
		assert(memcmp(&before, &after, sizeof(before)) != 0);                                      \
	} while (0)

// ---- The QR scan's screens and its picture ----

// Works independently of ui.c's morton(). It takes the bits of x and y one at a time, with x the
// lower bit of each pair.
static unsigned tileOrder(unsigned x, unsigned y) {
	unsigned n = 0;
	for (unsigned bit = 0; bit < 3; bit++) {
		n |= ((x >> bit) & 1u) << (2 * bit);
		n |= ((y >> bit) & 1u) << (2 * bit + 1);
	}
	return n;
}

static void scanPicture(void) {
	// The texture is 3DS tiles (8x8, Morton order inside, 64 to a row of the sheet) with the
	// frame's own first row first, and anything the frame does not cover is left as it was.
	static u16 frame[SCAN_WIDTH * SCAN_HEIGHT], sheet[PREVIEW_TEX_W * PREVIEW_TEX_H];
	for (unsigned y = 0; y < SCAN_HEIGHT; y++) {
		for (unsigned x = 0; x < SCAN_WIDTH; x++) {
			frame[y * SCAN_WIDTH + x] = (u16)(x * 7 + y * 131 + 1);
		}
	}
	for (size_t i = 0; i < sizeof(sheet) / sizeof(sheet[0]); i++) {
		sheet[i] = 0xABCD;
	}
	previewLoad(sheet, frame);
	for (unsigned y = 0; y < PREVIEW_TEX_H; y++) {
		for (unsigned x = 0; x < PREVIEW_TEX_W; x++) {
			size_t at = (((size_t)(y >> 3) * (PREVIEW_TEX_W >> 3) + (x >> 3)) << 6) +
						tileOrder(x & 7, y & 7);
			bool covered = x < SCAN_WIDTH && y < SCAN_HEIGHT;
			u16 want = covered ? frame[y * SCAN_WIDTH + x] : 0xABCD;
			if (sheet[at] != want) {
				fprintf(stderr, "testUi: texel (%u,%u): %04X, want %04X\n", x, y, sheet[at], want);
				abort();
			}
		}
	}
	// The sub-texture is the frame's rectangle at the sheet's top left, as tex3ds writes one.
	assert(PREVIEW_SUB.width == 400 && PREVIEW_SUB.height == 240 && PREVIEW_SUB.left == 0.0f &&
		   PREVIEW_SUB.top == 1.0f && PREVIEW_SUB.right == 400.0f / 512.0f &&
		   PREVIEW_SUB.bottom == 1.0f - 240.0f / 256.0f);
	assert(SCAN_WIDTH == 400 && SCAN_HEIGHT == 240 && PREVIEW_TEX_BYTES == 512 * 256 * 2);
}

// A wait's tick, as the status LED has it. A key can be set to go down after so many ticks.
static unsigned ticksUntilKey;
static u32 keyAfterTicks;
void appStatusTick(void) {
	if (ticksUntilKey && --ticksUntilKey == 0) {
		fake.keysDown = keyAfterTicks;
	}
}

static void scanScreens(void) {
	memset(&gApp, 0, sizeof(gApp));
	snprintf(displayKey, sizeof(displayKey), "KEY1-2345");
	// Each screen's words say what it is, what it asks, which keys answer, and that the computer
	// can only look. The color says what the card is: a notice, a question, work under way, or a
	// failure.
	static const struct {
		UiNotice screen;
		u32 hue;
		const char *title, *guide, *keys;
	} CARDS[] = {
		{UI_QR_SCAN, ACCENT, "Scan a QR code",
		 "Point the outer camera at a QR code with a link to a CIA, and hold still.\n"
		 "The computer can only look until you are done.",
		 "B  cancel"},
		{UI_QR_CONFIRM, AMBER, "Install from the scanned code?",
		 "Checked like a CIA sent from your computer.\n"
		 "An installed title is updated. 3Drop's own CIA relaunches 3Drop.\n"
		 "The computer can only look until this is done.",
		 "A  install      B  cancel"},
		{UI_QR_BUNDLE, BLUE, "Certificates", "The computer gets no reply until this is done.",
		 "B  cancel"},
		{UI_QR_CHECK, RED, "Certificate check failed",
		 "A  gets a fresh list and tries again.\n"
		 "Y  skips the check for this download only.",
		 "A  fresh list      Y  skip check      B  cancel"},
		{UI_QR_REFUSED, RED, "QR code not used", "", "ANY  continue"},
	};
	for (size_t i = 0; i < sizeof(CARDS) / sizeof(CARDS[0]); i++) {
		Screen s;
		build(&s, CARDS[i].screen, "the detail");
		if (strcmp(s.title, CARDS[i].title) != 0 || strcmp(s.guide, CARDS[i].guide) != 0 ||
			strcmp(s.keys, CARDS[i].keys) != 0 || !strstr(s.body, "the detail") ||
			s.hue != CARDS[i].hue) {
			fprintf(stderr, "testUi: screen %d: `%s` / `%s` / `%s`\n", (int)CARDS[i].screen,
					s.title, s.guide, s.keys);
			abort();
		}
		// Only a look is served at a screen, and the card that waits on a link says so. While the
		// list is fetched nothing is served, and its card says that.
		bool says = strstr(s.guide, "can only look") || strstr(s.body, "can only look");
		assert(says == (CARDS[i].screen == UI_QR_SCAN || CARDS[i].screen == UI_QR_CONFIRM));
		assert((strstr(s.guide, "gets no reply") != NULL) == (CARDS[i].screen == UI_QR_BUNDLE));
		assert(!showsKey(CARDS[i].screen)); // none of them draws the pairing key
		assert(CARDS[i].screen != UI_QR_BUNDLE || strstr(s.body, "curl.se"));
		assert(s.keys[0] && !s.dots); // a card with keys waits for them
	}
	// A form feed in a prompt's detail ends what the top screen says. The rest goes to the bottom
	// screen after its own words (the plain-http meaning, a check's cause), and the top keeps the
	// part before it. Without a form feed, all of it is for the top screen and the bottom screen
	// keeps its own words.
	Screen split;
	build(&split, UI_QR_CONFIRM,
		  "1  a.example\n\f\nhttp:// is not encrypted: someone can replace it.");
	assert(strcmp(split.body, "1  a.example\n") == 0);
	assert(strstr(split.guide, "Checked like a CIA sent from your computer."));
	assert(strstr(split.guide, "An installed title is updated. 3Drop's own CIA relaunches 3Drop."));
	assert(strstr(split.guide, "done.\nhttp:// is not encrypted: someone can replace it.") != NULL);
	build(&split, UI_QR_CHECK, "Link 2 of 5\nevil.example\n/a/b.cia?...\n\f\nThe cause.");
	assert(strcmp(split.body, "Link 2 of 5\nevil.example\n/a/b.cia?...\n") == 0);
	assert(strstr(split.guide, "only.\nThe cause.") != NULL);
	// The most a QR prompt puts on the top screen (qr.c): "Link 16 of 16", a 120-character site and
	// a path of 120 with its query's mark, all in rows of 30. That makes 267 bytes, held whole.
	char first[320] = "Link 16 of 16\n";
	for (int row = 0; row < 8; row++) {
		strcat(first, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
	}
	strcat(first, "?...\n");
	assert(strlen(first) == 267);
	char longest[400];
	snprintf(longest, sizeof(longest), "%s\f\nThe cause.", first);
	build(&split, UI_QR_CHECK, longest);
	assert(strcmp(split.body, first) == 0);
	build(&split, UI_QR_CHECK, "no form feed");
	assert(strcmp(split.body, "no form feed") == 0 && !strstr(split.guide, "no form feed"));
	build(&split, UI_QR_CHECK, NULL);
	assert(split.body[0] == '\0' && strstr(split.guide, "A  gets a fresh list"));
	// The idle screens tell where the scan is.
	Screen idle;
	build(&idle, SERVER_READY, NULL);
	assert(strstr(idle.keys, "Y  scan QR") && strstr(idle.keys, "START  exit") &&
		   strstr(idle.keys, "UP+SELECT+B  new key"));
	build(&idle, SERVER_AUTH_FAILED, NULL);
	assert(strstr(idle.keys, "Y  scan QR"));
	build(&idle, SERVER_NEEDS_KEY, NULL);
	assert(!strstr(idle.keys, "scan QR")); // with no key nothing is served, and Y opens nothing

	// A link's download: the card says so, B stops it, and START is no longer the way.
	gApp.status = SERVER_RECEIVING;
	gApp.wireTotal = 1000;
	gApp.wireBytes = gApp.uiWrittenBytes = 250;
	Screen wire = at();
	assert(strcmp(wire.title, "Receiving") == 0 && strstr(wire.keys, "START"));
	gApp.viaUrl = true;
	Screen link = at();
	assert(strcmp(link.title, "Downloading") == 0 && strcmp(link.keys, "B  cancel") == 0 &&
		   strstr(link.guide, "B stops the download") && strstr(link.guide, "gets no reply"));
	assert(strstr(link.body, "250 / 1000 bytes") && link.bar && link.percent == 25);
	// Before the server answers there is no size: the card says it is connecting, not 0 / 0.
	gApp.wireTotal = gApp.wireBytes = gApp.uiWrittenBytes = 0;
	link = at();
	assert(strstr(link.body, "Connecting...") && !strstr(link.body, "bytes") &&
		   link.hero[0] == '0');
	// A wire transfer is not touched by any of it.
	gApp.viaUrl = false;
	wire = at();
	assert(strstr(wire.body, "0 / 0 bytes") && !strstr(wire.body, "Connecting"));
	// Installing keeps its own words and keys through a link's install.
	gApp.viaUrl = true;
	gApp.status = SERVER_INSTALLING;
	gApp.wireTotal = 1000;
	gApp.wireBytes = 1000;
	gApp.uiWrittenBytes = 500;
	link = at();
	assert(strcmp(link.title, "Writing to the card") == 0 && strcmp(link.keys, "B  cancel") == 0 &&
		   link.bar);
	gApp.uiWrittenBytes = 1000;
	link = at();
	assert(strcmp(link.title, "Committing") == 0 && link.keys[0] == '\0');
	assert(strstr(link.guide, "Cannot cancel") && strstr(link.guide, "Do not power off"));

	// A failed link says why in words, after the wire's own, and keeps the code; one with no words
	// shows only the wire's.
	gApp.status = SERVER_INSTALL_FAILED;
	gApp.lastViaUrl = true;
	gApp.last = (Operation){OUTCOME_REJECTED, "http", DOWNLOAD_HTTP(404), 0, -1};
	gApp.lastBytes = 0;
	Screen failed = at();
	assert(strstr(failed.body, "0 bytes received before it stopped\nhttp") &&
		   strstr(failed.body, "The server answered HTTP 404."));
	assert(strstr(failed.guide, "scan again") && !strstr(failed.guide, "send again"));
	assert(strstr(failed.hero, "0x") && strstr(failed.keys, "START"));
	gApp.last.detail = "not-newer";
	gApp.last.result = 0;
	failed = at();
	// A scanned link's card speaks to the person holding the console, not to the CIA's author.
	assert(strstr(failed.body, "That version, or a newer one, is already installed."));
	assert(!strstr(failed.body, "Raise the CIA's version."));
	gApp.last.detail = "am-write";
	failed = at();
	assert(strstr(failed.body, "am-write") && !strstr(failed.body, "\nThe "));
	// The same failure from the wire keeps the wire's guide.
	gApp.lastViaUrl = false;
	failed = at();
	assert(strstr(failed.guide, "send again") && !strstr(failed.guide, "scan again"));
	assert(!strstr(failed.body, "already installed")); // a link's words, not the wire's
}

static unsigned flushes;
Result GSPGPU_FlushDataCache(const void *addr, u32 size) {
	flushes += size == PREVIEW_TEX_BYTES && addr == previewTex.data;
	return 0;
}

static void scanTexture(void) {
	static u16 one[SCAN_WIDTH * SCAN_HEIGHT], two[SCAN_WIDTH * SCAN_HEIGHT];
	for (size_t i = 0; i < SCAN_WIDTH * SCAN_HEIGHT; i++) {
		one[i] = (u16)(i * 3 + 1);
		two[i] = (u16)(i * 5 + 2);
	}
	memset(&gApp, 0, sizeof(gApp));
	gApp.status = SERVER_READY;
	// No texture, no picture: a frame offered to a scan that never opened draws nothing.
	assert(!previewReady);
	unsigned frames = syncFrames;
	uiScanFrame(one);
	assert(syncFrames == frames && imageDraws == 0 && previewFrame == NULL);
	uiScanClose(); // nothing to close
	assert(syncFrames == frames);

	// The renderer is not up: nothing to open a texture on, and none is made.
	ready = false;
	assert(!uiScanOpen() && !previewReady && previewTex.data == NULL);
	ready = true;

	// No memory for the texture: the scan goes on without a picture.
	fake.textureFails = true;
	assert(!uiScanOpen() && !previewReady && previewTex.data == NULL);
	fake.textureFails = false;

	// Opened: the texture is black and flushed, and opening again gives the same texture.
	flushes = 0;
	assert(uiScanOpen() && previewReady && previewTex.data && flushes == 1);
	void *data = previewTex.data;
	for (size_t i = 0; i < PREVIEW_TEX_W * PREVIEW_TEX_H; i++) {
		assert(((u16 *)data)[i] == 0);
	}
	assert(uiScanOpen() && previewTex.data == data && flushes == 1);

	// A frame is drawn on the top screen as the texture's rectangle, with its words on the bottom.
	// It is loaded after the frame began (the GPU may still read the texture until then) and
	// flushed once.
	frames = syncFrames;
	imageDraws = 0;
	glyphs = 0;
	uiScanFrame(one);
	assert(syncFrames == frames + 1 && imageDraws == 1 && flushes == 2);
	assert(texelAtBegin == 0 && textureAtBegin); // still the black one when the frame began
	assert(lastImage.tex == &previewTex && lastImage.subtex == &PREVIEW_SUB);
	assert(((u16 *)data)[0] == one[0]); // loaded for this frame
	assert(shown.screen == UI_QR_SCAN && shown.preview && previewFrame == NULL);
	// The bottom screen's words only: the top has the camera's picture and no text of its own.
	assert(glyphs == strlen(shown.guide) + strlen(shown.keys));
	assert(!uiShowsKey()); // a capture of this screen blacks out nothing
	// The next frame replaces it, tile by tile.
	uiScanFrame(two);
	assert(imageDraws == 2 && flushes == 3 && texelAtBegin == one[0] && ((u16 *)data)[0] == two[0]);
	u16 *sheet = data;
	size_t tile = ((size_t)(239 >> 3) * (PREVIEW_TEX_W >> 3) + (399 >> 3)) << 6;
	assert(sheet[tile + tileOrder(399 & 7, 239 & 7)] == two[239 * SCAN_WIDTH + 399]);
	// A redraw of the scan's screen with no new frame (the dot's breath, while a prompt waits after
	// a frame the GPU refused): the picture stays as it was, and no buffer is read for it.
	unsigned draws = imageDraws, loads = flushes;
	Screen again = shown;
	again.dot = 0;
	show(&again, C3D_FRAME_SYNCDRAW);
	assert(imageDraws == draws + 1 && flushes == loads && previewFrame == NULL);
	// A frame the GPU refuses is not retried with a buffer the camera has reused by then.
	gpuRefuses = true;
	uiScanFrame(one);
	gpuRefuses = false;
	assert(previewFrame == NULL && flushes == loads && imageDraws == draws + 1);
	// Any other screen after it draws as it always did: its text, and no picture.
	imageDraws = 0;
	drawScreen(NULL);
	assert(imageDraws == 0 && !shown.preview && glyphs > 0 && flushes == 3);

	// A picture is loaded only into a frame that shows it, and only once: a frame the scan did not
	// ask for does not read the buffer the camera may be writing.
	previewFrame = two;
	drawScreen(NULL);
	assert(flushes == 3 && previewFrame == NULL);

	// The close waits for the GPU (a frame begun), then frees the texture, once.
	frames = syncFrames;
	uiScanClose();
	assert(syncFrames == frames + 1 && textureAtBegin && previewTex.data == NULL && !previewReady);
	uiScanClose();
	assert(syncFrames == frames + 1);
	uiScanFrame(one);
	assert(syncFrames == frames + 1 && imageDraws == 0);
	// And it can be opened again.
	assert(uiScanOpen() && previewTex.data);
	uiScanClose();
}

// Pins whole the screens' sentences that say what happened and where the remedy is: an install that
// did not commit (canceled, unconfirmed, refused), a transfer's START, the wrong key, the port, the
// first screen's command and the certificate list's fetch.
static bool card(const char *title, const char *hero, const char *body, const char *guide) {
	Screen s = at();
	bool same = strcmp(s.title, title) == 0 && strcmp(s.hero, hero) == 0 &&
				strcmp(s.body, body) == 0 && strcmp(s.guide, guide) == 0 &&
				strcmp(s.keys, "START  dismiss") == 0;
	if (!same) {
		fprintf(stderr, "testUi: card `%s` `%s` `%s` `%s`\n", s.title, s.hero, s.body, s.guide);
	}
	return same;
}

static void outcomes(void) {
	memset(&gApp, 0, sizeof(gApp));
	Screen s;
	// START during a transfer cancels it: the pump takes the press, so nothing exits.
	static const unsigned TRANSFERS[] = {SERVER_RECEIVING, SERVER_FILE};
	for (size_t i = 0; i < 2; i++) {
		build(&s, TRANSFERS[i], NULL);
		assert(strcmp(s.guide, "START cancels the transfer.") == 0 &&
			   strcmp(s.keys, "START  cancel") == 0);
	}
	gApp.status = SERVER_INSTALL_FAILED;
	gApp.lastBytes = 4096;
	for (int link = 0; link < 2; link++) {
		gApp.lastViaUrl = link;
		// Canceled on purpose: no code, and no advice about a fault that did not happen; this CIA
		// alone is not installed, so a batch's earlier ones stand.
		gApp.last = (Operation){OUTCOME_REJECTED, "canceled", TRANSFER_CANCELED, 0x1234, 2};
		assert(card("Canceled", "", "4096 bytes received before it was canceled",
					"This CIA was not installed."));
		// AM's commit failed and the title may have landed: look first, as the host says.
		gApp.last = (Operation){OUTCOME_UNKNOWN, "am-finish", (Result)0xD8E08025, 0x1234, 2};
		char look[80];
		snprintf(look, sizeof(look),
				 "AM could not say how it ended. Look in HOME before you %s it again.",
				 link ? "scan" : "send");
		assert(card("Install unconfirmed", "0xD8E08025", "4096 bytes received\nam-finish", look));
		// The Cancel went through and the ticket's delete did not: nothing is installed, a ticket
		// may remain, HOME cannot show one, and FBI lists it by the title's id. The card keeps
		// the delete's Result and names that id, and gives no HOME advice.
		gApp.last =
			(Operation){OUTCOME_UNKNOWN, "ticket", (Result)0xD8A08004, 0x000400000F800100ULL, 2};
		assert(card("Canceled, ticket may remain", "0xD8A08004",
					"4096 bytes received before it was canceled\ntitle 000400000F800100  v2",
					"This CIA was not installed, but its ticket may remain, and HOME can't "
					"show it. FBI can delete it: Tickets, this title ID, Delete Ticket."));
	}
	// A refusal keeps its code and the sender's advice, or the scanner's. None of these cards says
	// the console listens, because a scanned link installs with no listener.
	gApp.lastViaUrl = false;
	gApp.last = (Operation){OUTCOME_REJECTED, "am-write", (Result)0xD8E0806A, 0x1234, 2};
	assert(card("Install failed", "0xD8E0806A", "4096 bytes received before it stopped\nam-write",
				"Check the CIA and free space, then send again."));
	gApp.lastViaUrl = true;
	assert(card("Install failed", "0xD8E0806A", "4096 bytes received before it stopped\nam-write",
				"Check the link and the free space, then scan again."));
	gApp.lastViaUrl = false;
	build(&s, SERVER_INSTALLED, NULL);
	assert(strcmp(s.guide, "Title installed and verified.") == 0 &&
		   strcmp(s.keys, "START  exit") == 0);
	// A wrong key is fixed on the computer, not by a new key here; the card may outlast the
	// request, so it names no last sender.
	build(&s, SERVER_AUTH_FAILED, NULL);
	assert(strcmp(s.title, "Rejected: wrong key") == 0 &&
		   strcmp(s.body, "A sender's key didn't match this one.") == 0 &&
		   strcmp(s.guide, "Still listening.\n"
						   "Set THREEDROP_KEY on your computer to the key above.") == 0);
	// Nothing on a console frees a port: the body names that cause, and the guide says what can be
	// done.
	build(&s, SERVER_LISTEN_FAILED, NULL);
	assert(strcmp(s.body, "Wi-Fi dropped, or another app is using port 1337.") == 0 &&
		   strcmp(s.guide, "Retrying every second. Check that Wi-Fi is on.") == 0);
	build(&s, SERVER_READY, NULL);
	assert(strcmp(s.guide, "Send from your computer:\ntools/drop.sh game.cia <ip>") == 0);
	// An unusable key.txt (empty, too long, only dashes) is never replaced, and the card does not
	// say it tried. The way out is the new-key chord, which the keys line offers.
	build(&s, SERVER_NEEDS_KEY, NULL);
	assert(strcmp(s.body, "No usable key is set, and none was created automatically.\n"
						  "Required file: " KEY_PATH) == 0 &&
		   strcmp(s.guide, "Put a key in key.txt, then relaunch, or make a new one with the keys "
						   "below.") == 0 &&
		   strcmp(s.keys, "START  exit    UP+SELECT+B  new key") == 0);
	// The list holds certificate authorities, and A fetches it again: not sites, not once.
	build(&s, UI_QR_BUNDLE, NULL);
	assert(strcmp(s.body, "Getting the list of certificate authorities from curl.se.") == 0);
	// X's cleanup says how it went (main.c), and the idle tick leaves that up for a second, as it
	// does an install's card.
	gApp.status = SERVER_READY;
	fake.nowMs = 1000000;
	uiNotice(UI_CLEANUP_FAILED, "0xD8A08004");
	assert(shown.screen == UI_CLEANUP_FAILED && strcmp(shown.title, "Cleanup failed") == 0 &&
		   strcmp(shown.body, "AM refused the cleanup: 0xD8A08004") == 0);
	assert(holdUntilMs == fake.nowMs + COMPLETE_HOLD_MS);
	fake.nowMs += COMPLETE_HOLD_MS - 1;
	uiIdleAnimationTick();
	assert(shown.screen == UI_CLEANUP_FAILED);
	fake.nowMs += 1;
	uiIdleAnimationTick();
	assert(shown.screen == SERVER_READY);
	uiNotice(UI_CLEANUP_DONE, NULL);
	assert(strcmp(shown.title, "Cleanup done") == 0 &&
		   strcmp(shown.body, "Imports that finished but were never committed were cleared from "
							  "the SD card.") == 0);
	// X runs in any state, so the notices promise no listener and no key. Over a console with no
	// pairing key, and over a failed install whose START only dismisses, each shows no keys and
	// then gives that state's own screen, with its own keys, back.
	static const struct {
		ServerStatus status;
		const char *keys;
	} UNDER[] = {{SERVER_NEEDS_KEY, "START  exit    UP+SELECT+B  new key"},
				 {SERVER_INSTALL_FAILED, "START  dismiss"}};
	static const struct {
		UiNotice notice;
		const char *detail, *guide;
	} NOTICES[] = {{UI_CLEANUP_DONE, NULL, "Returning to the status screen."},
				   {UI_CLEANUP_FAILED, "0xD8A08004",
					"The code is in the log too. Returning to the status screen."}};
	for (size_t i = 0; i < sizeof(UNDER) / sizeof(UNDER[0]); i++) {
		for (size_t n = 0; n < sizeof(NOTICES) / sizeof(NOTICES[0]); n++) {
			gApp.status = UNDER[i].status;
			uiNotice(NOTICES[n].notice, NOTICES[n].detail);
			assert(shown.screen == NOTICES[n].notice && shown.keys[0] == '\0');
			assert(strcmp(shown.guide, NOTICES[n].guide) == 0);
			fake.nowMs += COMPLETE_HOLD_MS;
			uiIdleAnimationTick();
			assert(shown.screen == UNDER[i].status && strcmp(shown.keys, UNDER[i].keys) == 0);
		}
	}
	gApp.status = SERVER_READY;
}

// Stands in for what a prompt's wait runs once a frame besides the LED's tick, as main.c's
// serveBusy does.
static unsigned askTicks;
static void countTick(void) {
	askTicks++;
}

static void asks(void) {
	// A prompt returns the keys of its mask that went down, whichever others did, and shows its
	// screen and detail first.
	fake.keysDown = KEY_A | KEY_X;
	assert(uiAsk(UI_QR_CONFIRM, "https://a/b.cia", KEY_A | KEY_B, NULL) == KEY_A);
	assert(shown.screen == UI_QR_CONFIRM && strstr(shown.body, "https://a/b.cia"));
	fake.keysDown = KEY_Y | KEY_A;
	assert(uiAsk(UI_QR_CHECK, "why", KEY_A | KEY_Y | KEY_B, NULL) == (KEY_A | KEY_Y));
	fake.keysDown = KEY_B;
	assert(uiAsk(UI_QR_CHECK, "why", KEY_A | KEY_Y | KEY_B, NULL) == KEY_B);
	fake.keysDown = 0x12345;
	assert(uiAsk(UI_QR_REFUSED, "x", 0xFFFFFFFFu, NULL) == 0x12345);
	// There is no time-out: the person may be reading a certificate's cause for minutes.
	fake.keysDown = 0;
	keyAfterTicks = KEY_A;
	ticksUntilKey = 10 * UI_KEY_WAIT_FRAMES;
	assert(uiAsk(UI_QR_CONFIRM, "x", KEY_A | KEY_B, NULL) == KEY_A && ticksUntilKey == 0);
	fake.keysDown = 0;
	// The tick it is given runs once a frame while it waits: here five frames, the last one the
	// key's, so a client that connects meanwhile is answered at the prompt.
	askTicks = 0;
	ticksUntilKey = 5;
	assert(uiAsk(UI_QR_CONFIRM, "x", KEY_A | KEY_B, countTick) == KEY_A && askTicks == 5);
	fake.keysDown = 0;
	// The app ending (a POWER press with no import open) answers nothing, and is not waited out.
	fake.keysDown = 0;
	fake.powerPressed = true;
	assert(uiAsk(UI_QR_CONFIRM, "x", KEY_A | KEY_B, NULL) == 0);
	fake.powerPressed = false;
}

// ---- Word wrap ----

// Checks what `in` becomes at `width` (ten pixels a character): the same text, with only spaces
// made newlines; no row wider than the width except a word's own; and no word alone on a
// paragraph's last row that the row above could have given (its last word, and this one, would have
// fit).
static void wrapProperties(const char *in, float width) {
	char out[TEXT_MAX];
	size_t n = strlen(in), start = 0, above = n; // above: the row above's start, n for none
	snprintf(out, sizeof(out), "%s", in);
	wrapRows(out, 1.0f, width);
	assert(strlen(out) == n);
	for (size_t i = 0; i <= n; i++) {
		assert(out[i] == in[i] || (in[i] == ' ' && out[i] == '\n'));
		if (out[i] != '\n' && out[i] != '\0') {
			continue;
		}
		size_t end = i;
		while (end > start && out[end - 1] == ' ') {
			end--; // spaces at a row's end are not seen
		}
		bool oneWord = memchr(out + start, ' ', end - start) == NULL;
		assert(oneWord || 10.0f * (float)(end - start) <= width);
		bool lastRow = in[i] != ' '; // a paragraph's: its newline was the text's own, or the end
		if (lastRow && oneWord && above != n) {
			size_t word = start - 1;
			while (word > above && out[word - 1] == ' ') {
				word--;
			}
			while (word > above && out[word - 1] != ' ') {
				word--;
			}
			assert(word == above || 10.0f * (float)(end - word) > width);
		}
		above = lastRow ? n : start;
		start = i + 1;
	}
}

static void wrapping(void) {
	// The rows at ten characters (width 100): greedy, but a lone last word takes one from above.
	// In order: all fits; a lone last word; greedy's own last row of two; a row above with no word
	// to give; two that would not fit together; each line's own last row; spaces kept; no widow.
	static const struct {
		const char *in, *out;
	} ROWS[] = {
		{"aaa bbb", "aaa bbb"},
		{"aaaa bbbb cccc", "aaaa\nbbbb cccc"},
		{"aaaa bbbb cc dd", "aaaa bbbb\ncc dd"},
		{"aaaaaa bbbbbb", "aaaaaa\nbbbbbb"},
		{"aa bbbbbbb cccccccc", "aa bbbbbbb\ncccccccc"},
		{"aaaa bbbb cccc\nxx yy", "aaaa\nbbbb cccc\nxx yy"},
		{"aaaa  bbbb cccc", "aaaa \nbbbb cccc"},
		{"ab cd ef gh ij", "ab cd ef\ngh ij"},
		{"", ""},
	};
	for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
		char rows[TEXT_MAX];
		snprintf(rows, sizeof(rows), "%s", ROWS[i].in);
		wrapRows(rows, 1.0f, 100.0f);
		if (strcmp(rows, ROWS[i].out) != 0) {
			fprintf(stderr, "testUi: wrap `%s` -> `%s`, want `%s`\n", ROWS[i].in, rows,
					ROWS[i].out);
			abort();
		}
	}
	// Every screen's words, at every width a screen could give them.
	for (unsigned n = 0; n < UI_SCREEN_COUNT; n++) {
		for (float width = 50.0f; width < 1700.0f; width += 7.0f) {
			wrapProperties(SCREENS[n].body, width);
			wrapProperties(SCREENS[n].guide, width);
		}
	}
	// The scale is the draw's: at half of it, twice as many characters fit a row.
	char half[TEXT_MAX] = "aaaaaaaa bbbbbbbb cccccccc";
	wrapRows(half, 0.5f, 100.0f);
	assert(strcmp(half, "aaaaaaaa\nbbbbbbbb cccccccc") == 0);

	// textWrapped() draws the rows, whole and not wrapped again, at the scale it was asked for...
	static Wrapped rows; // a call site's own, as body's and guide's are
	drawnCount = 0;
	textWrapped(&rows, "aaaa bbbb cccc", 0.0f, 0.0f, 1.0f, FG, 100.0f);
	assert(drawnCount == 1 && strcmp(drawnRows[0], "aaaa\nbbbb cccc") == 0 &&
		   drawnScale[0] == 1.0f);
	// ... a word wider than the box keeps a row of its own, and the lot shrinks to fit it (a long
	// file name on T8) ...
	drawnCount = 0;
	textWrapped(&rows, "ab WWWWWWWWWWWWWWWWWWWW cd", 0.0f, 0.0f, 1.0f, FG, 100.0f);
	assert(drawnCount == 1 && strcmp(drawnRows[0], "ab\nWWWWWWWWWWWWWWWWWWWW\ncd") == 0 &&
		   drawnScale[0] == 0.5f);
	// ... and a text that fits is not touched.
	drawnCount = 0;
	textWrapped(&rows, "aaa bbb", 0.0f, 0.0f, 1.0f, FG, 100.0f);
	assert(strcmp(drawnRows[0], "aaa bbb") == 0 && drawnScale[0] == 1.0f);
	// With no buffer to measure rows in, citro2d breaks them as before, at the scale asked for: the
	// words still show (they were not drawn at all).
	C2D_TextBuf measuring = measureBuf;
	measureBuf = NULL;
	drawnCount = 0;
	textWrapped(&rows, "aaaa bbbb cccc", 0.0f, 0.0f, 1.0f, FG, 100.0f);
	assert(drawnCount == 1 && strcmp(drawnRows[0], "aaaa bbbb cccc") == 0 && drawnScale[0] == 1.0f);
	measureBuf = measuring;

	// A screen's body and guide go through it: the top screen's rows hold 376 px, the bottom's 296,
	// at 5.5 px a character.
	Screen s = at();
	snprintf(s.body, sizeof(s.body), "%060d bbbb cccc", 0);
	snprintf(s.guide, sizeof(s.guide), "%048d bbbb cccc", 0);
	show(&s, C3D_FRAME_SYNCDRAW);
	unsigned wrapped = 0;
	for (unsigned i = 0; i < drawnCount; i++) {
		wrapped += strstr(drawnRows[i], "\nbbbb cccc") != NULL;
	}
	assert(wrapped == 2);

	// A redraw measures only a text that changed. Mid-install (T2) the body's count moves and its
	// rows are broken again, the guide's are not, and a frame like the last measures nothing.
	memset(&gApp, 0, sizeof(gApp));
	gApp.status = SERVER_INSTALLING;
	gApp.uiSunkProgress = true;
	gApp.wireTotal = 104857600;
	gApp.wireBytes = gApp.uiWrittenBytes = 52428800;
	s = at();
	show(&s, C3D_FRAME_SYNCDRAW);
	measures = 0;
	show(&s, C3D_FRAME_SYNCDRAW);
	assert(measures == 0);
	gApp.wireBytes = gApp.uiWrittenBytes += 65536;
	s = at();
	char body[TEXT_MAX];
	snprintf(body, sizeof(body), "%s", s.body);
	wrapRows(body, S_BODY, 376.0f); // what the new body alone costs
	unsigned bodyMeasures = measures;
	measures = 0;
	show(&s, C3D_FRAME_SYNCDRAW);
	assert(bodyMeasures > 0 && measures == bodyMeasures);
}

// A failed scan's card is the same after a file put, which clears viaUrl as any transfer does: its
// guide and its sentence about the server do not change under the person.
static void staleCard(void) {
	memset(&gApp, 0, sizeof(gApp));
	gApp.viaUrl = true; // the scan's download
	gApp.wireBytes = 4096;
	gApp.last = (Operation){OUTCOME_REJECTED, "http", DOWNLOAD_HTTP(404), 0, -1};
	appInstallDone(&gApp.last);
	Screen before = at();
	assert(strstr(before.guide, "scan again") && strstr(before.body, "HTTP 404."));
	appTransferStart(); // the put
	gApp.status = SERVER_FILE;
	assert(strcmp(at().title, "Receiving") == 0);
	appSetStatus(SERVER_INSTALL_FAILED); // cmdFs.c puts the earlier status back
	Screen after = at();
	assert(memcmp(&before, &after, sizeof(before)) == 0);
	// The next install's card is its own: a computer's, with the computer's words.
	gApp.last = (Operation){OUTCOME_REJECTED, "am-write", (Result)0xD8E0806A, 0, -1};
	appInstallDone(&gApp.last);
	assert(strstr(at().guide, "send again") && !strstr(at().body, "The server"));
}

int main(void) {
	fake.stepMs = 0;
	assert(uiInit("v1.2.3-45-gdeadbee-dirty"));
	const char *rateCase = getenv("RATE_CASE");
	if (rateCase) {
		assert(rateRule(rateCase) == 1);
		uiExit();
		TEST_PASS();
		return 0;
	}
	// Every screen, with every live field at its longest.
	memset(displayKey, 'k', sizeof(displayKey) - 1);
	gApp.installedTitleVersion = INT32_MAX;
	gApp.last = (Operation){.result = -1, .titleID = UINT64_MAX, .version = INT32_MAX};
	gApp.last.detail = "eeeeeeeeeeeeeeeeeeeeeee"; // the longest detail word is shorter
	gApp.wireTotal = UINT64_MAX / 100;			  // the percent's *100 must not wrap
	gApp.wireBytes = gApp.wireTotal - 1;
	gApp.batchActive = rateSampler.rateValid = true;
	gApp.batchIndex = gApp.batchTotal = gApp.batchInstalled = UINT32_MAX;
	rateSampler.rateHundredths = rateSampler.etaSeconds = finalizeSeconds = UINT32_MAX;
	drawScreen("255.255.255.255");
	for (unsigned n = 0; n <= UI_SCREEN_COUNT + 1; n++) {
		// Past the last row, the INSTALLING screen's tail: writing to the card, then committing.
		bool finalizing = n >= UI_SCREEN_COUNT, committing = n > UI_SCREEN_COUNT;
		gApp.wireBytes = gApp.wireTotal - !finalizing;
		gApp.uiWrittenBytes = committing ? gApp.wireTotal : gApp.wireTotal - 1;
		Screen s;
		build(&s, finalizing ? SERVER_INSTALLING : n, "detail line one.\ndetail line two.");
		assert(s.title[0] && (s.keys[0] || s.dots || s.bar || s.committing));
		unsigned keys = keyDraws;
		show(&s, C3D_FRAME_SYNCDRAW);
		assert(keyDraws == keys + showsKey(s.screen)); // a KEY_MAX key drawn whole, never a prefix
		assert(s.finalizing == finalizing && s.committing == committing);
		assert(shown.screen == s.screen);
		assert(glyphs == screenGlyphs(&s)); // printable, and all of it drawn
	}
	assert(densest > 0 && densest < TEXT_GLYPHS);
	printf("testUi: the densest frame parses %u glyphs of %u\n", densest, (unsigned)TEXT_GLYPHS);

	// Every value drawn changes the Screen, so it reaches the screen.
	memset(&gApp, 0, sizeof(gApp));
	memset(&rateSampler, 0, sizeof(rateSampler));
	gApp.status = SERVER_INSTALLING;
	gApp.wireTotal = 1000;
	gApp.uiSunkProgress = true;
	CHANGES(gApp.wireBytes = 600);		// downloaded: the dim fill
	CHANGES(gApp.uiWrittenBytes = 400); // installed: the percent, the bright fill and the count
	assert(at().percent == 40 && at().wirePercent == 60 && strstr(at().body, "400 / 1000 bytes"));
	assert(strcmp(at().rate, "--.-- MB/s") == 0 && strcmp(at().eta, "--m--s left") == 0);
	CHANGES(rateSampler.rateValid = true);
	CHANGES(rateSampler.rateHundredths = 105);
	assert(strcmp(at().rate, "1.05 MB/s") == 0 && strcmp(at().eta, "--m--s left") == 0);
	CHANGES(rateSampler.etaSeconds = 65); // 0 was unknown, never done
	assert(strcmp(at().eta, "1m05s left") == 0);
	CHANGES((gApp.batchActive = true, gApp.batchIndex = 2, gApp.batchTotal = 5,
			 gApp.batchInstalled = 1));
	assert(strstr(at().body, "\nCIA 2 of 5, 1 installed"));
	// Off the wire AM still writes the ring's tail: the bar goes on, the rate and ETA stop.
	CHANGES(gApp.wireBytes = 1000);
	assert(strcmp(at().title, "Writing to the card") == 0 && at().bar && !at().rate[0]);
	CHANGES(finalizeSeconds = 12);
	assert(strstr(at().body, "400 / 1000 bytes - still working, 12s elapsed."));
	// AM has every byte and commits: no bar, the counter goes on.
	CHANGES(gApp.uiWrittenBytes = 1000);
	assert(strcmp(at().title, "Finalizing") == 0 && !at().bar &&
		   strstr(at().body, "Committing the title - still working, 12s elapsed."));
	CHANGES(finalizeSeconds = 13);
	gApp.status = SERVER_INSTALL_FAILED;
	gApp.last.result = (Result)0xD8E0806A;
	assert(strcmp(at().hero, "0xD8E0806A") == 0);

	// A file counts its own bytes: a gzip body ends at its size, with no wire bar.
	memset(&gApp, 0, sizeof(gApp));
	gApp.status = SERVER_FILE;
	gApp.fileGzip = gApp.uiSunkProgress = true;
	gApp.wireTotal = gApp.uiWrittenBytes = 2000000;
	gApp.wireBytes = 15000;
	assert(strstr(at().body, "2000000 / 2000000 bytes") && at().percent == 100 &&
		   at().wirePercent == 0);
	CHANGES(gApp.uiWrittenBytes = 1000000);
	gApp.fileGzip = false;
	gApp.wireBytes = 1500000;
	assert(at().wirePercent == 75 && strstr(at().body, "1000000 / 2000000 bytes"));

	// A failed install's card is its own until the next install: a get after it starts clean and
	// counts its own bytes, and the card still says what the install took and why it stopped.
	memset(&gApp, 0, sizeof(gApp));
	gApp.last =
		(Operation){OUTCOME_REJECTED, "am-write", (Result)0xD8E0806A, 0x000400000F800100ULL, 3};
	gApp.lastBytes = gApp.wireBytes = 3145728;
	gApp.status = SERVER_INSTALL_FAILED;
	Screen card = at();
	VerbContext request = {.id = "fedcba9876543210fedcba9876543210"};
	statsStart(&request);
	gApp.wireBytes = gApp.wireTotal = 5;
	Screen kept = at();
	assert(memcmp(&card, &kept, sizeof(card)) == 0 && strcmp(kept.hero, "0xD8E0806A") == 0 &&
		   strstr(kept.body, "3145728 bytes received before it stopped\nam-write"));

	// The tick: a value change waits out UI_REDRAW_MIN_MS, a refused frame retries at once, a
	// status change draws at once, finalizing draws synchronously, an unchanged frame is not
	// resubmitted.
	memset(&gApp, 0, sizeof(gApp));
	gApp.status = SERVER_RECEIVING;
	gApp.wireTotal = 1000;
	fake.nowMs = 10000;
	drawScreen(NULL);
	unsigned sync = syncFrames;
	gApp.wireBytes = 100;
	fake.nowMs += UI_REDRAW_MIN_MS - 1;
	uiProgressTick();
	assert(tryFrames == 0);
	fake.nowMs += 1;
	gpuBusy = true;
	uiProgressTick();
	assert(tryFrames == 1 && shown.percent == 0);
	gpuBusy = false;
	uiProgressTick();
	assert(tryFrames == 2 && shown.percent == 10);
	gApp.status = SERVER_INSTALLING;
	uiProgressTick();
	assert(tryFrames == 3 && shown.screen == SERVER_INSTALLING);
	gApp.wireBytes = 1000; // same instant: finalizing is not throttled either
	uiProgressTick();
	assert(tryFrames == 3 && syncFrames == sync + 1 && shown.finalizing);
	fake.nowMs += UI_REDRAW_MIN_MS;
	uiProgressTick();
	assert(tryFrames == 3 && syncFrames == sync + 1);
	// No byte moves while AM commits, so the tail counts seconds from its first tick; the commit
	// draws at once, synchronously.
	fake.nowMs += 12000 - UI_REDRAW_MIN_MS;
	uiProgressTick();
	assert(finalizeSeconds == 12 && strstr(shown.body, "12s elapsed"));
	gApp.uiWrittenBytes = 1000;
	uiProgressTick();
	assert(tryFrames == 3 && syncFrames == sync + 3 && shown.committing);
	// The idle animation: 4 Hz, idle screens only. The INSTALLED card shows before READY returns.
	gApp.status = SERVER_READY;
	u8 before = phase;
	fake.nowMs = nextIdleMs;
	uiIdleAnimationTick();
	uiIdleAnimationTick();
	gApp.status = SERVER_RECEIVING;
	fake.nowMs += IDLE_TICK_MS;
	uiIdleAnimationTick();
	assert(phase == before + 1);
	gApp.status = SERVER_INSTALLED;
	uiCompletionTransition();
	assert(shown.screen == SERVER_INSTALLED && gApp.status == SERVER_READY);
	assert(holdUntilMs == fake.nowMs + COMPLETE_HOLD_MS);
	// A request that leaves the status idle keeps the card and its hold, in each status
	// appSetIdle can leave (a scan with no listener ends in STARTING or NEEDS_KEY); one that
	// changes the status draws.
	static const ServerStatus IDLE[] = {SERVER_STARTING, SERVER_NEEDS_KEY, SERVER_READY};
	for (size_t i = 0; i < sizeof(IDLE) / sizeof(IDLE[0]); i++) {
		gApp.status = IDLE[i];
		uiCompletionTransition();
		assert(shown.screen == SERVER_INSTALLED && holdUntilMs == fake.nowMs + COMPLETE_HOLD_MS);
	}
	gApp.status = SERVER_AUTH_FAILED;
	uiCompletionTransition();
	assert(shown.screen == SERVER_AUTH_FAILED);
	unsigned frames = syncFrames;
	uiCompletionTransition(); // a request that changed nothing draws nothing
	assert(syncFrames == frames);

	// The live rate: RECV86 4.3's rule, and UT-09's stall.
	assert(rateRule(NULL) == sizeof(RATE_CASES) / sizeof(RATE_CASES[0]));

	// With an import open, a POWER press does not end a key wait.
	fake.homeBlocked = fake.powerPressed = true;
	fake.keysDown = KEY_A;
	confirmAndRegenKey();
	assert(regens == 1);
	fake.homeBlocked = fake.powerPressed = false;
	fake.keysDown = 0;

	// Every screen that draws the key says so, and keeps it inside the rows a capture blacks out.
	memset(framebuffer, 0xFF, sizeof(framebuffer));
	snprintf(displayKey, sizeof(displayKey), "KEY1-2345");
	for (unsigned n = 0; n < UI_SCREEN_COUNT; n++) {
		Screen s;
		keyDraws = keyOutsideBand = 0;
		build(&s, n, NULL);
		show(&s, C3D_FRAME_SYNCDRAW);
		assert(uiShowsKey() == (keyDraws > 0) && keyOutsideBand == 0);
		captureChecks(uiShowsKey());
	}

	bmpTruth();
	scanPicture();
	scanScreens();
	scanTexture();
	asks();
	outcomes();
	wrapping();
	staleCard();

	// Capture-on-change: a new key's first frame and its frame CAPTURE_LATE_MS on; transfer frames
	// go to RAM slots (the overflow dropped and counted), written by the next idle frame.
	mkdir(CAPTURE_DIR, 0755);
	fclose(fopen(CAPTURE_DIR "/arm", "w"));
	u32 idle = SERVER_AUTH_FAILED << CAPTURE_SCREEN_SHIFT;
	u32 receiving = SERVER_RECEIVING << CAPTURE_SCREEN_SHIFT;
	benchFramePresented(idle, "Rejected: bad MAC", 1000);
	benchFramePresented(idle, "Rejected: bad MAC", 1000 + CAPTURE_LATE_MS - 1);
	assert(captured("000-Rejected-bad-MAC") >= 0 && captured("001-Rejected-bad-MAC") < 0);
	benchFramePresented(idle, "Rejected: bad MAC", 1000 + CAPTURE_LATE_MS);
	benchFramePresented(idle, "Rejected: bad MAC", 60000);
	memset(framebuffer, 0x11, sizeof(framebuffer));
	for (u32 key = receiving; key <= receiving + CAPTURE_SLOTS; key++) { // five of its variants
		benchFramePresented(key, "Receiving", 60000);
	}
	memset(framebuffer, 0x22, sizeof(framebuffer));
	assert(captured("001-Rejected-bad-MAC") >= 0 && captured("002-Receiving") < 0);
	benchFramePresented(idle, "Rejected: bad MAC", 60000);
	assert(captured("002-Receiving") == 0x11 && captured("005-Receiving") == 0x11);
	assert(captured("006-Rejected-bad-MAC") == 0x22);
	assert(strcmp(fake.log, "capture: dropped") == 0 && fake.logBytes == 1);
	// `off` writes the slots, frees them and removes the arm file, so the next boot stays off too.
	struct stat st;
	memset(framebuffer, 0x33, sizeof(framebuffer));
	benchFramePresented(receiving, "Receiving", 90000);
	assert(benchCaptureState().held == 1 && captured("007-Receiving") < 0);
	benchCaptureSet(false);
	CaptureState off = benchCaptureState();
	assert(captured("007-Receiving") == 0x33 && !off.armed && off.held == 0 && off.slots == 0 &&
		   stat(CAPTURE_DIR "/arm", &st) != 0);
	benchFramePresented(idle, "Rejected: bad MAC", 90001);
	assert(captured("008-Rejected-bad-MAC") < 0);
	benchCaptureSet(true);
	CaptureState on = benchCaptureState();
	assert(on.armed && on.slots == CAPTURE_SLOTS && on.frames == 8 &&
		   stat(CAPTURE_DIR "/arm", &st) == 0);
	// A stranded worker may still hold tier 1: the frames leading into the strand and the stranded
	// screen's own wait in the slots as a transfer's do, and go out once the worker is back; `off`
	// while it is stranded counts what is held as dropped.
	memset(framebuffer, 0x44, sizeof(framebuffer));
	benchFramePresented(receiving + 1, "Receiving", 100000); // a variant not yet seen: a new key
	strandedNow = true;
	memset(framebuffer, 0x55, sizeof(framebuffer));
	benchFramePresented(SERVER_STRANDED << CAPTURE_SCREEN_SHIFT, "Stranded", 100001);
	CaptureState stranded = benchCaptureState();
	assert(stranded.frames == 10 && stranded.held == 2 && stranded.dropped == 0);
	strandedNow = false;
	memset(framebuffer, 0x66, sizeof(framebuffer));
	benchFramePresented(idle, "Rejected: bad MAC", 100002);
	assert(captured("008-Receiving") == 0x44 && captured("009-Stranded") == 0x55 &&
		   captured("010-Rejected-bad-MAC") == 0x66 && benchCaptureState().held == 0);
	benchFramePresented(receiving + 2, "Receiving", 100003);
	strandedNow = true;
	benchCaptureSet(false);
	assert(captured("011-Receiving") < 0 && benchCaptureState().held == 0 &&
		   strcmp(fake.log, "capture: dropped") == 0 && fake.logBytes == 1);
	strandedNow = false;
	benchCaptureSet(true);
	// A frame that cannot be written is logged, keeps its number, and is left out of `frames`:
	// first a directory in the way of its top file, then a limit on file size under its bytes,
	// which is what a full card's short write comes to. A 1974-byte BMP (the fake's two columns)
	// stays in stdio's buffer, and fclose meets the limit; one of 5814 (eight columns) is
	// written by fwrite itself.
	unsigned named = benchCaptureState().frames;
	char blocked[96];
	snprintf(blocked, sizeof(blocked), CAPTURE_DIR "/%03u-Starting-up-top.bmp", named);
	assert(mkdir(blocked, 0755) == 0);
	benchFramePresented(100 << CAPTURE_SCREEN_SHIFT, "Starting up", 60000);
	assert(benchCaptureState().frames == named && strcmp(fake.log, "capture: write") == 0 &&
		   fake.logResult == EISDIR && fake.logBytes == 1);
	struct rlimit room, tiny;
	assert(getrlimit(RLIMIT_FSIZE, &room) == 0);
	tiny = room;
	tiny.rlim_cur = 1000;
	signal(SIGXFSZ, SIG_IGN); // else the write past the limit would end the test
	for (u32 lostBefore = 1; lostBefore <= 2; lostBefore++) {
		grabHeight = lostBefore == 1 ? 2 : 8;
		assert(setrlimit(RLIMIT_FSIZE, &tiny) == 0);
		benchFramePresented((100 + lostBefore) << CAPTURE_SCREEN_SHIFT, "Starting up", 60000);
		assert(setrlimit(RLIMIT_FSIZE, &room) == 0);
		assert(benchCaptureState().frames == named && strcmp(fake.log, "capture: write") == 0 &&
			   fake.logResult == EFBIG && fake.logBytes == lostBefore + 1);
	}
	signal(SIGXFSZ, SIG_DFL);
	grabHeight = 2;
	for (u32 key = 100; key < 200; key++) {
		benchFramePresented(key << CAPTURE_SCREEN_SHIFT, "Starting up", 60000);
	}
	assert(captured("047-Starting-up") >= 0 && captured("048-Starting-up") < 0);
	assert(benchCaptureState().frames == CAPTURE_MAX - 3); // all 48 named, three of them lost
	uiExit();
	TEST_PASS();
	return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Printable ASCII only, because CHN/KOR/TWN consoles map a regional system font
// and ASCII is all they share.
#include "ui.h"

#include "auth.h"
#include "codec.h"
#include "fs.h"
#include "knobs.h" // BENCH
#include "net.h"   // pressedKeys
#include "path.h"
#include "ring.h" // ringStranded
#include "scan.h" // SCAN_WIDTH, SCAN_HEIGHT
#include "sysinfo.h"
#include "urls.h" // fetchWhy

#include <3ds.h>
#include <citro2d.h>
#include <float.h>	// FLT_MAX
#include <limits.h> // INT_MAX
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h> // MIN, MAX

// Every string drawn lives in the Screen, so its size bounds a frame's glyphs (asserted below).
#define TEXT_GLYPHS 1024
// C2D_DrawText appends a quad per glyph without checking the vertex buffer.
#define MAX_OBJECTS (TEXT_GLYPHS + 64)
#define IDLE_TICK_MS 250ULL
#define TEXT_MAX 320 // a body or a guide, wrapped or not
#define COMPLETE_HOLD_MS 1000ULL
#define O2DS_NOTICE_FRAMES 180 // long enough to read, and a headless boot still continues
#define FINALIZE_COUNTER_S 10U // before this a counter reads as a stopwatch

// 1.0 is the system font's native 30 px cell.
#define S_SMALL 0.45f
#define S_BODY 0.55f
#define S_TITLE 0.7f
#define S_HERO 1.0f

#define RGB(r, g, b) (0xFF000000u | (u32)(b) << 16 | (u32)(g) << 8 | (u32)(r))
#define BG_TOP RGB(0x2B, 0x28, 0x26)
#define BG_BOTTOM RGB(0x20, 0x1D, 0x1C)
#define LINE RGB(0x3A, 0x38, 0x36)
#define INK RGB(0x23, 0x1F, 0x1E)
#define FG RGB(0xF4, 0xF1, 0xEF)
#define DIM RGB(0xAE, 0xAC, 0xB6)
#define ACCENT RGB(0xAE, 0x8B, 0xFF)
#define GREEN RGB(0x55, 0xC4, 0x83)
#define BLUE RGB(0x5A, 0xA9, 0xF0)
#define AMBER RGB(0xEA, 0x94, 0x2F)
#define RED RGB(0xF6, 0x6C, 0x6D)
#define K_EXIT "START  exit"
#define K_CANCEL "START  cancel"
#define K_REGEN "START  exit    Y  scan QR    UP+SELECT+B  new key"
#define K_NEW_KEY "START  exit    UP+SELECT+B  new key"
#define K_ANY "ANY  continue"
#define TEXT_OF(macro) TEXT_OF_(macro)
#define TEXT_OF_(word) #word
#define PORT_TEXT TEXT_OF(DEFAULT_PORT) // app.h's port; testTools.sh requires it to match wire.sh's

// ---- The rate/ETA sampler ----
// The figure is min(sunk, received) gained over the last RATE_WINDOW_MS of progress ticks. Sunk
// bytes cap it while the sink holds, and received bytes cap it while the sink drains its backlog,
// so it never reads faster than the transfer.
#define RATE_WINDOW_MS 5000ULL
// Until both of the minimums below are reached, one write, or one hold, would be the whole figure.
#define RATE_MIN_MS 2000ULL
#define RATE_MIN_BYTES (1024ULL * 1024ULL)
// A tick is kept at most every UI_REDRAW_MIN_MS. RATE_TICKS counts the window's
// ticks and the one before it.
#define RATE_TICKS (RATE_WINDOW_MS / UI_REDRAW_MIN_MS + 1ULL)
// The byte counts stop with the transfer, so only a clock can tell that it stopped. RATE_STALL_MS
// is longer than AM's ordinary long writes and shorter than its timeout-and-retry writes.
#define RATE_STALL_MS 15000ULL

typedef struct {
	u64 ms, received, sunk;
} RateTick;

typedef struct {
	bool active;
	u64 expectedBytes;
	u64 startSunk;
	// when the sunk count last moved
	u64 sunkMs;
	// oldest first
	RateTick ticks[RATE_TICKS];
	unsigned count;
	u32 rateHundredths;
	bool rateValid;
	u32 etaSeconds; // 0 is unknown, not finished
} RateSampler;

static RateSampler rateSampler;
static u32 finalizeSeconds; // since AM took the last byte (set by rateTick); counter shows from 10

// A gzip body's wire bytes are not the file's. For a get, the file bytes sent stand in for
// both counts. For a put, the received count is the bytes inflated so far, so its 1 MiB
// writes never read above it.
static u64 rateReceived(const App *app) {
	if (!app->fileGzip) {
		return app->wireBytes;
	}
	return app->fileSending ? app->uiWrittenBytes : app->inflatedBytes;
}

static bool transferRateEligible(const App *app) {
	return app->status == SERVER_RECEIVING || app->status == SERVER_FILE ||
		   (app->status == SERVER_INSTALLING && app->wireTotal > 0 &&
			app->wireBytes < app->wireTotal);
}

static bool installFinalizing(unsigned screen, const App *app) {
	return screen == SERVER_INSTALLING && app->wireTotal > 0 && app->wireBytes >= app->wireTotal;
}

static void resetRateSampler(const App *app, u64 now) {
	rateSampler = (RateSampler){.active = transferRateEligible(app),
								.expectedBytes = app->wireTotal,
								.startSunk = app->uiWrittenBytes,
								.sunkMs = now,
								.ticks = {{now, rateReceived(app), app->uiWrittenBytes}},
								.count = 1};
}

static void sampleLiveRate(const App *app, u64 now) {
	if (!rateSampler.active || !transferRateEligible(app)) {
		return;
	}
	RateTick *ticks = rateSampler.ticks;
	if (app->uiWrittenBytes != ticks[rateSampler.count - 1].sunk) {
		rateSampler.sunkMs = now;
	}
	if (now - ticks[rateSampler.count - 1].ms < UI_REDRAW_MIN_MS) {
		return;
	}
	// The base is the newest tick at least RATE_WINDOW_MS old, or the first.
	unsigned drop = 0;
	while (drop + 1 < rateSampler.count && now - ticks[drop + 1].ms >= RATE_WINDOW_MS) {
		drop++;
	}
	// If ticks are closer together than UI_REDRAW_MIN_MS, drop one more so the count
	// never goes past the array.
	if (rateSampler.count - drop == RATE_TICKS) {
		drop++;
	}
	rateSampler.count -= drop;
	memmove(ticks, ticks + drop, rateSampler.count * sizeof(ticks[0]));
	ticks[rateSampler.count++] = (RateTick){now, rateReceived(app), app->uiWrittenBytes};

	// rateTick resets the sampler on a sunk count that falls back, so none of these wrap.
	u64 span = now - ticks[0].ms;
	u64 gained = MIN(app->uiWrittenBytes - ticks[0].sunk, rateReceived(app) - ticks[0].received);
	u64 sunk = app->uiWrittenBytes - rateSampler.startSunk;
	// A stall shows "unknown" once it outlasts AM's long writes.
	rateSampler.rateValid =
		sunk >= RATE_MIN_BYTES && span >= RATE_MIN_MS && now - rateSampler.sunkMs < RATE_STALL_MS;
	// bytes/ms is decimal kB/s, ten of which is a hundredth of a MB/s.
	rateSampler.rateHundredths =
		rateSampler.rateValid ? (u32)MIN((gained + span * 5ULL) / (span * 10ULL), UINT32_MAX) : 0;
	// 0 is unknown, never "0 seconds remaining". The estimate runs a few seconds short because the
	// commit still follows the last sunk byte.
	rateSampler.etaSeconds = 0;
	if (rateSampler.rateValid && gained > 0 && rateSampler.expectedBytes > sunk) {
		u64 seconds = (rateSampler.expectedBytes - sunk) * span / (gained * 1000ULL);
		rateSampler.etaSeconds = (u32)MIN(seconds, UINT32_MAX);
	}
}

// The finalize clock is time-driven because no byte moves while AM commits.
static void rateTick(const App *app, u64 now) {
	static u64 finalizeStartMs;
	bool finalizing = installFinalizing(app->status, app);
	if (!finalizing) {
		finalizeStartMs = 0;
	} else if (finalizeStartMs == 0) {
		finalizeStartMs = now;
	}
	finalizeSeconds = finalizing ? (u32)((now - finalizeStartMs) / 1000ULL) : 0;
	bool reset = transferRateEligible(app)
					 ? !rateSampler.active || rateSampler.expectedBytes != app->wireTotal ||
						   app->uiWrittenBytes < rateSampler.ticks[rateSampler.count - 1].sunk
					 : rateSampler.active || rateSampler.rateValid;
	if (reset) {
		resetRateSampler(app, now);
	}
	sampleLiveRate(app, now);
}
// ---- end of the sampler ----

enum {
	UI_ORIGINAL_2DS = UI_NOTICE_END,
	UI_KEY_CONFIRM,
	UI_KEY_BUSY,
	UI_KEY_OK,
	UI_KEY_FAIL,
	UI_SCREEN_COUNT
};

// A row with no keys is a wait and shows the liveness dots instead, except
// Installing, which shows progress.
static const struct {
	u32 hue;
	const char *title, *body, *guide, *keys;
} SCREENS[] = {
	[SERVER_STARTING] = {ACCENT, "Starting up", "Waiting for the network to come up.",
						 "Retrying every second.", K_EXIT},
	[SERVER_NEEDS_KEY] = {RED, "No pairing key",
						  "No usable key is set, and none was created automatically.\n"
						  "Required file: " KEY_PATH,
						  "Put a key in key.txt, then relaunch, or make a new one with the keys "
						  "below.",
						  K_NEW_KEY},
	[SERVER_READY] = {GREEN, "Listening",
					  "Set THREEDROP_KEY on your computer to this key.\nlog: " LOG_PATH,
					  "Send from your computer:\ntools/drop.sh game.cia <ip>", K_REGEN},
	[SERVER_LISTEN_FAILED] = {RED, "Port error",
							  "Wi-Fi dropped, or another app is using port " PORT_TEXT ".",
							  "Retrying every second. Check that Wi-Fi is on.", K_EXIT},
	// The fix is on the computer, because a new key here would leave every paired computer with a
	// stale key. The card stays until the next install or dismissal, so its wording refers to a
	// sender, not to the last one.
	[SERVER_AUTH_FAILED] = {RED, "Rejected: wrong key", "A sender's key didn't match this one.",
							"Still listening.\n"
							"Set THREEDROP_KEY on your computer to the key above.",
							K_REGEN},
	// START cancels. The pump takes the press (transfer.c), so the loop never sees
	// it and never exits on it.
	[SERVER_RECEIVING] = {BLUE, "Receiving", "", "START cancels the transfer.", K_CANCEL},
	[SERVER_INSTALLING] = {AMBER, "Installing", "", "HOME disabled. Do not power off.", ""},
	// This card and the failed ones (buildFailure's too) say nothing about a listener,
	// because a scanned link installs with no key, so there may be none. The status screen
	// that replaces them does say.
	[SERVER_INSTALLED] = {GREEN, "Install complete", "", "Title installed and verified.", K_EXIT},
	// This row's words are for a refusal; buildFailure has the words for a cancel
	// and for an unknown outcome.
	[SERVER_INSTALL_FAILED] = {RED, "Install failed", "",
							   "Check the CIA and free space, then send again.", "START  dismiss"},
	[SERVER_FILE] = {BLUE, "", "", "START cancels the transfer.", K_CANCEL},
	// No key hint, because START, X and every transfer wait for the worker.
	[SERVER_STRANDED] = {AMBER, "Stuck in the system",
						 "The last transfer is waiting on a system call that has not returned. It "
						 "clears itself when the call returns; until then no transfer starts.",
						 "Nothing to do but wait.", ""},
	[UI_STARTUP] = {ACCENT, "Starting up", "Setting up...", "", ""},
	[UI_SELF_UPDATED] = {ACCENT, "Self-update installed", "Relaunching 3Drop...", "", ""},
	[UI_RUN_SELF] = {ACCENT, "Restarting", "Relaunching 3Drop...", "", ""},
	[UI_LAUNCH] = {ACCENT, "Launching", "Starting the app...", "", ""},
	// X works in any state. The notice holds for a second over that state's screen, then the
	// screen returns, so the notice promises nothing about that state's listener or keys (START
	// dismisses a failed install).
	[UI_CLEANUP_DONE] = {GREEN, "Cleanup done",
						 "Imports that finished but were never committed were cleared from the SD "
						 "card.",
						 "Returning to the status screen.", ""},
	[UI_CLEANUP_FAILED] = {RED, "Cleanup failed", "AM refused the cleanup: ",
						   "The code is in the log too. Returning to the status screen.", ""},
	// At a QR screen the computer can only look (`stats` and `screenshot`), and while a download
	// runs it gets no reply (main.c's serveBusy). The guides say so. The scan's top screen is the
	// camera's picture, so its words are all in the guide.
	[UI_QR_SCAN] = {ACCENT, "Scan a QR code", "",
					"Point the outer camera at a QR code with a link to a CIA, and hold still.\n"
					"The computer can only look until you are done.",
					"B  cancel"},
	[UI_QR_CONFIRM] = {AMBER, "Install from the scanned code?", "",
					   "Checked like a CIA sent from your computer.\n"
					   "An installed title is updated. 3Drop's own CIA relaunches 3Drop.\n"
					   "The computer can only look until this is done.",
					   "A  install      B  cancel"},
	[UI_QR_BUNDLE] = {BLUE, "Certificates",
					  "Getting the list of certificate authorities from curl.se.",
					  "The computer gets no reply until this is done.", "B  cancel"},
	[UI_QR_CHECK] = {RED, "Certificate check failed", "",
					 "A  gets a fresh list and tries again.\n"
					 "Y  skips the check for this download only.",
					 "A  fresh list      Y  skip check      B  cancel"},
	[UI_QR_REFUSED] = {RED, "QR code not used", "", "", K_ANY},
	[SERVER_FATAL] = {RED, "Fatal error", "", "Service init failed. Nothing to receive.", K_EXIT},
	[UI_ORIGINAL_2DS] = {AMBER, "Original 2DS detected",
						 "This console's single shared LCD can't do 800px wide mode, so 3Drop "
						 "uses the standard 400px top screen. Everything else is unaffected.",
						 "Shown once at startup on Original 2DS only.", K_ANY},
	[UI_KEY_CONFIRM] = {AMBER, "Generate a NEW key?",
						"This replaces key.txt on the SD card. THREEDROP_KEY stops working until "
						"every paired computer has the new key.",
						"Every paired computer must be updated after.",
						"A  generate new key      B  cancel"},
	[UI_KEY_BUSY] = {ACCENT, "Generating key...", "This takes a few seconds.", "", ""},
	[UI_KEY_OK] = {GREEN, "New pairing key", "Set THREEDROP_KEY on your computer to this.",
				   "Update THREEDROP_KEY on your computer.", K_ANY},
	[UI_KEY_FAIL] = {RED, "Key generation failed", "The old key is still in use.",
					 "Your existing key still works.", K_ANY},
};
_Static_assert(sizeof(SCREENS) / sizeof(SCREENS[0]) == UI_SCREEN_COUNT, "a screen has no row");

// Fixed arrays, zeroed before building, so memcmp is a complete change test.
typedef struct {
	u32 hue;
	u32 dot; // 0 means none
	u8 screen, percent, wirePercent, phase;
	bool bar, dots;
	bool finalizing, committing;
	bool preview; // the top screen is the scanner's picture
	char meta[64], title[48], hero[KEY_MAX + 1], rate[16], eta[20]; // hero holds the whole key
	char body[TEXT_MAX], guide[TEXT_MAX], keys[64];
} Screen;
_Static_assert(sizeof(Screen) + sizeof("3Drop") < TEXT_GLYPHS, "a frame can outgrow the glyphs");

static C3D_RenderTarget *top, *bottom;
static C2D_TextBuf textBuf, measureBuf;
static bool ready;
static Screen shown;
static u64 shownMs, nextIdleMs;
static u64 cardUntilMs, holdUntilMs; // when the card's hold ends; idle frames wait out holdUntilMs
static u8 phase;
static const char *version, *ipText;

// ---- The scanner's picture ----
// A texture a power of two wide and tall, holding the camera's 400x240 frame at its top left. A 3DS
// texture is 8x8 tiles in row order, with the texels in each tile in Morton order and the picture's
// first row first (tex3ds writes them that way, and Universal Updater tiles a camera frame the same
// way); its sub-texture is what tex3ds gives a picture at the top left of its sheet.
#define PREVIEW_TEX_W 512
#define PREVIEW_TEX_H 256
#define PREVIEW_TEX_BYTES (PREVIEW_TEX_W * PREVIEW_TEX_H * 2)
static C3D_Tex previewTex;
static bool previewReady;
static const u16
	*previewFrame; // the next show() loads it, once the GPU is done with the last frame
static const Tex3DS_SubTexture PREVIEW_SUB = {SCAN_WIDTH,
											  SCAN_HEIGHT,
											  0.0f,
											  1.0f,
											  (float)SCAN_WIDTH / PREVIEW_TEX_W,
											  1.0f - (float)SCAN_HEIGHT / PREVIEW_TEX_H};

// Returns the bits of x and y interleaved, x lowest, which is the texel order in a tile.
static unsigned morton(unsigned x, unsigned y) {
	return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

static void previewLoad(u16 *texture, const u16 *frame) {
	for (unsigned tileY = 0; tileY < SCAN_HEIGHT / 8; tileY++) {
		for (unsigned tileX = 0; tileX < SCAN_WIDTH / 8; tileX++) {
			u16 *tile = texture + ((size_t)tileY * (PREVIEW_TEX_W / 8) + tileX) * 64;
			for (unsigned y = 0; y < 8; y++) {
				const u16 *row = frame + (size_t)(tileY * 8 + y) * SCAN_WIDTH + tileX * 8;
				for (unsigned x = 0; x < 8; x++) {
					tile[morton(x, y)] = row[x];
				}
			}
		}
	}
}

static bool showsKey(unsigned screen) {
	return screen == SERVER_READY || screen == SERVER_AUTH_FAILED || screen == UI_KEY_OK;
}

bool uiShowsKey(void) {
	return showsKey(shown.screen);
}

#define ADD(field, ...) appendf((field), sizeof(field), __VA_ARGS__)

// ---- Word wrap ----
// C2D_WordWrap breaks greedily and can leave one word alone on the last row, which reads as a slip.
// The rows are broken here instead. The breaks are the same greedy ones, but then a last row of one
// word takes the last word of the row above, when both still fit. Only a space becomes a newline,
// so the text keeps its length. A word wider than a row keeps a row of its own, and text() shrinks
// all of the text to fit it.
#define ROW_MAX 160 // the most characters a row is measured over (more is wider than any screen)

// Returns the width of `length` characters of `str`, parsed into a buffer of their own because
// textBuf holds only what the frame draws (TEXT_GLYPHS). citro2d's parse never returns NULL
// (it stops at a full buffer, which a row this short never fills); if it did, the row would
// break instead of overflowing.
static float rowWidth(const char *str, size_t length, float scale) {
	char row[ROW_MAX];
	C2D_Text parsed;
	if (length >= sizeof(row)) {
		return FLT_MAX;
	}
	memcpy(row, str, length);
	row[length] = '\0';
	C2D_TextBufClear(measureBuf);
	return C2D_TextParse(&parsed, measureBuf, row) ? parsed.width * scale : FLT_MAX;
}

// Breaks one paragraph, with no newline in it, into rows no wider than `width`.
static void wrapParagraph(char *para, float scale, float width) {
	char *row = para + strspn(para, " "), *above = NULL; // the row being filled, and the one above
	for (char *word = row; *word;) {
		char *end = word + strcspn(word, " ");
		if (word > row && rowWidth(row, (size_t)(end - row), scale) > width) {
			word[-1] = '\n'; // the last space of those before the word
			above = row;
			row = word;
		}
		word = end + strspn(end, " ");
	}
	// where the last row's first word ends
	char *tail = row + strcspn(row, " ");
	if (!above || tail[strspn(tail, " ")]) { // no row above, or the last row is more than a word
		return;
	}
	char *last = row - 1; // the newline that ends the row above
	while (last > above && last[-1] == ' ') {
		last--;
	}
	while (last > above && last[-1] != ' ') {
		last--; // to the start of the last word of the row above
	}
	if (last == above) { // the row above is one word, so it has none to give
		return;
	}
	row[-1] = ' ';
	if (rowWidth(last, (size_t)(tail - last), scale) <= width) {
		last[-1] = '\n';
	} else {
		row[-1] = '\n';
	}
}

// Breaks `str` into rows in place, one paragraph at a time (a paragraph is a line ended
// by a newline of its own).
static void wrapRows(char *str, float scale, float width) {
	for (char *para = str; para;) {
		char *eol = strchr(para, '\n');
		if (eol) {
			*eol = '\0';
		}
		wrapParagraph(para, scale, width);
		if (eol) {
			*eol = '\n';
		}
		para = eol ? eol + 1 : NULL;
	}
}

// y is the baseline, so rows line up across sizes and across the regional fonts' cell metrics. A
// text wider than `width` shrinks to fit, and that includes a text broken into rows that has a word
// wider than a row. A text that citro2d wraps does not shrink.
static void text(const char *str, float x, float y, float scale, u32 color, u32 flags,
				 float width) {
	C2D_Text parsed;
	if (!textBuf || !str[0] || !C2D_TextParse(&parsed, textBuf, str)) {
		return;
	}
	C2D_TextOptimize(&parsed);
	if (!(flags & C2D_WordWrap) && width > 0.0f && parsed.width * scale > width) {
		scale = width / parsed.width;
	}
	C2D_DrawText(&parsed, flags | C2D_WithColor | C2D_AtBaseline, x, y, 0.0f, scale, scale, color,
				 width);
}

// A text's rows as a call site last drew them. A redraw breaks them again only when the text, its
// scale or its width changed. Mid-transfer the body's byte count changes, at most every
// UI_REDRAW_MIN_MS on the thread that runs the transfer, and the guide never changes.
typedef struct {
	char in[TEXT_MAX], rows[TEXT_MAX];
	float scale, width;
} Wrapped;
static Wrapped bodyRows, guideRows;

// Draws `str` in rows no wider than `width`. With no buffer to measure them in, citro2d breaks them
// the way it did before (a lone last word and all), so the words still show.
static void textWrapped(Wrapped *w, const char *str, float x, float y, float scale, u32 color,
						float width) {
	if (!measureBuf) {
		text(str, x, y, scale, color, C2D_WordWrap, width);
		return;
	}
	if (scale != w->scale || width != w->width || strcmp(str, w->in) != 0) {
		snprintf(w->in, sizeof(w->in), "%s", str);
		snprintf(w->rows, sizeof(w->rows), "%s", str);
		wrapRows(w->rows, scale, width);
		w->scale = scale;
		w->width = width;
	}
	text(w->rows, x, y, scale, color, 0, width);
}

static u32 withAlpha(u32 color, u32 alpha) {
	return (color & 0x00FFFFFFu) | alpha << 24;
}

// The dot is the notification LED's twin, and it is shown with the lid open too.
static u32 dotColor(u64 nowMs) {
	Led led = appLed();
	if (!led.rgb) {
		return 0;
	}
	u32 level = led.breathes ? ledBreath(nowMs / LED_SAMPLE_MS) : 0xFF, c[3];
	ledChannels(led.rgb, c);
	return withAlpha(RGB(c[0], c[1], c[2]), level);
}

// The dot's breath alone never adds a frame mid-transfer or per request.
static bool differs(const Screen *next) {
	Screen s = *next;
	if (withAlpha(s.dot, 0) == withAlpha(shown.dot, 0)) {
		s.dot = shown.dot;
	}
	return memcmp(&s, &shown, sizeof(s)) != 0;
}

// The logical size is 400x240 in both modes, because citro2d keeps the 400-wide projection on the
// wide target. A frame the GPU refused leaves `shown` alone, so the next tick retries it.
static void show(const Screen *s, u8 frameFlags) {
	if (!ready || (!top && !bottom) || !C3D_FrameBegin(frameFlags)) {
		return;
	}
	if (textBuf) {
		C2D_TextBufClear(textBuf);
	}
	// Beginning this frame waited for the last frame's GPU work, which may be reading the texture.
	if (s->preview && previewFrame) {
		previewLoad(previewTex.data, previewFrame);
		GSPGPU_FlushDataCache(previewTex.data, PREVIEW_TEX_BYTES);
	}
	previewFrame = NULL;
	for (int screen = 0; screen < 2; screen++) {
		C3D_RenderTarget *target = screen ? bottom : top;
		float w = screen ? 320.0f : 400.0f;
		if (!target) {
			continue;
		}
		C2D_TargetClear(target, screen ? BG_BOTTOM : BG_TOP);
		C2D_SceneBegin(target);
		for (int i = 0; s->dots && i < 3; i++) {
			u32 alpha = (u32)i == s->phase % 3U ? 0xFFU : 0x58U;
			C2D_DrawCircleSolid(w / 2 + (float)(i - 1) * 14.0f, 200.0f - screen * 80.0f, 0.0f, 3.0f,
								withAlpha(s->hue, alpha));
		}
		if (screen) {
			textWrapped(&guideRows, s->guide, 12.0f, 28.0f, S_BODY, FG, 296.0f);
			C2D_DrawRectSolid(0.0f, 206.0f, 0.0f, 320.0f, 1.0f, LINE);
			text(s->keys, 12.0f, 227.0f, S_SMALL, s->hue, 0, 296.0f);
			continue;
		}
		if (s->preview) {
			C2D_DrawImageAt((C2D_Image){&previewTex, &PREVIEW_SUB}, 0.0f, 0.0f, 0.0f, NULL, 1.0f,
							1.0f);
			continue;
		}
		text("3Drop", 12.0f, 16.0f, S_SMALL, FG, 0, 0.0f);
		text(s->meta, 376.0f, 16.0f, S_SMALL, DIM, C2D_AlignRight, 300.0f);
		if (s->dot) {
			C2D_DrawCircleSolid(388.0f, 12.0f, 0.0f, 3.5f, s->dot);
		}
		C2D_DrawRectSolid(0.0f, 24.0f, 0.0f, 400.0f, 34.0f, s->hue);
		text(s->title, 12.0f, 48.0f, S_TITLE, INK, 0, 376.0f);
		// These columns are right-anchored because the system font has no monospace face, so one
		// string would shift on every sample. A key here must stay inside UI_KEY_TOP..UI_KEY_END.
		text(s->hero, 12.0f, 100.0f, S_HERO, FG, 0, s->rate[0] ? 200.0f : 376.0f);
		text(s->rate, 300.0f, 100.0f, S_BODY, s->hue, C2D_AlignRight, 0.0f);
		text(s->eta, 388.0f, 100.0f, S_BODY, s->hue, C2D_AlignRight, 0.0f);
		textWrapped(&bodyRows, s->body, 12.0f, s->hero[0] ? 128.0f : 82.0f, S_BODY, DIM, 376.0f);
		if (s->bar) {
			// Dim shows what came off the wire and bright shows what was sunk, so the gap is what
			// is still in the ring to drain.
			C2D_DrawRectSolid(12.0f, 214.0f, 0.0f, 376.0f, 10.0f, LINE);
			C2D_DrawRectSolid(12.0f, 214.0f, 0.0f, 3.76f * s->wirePercent, 10.0f, DIM);
			C2D_DrawRectSolid(12.0f, 214.0f, 0.0f, 3.76f * s->percent, 10.0f, s->hue);
		}
	}
	C3D_FrameEnd(0);
	memcpy(&shown, s, sizeof(shown)); // with the padding bytes build() zeroed
	shownMs = uptimeMs();
	// Each commit or finalize stage, and its counter, counts as a screen of its own, so its first
	// frame is kept.
	BENCH(benchFramePresented((u32)s->screen << CAPTURE_SCREEN_SHIFT | (u32)s->committing << 2 |
								  (u32)s->finalizing << 1 |
								  (s->finalizing && finalizeSeconds >= FINALIZE_COUNTER_S),
							  s->title, shownMs));
}

// A gzip file's wire bytes are not the file's, so its wire bar is hidden.
static void buildTransfer(Screen *s, const App *app, const char *id) {
	// Past the wire the counter is the only sign of life, because the
	// first-write hold stalls the bar.
	char elapsed[40] = "";
	if (s->finalizing && finalizeSeconds >= FINALIZE_COUNTER_S) {
		snprintf(elapsed, sizeof(elapsed), " - still working, %lus elapsed.",
				 (unsigned long)finalizeSeconds);
	}
	if (s->committing) {
		snprintf(s->title, sizeof(s->title), "Finalizing");
		ADD(s->body, "Committing the title%s", elapsed[0] ? elapsed : " - this can take a moment.");
	} else {
		if (s->finalizing) {
			snprintf(s->title, sizeof(s->title), "Writing to the card");
		}
		s->bar = true;
		s->percent = (u8)percentOf(transferDoneBytes(app), app->wireTotal);
		s->wirePercent = app->status == SERVER_FILE && app->fileGzip
							 ? 0
							 : (u8)percentOf(app->wireBytes, app->wireTotal);
		ADD(s->hero, "%u%%", (unsigned)s->percent);
		if (app->viaUrl &&
			app->wireTotal == 0) { // a link's size is known only once its server answers
			ADD(s->body, "Connecting...");
		} else {
			ADD(s->body, "%llu / %llu bytes%s", (unsigned long long)transferDoneBytes(app),
				(unsigned long long)app->wireTotal, elapsed);
		}
	}
	if (!s->finalizing) {
		u32 rate = rateSampler.rateHundredths, eta = rateSampler.etaSeconds;
		if (!rateSampler.rateValid) {
			ADD(s->rate, "--.-- MB/s");
		} else {
			ADD(s->rate, "%lu.%02lu MB/s", (unsigned long)(rate / 100U),
				(unsigned long)(rate % 100U));
		}
		if (!rateSampler.rateValid || eta == 0) {
			ADD(s->eta, "--m--s left");
		} else {
			ADD(s->eta, "%lum%02lus left", (unsigned long)(eta / 60U), (unsigned long)(eta % 60U));
		}
	}
	if (id[0]) {
		ADD(s->body, "\n%s", id);
	}
	if (app->batchActive && app->batchTotal > 0) {
		ADD(s->body, "\nCIA %lu of %lu, %lu installed", (unsigned long)app->batchIndex,
			(unsigned long)app->batchTotal, (unsigned long)app->batchInstalled);
	}
}

// For a download from a scanned link (qr.c), the screen says so and B stops it.
static void buildLinkTransfer(Screen *s, unsigned screen) {
	if (screen == SERVER_RECEIVING) {
		snprintf(s->title, sizeof(s->title), "Downloading");
		snprintf(s->guide, sizeof(s->guide),
				 "B stops the download.\nThe computer gets no reply until it is done.");
		snprintf(s->keys, sizeof(s->keys), "B  cancel");
	} else if (screen == SERVER_INSTALLING) {
		if (s->committing) {
			snprintf(s->title, sizeof(s->title), "Committing");
			snprintf(s->guide, sizeof(s->guide), "Cannot cancel this commit. Do not power off.");
		} else {
			snprintf(s->guide, sizeof(s->guide), "B cancels before the commit. Do not power off.");
			snprintf(s->keys, sizeof(s->keys), "B  cancel");
		}
	}
}

// An install that did not commit falls into one of four cases: a cancel the person asked for
// (this CIA is not installed; the batch's earlier CIAs stay as they were), a cancel whose ticket
// would not go (nothing is installed, but a ticket may remain, and HOME shows none), an outcome
// AM could not confirm (it may have installed, so look before sending it again, as the host
// says), or a refusal. A scanned link's card adds what its wire word means, for the person who
// scanned, and speaks of scanning.
static void buildFailure(Screen *s, const App *app, const char *id) {
	const Operation *last = &app->last;
	unsigned long long bytes = app->lastBytes;
	if (last->result == TRANSFER_CANCELED) {
		snprintf(s->title, sizeof(s->title), "Canceled");
		ADD(s->body, "%llu bytes received before it was canceled", bytes);
		snprintf(s->guide, sizeof(s->guide), "This CIA was not installed.");
		return;
	}
	ADD(s->hero, "0x%08lX", (unsigned long)(u32)last->result);
	if (last->detail && strcmp(last->detail, "ticket") == 0) {
		// FBI lists a ticket by its title id, and deletes it from that list.
		snprintf(s->title, sizeof(s->title), "Canceled, ticket may remain");
		ADD(s->body, "%llu bytes received before it was canceled\n%s", bytes, id);
		snprintf(s->guide, sizeof(s->guide),
				 "This CIA was not installed, but its ticket may remain, and HOME can't show it. "
				 "FBI can delete it: Tickets, this title ID, Delete Ticket.");
	} else if (last->outcome == OUTCOME_UNKNOWN) {
		snprintf(s->title, sizeof(s->title), "Install unconfirmed");
		ADD(s->body, "%llu bytes received\n%s", bytes, last->detail);
		snprintf(s->guide, sizeof(s->guide),
				 "AM could not say how it ended. Look in HOME before you %s it again.",
				 app->lastViaUrl ? "scan" : "send");
	} else {
		ADD(s->body, "%llu bytes received before it stopped\n%s", bytes, last->detail);
		if (app->lastViaUrl) {
			snprintf(s->guide, sizeof(s->guide),
					 "Check the link and the free space, then scan again.");
		}
	}
	char words[120];
	if (app->lastViaUrl && fetchWhy(words, sizeof(words), last->detail, last->result)) {
		ADD(s->body, "\n%s", words);
	}
}

static void build(Screen *s, unsigned screen, const char *detail) {
	const App *app = &gApp;
	memset(s, 0, sizeof(*s));
	s->screen = (u8)screen;
	s->hue = SCREENS[screen].hue;
	s->dot = dotColor(uptimeMs());
	s->finalizing = installFinalizing(screen, app);
	s->committing = s->finalizing && app->uiWrittenBytes >= app->wireTotal;
	if (ipText && ipText[0]) {
		ADD(s->meta, "%s:%d   ", ipText, DEFAULT_PORT);
	}
	ADD(s->meta, "%s   build ", version);
	if (app->installedTitleVersion >= 0) {
		ADD(s->meta, "%d", app->installedTitleVersion);
	} else {
		ADD(s->meta, "?");
	}
	ADD(s->title, "%s", SCREENS[screen].title);
	// A '\f' in `detail` ends what the top screen says, and what follows it is added to
	// the bottom screen's text.
	const char *more = detail ? strchr(detail, '\f') : NULL;
	ADD(s->body, "%s%.*s", SCREENS[screen].body, more ? (int)(more - detail) : INT_MAX,
		detail ? detail : "");
	ADD(s->guide, "%s%s", SCREENS[screen].guide, more ? more + 1 : "");
	ADD(s->keys, "%s", SCREENS[screen].keys);
	if (showsKey(screen)) {
		ADD(s->hero, "%s", authDisplayKey() ? authDisplayKey() : "");
	}
	char id[48] = "";
	if (app->last.titleID != 0) {
		snprintf(id, sizeof(id), "title %016llX  v%d", (unsigned long long)app->last.titleID,
				 app->last.version);
	}
	switch (screen) {
	case SERVER_INSTALLED:
		ADD(s->body, "%llu bytes received and verified\n%s", (unsigned long long)app->lastBytes,
			id);
		break;
	case SERVER_INSTALL_FAILED:
		buildFailure(s, app, id);
		break;
	case SERVER_RECEIVING:
	case SERVER_INSTALLING:
		buildTransfer(s, app, id);
		if (app->viaUrl) {
			buildLinkTransfer(s, screen);
		}
		break;
	case SERVER_FILE:
		ADD(s->title, "%s", app->fileSending ? "Sending" : "Receiving");
		buildTransfer(s, app, app->fileName);
		break;
	case SERVER_STRANDED:
		if (!aptIsHomeAllowed()) {
			ADD(s->guide, "\nHOME is held until then.");
		}
		break;
	case SERVER_LISTEN_FAILED:
		if (ringStranded()) {
			s->keys[0] = '\0'; // START waits for the stranded worker too
		}
		break;
	default:
		break; // the row is the whole screen
	}
	s->dots = !s->keys[0] && screen != SERVER_INSTALLING; // after the switch, which can clear keys
	s->phase = s->dots ? phase : 0;
}

// A prompt can wait for minutes, so the LED keeps its cadence here. `tick`, if not NULL, runs once
// a frame to do what the caller owes the rest of the console meanwhile (main.c answers a client,
// and a key that the client's request saw still goes to the prompt, through pressedKeys).
static u32 waitKeys(u32 mask, int frames, void (*tick)(void)) {
	for (int i = 0; appRunning() && (frames < 0 || i < frames); i++) {
		gspWaitForVBlank();
		appStatusTick();
		if (tick) {
			tick();
		}
		Screen next = shown;
		next.dot = dotColor(uptimeMs());
		if (next.dot != shown.dot) {
			show(&next, C3D_FRAME_SYNCDRAW);
		}
		u32 down = pressedKeys() & mask;
		if (down) {
			return down;
		}
	}
	return 0;
}

// Shows a notice or one of this file's own screens.
static void showNotice(unsigned screen, const char *detail) {
	Screen s;
	phase++;
	build(&s, screen, detail);
	show(&s, C3D_FRAME_SYNCDRAW);
	if (screen == UI_FATAL) {
		waitKeys(KEY_START, -1, NULL);
	} else if (screen == UI_ORIGINAL_2DS) {
		waitKeys(0xFFFFFFFFu, O2DS_NOTICE_FRAMES, NULL);
	} else {
		gspWaitForVBlank(); // visible before the caller blocks (PBKDF2) or the process jumps
	}
	if (screen == UI_CLEANUP_DONE || screen == UI_CLEANUP_FAILED) {
		holdUntilMs = uptimeMs() + COMPLETE_HOLD_MS; // the idle tick waits it out, as for a card
	}
}

void uiNotice(UiNotice notice, const char *detail) {
	showNotice(notice, detail);
}

bool uiInit(const char *appVersion) {
	// This comes before any target exists, because citro2d sizes the top one at creation. libctru's
	// gfxSetWide does not check the model, and an Original 2DS cannot scan out 800 px.
	u8 model = sysinfoModelId();
	version = appVersion;
	gfxSetWide(model <= CFG_MODEL_N2DSXL && model != CFG_MODEL_2DS);
	if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) || !C2D_Init(MAX_OBJECTS)) {
		uiExit(); // each Fini is a no-op for what did not start
		return false;
	}
	C2D_Prepare();
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	// C2D_TextBufNew svcBreak()s when the shared font cannot be mapped.
	Result font = fontEnsureMapped();
	if (R_SUCCEEDED(font)) {
		textBuf = C2D_TextBufNew(TEXT_GLYPHS);
		measureBuf = C2D_TextBufNew(ROW_MAX);
	}
	char line[40];
	snprintf(line, sizeof(line), "ui: %s model=%u%s", gfxIsWide() ? "wide" : "400", (unsigned)model,
			 !textBuf	   ? " no-text"
			 : !measureBuf ? " citro2d-wrap"
						   : "");
	appendLog(line, font, 0);
	appendLog("ui: linear free", 0, linearSpaceFree());
	ready = true;
	nextIdleMs = uptimeMs() + IDLE_TICK_MS;
	if (model == CFG_MODEL_2DS) {
		showNotice(UI_ORIGINAL_2DS, NULL);
	}
	return true;
}

void uiExit(void) {
	C2D_TextBufDelete(textBuf); // uses free(), which is NULL-safe
	textBuf = NULL;
	C2D_TextBufDelete(measureBuf);
	measureBuf = NULL;
	C2D_Fini();
	C3D_Fini(); // also frees the render targets
	top = bottom = NULL;
	ready = false;
}

void drawScreen(const char *ip) {
	Screen s;
	ipText = ip ? ip : ipText;
	build(&s, appStatus(), NULL);
	show(&s, C3D_FRAME_SYNCDRAW);
}

// A request that finds the status idle keeps the INSTALLED card and its hold. Otherwise no
// screenshot could ever show the card.
void uiCompletionTransition(void) {
	Screen next;
	holdUntilMs = 0;
	if (statusIsIdle(gApp.status) && shown.screen == SERVER_INSTALLED) {
		holdUntilMs = cardUntilMs;
		return;
	}
	build(&next, appStatus(), NULL);
	if (differs(&next)) {
		show(&next, C3D_FRAME_SYNCDRAW);
	}
	if (appInstalledShown()) {
		holdUntilMs = cardUntilMs = uptimeMs() + COMPLETE_HOLD_MS;
	}
}

void uiIdleAnimationTick(void) {
	if (uptimeMs() < holdUntilMs) {
		return;
	}
	ServerStatus status = appStatus();
	u64 now = uptimeMs();
	if (statusIsTransfer(status) || now < nextIdleMs) {
		return;
	}
	nextIdleMs = now + IDLE_TICK_MS;
	phase++;
	Screen next;
	build(&next, status, NULL);
	// Use memcmp, not differs(), because idle is where the dot breathes.
	if (memcmp(&next, &shown, sizeof(next)) != 0) {
		show(&next, C3D_FRAME_SYNCDRAW);
	}
}

// Nonblocking, because a dropped frame is free while a blocked receive loop costs throughput.
void uiProgressTick(void) {
	const App *app = &gApp;
	u64 now = uptimeMs();
	rateTick(app, now);
	Screen next;
	build(&next, appStatus(), NULL);
	bool due = next.screen != shown.screen || next.finalizing != shown.finalizing ||
			   next.committing != shown.committing || now - shownMs >= UI_REDRAW_MIN_MS;
	if (due && differs(&next)) {
		// While finalizing, there is no receive loop left to protect and no later tick
		// to retry a refused frame.
		show(&next, next.finalizing ? C3D_FRAME_SYNCDRAW : C3D_FRAME_NONBLOCK);
	}
}

// ---- The QR scan's screens (qr.c) ----
// Holds the screen until a key in `mask` goes down. There is no time-out, because the person is
// reading a link or the cause of a failed certificate check.
u32 uiAsk(UiNotice screen, const char *detail, u32 mask, void (*tick)(void)) {
	showNotice(screen, detail);
	return waitKeys(mask, -1, tick);
}

bool uiScanOpen(void) {
	if (!ready || previewReady) {
		return previewReady;
	}
	previewReady = C3D_TexInit(&previewTex, PREVIEW_TEX_W, PREVIEW_TEX_H, GPU_RGB565);
	if (previewReady) {
		C3D_TexSetFilter(&previewTex, GPU_LINEAR, GPU_NEAREST);
		memset(previewTex.data, 0, PREVIEW_TEX_BYTES); // black until the first frame
		GSPGPU_FlushDataCache(previewTex.data, PREVIEW_TEX_BYTES);
	}
	return previewReady;
}

void uiScanClose(void) {
	previewFrame = NULL;
	if (!previewReady) {
		return;
	}
	// The GPU may still be reading the texture for the last frame, and
	// beginning a frame waits for it.
	if (C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
		C3D_FrameEnd(0);
	}
	C3D_TexDelete(&previewTex);
	previewReady = false;
}

// Draws the camera's picture on the top screen and the scan's words below,
// using the frame passed in.
void uiScanFrame(const u16 *frame) {
	if (!previewReady) {
		return;
	}
	Screen s;
	build(&s, UI_QR_SCAN, NULL);
	s.preview = true;
	previewFrame = frame;
	show(&s, C3D_FRAME_SYNCDRAW);
	previewFrame = NULL; // a frame the GPU refused is not retried with a buffer the camera reuses
}

// Nothing is served meanwhile, so an unanswered screen gives way; PBKDF2 freezes the busy frame.
// This is the one prompt where waitKeys gets no tick, because it replaces the key that requests are
// signed with, and a request answered halfway would be signed with the old one or the new.
void confirmAndRegenKey(void) {
	showNotice(UI_KEY_CONFIRM, NULL);
	if (!(waitKeys(KEY_A | KEY_B | KEY_START, UI_KEY_WAIT_FRAMES, NULL) & KEY_A)) {
		return;
	}
	showNotice(UI_KEY_BUSY, NULL);
	bool ok = authRegenerateKey(); // the running listener uses it at once
	if (ok) {
		appSetIdle();
	}
	showNotice(ok ? UI_KEY_OK : UI_KEY_FAIL, NULL);
	waitKeys(0xFFFFFFFFu, UI_KEY_WAIT_FRAMES, NULL);
}

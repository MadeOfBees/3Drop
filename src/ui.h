// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UI_H
#define UI_H

// The two screens. This covers what each status and notice shows, with all its copy in one table; a
// transfer's rate and time left; the pairing key; and the QR scan's pictures and questions.

#include "app.h"

// These are numbered after the statuses, so one table holds all the copy.
typedef enum {
	UI_STARTUP = SERVER_STATUS_COUNT,
	UI_SELF_UPDATED, // AFTER_SELF_UPDATED: relaunching into the new 3Drop
	UI_RUN_SELF,	 // AFTER_RELAUNCH: `run self`
	UI_LAUNCH,
	// Notices for X's cleanup (main.c). Each is held for a second before the status screen returns.
	UI_CLEANUP_DONE,
	UI_CLEANUP_FAILED, // the detail is its code
	// The QR scan (qr.c): the camera's picture on top and what it is for below; the links a code
	// holds; the certificate list being fetched; a check that failed, with the ways to continue; a
	// code or a camera that could not be used.
	UI_QR_SCAN,
	UI_QR_CONFIRM,
	UI_QR_BUNDLE,
	UI_QR_CHECK,
	UI_QR_REFUSED,
	UI_NOTICE_END,
	UI_FATAL = SERVER_FATAL, // the status's own screen, which blocks until START
} UiNotice;

// Returns false only when citro3d or citro2d cannot start. A missing render target or font degrades
// to what did start, so the server keeps running.
bool uiInit(const char *appVersion);
void uiExit(void);
void uiNotice(UiNotice notice, const char *detail);
void drawScreen(const char *ip); // NULL keeps the last address
// Called after a request or a scan. It draws the status's screen if it changed. An INSTALLED card
// then gives way to the idle status (appInstalledShown) and is held for a second, unless a request
// finds it held.
void uiCompletionTransition(void);
void uiProgressTick(void);
// uiProgressTick draws a changed value at most this often, because progress redraws compete with
// the receive loop. A caller that throttles its own ticks (qr.c) uses the same bound.
#define UI_REDRAW_MIN_MS 200ULL
void uiIdleAnimationTick(void);
#define UI_KEY_WAIT_FRAMES (30 * 60) // 30 s of VBlanks
void confirmAndRegenKey(void);
// Shows a screen of the QR scan with `detail` after its own words, and holds it until one of the
// keys in `mask` is pressed. Meanwhile the LED is kept alive, and `tick` runs once a frame if it is
// not NULL. Returns that key, or 0 when the app is asked to close. A '\f' in `detail` ends what is
// added to the top screen; what follows it is added to the bottom screen's words.
u32 uiAsk(UiNotice screen, const char *detail, u32 mask, void (*tick)(void));
// The scanner's picture is opened before the first frame and closed after the last. It can fail to
// open (the linear heap), and the scan then goes on with its words alone. A frame is what scanFrame
// returns.
bool uiScanOpen(void);
void uiScanClose(void);
void uiScanFrame(const u16 *frame);
// The pairing key stays inside these top-screen rows, which a screenshot blacks out.
#define UI_KEY_TOP 60
#define UI_KEY_END 114
bool uiShowsKey(void);

#endif // UI_H

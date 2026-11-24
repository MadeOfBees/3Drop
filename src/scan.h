// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SCAN_H
#define SCAN_H

#include "result.h"

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// The outer camera as a QR scanner, polled from the main thread. FBI and Universal Updater need a
// thread and a lock because they render on another thread; this needs neither. A frame is the top
// screen's size, 400x240 in RGB565, the same size both of them use.
#define SCAN_WIDTH 400
#define SCAN_HEIGHT 240
#define SCAN_FRAME_BYTES (SCAN_WIDTH * SCAN_HEIGHT * 2)

typedef enum {
	SCAN_IDLE,	 // no frame yet
	SCAN_FRAME,	 // a frame with no code in it
	SCAN_FOUND,	 // a frame with a code that decoded; scanPayload has it
	SCAN_BROKEN, // the camera failed; scanResult says why
} ScanStep;

// Calls camInit, sets the sensor up for codes the way Universal Updater does, and arms the first
// receive. Returns 0 or the error; a failed open cleans up after itself.
Result scanOpen(void);
// Safe to call after a failed open, and safe to call twice.
void scanClose(void);
// Waits up to `waitNs` for the next frame and, if one came, reads it: its grey copy goes to quirc,
// and the next receive is armed first, so the camera is never left without one while the frame is
// read.
ScanStep scanPoll(u64 waitNs);
// The last frame; valid until the next scanPoll.
const u16 *scanFrame(void);
// After SCAN_FOUND: the code's contents, `*length` bytes, valid until the next scanPoll.
const u8 *scanPayload(size_t *length);
// Why a poll returned SCAN_BROKEN. scanClose resets it to zero, so read it before closing.
Result scanResult(void);

// Converts RGB565 pixels to the 8-bit grey quirc reads: the mean of the three channels, computed
// the way FBI does it.
void scanGrey(u8 *dst, const u16 *src, size_t pixels);

#endif // SCAN_H

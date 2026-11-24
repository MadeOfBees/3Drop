// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SCREENSHOT_H
#define SCREENSHOT_H

// What the `screenshot` verb sends: both screens' pixels as shown now, and a BMP of either one
// with the pairing key's rows blacked out.

#include <3ds.h>
#include <stdbool.h>

typedef struct {
	u8 *fb[2]; // top, bottom
	u16 w[2], h[2];
	bool redactKey;
} Frame;
// Valid until the next frame is drawn.
bool frameGrab(Frame *frame);

// The key's rows go out black, so the key never leaves the console in an image.
u32 bmpSize(const Frame *frame, int screen);
u32 bmpEncode(u8 *out, const Frame *frame, int screen);

#endif // SCREENSHOT_H

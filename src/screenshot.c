// SPDX-License-Identifier: GPL-3.0-or-later
#include "screenshot.h"

#include "ui.h" // uiShowsKey

#include <3ds.h>
#include <citro3d.h>
#include <string.h>

// Under double buffering gfxGetFramebuffer returns the hidden back buffer, so the presented one is
// looked up with it off, then it goes back on.
bool frameGrab(Frame *frame) {
	// An empty frame draws and swaps nothing, but its Begin waits until the last real frame has
	// been transferred and swapped in, so what is grabbed is what the screen shows.
	if (!C3D_FrameBegin(0)) {
		return false;
	}
	C3D_FrameEnd(0);
	gspWaitForVBlank();
	for (int i = 0; i < 2; i++) {
		gfxScreen_t screen = i ? GFX_BOTTOM : GFX_TOP;
		gfxSetDoubleBuffering(screen, false);
		frame->fb[i] = gfxGetFramebuffer(screen, GFX_LEFT, &frame->w[i], &frame->h[i]);
		gfxSetDoubleBuffering(screen, true);
	}
	frame->redactKey = uiShowsKey();
	return true;
}

// Screen columns are stored bottom-up like BMP rows, in BGR8 like BMP bytes, so no flip or swizzle
// is needed. A wide-mode capture keeps its non-square pixels.
#define BMP_HEADER 54

static u32 rowBytes(u32 width) {
	return (width * 3 + 3) & ~3u; // rows pad to 4 bytes
}

u32 bmpSize(const Frame *frame, int screen) {
	return frame->fb[screen] ? BMP_HEADER + rowBytes(frame->h[screen]) * frame->w[screen] : 0;
}

u32 bmpEncode(u8 *out, const Frame *frame, int screen) {
	const u8 *fb = frame->fb[screen];
	u32 rows = frame->w[screen], width = frame->h[screen], stride = rowBytes(width);
	u32 size = bmpSize(frame, screen);
	if (size == 0) {
		return 0;
	}
	const u32 fields[][2] = {
		{2, size},			{10, BMP_HEADER},		{14, 40}, {18, width}, {22, rows},
		{26, 1 | 24 << 16}, {34, size - BMP_HEADER}}; // planes=1 and bpp=24 share one u32
	memset(out, 0, BMP_HEADER);
	out[0] = 'B';
	out[1] = 'M';
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		for (int b = 0; b < 4; b++) {
			out[fields[i][0] + b] = (u8)(fields[i][1] >> (8 * b));
		}
	}
	for (u32 r = 0; r < rows; r++) {
		u8 *row = out + BMP_HEADER + r * stride;
		u32 y = rows - 1 - r; // the screen row, top-down
		memset(row, 0, stride);
		if (screen == 0 && frame->redactKey && y >= UI_KEY_TOP && y < UI_KEY_END) {
			continue; // the key's rows stay black
		}
		for (u32 x = 0; x < width; x++) {
			memcpy(&row[x * 3], &fb[(x * rows + r) * 3], 3);
		}
	}
	return size;
}

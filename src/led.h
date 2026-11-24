// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LED_H
#define LED_H

// Main thread only, because nothing here is locked. The Testing Tool links this file alone.

#include <3ds.h>
#include <stdbool.h>

typedef struct {
	u32 rgb; // 0xRRGGBB, or 0 for off
	bool breathes;
} Led;
#define LED_OFF ((Led){0, false})
// The MCU and the screen's dot each take red, green and blue separately.
static inline void ledChannels(u32 rgb, u32 out[3]) {
	out[0] = rgb >> 16 & 0xFF;
	out[1] = rgb >> 8 & 0xFF;
	out[2] = rgb & 0xFF;
}
// A lit pattern plays once and ends dark, so a crash or hang shows as a dark LED. `delay` 0xFF is
// 15.9375 s for the whole pattern, not the 1/16 s per value that libctru's mcuhwc.h documents.
#define LED_SAMPLE_MS 498 // 15937.5 ms / 32
// Shared with the screen's dot, so the two breathe alike.
static inline u8 ledBreath(u64 sample) {
	static const u8 LEVELS[] = {0xFF, 0xAE, 0x60, 0xAE};
	return LEVELS[sample % 4];
}
// Two whole breaths, so a rewrite restarts the pattern in step and most of the lit life is left for
// a poll to come round.
#define LED_REFRESH_MS (8 * LED_SAMPLE_MS)

Result ledWrite(Led led);
Result ledOpen(void); // call mcuHwcExit only after a success, or libctru's count goes below zero
// Bench and Testing Tool only, because lint.sh bars raw MCU register access anywhere else.
Result ledRead(u8 rgb[3]);

// Opens ptm:u, for lidState alone. The LED needs no setup, since every ledWrite opens and closes
// its own mcu::HWC session. A failed open leaves the lid unknown, so the LED is never lit.
void ledInit(void);
void ledExit(void);
// The lid state is never a guess. An unknown lid authorizes no LED write.
typedef enum { LID_UNKNOWN, LID_OPEN, LID_CLOSED } Lid;
Lid lidState(void);

#endif // LED_H

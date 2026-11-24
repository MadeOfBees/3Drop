// SPDX-License-Identifier: GPL-3.0-or-later
#include "led.h"

#include <string.h>

// Opens non-blocking, because mcu::HWC serves one session, and under srv's default policy an open
// while another process holds it parks this thread with no timeout. Nothing else sets the policy,
// so the default is put back.
Result ledOpen(void) {
	srvSetBlockingPolicy(true);
	Result result = mcuHwcInit();
	srvSetBlockingPolicy(false);
	return result;
}

Result ledWrite(Led led) {
	InfoLedPattern pattern;
	// Every sample starts dark, which is the off pattern and a lit pattern's last sample.
	memset(&pattern, 0, sizeof(pattern));
	// A smoothing of 0 would switch to blinkSpeed behavior.
	pattern.smoothing = 1;
	pattern.delay = led.rgb ? 0xFF : 0x80;
	pattern.loopDelay = led.rgb ? 0xFF : 0; // 0xFF plays the pattern once
	u32 c[3];
	ledChannels(led.rgb, c);
	for (int i = 0; i < 31; i++) {
		u32 level = led.breathes ? ledBreath(i) : 0xFF;
		pattern.redPattern[i] = (u8)(c[0] * level / 0xFF);
		pattern.greenPattern[i] = (u8)(c[1] * level / 0xFF);
		pattern.bluePattern[i] = (u8)(c[2] * level / 0xFF);
	}
	Result result = ledOpen();
	if (R_FAILED(result)) {
		return result; // no session, so an exit now would take libctru's count below zero
	}
	result = MCUHWC_SetInfoLedPattern(&pattern);
	mcuHwcExit();
	return result;
}

static bool ptmuReady;

void ledInit(void) {
	ptmuReady = R_SUCCEEDED(ptmuInit());
}

void ledExit(void) {
	if (ptmuReady) {
		ptmuExit();
		ptmuReady = false;
	}
}

Lid lidState(void) {
	u8 shell; // 0 closed, 1 open
	if (!ptmuReady || R_FAILED(PTMU_GetShellState(&shell))) {
		return LID_UNKNOWN;
	}
	return shell != 0 ? LID_OPEN : LID_CLOSED;
}

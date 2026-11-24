// SPDX-License-Identifier: GPL-3.0-or-later
// Reads the LED back, for the bench build's `bench led` and the Testing Tool. It is never in a
// release.
#include "led.h"

#include <string.h>

Result ledRead(u8 rgb[3]) {
	// 3dbrew I2C_Registers: register 0x7F is read-only and 9 to 0x13 bytes long, depending on the
	// MCU firmware. It holds the LED's RGB at bytes 0x0D-0x0F.
	u8 state[0x10] = {0};
	Result result = ledOpen();
	if (R_SUCCEEDED(result)) {
		result = MCUHWC_ReadRegister(0x7F, state, sizeof(state));
		mcuHwcExit();
	}
	if (R_SUCCEEDED(result)) {
		memcpy(rgb, state + 0x0D, 3);
	}
	return result;
}

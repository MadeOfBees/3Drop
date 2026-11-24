// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SYSINFO_H
#define SYSINFO_H

// Never report an identity (device id, CID, MAC, serial, friend code, NNID), because `stats` goes
// to anyone holding the key, and the probe's answer (the model) goes to anyone on the subnet.

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Call before uiInit, which takes the model.
void sysinfoInit(void);
u8 sysinfoModelId(void); // 0xFF when CFG did not say
const char *sysinfoModel(void);
// Does no IPC per call, so it reports no battery percentage. mcu::HWC serves one session, and a
// second opener waits.
void sysinfoFormat(char *out, size_t cap);
// From 3dbrew BATTERY_LEDSTATE: bit 0 is the adapter, bit 1 is charging, and bits 2-4 are the level
// (0-5).
static inline bool sysinfoOnAc(void) {
	return (OS_SharedConfig->led_battery & 1u) != 0;
}

#endif // SYSINFO_H

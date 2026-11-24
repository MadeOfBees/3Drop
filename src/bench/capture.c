// SPDX-License-Identifier: GPL-3.0-or-later
// Captures the screens that the `screenshot` verb can't reach, because a transfer holds the one
// connection and the boot, fatal and key screens block. A transfer's frames go only to RAM slots,
// because a BMP write mid-transfer stalled the receive loop and set off the sender's ghost
// detector. Captures still perturb throughput.
#include "bench.h"

#include "app.h" // statusIsTransfer
#include "fs.h"	 // appendLog
#include "path.h"
#include "ring.h" // ringStranded, ringTier1
#include "screenshot.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CAPTURE_DIR APP_DIR "/capture"
#define CAPTURE_ARM CAPTURE_DIR "/arm"
#define CAPTURE_MAX 48 // per boot
// Late enough that live values have moved. Sooner than this, an install is still in the first-write
// hold.
#define CAPTURE_LATE_MS 30000ULL
#define CAPTURE_SLOTS 4
#define SLOT_BYTES (240 * (800 + 320) * 3) // a wide top and a bottom framebuffer

typedef struct {
	Frame frame;
	char top[96], bottom[96];
} Capture;

static Capture slots[CAPTURE_SLOTS]; // their frames point into slotMemory
static u8 *slotMemory;
static bool armed;
static unsigned count, held, dropped, lost; // lost: frames counted in count but never written

// Encodes in tier 1. Outside a transfer nothing holds it, unless a stranded worker may still hold
// it. If either of a frame's files fails to write, the frame is logged and counted as lost, and
// `frames` leaves it out.
static void captureWrite(const Capture *capture) {
	u8 *bmp = ringTier1();
	bool wrote = bmp != NULL;
	for (int i = 0; wrote && !ringStranded() && i < 2; i++) {
		u32 size = bmpEncode(bmp, &capture->frame, i);
		FILE *file = fopen(i ? capture->bottom : capture->top, "wb");
		wrote = file && fwrite(bmp, 1, size, file) == size;
		if (file) {
			wrote = fclose(file) == 0 && wrote;
		}
	}
	if (!wrote) {
		lost++;
		appendLog("capture: write", bmp ? errno : ENOMEM, lost);
	}
}

static void slotsWrite(void) {
	// When capture goes `off` while stranded, tier 1 may belong to the worker, so the held frames
	// are lost and counted.
	if (ringStranded()) {
		dropped += held;
		held = 0;
	}
	for (unsigned i = 0; i < held; i++) {
		captureWrite(&slots[i]);
	}
	if (dropped) {
		appendLog("capture: dropped", 0, dropped);
	}
	held = dropped = 0;
}

// The slots come after tier 1, so they never displace it.
static void arm(bool on) {
	if (on && !armed) {
		(void)ringTier1();
		slotMemory = (u8 *)linearAlloc(CAPTURE_SLOTS * SLOT_BYTES);
		appendLog(slotMemory ? "capture: armed" : "capture: no slot memory", 0,
				  CAPTURE_SLOTS * SLOT_BYTES);
	} else if (!on && armed) {
		slotsWrite();
		linearFree(slotMemory);
		slotMemory = NULL;
	}
	armed = on;
}

void benchCaptureSet(bool on) {
	bool saved;
	if (on) {
		mkdir(CAPTURE_DIR, 0777);
		FILE *file = fopen(CAPTURE_ARM, "w");
		saved = file && fclose(file) == 0;
	} else {
		saved = remove(CAPTURE_ARM) == 0 || errno == ENOENT;
	}
	if (!saved) { // it holds for this boot only, and a series' state read after `run self` shows it
		appendLog("capture: arm file", errno, on);
	}
	arm(on);
}

CaptureState benchCaptureState(void) {
	return (CaptureState){armed, count - lost, held, dropped, slotMemory ? CAPTURE_SLOTS : 0};
}

void benchFramePresented(u32 key, const char *title, u64 nowMs) {
	static bool booted;
	static u32 lastKey = UINT32_MAX;
	static u64 sinceMs;
	static bool lateTaken;
	struct stat st;
	if (!booted) {
		booted = true;
		arm(stat(CAPTURE_ARM, &st) == 0);
	}
	// A stranded worker may still hold tier 1, so its screen waits in the slots, as in a transfer.
	bool transfer = statusIsTransfer(key >> CAPTURE_SCREEN_SHIFT) || ringStranded();
	if (armed && !transfer && (held || dropped)) {
		slotsWrite();
	}
	if (!armed || count >= CAPTURE_MAX) {
		return;
	}
	if (key != lastKey) {
		lastKey = key;
		sinceMs = nowMs;
		lateTaken = false;
	} else if (lateTaken || nowMs - sinceMs < CAPTURE_LATE_MS) {
		return;
	} else {
		lateTaken = true;
	}
	Capture now;
	if ((transfer && (!slotMemory || held == CAPTURE_SLOTS)) || !frameGrab(&now.frame)) {
		dropped += transfer;
		return;
	}
	size_t topBytes = (size_t)now.frame.w[0] * now.frame.h[0] * 3;
	size_t bottomBytes = (size_t)now.frame.w[1] * now.frame.h[1] * 3;
	if (transfer && topBytes + bottomBytes > SLOT_BYTES) {
		dropped++;
		return;
	}
	char name[32];
	size_t n = 0;
	for (; *title && n < sizeof(name) - 1; title++) {
		if (isalnum((unsigned char)*title) || *title == '-' || *title == ' ') {
			name[n++] = *title == ' ' ? '-' : *title;
		}
	}
	name[n] = '\0';
	snprintf(now.top, sizeof(now.top), CAPTURE_DIR "/%03u-%s-top.bmp", count, name);
	snprintf(now.bottom, sizeof(now.bottom), CAPTURE_DIR "/%03u-%s-bottom.bmp", count, name);
	count++;
	if (transfer) {
		u8 *to = slotMemory + held * SLOT_BYTES;
		memcpy(to, now.frame.fb[0], topBytes);
		memcpy(to + topBytes, now.frame.fb[1], bottomBytes);
		now.frame.fb[0] = to;
		now.frame.fb[1] = to + topBytes;
		slots[held++] = now;
	} else {
		captureWrite(&now);
	}
}

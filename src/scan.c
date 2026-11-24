// SPDX-License-Identifier: GPL-3.0-or-later
#include "scan.h"

#include "quirc/quirc.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

// libctru's svcWaitSynchronization returns 0 when the handle signaled. Its timeout code
// (0x09401BFE) also passes R_SUCCEEDED, so only an exact 0 means the handle signaled.
#define SIGNALED 0

static struct {
	bool camera;
	bool capturing; // frames are still coming; false once a receive or a recovery fails
	bool started;	// a capture was started, so scanClose must stop it, even after the scan broke
	struct quirc *quirc;
	// code (4 KB) and data (9 KB) are on the heap because quirc_decode needs 9 KB of stack.
	struct quirc_code *code;
	struct quirc_data *data;
	u16 *frames[2]; // one receives a frame while the other holds the last; they swap each frame
	unsigned armed; // which of the two is receiving
	Handle received, failed;
	u32 unit;
	size_t payloadLength;
	Result result;
} scan;

void scanGrey(u8 *dst, const u16 *src, size_t pixels) {
	for (size_t i = 0; i < pixels; i++) {
		u32 px = src[i];
		dst[i] = (u8)((((px >> 11) & 0x1F) * 8 + ((px >> 5) & 0x3F) * 4 + (px & 0x1F) * 8) / 3);
	}
}

// Camera settings for reading codes: Universal Updater's photo mode (PHOTO_MODE_LETTER), plus
// FBI's noise filter, auto exposure and auto white balance. Sharpness is left alone: Universal
// Updater sets 127, which a console refuses as out of range (0xE0E053FD). Universal Updater
// never notices, but here any refused setting ends the scan.
static Result sensor(void) {
	Result r = 0;
	bool ok = R_SUCCEEDED(r = CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A)) &&
			  R_SUCCEEDED(r = CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A)) &&
			  R_SUCCEEDED(r = CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30)) &&
			  R_SUCCEEDED(r = CAMU_SetNoiseFilter(SELECT_OUT1, true)) &&
			  R_SUCCEEDED(r = CAMU_SetAutoExposure(SELECT_OUT1, true)) &&
			  R_SUCCEEDED(r = CAMU_SetAutoWhiteBalance(SELECT_OUT1, true)) &&
			  R_SUCCEEDED(r = CAMU_SetPhotoMode(SELECT_OUT1, PHOTO_MODE_LETTER)) &&
			  R_SUCCEEDED(r = CAMU_Activate(SELECT_OUT1));
	return ok ? 0 : r;
}

// libctru stores the event handle before it reads CAM's answer (its cam.c:88-112), so after a
// refusal the stored value is not a handle. The event is kept only once CAM has said yes;
// otherwise scanClose would close whatever number was left there.
static Result receive(void) {
	Handle event = 0;
	Result r = CAMU_SetReceiving(&event, scan.frames[scan.armed], PORT_CAM1, SCAN_FRAME_BYTES,
								 (s16)scan.unit);
	if (R_SUCCEEDED(r)) {
		scan.received = event;
	}
	return r;
}

// Sets up the camera port and the transfer size, arms the first receive, then starts the capture.
static Result port(void) {
	Handle failed = 0;
	Result r = CAMU_GetBufferErrorInterruptEvent(&failed, PORT_CAM1);
	if (R_FAILED(r)) {
		return r;
	}
	scan.failed = failed;
	bool ok =
		R_SUCCEEDED(r = CAMU_SetTrimming(PORT_CAM1, false)) &&
		R_SUCCEEDED(r = CAMU_GetMaxBytes(&scan.unit, SCAN_WIDTH, SCAN_HEIGHT)) &&
		R_SUCCEEDED(r = CAMU_SetTransferBytes(PORT_CAM1, scan.unit, SCAN_WIDTH, SCAN_HEIGHT)) &&
		R_SUCCEEDED(r = CAMU_ClearBuffer(PORT_CAM1)) && R_SUCCEEDED(r = receive()) &&
		R_SUCCEEDED(r = CAMU_StartCapture(PORT_CAM1));
	scan.started = scan.capturing = ok;
	return ok ? 0 : r;
}

void scanClose(void) {
	if (scan.camera) {
		// Stop the capture even when the scan broke: a failed receive or recovery doesn't stop it.
		// FBI also stops its capture when its loop ends, error or not.
		if (scan.started) {
			CAMU_StopCapture(PORT_CAM1);
			bool busy = false;
			// The capture stops when the current frame ends. Wait up to a second for that: nothing
			// here may wait forever.
			for (int i = 0; i < 1000 && R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy; i++) {
				svcSleepThread(1000000);
			}
			CAMU_ClearBuffer(PORT_CAM1);
		}
		CAMU_Activate(SELECT_NONE);
		camExit();
	}
	if (scan.received) {
		svcCloseHandle(scan.received);
	}
	if (scan.failed) {
		svcCloseHandle(scan.failed);
	}
	if (scan.quirc) { // quirc_destroy can't take NULL
		quirc_destroy(scan.quirc);
	}
	free(scan.code);
	free(scan.data);
	free(scan.frames[0]);
	free(scan.frames[1]);
	memset(&scan, 0, sizeof(scan));
}

Result scanOpen(void) {
	memset(&scan, 0, sizeof(scan));
	scan.quirc = quirc_new();
	scan.code = malloc(sizeof(*scan.code));
	scan.data = malloc(sizeof(*scan.data));
	scan.frames[0] = malloc(SCAN_FRAME_BYTES);
	scan.frames[1] = malloc(SCAN_FRAME_BYTES);
	if (!scan.quirc || !scan.code || !scan.data || !scan.frames[0] || !scan.frames[1] ||
		quirc_resize(scan.quirc, SCAN_WIDTH, SCAN_HEIGHT) < 0) {
		scanClose();
		return OUT_OF_MEMORY;
	}
	Result r = camInit();
	if (R_FAILED(r)) {
		scanClose();
		return r;
	}
	scan.camera = true;
	r = sensor();
	if (R_SUCCEEDED(r)) {
		r = port();
	}
	if (R_FAILED(r)) {
		scanClose();
	}
	return r;
}

// Looks for a QR code in the frame: a grey copy goes to quirc, and the first code that decodes
// wins. Mirrored codes don't count: quirc reads them only after quirc_flip, which isn't called.
static ScanStep decodeFrame(const u16 *frame) {
	int w, h;
	u8 *image = quirc_begin(scan.quirc, &w, &h);
	scanGrey(image, frame, (size_t)w * (size_t)h);
	quirc_end(scan.quirc);
	int count = quirc_count(scan.quirc);
	for (int i = 0; i < count; i++) {
		quirc_extract(scan.quirc, i, scan.code);
		if (quirc_decode(scan.code, scan.data) == QUIRC_SUCCESS) {
			scan.payloadLength = (size_t)scan.data->payload_len;
			return SCAN_FOUND;
		}
	}
	return SCAN_FRAME;
}

// The camera dropped a frame because no receive was waiting for it. Recover the way FBI does:
// clear the buffer, arm a new receive and start the capture again.
static Result recover(void) {
	svcCloseHandle(scan.received);
	scan.received = 0;
	Result r = 0;
	bool ok = R_SUCCEEDED(r = CAMU_ClearBuffer(PORT_CAM1)) && R_SUCCEEDED(r = receive()) &&
			  R_SUCCEEDED(r = CAMU_StartCapture(PORT_CAM1));
	return ok ? 0 : r;
}

ScanStep scanPoll(u64 waitNs) {
	if (!scan.capturing) {
		return SCAN_BROKEN;
	}
	scan.payloadLength = 0;
	if (svcWaitSynchronization(scan.received, (s64)waitNs) == SIGNALED) {
		svcCloseHandle(scan.received);
		scan.received = 0;
		unsigned done = scan.armed;
		scan.armed ^= 1;
		scan.result = receive();
		if (R_FAILED(scan.result)) {
			scan.capturing = false;
			return SCAN_BROKEN;
		}
		return decodeFrame(scan.frames[done]);
	}
	if (svcWaitSynchronization(scan.failed, 0) == SIGNALED) {
		scan.result = recover();
		if (R_FAILED(scan.result)) {
			scan.capturing = false;
			return SCAN_BROKEN;
		}
	}
	return SCAN_IDLE;
}

const u16 *scanFrame(void) {
	return scan.frames[scan.armed ^ 1]; // the other buffer is still receiving
}

const u8 *scanPayload(size_t *length) {
	*length = scan.payloadLength;
	return scan.data ? scan.data->payload : NULL;
}

Result scanResult(void) {
	return scan.result;
}

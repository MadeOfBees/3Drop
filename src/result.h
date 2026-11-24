// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RESULT_H
#define RESULT_H

// These are real Results in module RM_APPLICATION (254), so R_FAILED is true for each and none
// equals an OS code. A failure reports the platform's own Result wherever there is one.

#include <3ds.h>

#define APP_RESULT(description)                                                                    \
	((Result)MAKERESULT(RL_PERMANENT, RS_INTERNAL, RM_APPLICATION, (description)))

// Results from the pumps (transfer.c). download.c also uses RECV_FAILED for a body longer than its
// Content-Length.
#define TRANSFER_RECV_FAILED APP_RESULT(1)		// recv() failed
#define TRANSFER_SHORT_READ APP_RESULT(2)		// the sender closed before the declared size
#define TRANSFER_CANCELED APP_RESULT(3)			// START was pressed on the console
#define TRANSFER_EXITED APP_RESULT(4)			// APT asked the app to quit
#define TRANSFER_IDLE_TIMEOUT APP_RESULT(5)		// the peer went quiet
#define TRANSFER_ABSOLUTE_TIMEOUT APP_RESULT(6) // the whole body took too long
#define TRANSFER_WORKER_STALL APP_RESULT(7)		// the ring's worker stalled past WORKER_STALL_MS
#define TRANSFER_SEND_FAILED APP_RESULT(22)		// send() failed, or the peer closed
// Results for the checks on the received bytes. STAGE_METADATA is also used for a certificate page
// that no parser accepts (qr.c) and for a download that never sent a CIA's prefix (download.c).
#define STAGE_METADATA APP_RESULT(9)   // a CIA prefix failed ciaParse, or a /boot.firm check failed
#define STAGE_DIGEST APP_RESULT(10)	   // the bytes are not the ones the request's digest names
#define DIGEST_PAST_END APP_RESULT(23) // a digest unit came after the body's short last one
// Results for the SD card and memory.
#define SD_SHORT_IO APP_RESULT(11) // FSFILE_Read/Write (SD or AM) succeeded but moved fewer bytes
#define SD_NO_SPACE APP_RESULT(14) // the install needs more free clusters than the card has
#define SD_BAD_RESOURCE APP_RESULT(15) // the archive resource reported clusterSize 0
#define PATH_TOO_LONG APP_RESULT(16)
#define OUT_OF_MEMORY APP_RESULT(17)
#define CAPTURE_FAILED APP_RESULT(19) // a screenshot could not be grabbed
#define ON_BATTERY APP_RESULT(24)	  // a /boot.firm put while the console is off its charger
// The lock that keeps Wi-Fi up with the lid closed (net.c). This Result means Enter and Lock
// succeeded, but NDM's exclusive state is not the one the lock asks for, so something else
// answered Enter for NDM.
#define KEEPALIVE_FAKED APP_RESULT(31)
// Results for a download by URL (download.c). A libcurl failure keeps its own code and an HTTP
// error keeps its status, each in a range of its own, so the Result on the card and in the log
// names it.
#define DOWNLOAD_NO_LENGTH APP_RESULT(25) // the server gave no Content-Length
#define DOWNLOAD_TOO_BIG APP_RESULT(26)	  // more bytes than a bundle may be
#define DOWNLOAD_NO_THREAD APP_RESULT(27) // the download's thread could not be made
#define DOWNLOAD_NO_TLS APP_RESULT(28)	  // sslc would not start, so there is no https
#define DOWNLOAD_NO_FILE APP_RESULT(29)	  // the bundle's temp file would not open or write
#define DOWNLOAD_NO_CURL APP_RESULT(30)	  // libcurl would not start, so nothing can be downloaded
#define DOWNLOAD_CURL(code) APP_RESULT(0x100 + (code)) // libcurl's CURLcode, below 0x100
#define DOWNLOAD_HTTP_BASE 0x200
#define DOWNLOAD_HTTP(status) APP_RESULT(DOWNLOAD_HTTP_BASE + ((status)-100)) // 100..599
// Returns the status a Result carries, or 0 for any other Result.
static inline int downloadHttpStatus(Result r) {
	u32 desc = R_DESCRIPTION(r);
	return R_MODULE(r) == RM_APPLICATION && desc >= DOWNLOAD_HTTP_BASE &&
				   desc < DOWNLOAD_HTTP_BASE + 500
			   ? (int)(desc - DOWNLOAD_HTTP_BASE) + 100
			   : 0;
}

// Zero is REJECTED, so an Operation nobody filled in never reads as committed. COMMITTED means
// done and clean. Done with a caveat (a ticket AM could not prove gone, a commit that may have
// landed) is UNKNOWN. The host explains it and exits 1.
typedef enum { OUTCOME_REJECTED, OUTCOME_COMMITTED, OUTCOME_UNKNOWN } Outcome;

typedef struct {
	Outcome outcome;
	const char *detail; // a string literal, so it outlives the call, and it is a stable wire word
	Result result;
	u64 titleID; // 0 means none is named
	int version; // -1 means unknown
} Operation;

// Where every operation starts: rejected until it commits, with its version unknown.
#define OP_REJECTED ((Operation){.version = -1})

// A read or write that succeeded but moved a different number of bytes than it was asked to move
// is a failure.
static inline Result shortIo(Result r, u32 moved, u32 asked) {
	return R_SUCCEEDED(r) && moved != asked ? SD_SHORT_IO : r;
}

static inline void opSet(Operation *op, Outcome outcome, const char *detail, Result result) {
	op->outcome = outcome;
	op->detail = detail;
	op->result = result;
}

#endif // RESULT_H

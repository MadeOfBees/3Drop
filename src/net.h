// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NET_H
#define NET_H

// The socket layer: starting the network, the listener and the buffer sizes tuned for it, reading a
// request's lines, sending bytes, and closing a client without cutting its last reply off.

#include "app.h" // gApp, for sendFailEvent
#include "knobs.h"

#include <errno.h> // listenerAcceptFatal() classifies accept() errno
#include <stdbool.h>
#include <stddef.h>

// Static storage only, because the main thread's stack is 32768 bytes.
#define RECV_BUFFER_SIZE (128 * 1024)

// Never below the window, because a smaller read re-opens it late and stalls the sender. This takes
// a whole window in one soc:U IPC.
#define SOC_FAST_RECV_SIZE 65536
_Static_assert(SOC_FAST_RECV_SIZE <= RECV_BUFFER_SIZE,
			   "SOC_FAST_RECV_SIZE must fit the RECV_BUFFER_SIZE receive buffer");

// soc:U offers no window scaling, so the 16-bit window field is the ceiling. A listener set to
// 262144 advertised a window of 0 in its SYN-ACKs.
#define LISTENER_WINDOW_MAX 65535
// Above this a burst loses frames and the sender stalls on RTOs; below it, the cost is wall time.
// The value was swept on stall time, not rate, and is not derived from SOC_FAST_RECV_SIZE.
#define LISTENER_WINDOW_BYTES 49152
// This is the band the sweep found safe. One step past it, 53248 stalled ten times as often.
_Static_assert(LISTENER_WINDOW_BYTES >= 45056 && LISTENER_WINDOW_BYTES <= 51200,
			   "the listener window leaves its swept band");
_Static_assert(SOC_FAST_RECV_SIZE >= LISTENER_WINDOW_BYTES,
			   "a receive smaller than the window re-opens it late and stalls the sender");
// An accepted socket ignores SO_SNDBUF (it reads back 8192), so the listener sets it before bind.
#define LISTENER_SEND_BUFFER 65536
// The one backoff: sleep a millisecond on EAGAIN or when a worker is behind. poll() measured
// slower, and a VBlank wait idles the socket for 16.7 ms.
#define WAIT_NS 1000000ULL
// How long a peer may go quiet while sending or taking a body, a listing or a trailer. A disconnect
// shows at once as recv() == 0, so only a stall by a live peer is waited out.
#define TRANSFER_WIRE_STALL_MS 180000

// A dead listener keeps a valid descriptor, so only accept()'s errno shows it. ECONNABORTED is one
// peer's early hang-up, not the listener's.
static inline bool listenerAcceptFatal(int acceptErrno) {
	return acceptErrno != EAGAIN && acceptErrno != EINTR && acceptErrno != ECONNABORTED;
}

// accept() runs every frame, so this limit is under a second.
// One odd errno must not cost a rebuild.
#define LISTENER_ACCEPT_FATAL_LIMIT 8

Result initNetwork(void);
void exitNetwork(void);

// Returns "unknown" until Wi-Fi associates. It is always the same buffer, rewritten in place at
// each call, so a pointer kept from an earlier call shows the new address at its next read.
const char *getIPText(void);

// The listening socket, or -1 while there is none. main.c serves it, and a bench knob rebuilds it.
extern int gListener;
int openListener(u16 port); // a buffer it cannot set is logged, and it serves anyway
bool setNonblocking(int fd);

// Opens libctru's ndm:u session as one that Luma's ndm:u workaround leaves alone (net.c); ndmuExit
// closes it.
void initNetworkKeepalive(void);
int bringNetworkUp(void);
// A lock bringNetworkUp could not take is asked for again this often, by the idle loop alone. A try
// is at most five ndm:u calls, and a lock NDM keeps refusing is logged once.
#define KEEPALIVE_RETRY_MS 10000
void retryNetworkKeepalive(void);
void unlockNetworkKeepalive(void);

// Returns whether START was pressed since the last read. A caller that waits ticks the LED itself.
bool startPressed(void);
// libctru computes hidKeysDown() afresh at each scan, so a key one scan sees is gone from the next.
// A request served inside a wait (main.c's serveBusy) scans for START too, so it would take the
// keys of the wait that serves it, such as a B, or the answer to a question. So while a busy
// request is served (protocol.c says so by calling keepKeys), startPressed keeps every key it sees,
// and the wait reads them once, together with its own scan's keys, in pressedKeys().
void keepKeys(bool keep);
u32 pressedKeys(void);
// A request the main loop serves keeps no key. On the idle screen, though, a START taken by one of
// its waits (which ends that request) is also that screen's START, and quits. main.c's serveInput
// reads it once here. Without this, a computer that polls (stats every 2 s) ate a START now and
// then. A transfer's START only cancels it, even one the idle screen shows no progress for (a
// screenshot).
bool idleStartTaken(void);
// START is checked at this cadence while a body or reply is in progress, not once per EAGAIN. A
// scan sees a key only while it is held, so a tap shorter than the gap between two scans is lost.
#define START_POLL_MS 20ULL
// A pending connection or request ends the wait at once, where a VBlank wait always costs a frame.
void netWait(int fd);

bool wouldBlock(void);

// Unauthenticated input gets a short timeout, because a slow peer would hold the single-client
// listener.
#define PREAUTH_INPUT_TIMEOUT_MS 10000
// The QR flow stands still while it serves a request, and a LAN request is complete within
// milliseconds.
#define BUSY_PREAUTH_INPUT_TIMEOUT_MS 1000
// How long a host waits for a reply (tools/wire.sh wireTimeout's default). A request that queued
// longer may be from a tool that gave up.
#define HOST_REPLY_WAIT_MS 15000

// CR stays data, so a CRLF line is rejected rather than authenticated as something else.
typedef enum { LINES_OK, LINES_GONE, LINES_TOO_LONG } LinesRead;
LinesRead readRequestLines(int client, char *buf, size_t cap, char **second, u64 timeoutMs);

bool sendStr(int client, const char *s);
bool sendBytes(int client, const unsigned char *data, size_t len);
// A peer resets as fast as it reconnects. A send cut short is logged only for a spent request, so
// replays of a captured read, or junk, neither wear the card nor push the log's history out.
static inline const char *sendFailEvent(const char *event) {
	return gApp.requestSpent ? event : NULL;
}

// Swept together with LISTENER_SEND_BUFFER; the results are flat from 16384, because send cost is
// per segment, not per call.
#define SEND_CHUNK_SIZE 65536
static inline size_t effectiveSendSize(void) {
	return (size_t)knobOr(KNOB(sendSize), SEND_CHUNK_SIZE);
}

// Parks the client until the next one connects, and main closes it then (closeParked(true)),
// because a second parked client would overwrite the first and leak it.
void gracefulClose(int client);
// Only a host that sends no next request waits this out, because its next connect closes the
// socket.
#define PARKED_CLOSE_MS 30000
void closeParked(bool now);

#endif // NET_H

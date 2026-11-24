// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.h"
#include "app.h"
#include "fs.h" // appendLog
#include "knobs.h"
#include "result.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/param.h> // MIN
#include <sys/socket.h>
#include <unistd.h>

int gListener = -1;

#define SOC_ALIGN 0x1000
// A 65536-byte SO_SNDBUF reserves ~112 KB a socket, and a 1048576-byte arena ran out at the third.
#define SOC_BUFFER_SIZE 0x200000
#define CLOSE_DRAIN_TIMEOUT_MS 5000

static u32 *socBuffer;

const char *getIPText(void) {
	static char ipText[INET_ADDRSTRLEN] = "unknown";
	struct in_addr ip, netmask, broadcast;
	// SOC's address is 0 until Wi-Fi associates, and the last one shown stays until then.
	if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) == 0 && ip.s_addr != 0) {
		strlcpy(ipText, inet_ntoa(ip), sizeof(ipText));
	}
	return ipText;
}

Result initNetwork(void) {
	socBuffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFER_SIZE);
	if (!socBuffer) {
		return OUT_OF_MEMORY;
	}
	Result r = socInit(socBuffer, SOC_BUFFER_SIZE);
	if (R_FAILED(r)) {
		free(socBuffer);
		socBuffer = NULL;
	}
	return r;
}

void exitNetwork(void) {
	socExit();
	free(socBuffer);
}

// Deadlines and START depend on EAGAIN because a socket left blocking would silently ignore both.
bool setNonblocking(int fd) {
	// SOC's only file flag is O_NONBLOCK, so there is no other flag to keep.
	return fcntl(fd, F_SETFL, O_NONBLOCK) == 0;
}

static bool listenerOption(int server, int name, int value, const char *failed) {
	if (setsockopt(server, SOL_SOCKET, name, &value, sizeof(value)) == 0) {
		return true;
	}
	appendLog(failed, errno, (u64)value);
	return false;
}

// SOC accepts options it does not apply, so an option that was set is read back on the listening
// socket, where `bench state` measured the value. If the socket does not hold the value, it is
// logged with Result 0. An option that was refused outright was already logged by listenerOption.
static void logIfNotHeld(bool set, int server, int name, int value, const char *failed) {
	int held = -1;
	socklen_t size = sizeof(held);
	if (set && (getsockopt(server, SOL_SOCKET, name, &held, &size) != 0 || held != value)) {
		appendLog(failed, 0, (u64)value);
	}
}

int openListener(u16 port) {
	int server = socket(AF_INET, SOCK_STREAM, 0);
	if (server < 0) {
		return -1;
	}

	int reuse = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	// SO_RCVBUF and SO_SNDBUF are set before bind because an accepted socket ignores both.
	// SO_RCVBUF moves only the advertised window. TCP_NODELAY is not set because it slowed sends.
	int window = knobOr(KNOB(rcvBuf), LISTENER_WINDOW_BYTES);
	int sendBuffer = knobOr(KNOB(sndBuf), LISTENER_SEND_BUFFER);
	bool windowSet = listenerOption(server, SO_RCVBUF, window, "rcvbuf-failed");
	bool sendBufferSet = listenerOption(server, SO_SNDBUF, sendBuffer, "sndbuf-failed");

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		goto fail;
	}

	// Clients are served inline, one at a time. A second one waits, so a busy console looks dead.
	if (listen(server, 1) < 0) {
		goto fail;
	}

	// accept() must be polled because the main loop keeps drawing and watching START.
	if (!setNonblocking(server)) {
		goto fail;
	}

	logIfNotHeld(windowSet, server, SO_RCVBUF, window, "rcvbuf-failed");
	logIfNotHeld(sendBufferSet, server, SO_SNDBUF, sendBuffer, "sndbuf-failed");
	return server;
fail:
	close(server);
	return -1;
}

static bool keepaliveHeld;
static u64 keepaliveFailures, keepaliveRetryAtMs;

// libctru's ndm:u session, which its NDMU_ calls use (libctru's ndm.c).
extern Handle ndmuHandle;

// Once one of Rosalina's network functions has run since boot (InputRedirection, the time sync, the
// debugger), Luma3DS 13's kernel answers an app's ndm:u EnterExclusiveState and LeaveExclusiveState
// itself, and NDM never sees them (k11_extension's SendSyncRequest.c). The lock then holds nothing,
// and the lid takes Wi-Fi down until a reboot. The kernel identifies a session by the 8 name bytes
// sent to srv, but srv matches only as many bytes as the name length in the request says. So
// "ndm:u" plus 3 more bytes, with length 5, gets NDM's ndm:u under a name the kernel does not
// answer for.
static Result askSrvForNdm(u32 header) {
	u32 *cmd = getThreadCommandBuffer();
	cmd[0] = header;
	memcpy(&cmd[1], "ndm:u-3d", 8);
	cmd[3] = 5;
	// GetServiceHandle flags: with 1, a full port answers at once, where srv would wait for a slot.
	cmd[4] = 1;
	Result r = svcSendSyncRequest(*srvGetSessionHandle());
	return R_SUCCEEDED(r) ? (Result)cmd[1] : r;
}

void initNetworkKeepalive(void) {
	if (R_FAILED(ndmuInit())) {
		return; // every lock fails, and the first failure is logged
	}
	// Ask IsServiceRegistered first. srv answers it at once, but it holds a GetServiceHandle for a
	// name it does not know until that name registers, with no timeout.
	u32 *cmd = getThreadCommandBuffer();
	Result r = askSrvForNdm(IPC_MakeHeader(0xE, 3, 0));
	bool known = R_SUCCEEDED(r) && (cmd[2] & 0xFF) != 0;
	if (known) {
		r = askSrvForNdm(IPC_MakeHeader(0x5, 4, 0));
	}
	Handle own = known && R_SUCCEEDED(r) ? (Handle)cmd[3] : 0;
	if (own == 0) {
		// libctru's session stays, and a lock faked through it shows. The bytes field says whether
		// srv knew the name.
		appendLog("ndm-open-failed", r, known);
		return;
	}
	svcCloseHandle(ndmuHandle);
	ndmuHandle = own;
}

// NDM shows a lock it took as its exclusive state. A faked Enter leaves the state as it was, which
// is NONE unless another process holds one. If the state cannot be read, the lock is left as it
// was.
static bool keepaliveReal(void) {
	ndmExclusiveState state;
	return R_FAILED(NDMU_GetExclusiveState(&state)) || state == NDM_EXCLUSIVE_STATE_INFRASTRUCTURE;
}

// The first failure and the recovery are logged, because otherwise a lid-closed Wi-Fi drop has no
// cause.
static void lockNetworkKeepalive(void) {
	Result r = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
	if (R_SUCCEEDED(r) && R_FAILED(r = NDMU_LockState())) {
		NDMU_LeaveExclusiveState();
	} else if (R_SUCCEEDED(r) && !keepaliveReal()) {
		NDMU_UnlockState(); // NDM counted this one
		NDMU_LeaveExclusiveState();
		r = KEEPALIVE_FAKED;
	}
	if (R_FAILED(r)) {
		if (keepaliveFailures == 0) {
			appendLog("ndm-lock-failed", r, 0);
		}
		keepaliveFailures++;
		keepaliveRetryAtMs = uptimeMs() + KEEPALIVE_RETRY_MS;
		return;
	}
	keepaliveHeld = true;
	if (keepaliveFailures > 0) {
		appendLog("ndm-lock-recovered", 0, keepaliveFailures);
		keepaliveFailures = 0;
	}
}

int bringNetworkUp(void) {
	// Take the lock first, because with the lid closed, a relaunch's Wi-Fi stays down until the
	// lock is taken.
	if (!keepaliveHeld) {
		lockNetworkKeepalive();
	}
	return openListener(DEFAULT_PORT);
}

// Once the listener is open, nothing else asks again, so a lock refused as the listener opened
// would stay refused.
void retryNetworkKeepalive(void) {
	if (!keepaliveHeld && uptimeMs() >= keepaliveRetryAtMs) {
		lockNetworkKeepalive();
	}
}

void unlockNetworkKeepalive(void) {
	if (keepaliveHeld) {
		NDMU_UnlockState();
		NDMU_LeaveExclusiveState();
		keepaliveHeld = false;
	}
}

// keepKeys: the current request is served inside a wait, and its scans leave the keys they see in
// `kept` for that wait.
static bool keeping;
static u32 kept;
static bool startOnIdle; // idleStartTaken (net.h)

void keepKeys(bool keep) {
	keeping = keep;
}

bool startPressed(void) {
	hidScanInput();
	u32 down = hidKeysDown();
	if (keeping) {
		kept |= down;
	}
	return (down & KEY_START) != 0;
}

bool idleStartTaken(void) {
	bool taken = startOnIdle;
	startOnIdle = false;
	return taken;
}

u32 pressedKeys(void) {
	hidScanInput();
	u32 down = hidKeysDown() | kept;
	kept = 0;
	return down;
}

// Ticks the LED, then checks START, for a request's wait, which has no tick of its own. On the idle
// screen, a START seen here also counts as that screen's (idleStartTaken); a transfer's own scan
// only cancels that transfer.
static bool cancelPoll(void) {
	appStatusTick();
	bool start = startPressed();
	if (start && !keeping && statusIsIdle(appStatus())) {
		startOnIdle = true;
	}
	return start;
}

void netWait(int fd) {
	struct pollfd wait = {.fd = fd, .events = POLLIN};
	if (fd < 0 || poll(&wait, 1, 16) < 0) {
		gspWaitForVBlank();
	}
}

// newlib's EWOULDBLOCK is EAGAIN.
bool wouldBlock(void) {
	return errno == EAGAIN;
}

LinesRead readRequestLines(int client, char *buf, size_t cap, char **second, u64 timeoutMs) {
	// Peek first, then read exactly through the second newline, because the body follows unframed.
	// One clock covers both lines, otherwise a peer could hold the listener for a deadline per
	// line.
	u64 start = uptimeMs();
	while (appRunning()) {
		ssize_t n = recv(client, buf, cap, MSG_PEEK);
		if (n == 0 || (n < 0 && !wouldBlock())) {
			return LINES_GONE;
		}
		char *first = n > 0 ? memchr(buf, '\n', (size_t)n) : NULL;
		char *end = first ? memchr(first + 1, '\n', (size_t)(buf + n - first - 1)) : NULL;
		if (end) {
			ssize_t take = end + 1 - buf;
			if (recv(client, buf, (size_t)take, 0) != take) {
				return LINES_GONE;
			}
			*first = *end = '\0';
			*second = first + 1;
			return LINES_OK;
		}
		if ((size_t)n == cap) {
			return LINES_TOO_LONG;
		}
		if (uptimeMs() - start >= timeoutMs) {
			return LINES_GONE;
		}
		// Peeked bytes stay readable, so poll would not wait for the rest of a partial request.
		if (n > 0) {
			gspWaitForVBlank();
		} else {
			netWait(client);
		}
		if (cancelPoll()) {
			return LINES_GONE;
		}
	}
	return LINES_GONE;
}

// No appendLog here, because a stranded worker may still be writing the card.
static bool drainPeer(int client) {
	unsigned char buffer[4096]; // the data is thrown away, so a small buffer only means more loops
	u64 startTime = uptimeMs();

	while (appRunning() && uptimeMs() - startTime < CLOSE_DRAIN_TIMEOUT_MS) {
		ssize_t n = recv(client, buffer, sizeof(buffer), 0);
		if (n > 0) {
			continue;
		}
		if (n == 0) {
			return true;
		}
		if (!wouldBlock()) {
			break;
		}
		netWait(client);
		if (cancelPoll()) {
			break;
		}
	}
	return false;
}

bool sendBytes(int client, const unsigned char *data, size_t len) {
	size_t sent = 0;
	u64 idleStart = uptimeMs(), lastPoll = idleStart;
	// Why the send ended early, for the log. It is APT's quit unless the loop sets another reason.
	const char *stopped = "send-exit";
	int err = 0;
	while (sent < len && appRunning()) {
		size_t chunk = MIN(len - sent, effectiveSendSize());
		ssize_t n = send(client, data + sent, chunk, 0);
		if (n > 0) {
			sent += (size_t)n;
			idleStart = uptimeMs();
			continue;
		}
		if (n == 0 || !wouldBlock()) {
			stopped = n == 0 ? "send-zero" : "send-failed";
			err = n == 0 ? 0 : errno;
			break;
		}
		u64 now = uptimeMs();
		if (now >= idleStart + TRANSFER_WIRE_STALL_MS) {
			stopped = "send-idle-timeout";
			break;
		}
		if (now - lastPoll < START_POLL_MS) {
			svcSleepThread(WAIT_NS);
			continue;
		}
		lastPoll = now;
		if (cancelPoll()) {
			stopped = "send-canceled";
			break;
		}
	}
	if (sent < len && sendFailEvent(stopped)) {
		appendLog(stopped, err, sent);
	}
	return sent == len;
}

bool sendStr(int client, const char *s) {
	return sendBytes(client, (const unsigned char *)s, strlen(s));
}

static int parked = -1;
static u64 parkedAtMs;

// The drain keeps unread input from turning close into an RST. The close waits for the next client
// because a close right after the last send lost the tail of a long reply, even with SO_LINGER.
void gracefulClose(int client) {
	shutdown(client, SHUT_WR);
	if (drainPeer(client)) {
		parked = client;
		parkedAtMs = uptimeMs();
	} else {
		close(client);
	}
}

void closeParked(bool now) {
	if (parked >= 0 && (now || uptimeMs() - parkedAtMs >= PARKED_CLOSE_MS)) {
		close(parked);
		parked = -1;
	}
}

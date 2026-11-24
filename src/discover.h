// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DISCOVER_H
#define DISCOVER_H

#include <3ds.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>

// A host finds the console without its address by the name 3drop-<tag>.local over mDNS (RFC 6762:
// an A record, and an NSEC for any other type) and by an answer to a probe on UDP DEFAULT_PORT.
// Both answer anyone on the subnet before any authentication, so the host proves each address with
// a signed request (tools/wire.sh _wireSeek is the twin).

#define MDNS_PORT 5353
#define MDNS_GROUP 0xE00000FBu // 224.0.0.251
// As labels, 3drop-<tag>.local is 22 bytes with NAME_TAG_HEX's 8.
#define MDNS_NAME_MAX 32
// The largest answer: the header, a legacy query's question, an A and an NSEC naming the name.
#define MDNS_REPLY_MAX (12 + (MDNS_NAME_MAX + 4) + (MDNS_NAME_MAX + 14) + (2 * MDNS_NAME_MAX + 13))

// The probe is PROBE_MAGIC padded with NULs to PROBE_LEN, and its reply is never longer, so it
// amplifies nothing; twinned with tools/wire.sh WIRE_PROBE.
#define PROBE_LEN 40
#define PROBE_MAGIC "3drop-probe/1"

// A window that allows at most `most` events in each second, counted from the first of them.
typedef struct {
	u64 startMs;
	u32 count;
} RateWindow;

// The state of the answers, kept separate from the sockets. This is the pure half, which gets
// tested and fuzzed on the host.
typedef struct {
	u32 ip, mask; // the console's, in network order; ip 0 answers nothing
	u8 name[MDNS_NAME_MAX];
	size_t nameLen;
	RateWindow groupA, groupNsec, unicast, probes;
#if THREEDROP_TEST_VERBS
	unsigned long asked; // queries for the name from the subnet, for `bench disc`
#endif
} Responder;

bool responderName(Responder *r, const char *tag);
// Puts the answer to one datagram that came to MDNS_PORT in out and returns its length, or 0 for
// none. *to is where it goes, the group or the asker.
size_t mdnsReply(Responder *r, const u8 *in, size_t n, const struct sockaddr_in *from, u64 now,
				 u8 out[MDNS_REPLY_MAX], struct sockaddr_in *to);
size_t mdnsAnnouncement(const Responder *r, u8 out[MDNS_REPLY_MAX]);
// Puts the reply `3drop <ip> <port> <model>` in out for an exact probe and returns
// its length, or 0 for none.
size_t probeReply(Responder *r, const u8 *in, size_t n, const struct sockaddr_in *from, u64 now,
				  const char *model, char out[PROBE_LEN]);

// The main loop's half. It runs inline in its idle pass, because a thread would hold SOC's one call
// in flight. Returns true when the console's address changed, which the screen shows.
bool discoverTick(void);
// What queued while a request held the loop is read and never answered late.
void discoverDrain(void);
// Closes along with the listener, and for a new key's name; the next tick opens it again.
void discoverClose(void);
#if THREEDROP_TEST_VERBS
// Appends ` disc=<open|closed> name=… asked=…` for `bench disc`. It and the counters it
// reads (asked, answered, announced, probed, drained, unsent) are bench-build state, and a
// release build keeps neither.
void discoverFormat(char *dst, size_t cap);
#endif

#endif // DISCOVER_H

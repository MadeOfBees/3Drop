// SPDX-License-Identifier: GPL-3.0-or-later
#include "discover.h"

#include "app.h"  // DEFAULT_PORT, uptimeMs
#include "auth.h" // authNameTag
#include "codec.h"
#include "fs.h" // appendLog
#include "knobs.h"
#include "net.h" // setNonblocking, wouldBlock
#include "sysinfo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// ---- The answers, which have no socket and no clock of their own -------------------------------

#define MDNS_HEADER 12
// A busy resolver packs other questions into the same query as ours. The cap bounds how much of
// one query is walked.
#define MDNS_QUESTIONS_MAX 64
#define MDNS_HOPS_MAX 8
// The QR, opcode and RCODE bits. A message with any of them set is a response, not a standard
// query, or one that RFC 6762 §18.11 says to ignore.
#define FLAGS_NOT_QUERY 0xF80F
#define FLAGS_ANSWER 0x8400 // QR and AA
#define FLAGS_RD 0x0100
#define TYPE_A 1
#define TYPE_NSEC 47
#define TYPE_ANY 255
#define CLASS_IN 1
#define CLASS_ANY 255
#define CLASS_QU 0x8000	   // in a question, the asker wants a unicast answer
#define CLASS_FLUSH 0x8000 // in a record, it replaces what caches hold for the name
#define RECORD_TTL_S 120
#define LEGACY_TTL_S 10 // RFC 6762 §6.7: a one-shot resolver caches for no longer than this
#define RECORD_A 1
#define RECORD_NSEC 2
#define UNICAST_PER_S 4
#define PROBES_PER_S 4

static u16 be16(const u8 *p) {
	return (u16)(p[0] << 8 | p[1]);
}

static size_t put16(u8 *out, size_t at, unsigned v) {
	out[at] = (u8)(v >> 8);
	out[at + 1] = (u8)v;
	return at + 2;
}

static u8 lowerAscii(u8 c) {
	return c >= 'A' && c <= 'Z' ? (u8)(c + ('a' - 'A')) : c;
}

static bool allow(RateWindow *w, u64 now, u32 most) {
	if (now - w->startMs >= 1000) {
		w->startMs = now;
		w->count = 0;
	}
	return w->count++ < most;
}

// The tag is lowercase, so only the query's name needs its ASCII case folded to match it.
bool responderName(Responder *r, const char *tag) {
	char label[MDNS_NAME_MAX];
	int n = snprintf(label, sizeof(label), "3drop-%s", tag);
	if (n <= 0 || (size_t)n + 8 > sizeof(r->name)) {
		return false;
	}
	r->name[0] = (u8)n;
	memcpy(r->name + 1, label, (size_t)n);
	memcpy(r->name + 1 + n, "\5local", 7); // the literal's NUL is the root label
	r->nameLen = (size_t)n + 8;
	return true;
}

// Returns true for a host on the console's own subnet. mDNS is link-local, and an answer to a
// host off the subnet would be reflected. An answer to the subnet's own address, its broadcast
// address or port 0 would go to every host or to none.
static bool fromSubnet(const Responder *r, const struct sockaddr_in *from) {
	u32 at = from->sin_addr.s_addr;
	return r->ip != 0 && r->mask != 0 && at != r->ip && (at & r->mask) == (r->ip & r->mask) &&
		   (at & ~r->mask) != 0 && (at | r->mask) != 0xFFFFFFFFu && from->sin_port != 0;
}

// Returns 1 when the name at *at is ours (ignoring ASCII case), 0 when it is another name and
// -1 when it is malformed. *at moves past the name. Pointers only go backwards and only a few
// are allowed, so no name can loop or run long.
static int nameIsOurs(const Responder *r, const u8 *p, size_t n, size_t *at) {
	size_t i = *at, ours = 0, total = 0, end = 0;
	bool same = true;
	int hops = 0;
	while (i < n) {
		u8 len = p[i];
		if ((len & 0xC0) == 0xC0) {
			if (i + 1 >= n || ++hops > MDNS_HOPS_MAX) {
				return -1;
			}
			size_t to = (size_t)(len & 0x3F) << 8 | p[i + 1];
			if (to < MDNS_HEADER || to >= i) {
				return -1;
			}
			end = end ? end : i + 2;
			i = to;
			continue;
		}
		if ((len & 0xC0) != 0 || i + 1 + len > n || (total += 1u + len) > 255) {
			return -1; // label types 0x40 and 0x80 are not valid in a name
		}
		same = same && ours < r->nameLen && r->name[ours] == len;
		for (u8 k = 0; same && k < len; k++) {
			same = lowerAscii(p[i + 1 + k]) == r->name[ours + 1 + k];
		}
		if (len == 0) {
			*at = end ? end : i + 1;
			return same;
		}
		ours += 1u + len;
		i += 1u + len;
	}
	return -1;
}

typedef struct {
	u8 records;	   // which of RECORD_A and RECORD_NSEC were asked for
	bool group;	   // a question for the name came without QU, so its answer goes to the group
	u16 type, cls; // type and class of the first such question, which a legacy answer repeats
} Asked;

// Nothing past the questions is read, because consoles on one key share the name, and discover
// proves each one.
static bool questionsFor(const Responder *r, const u8 *p, size_t n, Asked *q) {
	*q = (Asked){0};
	if (n < MDNS_HEADER || (be16(p + 2) & FLAGS_NOT_QUERY)) {
		return false;
	}
	u16 count = be16(p + 4);
	if (count > MDNS_QUESTIONS_MAX) {
		return false;
	}
	size_t at = MDNS_HEADER;
	for (u16 i = 0; i < count; i++) {
		int ours = nameIsOurs(r, p, n, &at);
		if (ours < 0 || at + 4 > n) {
			return false;
		}
		u16 type = be16(p + at), cls = be16(p + at + 2), inClass = cls & ~CLASS_QU;
		at += 4;
		if (!ours || (inClass != CLASS_IN && inClass != CLASS_ANY)) {
			continue;
		}
		if (!q->records) {
			q->type = type;
			q->cls = inClass;
		}
		q->records |= type == TYPE_A || type == TYPE_ANY ? RECORD_A : RECORD_NSEC;
		q->group = q->group || !(cls & CLASS_QU);
	}
	return q->records != 0;
}

static size_t putName(const Responder *r, u8 *out, size_t at) {
	memcpy(out + at, r->name, r->nameLen);
	return at + r->nameLen;
}

static size_t putRecordHead(const Responder *r, u8 *out, size_t at, unsigned type, bool legacy) {
	at = putName(r, out, at);
	at = put16(out, at, type);
	at = put16(out, at, legacy ? CLASS_IN : CLASS_FLUSH | CLASS_IN);
	at = put16(out, at, 0);
	return put16(out, at, legacy ? LEGACY_TTL_S : RECORD_TTL_S);
}

// The record set that lets Windows look a name up again (measured): an A record answers an A
// query, an NSEC alone answers any other type, and the Additional section stays empty because an
// A record there failed every re-lookup on Windows. `allowed` is the set of asked-for records that
// the rate limits let through.
static size_t answer(const Responder *r, u16 id, u16 flags, const Asked *q, u8 allowed, bool legacy,
					 u8 *out) {
	size_t at = put16(out, 0, legacy ? id : 0);
	at = put16(out, at, FLAGS_ANSWER | (legacy ? flags & FLAGS_RD : 0));
	at = put16(out, at, legacy);
	at = put16(out, at, !!(allowed & RECORD_A) + !!(allowed & RECORD_NSEC));
	at = put16(out, at, 0);
	at = put16(out, at, 0);
	if (legacy) {
		at = putName(r, out, at);
		at = put16(out, at, q->type);
		at = put16(out, at, q->cls);
	}
	if (allowed & RECORD_A) {
		at = putRecordHead(r, out, at, TYPE_A, legacy);
		at = put16(out, at, 4);
		memcpy(out + at, &r->ip, 4);
		at += 4;
	}
	if (allowed & RECORD_NSEC) {
		at = putRecordHead(r, out, at, TYPE_NSEC, legacy);
		at = put16(out, at, (unsigned)r->nameLen + 3);
		at = putName(r, out, at);
		out[at++] = 0; // window 0, one bitmap byte, which says only A exists
		out[at++] = 1;
		out[at++] = 0x40;
	}
	return at;
}

size_t mdnsReply(Responder *r, const u8 *in, size_t n, const struct sockaddr_in *from, u64 now,
				 u8 out[MDNS_REPLY_MAX], struct sockaddr_in *to) {
	Asked q;
	if (!fromSubnet(r, from) || !questionsFor(r, in, n, &q)) {
		return 0;
	}
	BENCH(r->asked++);
	bool legacy = from->sin_port != htons(MDNS_PORT); // a one-shot resolver, not mDNS
	u8 records = q.records;
	*to = *from;
	if (!legacy && q.group) { // RFC 6762 §6: a record goes to the group at most once a second
		if ((records & RECORD_A) && !allow(&r->groupA, now, 1)) {
			records &= (u8)~RECORD_A;
		}
		if ((records & RECORD_NSEC) && !allow(&r->groupNsec, now, 1)) {
			records &= (u8)~RECORD_NSEC;
		}
		to->sin_addr.s_addr = htonl(MDNS_GROUP);
	} else if (!allow(&r->unicast, now, UNICAST_PER_S)) {
		records = 0;
	}
	return records ? answer(r, be16(in), be16(in + 2), &q, records, legacy, out) : 0;
}

size_t mdnsAnnouncement(const Responder *r, u8 out[MDNS_REPLY_MAX]) {
	return answer(r, 0, 0, &(Asked){0}, RECORD_A, false, out);
}

size_t probeReply(Responder *r, const u8 *in, size_t n, const struct sockaddr_in *from, u64 now,
				  const char *model, char out[PROBE_LEN]) {
	static const char PROBE[PROBE_LEN] = PROBE_MAGIC;
	if (!fromSubnet(r, from) || n != PROBE_LEN || memcmp(in, PROBE, PROBE_LEN) != 0 ||
		!allow(&r->probes, now, PROBES_PER_S)) {
		return 0;
	}
	struct in_addr ip = {.s_addr = r->ip};
	int len = snprintf(out, PROBE_LEN, "3drop %s %d %s\n", inet_ntoa(ip), DEFAULT_PORT, model);
	return len > 0 && len < PROBE_LEN ? (size_t)len : 0; // a cut-short reply would mislead
}

// ---- The sockets --------------------------------------------------------------------------------

#define DATAGRAM_MAX 1500
// The most datagrams read per socket in one pass. Each pass waits on the listener, so a flood
// cannot spin the loop.
#define READ_BUDGET 8
#define DRAIN_BUDGET 64		   // per socket after a request
#define ADDRESS_PERIOD_MS 3000 // a lease or a roam can move the address without a listener outage
#define REOPEN_MS 3000
#define ANNOUNCEMENTS 2 // RFC 6762 §8.3: at least two, a second apart

static struct {
	Responder r;
	int mdns, probe; // both open or both -1
	u64 addressAtMs, reopenAtMs, announceAtMs;
	int announcements;
	bool failing; // the last open failed, and said so once
	unsigned long opened;
#if THREEDROP_TEST_VERBS
	unsigned long answered, announced, probed, drained, unsent; // for `bench disc`
#endif
} disc = {.mdns = -1, .probe = -1};

static u8 rx[DATAGRAM_MAX + 1]; // a datagram that fills it may be cut short, so it is dropped

static int udpOpen(u16 port) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0), reuse = 1;
	struct sockaddr_in at = {.sin_family = AF_INET, .sin_port = htons(port)};
	at.sin_addr.s_addr = INADDR_ANY;
	if (fd < 0) {
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if (bind(fd, (struct sockaddr *)&at, sizeof(at)) != 0 || !setNonblocking(fd)) {
		int err = errno;
		close(fd);
		errno = err;
		return -1;
	}
	return fd;
}

// The join is on INADDR_ANY because a join on the console's own address returns 0 but delivers
// nothing (measured).
static bool groupMembership(int fd, int name) {
	struct ip_mreq group;
	memset(&group, 0, sizeof(group));
	group.imr_multiaddr.s_addr = htonl(MDNS_GROUP);
	group.imr_interface.s_addr = INADDR_ANY;
	return setsockopt(fd, IPPROTO_IP, name, &group, sizeof(group)) == 0;
}

void discoverClose(void) {
	if (disc.mdns >= 0) {
		(void)groupMembership(disc.mdns, IP_DROP_MEMBERSHIP);
		close(disc.mdns);
	}
	if (disc.probe >= 0) {
		close(disc.probe);
	}
	disc.mdns = disc.probe = -1;
}

// Returns the failed call's errno. It is never 0, which would read as a successful open.
static int failure(void) {
	return errno ? errno : EIO;
}

// Returns 0, or the errno of the step that failed. A failure leaves nothing open.
static int openSockets(void) {
	char tag[NAME_TAG_HEX + 1];
	if (!authNameTag(tag) || !responderName(&disc.r, tag)) {
		return EINVAL; // no key, so no name
	}
	if ((disc.mdns = udpOpen(MDNS_PORT)) < 0) {
		return failure();
	}
	if (!groupMembership(disc.mdns, IP_ADD_MEMBERSHIP) ||
		(disc.probe = udpOpen(DEFAULT_PORT)) < 0) {
		int err = failure();
		discoverClose();
		return err;
	}
	return 0;
}

// A failure is logged once per outage, because a retry every REOPEN_MS must not fill the card.
static void discoverOpen(u64 now) {
	int err = openSockets(), ttl = 255, loop = 0;
	if (err) {
		if (!disc.failing) {
			appendLog("disc-open-failed", err, disc.opened);
		}
		disc.failing = true;
		disc.reopenAtMs = now + REOPEN_MS;
		return;
	}
	// RFC 6762 §11: the TTL is 255, which a receiver checks to know the answer came from the link.
	setsockopt(disc.mdns, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	setsockopt(disc.mdns, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
	setsockopt(disc.mdns, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
	disc.failing = false;
	disc.opened++;
	disc.announcements = ANNOUNCEMENTS;
	disc.announceAtMs = now;
}

// A socket from before a Wi-Fi drop can be dead without any sign, so both are reopened shortly.
static void reopenSoon(void) {
	discoverClose();
	disc.reopenAtMs = uptimeMs() + REOPEN_MS;
}

// The sockets are reopened only when a socket or the network is gone, because a failed send to one
// asker's address must not let that asker keep the name offline for 3 s at a time. Returns
// true when the send went through.
static bool sendDatagram(int fd, const void *bytes, size_t n, const struct sockaddr_in *to) {
	if (sendto(fd, bytes, n, 0, (const struct sockaddr *)to, sizeof(*to)) == (ssize_t)n) {
		return true;
	}
	if (errno == ENETDOWN || errno == ENETUNREACH || errno == EBADF) {
		reopenSoon();
	} else {
		BENCH(disc.unsent++);
	}
	return false;
}

static void mdnsDatagram(size_t n, const struct sockaddr_in *from, u64 now) {
	static u8 out[MDNS_REPLY_MAX];
	struct sockaddr_in to;
	size_t len = mdnsReply(&disc.r, rx, n, from, now, out, &to);
	if (len && sendDatagram(disc.mdns, out, len, &to)) {
		BENCH(disc.answered++);
	}
}

static void probeDatagram(size_t n, const struct sockaddr_in *from, u64 now) {
	char out[PROBE_LEN];
	size_t len = probeReply(&disc.r, rx, n, from, now, sysinfoModel(), out);
	if (len && sendDatagram(disc.probe, out, len, from)) {
		BENCH(disc.probed++);
	}
}

typedef void (*Datagram)(size_t n, const struct sockaddr_in *from, u64 now);

// Reads up to `budget` datagrams, passing each to `handle`, or draining them when it is NULL.
// The descriptor is read through a pointer on each pass because a failed send in a handler closes
// the pair of sockets, which ends the loop.
static void readSome(const int *fd, unsigned budget, Datagram handle) {
	for (unsigned i = 0; *fd >= 0 && i < budget; i++) {
		struct sockaddr_in from;
		socklen_t size = sizeof(from);
		ssize_t n = recvfrom(*fd, rx, sizeof(rx), 0, (struct sockaddr *)&from, &size);
		if (n < 0) {
			if (!wouldBlock()) {
				reopenSoon();
			}
			return;
		}
		if (!handle) {
			BENCH(disc.drained++); // read and dropped; the read is the drain
		} else if ((size_t)n < sizeof(rx)) {
			handle((size_t)n, &from, uptimeMs());
		}
	}
}

// Returns true when SOC's address or mask differs from the last read.
static bool addressMoved(void) {
	struct in_addr ip, mask, broadcast;
	if (SOCU_GetIPInfo(&ip, &mask, &broadcast) != 0) {
		ip.s_addr = mask.s_addr = 0;
	}
	bool moved = ip.s_addr != disc.r.ip || mask.s_addr != disc.r.mask;
	disc.r.ip = ip.s_addr;
	disc.r.mask = mask.s_addr;
	return moved;
}

static void announce(u64 now) {
	static u8 out[MDNS_REPLY_MAX];
	if (disc.mdns < 0 || disc.announcements == 0 || now < disc.announceAtMs || disc.r.ip == 0) {
		return;
	}
	disc.announcements--;
	disc.announceAtMs = now + 1000;
	struct sockaddr_in group = {.sin_family = AF_INET, .sin_port = htons(MDNS_PORT)};
	group.sin_addr.s_addr = htonl(MDNS_GROUP);
	if (sendDatagram(disc.mdns, out, mdnsAnnouncement(&disc.r, out), &group)) {
		BENCH(disc.announced++);
	}
}

bool discoverTick(void) {
	u64 now = uptimeMs();
	bool moved = false;
	if (KNOB(noDisc)) {
		discoverClose();
		return false;
	}
	if (now >= disc.addressAtMs) {
		disc.addressAtMs = now + ADDRESS_PERIOD_MS;
		if ((moved = addressMoved())) {
			discoverClose(); // the group is joined and the name announced again at the new address
		}
	}
	if (disc.mdns < 0 && now >= disc.reopenAtMs) {
		discoverOpen(now);
	}
	readSome(&disc.mdns, READ_BUDGET, mdnsDatagram);
	readSome(&disc.probe, READ_BUDGET, probeDatagram);
	announce(now);
	return moved;
}

void discoverDrain(void) {
	readSome(&disc.mdns, DRAIN_BUDGET, NULL);
	readSome(&disc.probe, DRAIN_BUDGET, NULL);
}

#if THREEDROP_TEST_VERBS
void discoverFormat(char *dst, size_t cap) {
	char name[MDNS_NAME_MAX + 8] = "-";
	if (disc.r.nameLen) {
		snprintf(name, sizeof(name), "%.*s.local", disc.r.name[0], (const char *)disc.r.name + 1);
	}
	(void)appendf(dst, cap,
				  " disc=%s name=%s asked=%lu answered=%lu announced=%lu probed=%lu drained=%lu "
				  "opened=%lu unsent=%lu",
				  disc.mdns >= 0 ? "open" : "closed", name, disc.r.asked, disc.answered,
				  disc.announced, disc.probed, disc.drained, disc.opened, disc.unsent);
}
#endif

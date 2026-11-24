// cc: -DTHREEDROP_TEST_VERBS=1 -fsanitize=undefined -fno-sanitize-recover=all
// cc: src/discover.c src/codec.c
// The console's name and probe answers (src/discover.c), which anyone on the subnet can read before
// any authentication, pinned byte for byte. The fuzz's datagrams sit against guard pages, so a read
// past either end faults (ASan hangs at its start in the gate's sandbox).
#include "fakes.h"

#include "app.h"  // DEFAULT_PORT
#include "auth.h" // NAME_TAG_HEX
#include "discover.h"
#include "knobs.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

Knobs gKnobs;

#define DATAGRAM_MAX 1500
#define NAME                                                                                       \
	"\x0e"                                                                                         \
	"3drop-1938bccf"                                                                               \
	"\x05"                                                                                         \
	"local" // the root label follows each use
#define AT_76 "\xc0\xa8\2\x4c"
#define AT_80 "\xc0\xa8\2\x50"
#define A_RR(ip) "\0\0\1\x80\1\0\0\0\x78\0\4" ip
#define NSEC_RR NAME "\0\0\x2f\x80\1\0\0\0\x78\0\x19" NAME "\0\0\1\x40"
#define HEAD(id, flags, qd, an) id flags "\0" qd "\0" an "\0\0\0\0"

// mDNSResponder's A and AAAA in one query, with the second name a pointer to the first.
static const char MAC[] = HEAD("\0\0", "\0\0", "\2", "\0") NAME "\0\0\1\0\1\xc0\x0c\0\x1c\0\1";
static const char MAC_ANSWER[] = HEAD("\0\0", "\x84\0", "\0", "\2") NAME A_RR(AT_76) NSEC_RR;
// Windows' A query, which is QM, and its AAAA query, which is QU.
static const char WIN_A[] = HEAD("\0\0", "\0\0", "\1", "\0") NAME "\0\0\1\0\1";
static const char WIN_AAAA[] = HEAD("\0\0", "\0\0", "\1", "\0") NAME "\0\0\x1c\x80\1";
// The same AAAA query, but QM, so its answer goes to the group.
static const char GROUP_AAAA[] = HEAD("\0\0", "\0\0", "\1", "\0") NAME "\0\0\x1c\0\1";
static const char A_ANSWER[] = HEAD("\0\0", "\x84\0", "\0", "\1") NAME A_RR(AT_76);
static const char NSEC_ANSWER[] = HEAD("\0\0", "\x84\0", "\0", "\1") NSEC_RR;
// dig's legacy unicast query. The answer echoes its ID, RD and question, with TTL 10 and no
// cache-flush.
static const char DIG[] = HEAD("\xbe\xef", "\1\0", "\1", "\0") NAME "\0\0\1\0\1";
static const char DIG_ANSWER[] =
	HEAD("\xbe\xef", "\x85\0", "\1", "\1") NAME "\0\0\1\0\1" NAME "\0\0\1\0\1\0\0\0\x0a\0\4" AT_76;
static const char PROBE[PROBE_LEN] = PROBE_MAGIC;

static struct sockaddr_in at(const char *ip, u16 port) {
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
	a.sin_addr.s_addr = inet_addr(ip);
	return a;
}

static bool samePeer(struct sockaddr_in a, struct sockaddr_in b) {
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static Responder fresh(void) {
	Responder r = {.ip = inet_addr("192.168.2.76"), .mask = inet_addr("255.255.255.0")};
	assert(responderName(&r, "1938bccf"));
	return r;
}

static u8 out[MDNS_REPLY_MAX];
static struct sockaddr_in to;

static size_t ask(Responder *r, const char *query, size_t n, struct sockaddr_in from, u64 now) {
	memset(out, 0xAA, sizeof(out));
	return mdnsReply(r, (const u8 *)query, n, &from, now, out, &to);
}
#define ASK(r, query, from, now) ask(r, query, sizeof(query) - 1, from, now)
#define SAID(len, want) ((len) == sizeof(want) - 1 && memcmp(out, want, sizeof(want) - 1) == 0)

// Tests the adversary's record set (A answers A, an NSEC alone answers the rest, nothing in
// Additional), where each answer goes, and the rate limits: a record to the group once a second, 4
// unicast a second.
static void answers(void) {
	Responder r = fresh();
	struct sockaddr_in mac = at("192.168.2.73", 5353), win = at("192.168.2.67", 5353);
	struct sockaddr_in dig = at("192.168.2.73", 54321), group = at("224.0.0.251", 5353);
	assert(SAID(ASK(&r, MAC, mac, 10000), MAC_ANSWER) && samePeer(to, group));
	assert(ASK(&r, MAC, mac, 10999) == 0); // both records went to the group under a second ago
	assert(SAID(ASK(&r, WIN_AAAA, win, 10999), NSEC_ANSWER) && samePeer(to, win)); // QU: unicast
	assert(SAID(ASK(&r, WIN_A, win, 11000), A_ANSWER) && samePeer(to, group));
	assert(SAID(ASK(&r, DIG, dig, 12000), DIG_ANSWER) && samePeer(to, dig));
	assert(r.asked == 5);
	for (int i = 0; i < 5; i++) {
		assert((ASK(&r, DIG, dig, 30000) != 0) == (i < 4));
	}
	assert(ASK(&r, DIG, dig, 31000) != 0); // a second later the window opens again
	// The announcement: the A record alone, to the group.
	assert(SAID(mdnsAnnouncement(&r, out), A_ANSWER));
	// A tag too long for a name is refused, never cut.
	assert(!responderName(&r, "0123456789abcdef0123456789abcdef"));
}

// The group's limit of one a second is kept per record. For a query for both, the NSEC sent under a
// second ago is left out, and the A answers alone with a count of one.
static void groupWindows(void) {
	struct sockaddr_in mac = at("192.168.2.73", 5353), group = at("224.0.0.251", 5353);
	Responder r = fresh();
	assert(SAID(ASK(&r, GROUP_AAAA, mac, 40000), NSEC_ANSWER) && samePeer(to, group));
	assert(SAID(ASK(&r, MAC, mac, 40500), A_ANSWER) && samePeer(to, group));
}

// A question in a query of its own: the header, then each of the parts, which hold the name.
static size_t query(u8 *q, u16 qd, u16 flags, const char *name, size_t nameLen, u16 type, u16 cls) {
	memset(q, 0, 12);
	q[2] = (u8)(flags >> 8);
	q[3] = (u8)flags;
	q[4] = (u8)(qd >> 8);
	q[5] = (u8)qd;
	memcpy(q + 12, name, nameLen);
	size_t n = 12 + nameLen;
	q[n++] = (u8)(type >> 8);
	q[n++] = (u8)type;
	q[n++] = (u8)(cls >> 8);
	q[n++] = (u8)cls;
	return n;
}
#define QUERY(q, qd, flags, name, type, cls) query(q, qd, flags, name, sizeof(name) - 1, type, cls)

// The responder answers only exactly its name, in class IN or ANY, ignoring ASCII case. It answers
// nothing malformed, nothing from the console itself or another subnet, and nothing while it has no
// address.
static void refusals(void) {
	Responder r = fresh();
	struct sockaddr_in mac = at("192.168.2.73", 5353);
	u8 q[600];
	u64 now = 100000;
#define ANSWERED(n) (ask(&r, (const char *)q, n, mac, now += 1000) != 0)
	assert(ANSWERED(QUERY(q, 1, 0,
						  "\x0e"
						  "3DROP-1938BCCF\5LOCAL\0",
						  1, 1)));
	assert(ANSWERED(QUERY(q, 1, 0x0200, NAME "\0", 1, 1)));	 // TC: known answers follow
	assert(ANSWERED(QUERY(q, 1, 0, NAME "\0", 255, 255)));	 // ANY, in ANY
	assert(ANSWERED(QUERY(q, 1, 0, NAME "\0", 13, 0x8001))); // HINFO: the NSEC
	assert(!ANSWERED(QUERY(q, 1, 0, NAME "\0", 1, 3)));		 // CH
	assert(!ANSWERED(QUERY(q, 1, 0, "\4desk\5local\0", 1, 1)));
	assert(!ANSWERED(QUERY(q, 1, 0,
						   "\x0e"
						   "3drop-1938bccf\0",
						   1, 1)));
	assert(!ANSWERED(QUERY(q, 1, 0, NAME "\1x\0", 1, 1)));	  // a label past the name
	assert(!ANSWERED(QUERY(q, 1, 0, "\1x" NAME "\0", 1, 1))); // the name as a suffix
	assert(!ANSWERED(QUERY(q, 1, 0x8400, NAME "\0", 1, 1)));  // a response
	assert(!ANSWERED(QUERY(q, 1, 0x2800, NAME "\0", 1, 1)));  // another opcode
	assert(!ANSWERED(QUERY(q, 1, 0x0003, NAME "\0", 1, 1)));  // an RCODE (RFC 6762 §18.11)
	assert(!ANSWERED(QUERY(q, 0, 0, NAME "\0", 1, 1)));		  // no question
	assert(!ANSWERED(QUERY(q, 65535, 0, NAME "\0", 1, 1)));	  // more than it reads
	assert(!ANSWERED(QUERY(q, 1, 0, "\xc0\x0c", 1, 1)));	  // a pointer to itself
	assert(!ANSWERED(QUERY(q, 1, 0, "\xc0\x0f\0", 1, 1)));	  // forward
	assert(!ANSWERED(QUERY(q, 1, 0, "\xc0\x05", 1, 1)));	  // into the header
	assert(!ANSWERED(QUERY(q, 1, 0, "\xff\xff", 1, 1)));	  // past the end
	assert(!ANSWERED(QUERY(q, 1, 0,
						   "\x0e"
						   "3drop",
						   1, 1))); // cut in the name
	size_t n = QUERY(q, 1, 0, NAME "\0", 1, 1);
	assert(!ANSWERED(n - 1) && !ANSWERED(11)); // cut in the type, and in the header
	// Questions after ours that do not parse make the whole query malformed.
	n = QUERY(q, 2, 0, NAME "\0", 1, 1);
	q[n++] = 0x40;
	assert(!ANSWERED(n));
	// Ours among a busy resolver's others is answered, up to 64 questions; past that it is not.
	for (unsigned count = 64; count <= 65; count++) {
		n = QUERY(q, count, 0, NAME "\0", 1, 1);
		for (unsigned i = 1; i < count; i++) {
			memcpy(q + n, "\xc0\x0c\0\x1c\0\1", 6); // ours again, AAAA, by a pointer
			n += 6;
		}
		assert(ANSWERED(n) == (count == 64));
	}
	// Labels of 63 parse, and so does a name of 255 bytes, as another question after it shows. A
	// name of 256 does not.
	static char longest[300];
	memset(longest, 'a', sizeof(longest));
	longest[0] = longest[64] = longest[128] = 63;
	longest[192] = 61; // and the root: 255 bytes
	longest[254] = 0;
	n = query(q, 2, 0, longest, 255, 1, 1);
	memcpy(q + n, NAME "\0\0\1\0\1", sizeof(NAME) + 4);
	assert(ANSWERED(n + sizeof(NAME) + 4));
	longest[192] = 62;
	longest[254] = 'a';
	longest[255] = 0;
	n = query(q, 2, 0, longest, 256, 1, 1);
	memcpy(q + n, NAME "\0\0\1\0\1", sizeof(NAME) + 4);
	assert(!ANSWERED(n + sizeof(NAME) + 4));
	// Label types 0x40 and 0x80 are not valid in a name, even with the bytes their lengths would
	// need. The query is malformed, so ours after it goes unanswered.
	for (unsigned type = 0x40; type <= 0x80; type += 0x40) {
		static char label[200];
		memset(label, 'a', sizeof(label));
		label[0] = (char)type;
		label[1 + type] = 0;
		n = query(q, 2, 0, label, 2 + type, 1, 1);
		memcpy(q + n, NAME "\0\0\1\0\1", sizeof(NAME) + 4);
		assert(!ANSWERED(n + sizeof(NAME) + 4));
	}
	// A pointer back to the label before it would loop forever, so 8 hops end it.
	assert(!ANSWERED(QUERY(q, 1, 0,
						   "\x09"
						   "123456789\xc0\x0c",
						   1, 1)));
#undef ANSWERED
	// It does not answer the console itself, another subnet, the subnet's own or broadcast address
	// (an answer there reaches every host), or port 0, and it does not answer when it has no
	// address or mask of its own.
	static const char *const NOT_A_HOST[] = {"192.168.2.76", "10.0.0.5", "192.168.2.0",
											 "192.168.2.255", "255.255.255.255"};
	for (size_t i = 0; i < sizeof(NOT_A_HOST) / sizeof(NOT_A_HOST[0]); i++) {
		assert(ASK(&r, DIG, at(NOT_A_HOST[i], 54321), now += 1000) == 0);
	}
	assert(ASK(&r, DIG, at("192.168.2.73", 0), now += 1000) == 0);
	assert(ASK(&r, DIG, at("192.168.2.73", 54321), now += 1000) != 0);
	r.mask = 0;
	assert(ASK(&r, DIG, at("192.168.2.73", 54321), now += 1000) == 0);
	r.mask = inet_addr("255.255.255.0");
	r.ip = 0;
	assert(ASK(&r, DIG, at("192.168.2.73", 54321), now += 1000) == 0);
}

// The probe is answered for its exact bytes alone, unicast to the asker on the subnet, limited to 4
// a second, and the reply is never longer than the probe, even at the widest address and model.
static void probes(void) {
	// The probe's bytes are pinned here, and testDiscover.sh pins tools/wire.sh's to the same
	// bytes.
	assert(PROBE_LEN == 40 && strcmp(PROBE_MAGIC, "3drop-probe/1") == 0);
	Responder r = fresh();
	struct sockaddr_in mac = at("192.168.2.73", 40000);
	char reply[PROBE_LEN];
	static u8 in[DATAGRAM_MAX + 1];
	for (int i = 0; i < 6; i++) {
		size_t len = probeReply(&r, (const u8 *)PROBE, PROBE_LEN, &mac, 80000, "o3ds", reply);
		assert((len != 0) == (i < 4));
	}
	assert(probeReply(&r, (const u8 *)PROBE, PROBE_LEN, &mac, 81000, "o3ds", reply) ==
			   sizeof("3drop 192.168.2.76 1337 o3ds\n") - 1 &&
		   memcmp(reply, "3drop 192.168.2.76 1337 o3ds\n", 29) == 0);
	u64 now = 90000;
	memcpy(in, PROBE, PROBE_LEN);
	for (size_t n = 0; n <= DATAGRAM_MAX; n++) {
		assert(n == PROBE_LEN ||
			   probeReply(&r, in, n, &mac, now += 1000, "o3ds", reply) == 0); // any other length
	}
	for (size_t i = 0; i < PROBE_LEN * 8; i++) {
		in[i / 8] ^= (u8)(1u << (i % 8));
		assert(probeReply(&r, in, PROBE_LEN, &mac, now += 1000, "o3ds", reply) == 0);
		in[i / 8] ^= (u8)(1u << (i % 8));
	}
	struct sockaddr_in far = at("10.0.0.5", 40000), self = at("192.168.2.76", 40000);
	assert(probeReply(&r, in, PROBE_LEN, &far, now += 1000, "o3ds", reply) == 0);
	assert(probeReply(&r, in, PROBE_LEN, &self, now += 1000, "o3ds", reply) == 0);
	r.ip = inet_addr("255.255.255.254");
	struct sockaddr_in near = at("255.255.255.1", 40000);
	static const char WIDEST[] = "3drop 255.255.255.254 1337 n3dsxl\n";
	assert(probeReply(&r, in, PROBE_LEN, &near, now += 1000, "n3dsxl", reply) ==
			   sizeof(WIDEST) - 1 &&
		   memcmp(reply, WIDEST, sizeof(WIDEST) - 1) == 0 && sizeof(WIDEST) - 1 < PROBE_LEN);
}

// ---- The fuzz ---------------------------------------------------------------------------------

// The test's own reading of a query, separate from discover.c's. It follows pointers to any offset
// not yet visited, rebuilds the name as uncompressed lowercase labels and compares it whole.
// Returns true when a question that parses names the responder's name in class IN or ANY.
static bool namesUs(const u8 *p, size_t n) {
	static const char OURS[] = NAME;
	if (n < 12 || (p[2] & 0xF8)) {
		return false;
	}
	size_t at = 12, count = (size_t)p[4] << 8 | p[5];
	for (size_t q = 0; q < count; q++) {
		static bool visited[DATAGRAM_MAX + 1];
		u8 name[300];
		size_t len = 0, i = at, end = 0;
		bool whole = false;
		memset(visited, 0, sizeof(visited));
		while (i < n && !visited[i] && !whole) {
			visited[i] = true;
			u8 label = p[i];
			if ((label & 0xC0) == 0xC0) {
				if (i + 1 >= n) {
					return false;
				}
				end = end ? end : i + 2;
				i = (size_t)(label & 0x3F) << 8 | p[i + 1];
				continue;
			}
			if ((label & 0xC0) || i + 1 + label > n || len + 1 + label > 255) {
				return false;
			}
			name[len++] = label;
			for (u8 k = 0; k < label; k++) {
				u8 c = p[i + 1 + k];
				name[len++] = c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c;
			}
			whole = label == 0;
			i += 1u + label;
		}
		at = end ? end : i;
		if (!whole || at + 4 > n) {
			return false;
		}
		u16 cls = (u16)((p[at + 2] << 8 | p[at + 3]) & 0x7FFF);
		at += 4;
		if (len == sizeof(OURS) && memcmp(name, OURS, sizeof(OURS)) == 0 &&
			(cls == 1 || cls == 255)) {
			return true;
		}
	}
	return false;
}

static u64 seed = 0x3D50F00DCAFEull;
static u32 below(u32 n) { // xorshift64, so every run has the same cases
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (u32)(seed % n);
}

static size_t mutate(u8 *b, size_t n) {
	static const u8 EDGES[] = {0x00, 0x01, 0x05, 0x0c, 0x0e, 0x3f, 0x40,
							   0x7f, 0x80, 0x84, 0xbf, 0xc0, 0xc1, 0xff};
	for (u32 ops = 1 + below(4); ops > 0; ops--) {
		size_t i = n ? below((u32)n) : 0;
		switch (below(6)) {
		case 0:
			b[i] ^= n ? (u8)(1u << below(8)) : 0;
			break;
		case 1:
			b[i] = n ? EDGES[below(sizeof(EDGES))] : b[i];
			break;
		case 2:
			n = below((u32)n + 1); // cut anywhere
			break;
		case 3: // a pointer to anywhere, the header included
			if (i + 1 < n) {
				b[i] = 0xC0;
				b[i + 1] = (u8)below((u32)n + 16);
			}
			break;
		case 4: // the question count
			if (n >= 6) {
				b[4] = below(2) ? 0xFF : 0;
				b[5] = (u8)below(12);
			}
			break;
		default: { // repeats a run of the datagram further on
			size_t from = below((u32)n + 1), take = below(64);
			take = from + take > n ? n - from : take;
			take = n + take > DATAGRAM_MAX ? DATAGRAM_MAX - n : take;
			memmove(b + i + take, b + i, n - i);
			memmove(b + i, b + (from < i ? from : from + take), take);
			n += take;
		}
		}
	}
	return n;
}

// A page between two that fault. With a datagram at its end, a read past the datagram's last byte
// faults; with one at its start, a read before its first byte faults.
static u8 *guardedPage(size_t page) {
	u8 *base = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(base != MAP_FAILED && mprotect(base, page, PROT_NONE) == 0 &&
		   mprotect(base + 2 * page, page, PROT_NONE) == 0);
	return base + page;
}

#define FUZZ_ROUNDS 12000

static void fuzz(void) {
	static const struct {
		const char *bytes;
		size_t n;
	} SEEDS[] = {
#define SEED(s) {s, sizeof(s) - 1}
		SEED(MAC),
		SEED(WIN_A),
		SEED(WIN_AAAA),
		SEED(DIG),
		SEED(MAC_ANSWER), // another device's response
		SEED(HEAD("\0\0", "\0\0", "\1", "\0") "\4desk\5local\0\0\1\x80\1"), // the router's QU
		SEED(HEAD("\0\0", "\0\0", "\4", "\0") "\5other\5local\0\0\1\0\1" NAME
											  "\0\0\x1c\0\1\xc0\x0c\0\1\0\1\xc0\x1d\0\1\0\1"),
		{PROBE, PROBE_LEN},
#undef SEED
	};
	const struct sockaddr_in SOURCES[] = {at("192.168.2.73", 5353), at("192.168.2.67", 61000),
										  at("10.0.0.5", 5353), at("192.168.2.76", 5353)};
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	u8 *in = guardedPage(page), *reply = guardedPage(page) + page - MDNS_REPLY_MAX;
	static u8 work[DATAGRAM_MAX];
	// The largest answer fills MDNS_REPLY_MAX up to its guard page. It comes from the longest name,
	// in a legacy query for A and AAAA.
	Responder longest = fresh();
	assert(responderName(&longest, "0123456789abcdef01") && longest.nameLen == MDNS_NAME_MAX);
	memcpy(in, HEAD("\xbe\xef", "\0\0", "\2", "\0"), 12);
	memcpy(in + 12, longest.name, MDNS_NAME_MAX);
	memcpy(in + 12 + MDNS_NAME_MAX, "\0\1\0\1\xc0\x0c\0\x1c\0\1", 10);
	struct sockaddr_in legacy = at("192.168.2.73", 54321), sentTo;
	assert(mdnsReply(&longest, in, 12 + MDNS_NAME_MAX + 10, &legacy, 1000, reply, &sentTo) ==
		   MDNS_REPLY_MAX);
	Responder r = fresh();
	u64 now = 1000000;
	unsigned answered = 0;
	for (size_t s = 0; s < sizeof(SEEDS) / sizeof(SEEDS[0]); s++) {
		for (unsigned round = 0; round < FUZZ_ROUNDS; round++) {
			memcpy(work, SEEDS[s].bytes, SEEDS[s].n);
			size_t n = round ? mutate(work, SEEDS[s].n) : SEEDS[s].n;
			struct sockaddr_in from = SOURCES[round % 4];
			bool subnet = round % 4 < 2;
			for (int end = 0; end < 2; end++) {
				u8 *datagram = end ? in + page - n : in;
				memcpy(datagram, work, n);
				size_t len = mdnsReply(&r, datagram, n, &from, now += 1000, reply, &sentTo);
				assert(len <= MDNS_REPLY_MAX && (len == 0 || subnet));
				if (len) {
					answered++;
					assert(namesUs(work, n) && (reply[2] & 0x80) &&
						   (samePeer(sentTo, from) || samePeer(sentTo, at("224.0.0.251", 5353))));
				}
				char line[PROBE_LEN];
				len = probeReply(&r, datagram, n, &from, now, "n3dsxl", line);
				assert(len == 0 || (subnet && n == PROBE_LEN && memcmp(work, PROBE, n) == 0));
			}
		}
	}
	assert(answered > FUZZ_ROUNDS / 4); // it reached the answers, not only the refusals
}

// ---- The sockets, on a scripted SOC -----------------------------------------------------------

// Sockets are numbers bound to a port, and each port's datagrams are queued with their sources. The
// struct also keeps what was sent last and where, and what each option was set to.
typedef struct {
	u8 bytes[128];
	size_t n;
	struct sockaddr_in from;
} Queued;
static struct {
	int next;
	u16 port[64];
	Queued inbox[2][32]; // 5353's and 1337's
	size_t head[2], tail[2];
	unsigned binds, joins, drops, sends;
	u32 joinedOn;
	int multicastTtl, ttl, loop;
	u8 sent[256];
	size_t sentN;
	struct sockaddr_in sentTo;
	bool joinFails, noKey;
	int sendErrno, recvErrno; // the next send fails with sendErrno, every receive with recvErrno
	u32 ip, mask;
	const char *tag;
} soc = {.next = 10, .tag = "1938bccf"};

static int box(u16 port) {
	return port == MDNS_PORT ? 0 : 1;
}

static void queue(u16 port, const void *bytes, size_t n, struct sockaddr_in from) {
	Queued *q = &soc.inbox[box(port)][soc.tail[box(port)]++ % 32];
	memcpy(q->bytes, bytes, n);
	q->n = n;
	q->from = from;
}
#define QUEUE(port, bytes, from) queue(port, bytes, sizeof(bytes) - 1, from)

static size_t waiting(u16 port) {
	return soc.tail[box(port)] - soc.head[box(port)];
}

int socket(int domain, int type, int protocol) {
	assert(domain == AF_INET && type == SOCK_DGRAM);
	return soc.next++;
}
int bind(int fd, const struct sockaddr *address, socklen_t size) {
	const struct sockaddr_in *a = (const struct sockaddr_in *)address;
	assert(a->sin_addr.s_addr == INADDR_ANY);
	soc.port[fd] = ntohs(a->sin_port);
	soc.binds++;
	return 0;
}
int setsockopt(int fd, int level, int name, const void *value, socklen_t size) {
	if (level == IPPROTO_IP && (name == IP_ADD_MEMBERSHIP || name == IP_DROP_MEMBERSHIP)) {
		const struct ip_mreq *m = value;
		assert(m->imr_multiaddr.s_addr == htonl(MDNS_GROUP) && soc.port[fd] == MDNS_PORT);
		if (name == IP_DROP_MEMBERSHIP) {
			soc.drops++;
		} else if (soc.joinFails) {
			errno = ENOBUFS;
			return -1;
		} else {
			soc.joins++;
			soc.joinedOn = m->imr_interface.s_addr;
		}
	}
	int v = size == sizeof(int) ? *(const int *)value : -1;
	soc.multicastTtl = level == IPPROTO_IP && name == IP_MULTICAST_TTL ? v : soc.multicastTtl;
	soc.ttl = level == IPPROTO_IP && name == IP_TTL ? v : soc.ttl;
	soc.loop = level == IPPROTO_IP && name == IP_MULTICAST_LOOP ? v : soc.loop;
	return 0;
}
int close(int fd) {
	soc.port[fd] = 0;
	return 0;
}
ssize_t recvfrom(int fd, void *buffer, size_t cap, int flags, struct sockaddr *from,
				 socklen_t *size) {
	int b = box(soc.port[fd]);
	assert(soc.port[fd] != 0 && cap > DATAGRAM_MAX);
	if (soc.recvErrno) {
		errno = soc.recvErrno;
		return -1;
	}
	if (soc.head[b] == soc.tail[b]) {
		errno = EAGAIN;
		return -1;
	}
	const Queued *q = &soc.inbox[b][soc.head[b]++ % 32];
	memcpy(buffer, q->bytes, q->n);
	memcpy(from, &q->from, sizeof(q->from));
	*size = sizeof(q->from);
	return (ssize_t)q->n;
}
ssize_t sendto(int fd, const void *bytes, size_t n, int flags, const struct sockaddr *at,
			   socklen_t size) {
	assert(soc.port[fd] != 0 && n <= sizeof(soc.sent));
	if (soc.sendErrno) {
		errno = soc.sendErrno;
		soc.sendErrno = 0;
		return -1;
	}
	memcpy(soc.sent, bytes, n);
	soc.sentN = n;
	memcpy(&soc.sentTo, at, sizeof(soc.sentTo));
	soc.sends++;
	return (ssize_t)n;
}
Result SOCU_GetIPInfo(struct in_addr *ip, struct in_addr *netmask, struct in_addr *broadcast) {
	ip->s_addr = soc.ip;
	netmask->s_addr = soc.mask;
	broadcast->s_addr = soc.ip | ~soc.mask;
	return 0;
}
bool authNameTag(char tag[NAME_TAG_HEX + 1]) {
	snprintf(tag, NAME_TAG_HEX + 1, "%s", soc.tag);
	return !soc.noKey;
}
const char *sysinfoModel(void) {
	return "o3ds";
}

#define SENT(want) (soc.sentN == sizeof(want) - 1 && memcmp(soc.sent, want, soc.sentN) == 0)

static bool isOpen(void) {
	return soc.port[soc.next - 2] == MDNS_PORT && soc.port[soc.next - 1] == DEFAULT_PORT;
}

static void queueProbe(void) {
	queue(DEFAULT_PORT, PROBE, PROBE_LEN, at("192.168.2.73", 40000));
}

static const char *counters(void) {
	static char text[256];
	text[0] = '\0';
	discoverFormat(text, sizeof(text));
	return text;
}

// The sockets are bound and joined on INADDR_ANY with TTL 255, and the responder's own sends are
// not looped back. It makes two announcements a second apart, answers a query on a later tick, at
// most 8 per socket per tick, and a drain answers none.
static void life(void) {
	struct sockaddr_in mac = at("192.168.2.73", 5353), group = at("224.0.0.251", 5353);
	soc.ip = inet_addr("192.168.2.76");
	soc.mask = inet_addr("255.255.255.0");
	fake.nowMs = 100000;
	assert(discoverTick()); // the address appeared, which the screen shows
	assert(isOpen() && soc.joins == 1 && soc.joinedOn == INADDR_ANY && soc.multicastTtl == 255 &&
		   soc.ttl == 255 && soc.loop == 0);
	assert(soc.sends == 1 && SENT(A_ANSWER) && samePeer(soc.sentTo, group));
	fake.nowMs += 999;
	assert(!discoverTick() && soc.sends == 1);
	fake.nowMs += 1;
	assert(!discoverTick() && soc.sends == 2 && SENT(A_ANSWER));
	fake.nowMs += 5000; // two, then no more; the address read again, the same
	assert(!discoverTick() && soc.sends == 2 && soc.binds == 2);

	QUEUE(MDNS_PORT, MAC, mac);
	discoverTick();
	assert(soc.sends == 3 && SENT(MAC_ANSWER) && samePeer(soc.sentTo, group));
	for (int i = 0; i < 20; i++) {
		QUEUE(MDNS_PORT, HEAD("\0\0", "\0\0", "\1", "\0") "\4desk\5local\0\0\1\0\1", mac);
	}
	discoverTick();
	assert(waiting(MDNS_PORT) == 12 && soc.sends == 3);
	discoverTick();
	discoverTick();
	assert(waiting(MDNS_PORT) == 0);

	queueProbe();
	discoverTick();
	assert(soc.sends == 4 && SENT("3drop 192.168.2.76 1337 o3ds\n") &&
		   samePeer(soc.sentTo, at("192.168.2.73", 40000)));

	for (int i = 0; i < 5; i++) {
		fake.nowMs += 1000;
		QUEUE(MDNS_PORT, MAC, mac);
		queueProbe();
	}
	discoverDrain();
	assert(waiting(MDNS_PORT) == 0 && waiting(DEFAULT_PORT) == 0 && soc.sends == 4);
	assert(strcmp(counters(), " disc=open name=3drop-1938bccf.local asked=1 answered=1 "
							  "announced=2 probed=1 drained=10 opened=1 unsent=0") == 0);
}

// The address is read every 3 s. A new address makes the responder rejoin, announce and redraw;
// with none, nothing is sent.
static void moves(void) {
	struct sockaddr_in mac = at("192.168.2.73", 5353);
	static const char MOVED[] = HEAD("\0\0", "\x84\0", "\0", "\1") NAME A_RR(AT_80);
	fake.nowMs += 10000;
	discoverTick(); // the address read is due now, then due again in 3 s
	unsigned sends = soc.sends, binds = soc.binds;
	soc.ip = inet_addr("192.168.2.80");
	fake.nowMs += 2999;
	assert(!discoverTick() && soc.binds == binds);
	fake.nowMs += 1;
	assert(discoverTick() && soc.drops >= 1 && soc.binds == binds + 2 && isOpen());
	assert(soc.sends == sends + 1 && SENT(MOVED));
	soc.ip = 0;
	fake.nowMs += 3000;
	sends = soc.sends;
	assert(discoverTick() && isOpen());
	QUEUE(MDNS_PORT, DIG, at("192.168.2.73", 54321));
	fake.nowMs += 1000;
	discoverTick();
	assert(soc.sends == sends && waiting(MDNS_PORT) == 0);
	soc.ip = inet_addr("192.168.2.76");
	fake.nowMs += 2000;
	assert(discoverTick() && soc.sends == sends + 1);
	fake.nowMs += 1000;
	discoverTick(); // the second announcement
	QUEUE(MDNS_PORT, MAC, mac);
	discoverTick();
	assert(soc.sends == sends + 3 && SENT(MAC_ANSWER));
}

// One asker's failed send is counted and the sockets are kept. A failed socket or network closes
// them for 3 s. A failed open (or no key) is logged once per outage. The knob closes them, and a
// new key changes the name.
static void failures(void) {
	struct sockaddr_in mac = at("192.168.2.73", 5353);
	unsigned binds = soc.binds;
	fake.nowMs += 1000;
	soc.sendErrno = EHOSTUNREACH;
	QUEUE(MDNS_PORT, MAC, mac);
	discoverTick();
	assert(isOpen() && soc.binds == binds && strstr(counters(), " unsent=1"));
	fake.nowMs += 1000;
	soc.sendErrno = ENETUNREACH;
	QUEUE(MDNS_PORT, MAC, mac);
	discoverTick();
	assert(!isOpen() && soc.binds == binds);
	fake.nowMs += 2999;
	discoverTick();
	assert(soc.binds == binds);
	fake.nowMs += 1;
	discoverTick();
	assert(isOpen() && soc.binds == binds + 2);

	soc.recvErrno = EAGAIN;
	discoverTick();
	assert(isOpen());
	soc.recvErrno = ENETDOWN;
	discoverTick();
	soc.recvErrno = 0;
	assert(!isOpen());
	fake.nowMs += 3000;
	discoverTick();
	assert(isOpen());

	unsigned logs = fake.logs;
	discoverClose();
	soc.joinFails = true;
	discoverTick();
	assert(!isOpen() && fake.logs == logs + 1 && strcmp(fake.log, "disc-open-failed") == 0 &&
		   fake.logResult == ENOBUFS);
	fake.nowMs += 3000;
	discoverTick();
	assert(!isOpen() && fake.logs == logs + 1);
	soc.joinFails = false;
	fake.nowMs += 3000;
	discoverTick();
	assert(isOpen());
	discoverClose();
	soc.noKey = true;
	discoverTick();
	assert(!isOpen() && fake.logs == logs + 2 && fake.logResult == EINVAL);
	soc.noKey = false;

	gKnobs.noDisc = 1;
	fake.nowMs += 3000;
	discoverTick();
	assert(!isOpen());
	queueProbe();
	discoverTick();
	assert(!isOpen() && waiting(DEFAULT_PORT) == 1);
	gKnobs.noDisc = 0;
	unsigned sends = soc.sends;
	discoverTick();
	assert(isOpen() && waiting(DEFAULT_PORT) == 0 && soc.sends == sends + 2); // announced, answered

	static const char RENAMED[] =
		HEAD("\0\0", "\x84\0", "\0", "\1") "\x0e"
										   "3drop-00c0ffee\5local" A_RR(AT_76);
	soc.tag = "00c0ffee";
	discoverClose();
	discoverTick();
	assert(isOpen() && SENT(RENAMED));
}

int main(void) {
	answers();
	groupWindows();
	refusals();
	probes();
	fuzz();
	life();
	moves();
	failures();
	TEST_PASS();
	return 0;
}

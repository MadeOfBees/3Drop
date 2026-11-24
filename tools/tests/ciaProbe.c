// Tests the CIA parser against built and malformed CIAs. It also writes the CIAs that testCIA.sh
// reads with the host's reader, so that the parser and the reader are held to one layout.
#include "cia.h"
#include "ciaBuild.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void unsignedTmd(TestCIA *c) {
	u64 tmd = c->tmdBody - 0x140, tmdSize = TMD_RECORDS + TMD_RECORD;
	memmove(c->bytes + tmd, c->bytes + c->tmdBody, tmdSize);
	BE32(c->bytes + tmd, 0x00020000);
	LE32(c->bytes + 0x10, (u32)tmdSize);
	c->prefix = tmd + tmdSize;
	c->extent = align64(align64(c->prefix) + 1);
}

static int failures;
static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "testCIA: %s\n", what);
		failures++;
	}
}

// A malformed CIA is refused and the output is left untouched.
static const CiaInfo UNTOUCHED = {.titleID = 1, .contentSize = 2, .version = 3};
static void expectMalformed(const char *what, TestCIA c) {
	CiaInfo info = UNTOUCHED;
	check(!ciaParse(c.bytes, c.prefix, c.extent, &info) && info.titleID == UNTOUCHED.titleID &&
			  info.contentSize == UNTOUCHED.contentSize && info.version == UNTOUCHED.version,
		  what);
	free(c.bytes);
}

static void parserCases(void) {
	const u64 id = 0x0004000003D37200ULL;
	for (u32 t = 0x00010000; t <= 0x00010005; t++) {
		for (u32 m = 0x00010000; m <= 0x00010005; m++) {
			TestCIA c = build(t, m, id, 1, 1);
			CiaInfo l = {0};
			check(ciaParse(c.bytes, c.prefix, c.extent, &l) && l.titleID == id &&
					  l.contentSize == 1 && l.version == TEST_CIA_VERSION,
				  "valid signature variant refused");
			free(c.bytes);
		}
	}
	TestCIA c = build(0x00010003, 0x00010003, id, 64, UINT16_MAX);
	CiaInfo l = {0};
	check(c.prefix == CIA_MAX_PREFIX_SIZE && ciaParse(c.bytes, c.prefix, c.extent, &l),
		  "maximum prefix");
	free(c.bytes);

	c = build(0x00010004, 0x00010004, id, 1, 1); // the optional meta block, at its only size
	LE32(c.bytes + 0x14, 0x3AC0);
	check(ciaParse(c.bytes, c.prefix, c.extent + 0x3AC0, &l), "meta block of 0x3AC0");
	free(c.bytes);

	// Each malformed CIA below is otherwise intact, so only the check its name describes can refuse
	// it.
#define MALFORMED(what, contents, mutation)                                                        \
	do {                                                                                           \
		TestCIA m = build(0x00010004, 0x00010004, id, 1, contents);                                \
		mutation;                                                                                  \
		expectMalformed(what, m);                                                                  \
	} while (0)
	MALFORMED("short header", 1, LE32(m.bytes, 0x20));
	// The CIA is laid out for 3Drop's checks, but the header sends AM to read another ticket and
	// TMD.
	MALFORMED("certificate chain not 0xA00", 1, LE32(m.bytes + 0x08, 0xA40));
	MALFORMED("non-final header type", 1, m.bytes[0x04] = 1);
	MALFORMED("meta block of another size", 1, {
		LE32(m.bytes + 0x14, 0x40);
		m.extent += 0x40;
	});
	MALFORMED("no content bytes", 1, {
		put(m.bytes + m.tmdRecords + 8, 0, 8, true);
		put(m.bytes + 0x18, 0, 8, false);
		m.extent = align64(m.prefix);
	});
	// A content size past the whole CIA, with an extent that wraps around to the CIA's size.
	MALFORMED("content size past the CIA", 1, {
		u64 wraps = UINT64_C(0) - align64(m.prefix) + 64;
		put(m.bytes + m.tmdRecords + 8, wraps, 8, true);
		put(m.bytes + 0x18, wraps, 8, false);
		m.extent = 64;
	});
	// An unknown signature type, with the ticket or TMD laid out as if its signature took no bytes.
	MALFORMED("unknown ticket signature", 1, {
		BE32(m.bytes + CERT_OFFSET + 0xA00, 0x00020000);
		put(m.bytes + CERT_OFFSET + 0xA00 + 0x9C, id, 8, true);
	});
	MALFORMED("unknown TMD signature", 1, unsignedTmd(&m));
	MALFORMED("ticket and TMD title differ", 1, put(m.bytes + m.tmdBody + 0x4C, id + 1, 8, true));
	// A TMD whose size doesn't match its content count: sized for two but counting one...
	MALFORMED("TMD sized for more contents than it counts", 2, {
		put(m.bytes + m.tmdBody + 0x9E, 1, 2, true);
		put(m.bytes + m.tmdRecords + 8, 2, 8, true);
		m.bytes[0x20] = 0x80;
	});
	// ...or counting two, with the second record past the TMD, outside the prefix.
	MALFORMED("TMD counting more contents than it holds", 1, {
		put(m.bytes + m.tmdBody + 0x9E, 2, 2, true);
		put(m.bytes + m.tmdRecords + TMD_RECORD + 4, 1, 2, true);
		m.bytes[0x20] = 0xC0;
	});
	MALFORMED("CIA content index differs", 1, m.bytes[0x20] = 0);
	MALFORMED("duplicate TMD content index", 2, {
		put(m.bytes + m.tmdRecords + TMD_RECORD + 4, 0, 2, true);
		m.bytes[0x20] = 0x80;
	});
	MALFORMED("content sizes whose sum wraps", 2, {
		put(m.bytes + m.tmdRecords + 8, UINT64_MAX, 8, true);
		put(m.bytes + m.tmdRecords + TMD_RECORD + 8, 2, 8, true);
		put(m.bytes + 0x18, 1, 8, false);
	});
	MALFORMED("content sizes that are not the header's", 2, put(m.bytes + 0x18, 3, 8, false));
	// A ticket cut short of its fixed body, with its title id still inside it and the TMD moved up
	// to follow it.
	MALFORMED("ticket shorter than its body", 1, {
		u64 ticket = CERT_OFFSET + 0xA00; /* one declarator a line: the macro splits on commas */
		u64 size = 0x140 + 0xA4;
		u64 tmd = m.tmdBody - 0x140;
		u64 moved = tmd - align64(ticket + size);
		memmove(m.bytes + tmd - moved, m.bytes + tmd, m.prefix - tmd);
		LE32(m.bytes + 0x0C, (u32)size);
		m.tmdBody -= moved;
		m.tmdRecords -= moved;
		m.prefix -= moved;
		m.extent = align64(align64(m.prefix) + 1);
	});
	MALFORMED("wrong full extent", 1, m.extent++);
	MALFORMED("wrong prefix extent", 1, m.prefix--);
	// A prefix 64 bytes longer or shorter than the header lays out, with its extent moved to follow
	// and a second TMD put there that every other check accepts. Only the header's layout refuses
	// it.
	for (int shift = -64; shift <= 64; shift += 128) {
		MALFORMED(shift < 0 ? "prefix 64 bytes short" : "prefix 64 bytes long", 1, {
			u8 *moved = m.bytes + m.tmdBody + shift;
			BE32(m.bytes + m.tmdBody - signatureSize(0x00010004) + shift, 0x00010004);
			put(moved + 0x4C, id, 8, true);
			put(moved + 0x9C, TEST_CIA_VERSION, 2, true);
			put(moved + 0x9E, 1, 2, true);
			put(moved + TMD_RECORDS + 4, 0, 2, true);
			put(moved + TMD_RECORDS + 8, 1, 8, true);
			m.prefix += shift;
			m.extent += shift;
		});
	}
}

// The prefix a download reads from the first CIA_HEADER_PEEK bytes (the host's ciaLayout, in C).
// For every layout the parser accepts it is the size ciaParse then insists on, and for a header the
// parser would refuse there is no size.
static void prefixCases(void) {
	const u64 id = 0x0004000003D37200ULL;
	for (u32 t = 0x00010000; t <= 0x00010005; t++) {
		for (u32 m = 0x00010000; m <= 0x00010005; m++) {
			TestCIA c = build(t, m, id, 3, 2);
			u64 prefix = 0;
			check(ciaPrefixSize(c.bytes, CIA_HEADER_PEEK, c.extent, &prefix) && prefix == c.prefix,
				  "prefix size of a valid CIA");
			free(c.bytes);
		}
	}
	TestCIA c = build(0x00010003, 0x00010003, id, 64, UINT16_MAX);
	u64 prefix = 0;
	check(ciaPrefixSize(c.bytes, CIA_HEADER_PEEK, c.extent, &prefix) &&
			  prefix == CIA_MAX_PREFIX_SIZE,
		  "prefix size at the maximum");
	// More bytes than the peek make no difference.
	check(ciaPrefixSize(c.bytes, c.prefix, c.extent, &prefix) && prefix == CIA_MAX_PREFIX_SIZE,
		  "prefix size with the whole prefix in hand");
	// One short of the header's sizes, and past them: refused, *out untouched.
	prefix = 77;
	check(!ciaPrefixSize(c.bytes, CIA_HEADER_PEEK - 1, c.extent, &prefix) && prefix == 77,
		  "prefix size from too few bytes");
	free(c.bytes);

#define PEEK_REFUSED(what, mutation)                                                               \
	do {                                                                                           \
		TestCIA r = build(0x00010004, 0x00010004, id, 1, 1);                                       \
		u64 out = 77;                                                                              \
		mutation;                                                                                  \
		check(!ciaPrefixSize(r.bytes, CIA_HEADER_PEEK, r.extent, &out) && out == 77, what);        \
		free(r.bytes);                                                                             \
	} while (0)
	PEEK_REFUSED("prefix: header size not 0x2020", LE32(r.bytes, 0x2000));
	PEEK_REFUSED("prefix: certificate chain not 0xA00", LE32(r.bytes + 0x08, 0xA40));
	PEEK_REFUSED("prefix: no ticket", LE32(r.bytes + 0x0C, 0));
	PEEK_REFUSED("prefix: no TMD", LE32(r.bytes + 0x10, 0));
	// A ticket whose size alone would put the prefix past the largest one a console accepts (the
	// arithmetic must not wrap, because the sizes are 32 bits wide).
	PEEK_REFUSED("prefix past the largest", LE32(r.bytes + 0x0C, UINT32_MAX));
	PEEK_REFUSED("prefix past the largest (TMD)", LE32(r.bytes + 0x10, UINT32_MAX));
	// A prefix past the largest, in a CIA big enough to hold it, so only the limit refuses it.
	{
		TestCIA r = build(0x00010004, 0x00010004, id, 1, 1);
		u64 out = 77;
		LE32(r.bytes + 0x0C, 0x306000); // a ticket as large as the largest prefix, and more
		check(!ciaPrefixSize(r.bytes, CIA_HEADER_PEEK, UINT64_C(1) << 40, &out) && out == 77,
			  "prefix: past the largest, in a larger CIA");
		LE32(r.bytes + 0x0C, 0x1000);
		check(ciaPrefixSize(r.bytes, CIA_HEADER_PEEK, UINT64_C(1) << 40, &out) && out != 77,
			  "prefix: a large ticket within the limit");
		free(r.bytes);
	}
	// A prefix that is the whole CIA leaves no content to install.
	{
		TestCIA r = build(0x00010004, 0x00010004, id, 1, 1);
		u64 out = 77;
		check(!ciaPrefixSize(r.bytes, CIA_HEADER_PEEK, r.prefix, &out) && out == 77,
			  "prefix: as large as the CIA");
		check(ciaPrefixSize(r.bytes, CIA_HEADER_PEEK, r.prefix + 1, &out) && out == r.prefix,
			  "prefix: one byte short of the CIA");
		free(r.bytes);
	}
}

// Layouts other than makerom's for an application, for testCIA.sh to read with the host's reader.
// They cover a ticket with more content-index groups (as a DLC's ticket can have), each signature
// size, and a meta block.
static int writeVectors(const char *dir) {
	static const struct {
		const char *name;
		u32 ticketSig, tmdSig, groups;
		u16 contents;
		bool meta;
		u64 id;
	} VECTORS[] = {
		{"app", 0x00010004, 0x00010004, 1, 1, true, 0x0004000001234500ULL}, // makerom's layout
		{"dlc", 0x00010004, 0x00010004, 9, 3, false, 0x0004008C01234502ULL},
		{"rsa4096", 0x00010000, 0x00010003, 2, 2, false, 0x0004000E01234500ULL},
		{"ecdsa", 0x00010005, 0x00010002, 1, 1, true, 0x0004000001234600ULL},
	};
	for (size_t i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++) {
		TestCIA c = build(VECTORS[i].ticketSig, VECTORS[i].tmdSig, VECTORS[i].id, VECTORS[i].groups,
						  VECTORS[i].contents);
		if (VECTORS[i].meta) {
			LE32(c.bytes + 0x14, 0x3AC0);
			c.extent += 0x3AC0;
		}
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.cia", dir, VECTORS[i].name);
		FILE *out = fopen(path, "wb");
		bool written = out && fwrite(c.bytes, 1, c.extent, out) == c.extent;
		written = out && fclose(out) == 0 && written;
		free(c.bytes);
		if (!written) {
			fprintf(stderr, "testCIA: cannot write %s\n", path);
			return 1;
		}
	}
	return 0;
}

// The title and version the parser reads from a CIA file, given the prefix size the host sends.
static int parsed(const char *path, const char *prefixText) {
	u64 prefix = strtoull(prefixText, NULL, 10);
	FILE *in = fopen(path, "rb");
	long size = in && fseek(in, 0, SEEK_END) == 0 ? ftell(in) : -1;
	uint8_t *bytes = prefix <= CIA_MAX_PREFIX_SIZE ? malloc((size_t)prefix + 1) : NULL;
	CiaInfo l = {0};
	bool ok = bytes && size > 0 && fseek(in, 0, SEEK_SET) == 0 &&
			  fread(bytes, 1, (size_t)prefix, in) == prefix &&
			  ciaParse(bytes, prefix, (u64)size, &l);
	if (in) {
		fclose(in);
	}
	free(bytes);
	if (!ok) {
		fprintf(stderr, "testCIA: the parser refused %s with a prefix of %s\n", path, prefixText);
		return 1;
	}
	printf("%016llx %u\n", (unsigned long long)l.titleID, l.version);
	return 0;
}

// ciaProbe [dir]: runs the parser's cases, then writes the vectors into dir.
// ciaProbe <cia> <prefix-size>: prints what the parser reads from a CIA.
int main(int argc, char **argv) {
	if (argc == 3) {
		return parsed(argv[1], argv[2]);
	}
	parserCases();
	prefixCases();
	if (failures) {
		return 1;
	}
	return argc == 2 ? writeVectors(argv[1]) : 0;
}

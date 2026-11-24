// A CIA built in memory for the host tests, in 3dbrew's layout.
#ifndef TEST_CIA_BUILD_H
#define TEST_CIA_BUILD_H

#include <3ds.h>
#include <stdbool.h>
#include <stdlib.h>

#define CIA_BUFFER_BYTES 0x400000U
#define TEST_CIA_VERSION 7 // the title version build() writes into every TMD
#define CERT_OFFSET 0x2040U
#define TICKET_FIXED 0x164U
#define TMD_RECORDS 0x9C4U
#define TMD_RECORD 0x30U

static void put(uint8_t *at, u64 value, int bytes, bool bigEndian) {
	for (int i = 0; i < bytes; i++) {
		at[bigEndian ? bytes - 1 - i : i] = (uint8_t)(value >> (8 * i));
	}
}
#define BE32(at, v) put((at), (v), 4, true)
#define LE32(at, v) put((at), (v), 4, false)

static u64 align64(u64 value) {
	return (value + 0x3F) & ~UINT64_C(0x3F);
}

// The type mod 3 selects RSA-4096, RSA-2048 or ECDSA. This repeats cia.c's table.
static u64 signatureSize(u32 type) {
	return (type & 0xFFFF) % 3 == 0 ? 0x240 : (type & 0xFFFF) % 3 == 1 ? 0x140 : 0x80;
}

typedef struct {
	uint8_t *bytes;
	u64 prefix, extent, tmdBody, tmdRecords;
} TestCIA;

// Builds a CIA of title `id` with `contents` contents of one byte each and a ticket sized for
// `groups` index groups.
static TestCIA build(u32 ticketSig, u32 tmdSig, u64 id, u32 groups, u16 contents) {
	TestCIA c = {.bytes = calloc(CIA_BUFFER_BYTES, 1)};
	u8 *b = c.bytes;
	u64 ticket = CERT_OFFSET + 0xA00;
	u64 ticketSize = signatureSize(ticketSig) + TICKET_FIXED + 0x28 + groups * 0x84;
	BE32(b + ticket, ticketSig);
	put(b + ticket + signatureSize(ticketSig) + 0x9C, id, 8, true);
	u64 tmd = align64(ticket + ticketSize);
	u64 tmdSize = signatureSize(tmdSig) + TMD_RECORDS + contents * TMD_RECORD;
	BE32(b + tmd, tmdSig);
	c.tmdBody = tmd + signatureSize(tmdSig);
	put(b + c.tmdBody + 0x4C, id, 8, true);
	put(b + c.tmdBody + 0x9C, TEST_CIA_VERSION, 2, true);
	put(b + c.tmdBody + 0x9E, contents, 2, true);
	c.tmdRecords = c.tmdBody + TMD_RECORDS;
	for (u32 i = 0; i < contents; i++) {
		put(b + c.tmdRecords + i * TMD_RECORD + 4, i, 2, true);
		put(b + c.tmdRecords + i * TMD_RECORD + 8, 1, 8, true);
		b[0x20 + (i >> 3)] |= (uint8_t)(0x80U >> (i & 7));
	}
	c.prefix = tmd + tmdSize;
	c.extent = align64(align64(c.prefix) + contents);
	LE32(b, 0x2020);
	LE32(b + 0x08, 0xA00);
	LE32(b + 0x0C, (u32)ticketSize);
	LE32(b + 0x10, (u32)tmdSize);
	put(b + 0x18, contents, 8, false);
	return c;
}

#endif // TEST_CIA_BUILD_H

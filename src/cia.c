// SPDX-License-Identifier: GPL-3.0-or-later
#include "cia.h"

#include "codec.h" // readBE, readLE

#include <string.h>

// 3dbrew "CIA": header, certificate chain, ticket, TMD, contents and meta, each 64-byte aligned.
#define HEADER_SIZE 0x2020u
#define BITMAP_OFFSET 0x20u
#define BITMAP_SIZE 0x2000u
#define CERT_CHAIN_SIZE 0xA00u
#define TICKET_OFFSET (ciaAlign(HEADER_SIZE) + CERT_CHAIN_SIZE)
#define META_SIZE 0x3AC0u
#define TICKET_BODY_SIZE 0x164u
#define TICKET_TITLE_ID 0x9Cu // offsets within the ticket and TMD bodies
#define TMD_TITLE_ID 0x4Cu
#define TMD_VERSION 0x9Cu
#define TMD_CONTENT_COUNT 0x9Eu
#define TMD_RECORDS 0x9C4u
#define TMD_RECORD_SIZE 0x30u

// Returns the signature size for RSA-4096, RSA-2048 and ECDSA, each with SHA-1 and SHA-256,
// and 0 for any other type.
static u64 signatureSize(u64 type) {
	static const u64 SIZES[] = {0x240, 0x140, 0x80};
	return type >= 0x10000 && type <= 0x10005 ? SIZES[(type - 0x10000) % 3] : 0;
}

bool ciaPrefixSize(const u8 *header, u64 have, u64 ciaSize, u64 *out) {
	if (have < CIA_HEADER_PEEK || readLE(header, 4) != HEADER_SIZE ||
		readLE(header + 8, 4) != CERT_CHAIN_SIZE) {
		return false;
	}
	u64 ticketSize = readLE(header + 0x0C, 4), tmdSize = readLE(header + 0x10, 4);
	if (ticketSize < 4 || tmdSize < 4) {
		return false;
	}
	// ciaParse uses this layout too, since it calls this function.
	// Both sizes are 32 bits, so nothing wraps.
	u64 prefix = ciaAlign(TICKET_OFFSET + ticketSize) + tmdSize;
	if (prefix > CIA_MAX_PREFIX_SIZE || prefix >= ciaSize) {
		return false;
	}
	*out = prefix;
	return true;
}

bool ciaParse(const u8 *prefix, u64 prefixSize, u64 ciaSize, CiaInfo *out) {
	// static keeps it off the small stack, and there is no allocation that could fail.
	static u8 seen[BITMAP_SIZE];
	u64 layout;
	if (!ciaPrefixSize(prefix, prefixSize, ciaSize, &layout) || layout != prefixSize) {
		return false;
	}
	u64 ticketSize = readLE(prefix + 0x0C, 4), tmdSize = readLE(prefix + 0x10, 4);
	u64 metaSize = readLE(prefix + 0x14, 4), contentSize = readLE(prefix + 0x18, 8);
	u64 tmd = prefixSize - tmdSize;
	// The sizes are all u32 except contentSize, which cannot pass ciaSize, so no sum below wraps.
	if (readLE(prefix + 4, 4) != 0 || (metaSize != 0 && metaSize != META_SIZE) ||
		contentSize == 0 || contentSize > ciaSize ||
		ciaSize != ciaAlign(ciaAlign(prefixSize) + contentSize) + metaSize) {
		return false;
	}
	const u8 *tk = prefix + TICKET_OFFSET, *tm = prefix + tmd;
	u64 tkSig = signatureSize(readBE(tk, 4)), tmSig = signatureSize(readBE(tm, 4));
	if (tkSig == 0 || tmSig == 0 || ticketSize < tkSig + TICKET_BODY_SIZE ||
		tmdSize < tmSig + TMD_RECORDS) {
		return false;
	}
	tm += tmSig;
	u64 titleID = readBE(tm + TMD_TITLE_ID, 8), count = readBE(tm + TMD_CONTENT_COUNT, 2), sum = 0;
	if (count == 0 || tmdSize != tmSig + TMD_RECORDS + count * TMD_RECORD_SIZE ||
		readBE(tk + tkSig + TICKET_TITLE_ID, 8) != titleID) {
		return false;
	}
	memset(seen, 0, sizeof(seen));
	for (const u8 *record = tm + TMD_RECORDS; count-- > 0; record += TMD_RECORD_SIZE) {
		u64 index = readBE(record + 4, 2);
		u8 bit = (u8)(0x80u >> (index & 7));
		if ((seen[index >> 3] & bit) != 0 ||
			__builtin_add_overflow(sum, readBE(record + 8, 8), &sum)) {
			return false;
		}
		seen[index >> 3] |= bit;
	}
	if (sum != contentSize || memcmp(seen, prefix + BITMAP_OFFSET, BITMAP_SIZE) != 0) {
		return false;
	}
	*out = (CiaInfo){titleID, contentSize, (u16)readBE(tm + TMD_VERSION, 2)};
	return true;
}

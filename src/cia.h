// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CIA_H
#define CIA_H

#include <3ds.h>
#include <stdbool.h>

// The largest prefix: a ticket with 64 groups plus a TMD with 65535 contents. A prefix must fit in
// the ring's head.
#define CIA_MAX_PREFIX_SIZE 0x305B14ULL

typedef struct {
	u64 titleID;
	u64 contentSize; // what AM writes to the card
	u16 version;
} CiaInfo;

// The ticket must name the TMD's title, because AM commits the ticket first. The declared size must
// be the exact extent of the CIA, so that no byte past it is read.
bool ciaParse(const u8 *prefix, u64 prefixSize, u64 ciaSize, CiaInfo *out);
// The header's own fields through the TMD's size are all a download needs to know where the prefix
// ends before it has the prefix.
#define CIA_HEADER_PEEK 0x14u
// Works out ciaParse's prefix size from the first CIA_HEADER_PEEK bytes. It is the `ciaPrefix` that
// tools/cia.sh ciaLayout sets, the one the host sends. Returns false for a header whose sizes
// ciaParse would refuse, a prefix larger than CIA_MAX_PREFIX_SIZE, or a prefix that leaves no
// content. *out is written only when it returns true.
bool ciaPrefixSize(const u8 *header, u64 have, u64 ciaSize, u64 *out);
// Each CIA section starts on a 64-byte boundary.
static inline u64 ciaAlign(u64 v) {
	return (v + 63) & ~(u64)63;
}

#endif // CIA_H

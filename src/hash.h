// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HASH_H
#define HASH_H

#include <3ds.h>
#include <mbedtls/sha256.h>
#include <stdbool.h>

// The wire's digest is the SHA-256 of the SHA-256s of a body's DIGEST_UNIT units in order. The last
// unit may be short, and an empty body has no units (host twin: tools/wire.sh unitDigest). The
// engine is one-shot, and a second call starts over, so a body is hashed one unit per call and then
// the units' digests once. Only one body can be hashed at a time. A verb that digests refuses
// `stranded` first, because a stranded worker may still be in the engine.
#define DIGEST_UNIT (1u << 20)
// An input under this many bytes is hashed in software, without a call to the engine.
#define ENGINE_FLOOR 8192

// One SHA-256 of linear memory, in a single call. It uses the engine, or software for an input
// under ENGINE_FLOOR or past DIGEST_UNIT (the most the engine has been measured on).
Result sha256Of(const void *p, u32 n, u8 out[32]);
// Reserves the digest's memory right after tier 1 (ringTier1), so neither displaces the other.
// Returns false when there is no room.
bool digestReserve(void);
void digestStart(void);
// Starts from a unit boundary, with whole units and then at most one short unit, the body's last.
// The first failure sticks, and nothing after it is hashed.
Result digestUnits(const void *body, u32 n);
Result digestFinish(u8 out[32]);
Result digestOf(const void *body, u32 n, u8 out[32]); // for a body held whole
bool digestEqual(const u8 a[32], const u8 b[32]);	  // constant time

// One file's digest, carried across bodies as a put's ranges are. It is the SHA-256 of the file's
// unit digests so far, computed in software at 32 bytes a unit, so the unit list need not outlive a
// request. It is plain data, so a copy is a clone.
typedef mbedtls_sha256_context DigestChain;
void digestChainStart(DigestChain *chain);
// Adds the units digestUnits took since digestStart, after those the chain already holds.
void digestChainAdd(DigestChain *chain);
// Returns the wire's digest of every unit added so far. It works from a copy, so the chain goes on.
void digestChainPeek(DigestChain chain, u8 out[32]);

#endif // HASH_H

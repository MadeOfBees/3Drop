// SPDX-License-Identifier: GPL-3.0-or-later
#include "hash.h"

#include "result.h"

#include <mbedtls/constant_time.h>
#include <sys/param.h> // MIN

#define DIGEST_UNITS_MAX 4096 // units for a put's largest body, which is UINT32_MAX bytes

static struct {
	u8 (*list)[32]; // in linear memory, because the engine is measured on linear memory only
	u32 units;
	bool ended; // a short unit came, so the body is over
	Result failure;
} digest;

// The buffer is flushed first, because an unflushed buffer hashes stale memory, silently.
Result sha256Of(const void *p, u32 n, u8 out[32]) {
	// Below ENGINE_FLOOR the engine's fixed cost of ~0.4 ms loses to software.
	// Past a unit the engine is unmeasured.
	if (n < ENGINE_FLOOR || n > DIGEST_UNIT) {
		return mbedtls_sha256_ret(p, n, out, 0);
	}
	Result r = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)p, n);
	return R_FAILED(r) ? r : FSUSER_UpdateSha256Context(p, n, out);
}

bool digestReserve(void) {
	if (!digest.list) {
		digest.list = linearMemAlign(DIGEST_UNITS_MAX * sizeof(*digest.list), 0x1000);
	}
	return digest.list != NULL;
}

void digestStart(void) {
	digest.units = 0;
	digest.ended = false;
	digest.failure = digestReserve() ? 0 : OUT_OF_MEMORY;
}

Result digestUnits(const void *body, u32 n) {
	const u8 *p = body;
	while (n > 0 && R_SUCCEEDED(digest.failure)) {
		u32 size = MIN(n, DIGEST_UNIT);
		if (digest.ended) {
			digest.failure = DIGEST_PAST_END;
		} else if (digest.units == DIGEST_UNITS_MAX) {
			digest.failure = OUT_OF_MEMORY;
		} else {
			digest.failure = sha256Of(p, size, digest.list[digest.units++]);
		}
		digest.ended = size < DIGEST_UNIT;
		p += size;
		n -= size;
	}
	return digest.failure;
}

Result digestFinish(u8 out[32]) {
	return R_FAILED(digest.failure)
			   ? digest.failure
			   : sha256Of(digest.list, digest.units * sizeof(*digest.list), out);
}

Result digestOf(const void *body, u32 n, u8 out[32]) {
	digestStart();
	digestUnits(body, n);
	return digestFinish(out);
}

bool digestEqual(const u8 a[32], const u8 b[32]) {
	return mbedtls_ct_memcmp(a, b, 32) == 0;
}

void digestChainStart(DigestChain *chain) {
	mbedtls_sha256_init(chain);
	mbedtls_sha256_starts_ret(chain, 0);
}

void digestChainAdd(DigestChain *chain) {
	mbedtls_sha256_update_ret(chain, (const u8 *)digest.list, digest.units * sizeof(*digest.list));
}

void digestChainPeek(DigestChain chain, u8 out[32]) {
	mbedtls_sha256_finish_ret(&chain, out);
}

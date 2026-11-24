// SPDX-License-Identifier: GPL-3.0-or-later
#include "auth.h"

#include "app.h" // uptimeMs
#include "codec.h"
#include "path.h"

#include <errno.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Short enough to read off the screen (~60 bits), so both ends stretch it with PBKDF2.
#define KEY_GEN_LEN 12
// PBKDF2_ITERS and PBKDF2_SALT have twins in tools/wire.sh. A fixed salt is fine, because a 2^60
// keyspace is too large to precompute against.
#define PBKDF2_ITERS 10000
static const u8 PBKDF2_SALT[8] = {0x33, 0x44, 0x7a, 0x6b, 0x33, 0x44, 0xff, 0xee};

static mbedtls_md_context_t hmac; // main thread only
// The size of PBKDF2's 32 bytes as hex text and a NUL. tools/wire.sh wireKey uses the same text
// as its key.
#define MAC_KEY_TEXT (2 * 32 + 1)
static char macKey[MAC_KEY_TEXT];
static char displayKey[KEY_MAX + 1];

Result authInit(void) {
	mbedtls_md_init(&hmac);
	Result r = psInit();
	if (R_SUCCEEDED(r) &&
		mbedtls_md_setup(&hmac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), 1) != 0) {
		r = OUT_OF_MEMORY; // its only failure other than bad arguments
	}
	return r;
}

void authExit(void) {
	mbedtls_md_free(&hmac);
	psExit();
}

bool authKeyLoaded(void) {
	return macKey[0] != '\0';
}

const char *authDisplayKey(void) {
	return authKeyLoaded() ? displayKey : NULL;
}

static void trimLine(char *s) {
	s[strcspn(s, "\r\n")] = '\0';
}

// FAT has no atomic replace, so a failure keeps the old file and a cut write leaves it aside,
// never a partial file.
_Static_assert(sizeof(KEY_PATH) <= sizeof(KEY_CACHE_PATH), "writeWhole's temp fits the longest");
static bool writeWhole(const char *path, const char *text) {
	char temp[sizeof(KEY_CACHE_PATH) + sizeof(".tmp")],
		aside[sizeof(KEY_CACHE_PATH) + sizeof(".old")];
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	snprintf(aside, sizeof(aside), "%s.old", path);
	FILE *file = fopen(temp, "w");
	if (!file) {
		return false;
	}
	bool written = fputs(text, file) >= 0;
	if (fclose(file) == 0 && written) {
		remove(aside);
		bool moved = rename(path, aside) == 0; // there is no file to move on a first boot
		if (rename(temp, path) == 0) {
			remove(aside);
			return true;
		}
		if (moved) {
			rename(aside, path);
		}
	}
	remove(temp);
	return false;
}

// Only a well-formed entry for this key and count is a hit, because a garbled one would become
// the HMAC key, and every request would fail while the screen shows the right key.
static bool loadCachedKey(const char *key, char out[MAC_KEY_TEXT]) {
	// Room past the hex, so a longer line fails hexDecode rather than being cut to fit.
	char cachedKey[KEY_MAX + 2] = "", iters[16] = "", derived[MAC_KEY_TEXT + 5] = "";
	FILE *file = fopen(KEY_CACHE_PATH, "r");
	if (!file) {
		return false;
	}
	bool got = fgets(cachedKey, sizeof(cachedKey), file) && fgets(iters, sizeof(iters), file) &&
			   fgets(derived, sizeof(derived), file);
	fclose(file);
	trimLine(cachedKey);
	trimLine(iters);
	trimLine(derived);
	u8 bytes[32];
	if (!got || strcmp(cachedKey, key) != 0 || strtoul(iters, NULL, 10) != PBKDF2_ITERS ||
		!hexDecode(bytes, derived, sizeof(bytes))) {
		return false;
	}
	memcpy(out, derived, MAC_KEY_TEXT);
	return true;
}

typedef enum { KEY_OK, KEY_MISSING, KEY_ERROR } KeyState;

// Returns MISSING only when the file does not exist, because an ERROR must never lead to a new
// key over this one.
static KeyState loadKey(void) {
	FILE *file = fopen(KEY_PATH, "r");
	if (!file) {
		return errno == ENOENT ? KEY_MISSING : KEY_ERROR;
	}
	char line[KEY_MAX + 2] = "";
	bool got = fgets(line, sizeof(line), file) != NULL;
	fclose(file);
	trimLine(line);
	if (!got || strlen(line) > KEY_MAX) {
		return KEY_ERROR;
	}
	// Dashes are cosmetic, as on the host (deriveDK).
	char stripped[KEY_MAX + 1];
	size_t n = 0;
	for (const char *p = line; *p; p++) {
		if (*p != '-') {
			stripped[n++] = *p;
		}
	}
	if (n == 0) {
		return KEY_ERROR;
	}
	char derived[MAC_KEY_TEXT];
	if (!loadCachedKey(line, derived)) {
		u8 dk[32];
		if (mbedtls_pkcs5_pbkdf2_hmac(&hmac, (const u8 *)stripped, n, PBKDF2_SALT,
									  sizeof(PBKDF2_SALT), PBKDF2_ITERS, sizeof(dk), dk) != 0) {
			return KEY_ERROR;
		}
		hexEncode(derived, dk, sizeof(dk));
		char cache[KEY_MAX + 96];
		snprintf(cache, sizeof(cache), "%s\n%d\n%s\n", line, PBKDF2_ITERS, derived);
		// Best-effort: without it the next boot derives again.
		(void)writeWhole(KEY_CACHE_PATH, cache);
	}
	memcpy(macKey, derived, sizeof(macKey));
	snprintf(displayKey, sizeof(displayKey), "%s", line);
	return KEY_OK;
}

// 32 symbols, one per 5 random bits, so there is no modulo bias. The alphabet has no i, l or o,
// which are misread as 1 and 0, and no u, which leaves 32.
static bool mintKey(void) {
	static const char ALPHABET[] = "0123456789abcdefghjkmnpqrstvwxyz";
	u8 raw[KEY_GEN_LEN];
	if (R_FAILED(PS_GenerateRandomBytes(raw, sizeof(raw)))) {
		return false;
	}
	char key[KEY_GEN_LEN + 4]; // KEY_GEN_LEN chars, 2 dashes, a newline and a NUL
	int p = 0;
	for (int i = 0; i < KEY_GEN_LEN; i++) {
		if (i == 4 || i == 8) {
			key[p++] = '-';
		}
		key[p++] = ALPHABET[raw[i] & 0x1F];
	}
	key[p++] = '\n';
	key[p] = '\0';
	return writeWhole(KEY_PATH, key);
}

bool authLoadKey(void) {
	KeyState state = loadKey();
	// A write cut between its renames left the old key aside, so that key is restored before any
	// new one is made.
	if (state == KEY_MISSING && rename(KEY_PATH ".old", KEY_PATH) == 0) {
		state = loadKey();
	}
	if (state == KEY_MISSING && mintKey()) {
		state = loadKey();
	}
	return state == KEY_OK;
}

// A new key that will not load gives way to the one in use, which a restart then loads too.
bool authRegenerateKey(void) {
	char old[KEY_MAX + 2];
	bool had = authKeyLoaded();
	snprintf(old, sizeof(old), "%s\n", had ? displayKey : "");
	if (!mintKey()) {
		return false;
	}
	if (loadKey() == KEY_OK) {
		return true;
	}
	if (had) {
		(void)writeWhole(KEY_PATH, old);
	}
	return false;
}

// Joined with '\n', which the grammar forbids inside a field, so the concatenation is unambiguous.
static bool macBegin(const char *domain, const char *request) {
	return authKeyLoaded() &&
		   mbedtls_md_hmac_starts(&hmac, (const u8 *)macKey, strlen(macKey)) == 0 &&
		   mbedtls_md_hmac_update(&hmac, (const u8 *)domain, strlen(domain)) == 0 &&
		   mbedtls_md_hmac_update(&hmac, (const u8 *)request, strlen(request)) == 0;
}

static bool macAppend(const char *field) {
	return mbedtls_md_hmac_update(&hmac, (const u8 *)"\n", 1) == 0 &&
		   mbedtls_md_hmac_update(&hmac, (const u8 *)field, strlen(field)) == 0;
}

static bool macFinish(uint8_t out[MAC_SIZE], bool ok) {
	if (!ok || mbedtls_md_hmac_finish(&hmac, out) != 0) {
		memset(out, 0, MAC_SIZE);
		return false;
	}
	return true;
}

bool macRequest(uint8_t out[MAC_SIZE], const char *request) {
	return macFinish(out, macBegin("3drop/request\n", request));
}

void macResponse(uint8_t out[MAC_SIZE], const char *request, const char *response) {
	macFinish(out, macBegin("3drop/response\n", request) && macAppend(response));
}

void macTrailer(uint8_t out[MAC_SIZE], const char *request, const char *response,
				const char *trailer) {
	macFinish(out,
			  macBegin("3drop/trailer\n", request) && macAppend(response) && macAppend(trailer));
}

bool authNameTag(char out[NAME_TAG_HEX + 1]) {
	uint8_t mac[MAC_SIZE];
	if (!macFinish(mac, macBegin("3drop/name", ""))) {
		return false;
	}
	hexEncode(out, mac, NAME_TAG_HEX / 2);
	return true;
}

// ---- The nonce ------------------------------------------------------------------------------

#define NONCE_TTL_MS 300000

static struct {
	char hex[NONCE_HEX + 1]; // "" when none is live
	u64 issuedMs;
} nonce;

const char *nonceLive(void) {
	u64 now = uptimeMs();
	if (nonce.hex[0] == '\0' || now - nonce.issuedMs >= NONCE_TTL_MS / 2) {
		u8 raw[NONCE_HEX / 2];
		if (R_FAILED(PS_GenerateRandomBytes(raw, sizeof(raw)))) {
			nonce.hex[0] = '\0';
			return NULL;
		}
		hexEncode(nonce.hex, raw, sizeof(raw));
		nonce.issuedMs = now;
	}
	return nonce.hex;
}

bool nonceIs(const char *hex) {
	return nonce.hex[0] != '\0' && uptimeMs() - nonce.issuedMs < NONCE_TTL_MS &&
		   strcmp(hex, nonce.hex) == 0;
}

bool nonceSpend(const char *hex) {
	if (!nonceIs(hex)) {
		return false;
	}
	nonce.hex[0] = '\0';
	return true;
}

void nonceExpireBefore(u64 ms) {
	if (nonce.issuedMs <= ms) {
		nonce.hex[0] = '\0';
	}
}

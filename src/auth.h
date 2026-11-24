// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef AUTH_H
#define AUTH_H

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

// The key is shared, so stock `openssl` on the host computes the same MACs. The key itself is never
// sent.
#define MAC_SIZE 64 // HMAC-SHA512 output
#define MAC_HEX (MAC_SIZE * 2)
#define KEY_MAX 128 // the longest key key.txt may hold

Result authInit(void);
void authExit(void);

// Mints a key only when key.txt does not exist, because a passing read fault must never replace it.
bool authLoadKey(void);
bool authRegenerateKey(void);
bool authKeyLoaded(void);
const char *authDisplayKey(void);

// Each has its own domain, and a reply binds its exact request, so no MAC passes for another
// position or operation. With no key, or on any failure, the MAC is zeroed, so they fail closed.
bool macRequest(uint8_t out[MAC_SIZE], const char *request);
void macResponse(uint8_t out[MAC_SIZE], const char *request, const char *response);
// Binds the header already sent and the body's digest, so a trailer cannot be spliced onto another.
void macTrailer(uint8_t out[MAC_SIZE], const char *request, const char *response,
				const char *trailer);

// The <tag> in the console's name, 3drop-<tag>.local, is this many hex digits of a MAC of a label
// that no exchange signs. The host derives the same tag from its key (tools/wire.sh _wireName). It
// carries none of the key, though a guess at the key can be tested against it offline. Returns
// false with no key.
#define NAME_TAG_HEX 8
bool authNameTag(char out[NAME_TAG_HEX + 1]);

// A nonce is public, because every signed reply carries it. It needs to be single-use, not secret.
#define NONCE_HEX 64
// Re-minted once it is past half its life, so a reply's nonce has half its life left. Returns NULL
// if the RNG fails.
const char *nonceLive(void);
bool nonceIs(const char *hex); // nonceSpend's test, without spending the nonce
bool nonceSpend(const char *hex);
// A nonce handed out before ms is dropped, so a change signed with it is refused without running
// (`challenge`).
void nonceExpireBefore(u64 ms);

#endif // AUTH_H

// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CODEC_H
#define CODEC_H

// A hex field has exactly one spelling, lowercase and at its exact length, so host and device
// cannot disagree about case, padding or where it ends. A decimal field doesn't need one spelling,
// because it takes leading zeros.

#include <3ds.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

bool hexDecode(u8 *out, const char *hex, size_t n);
void hexEncode(char *out, const u8 *bytes, size_t n); // out holds 2n + 1
bool parseU64(const char *text, u64 *out);
u64 readBE(const u8 *p, size_t n); // the most significant byte first
u64 readLE(const u8 *p, size_t n); // the least significant byte first
bool parseTid(const char *text, u64 *out);
// A digest (hash.h) is `u:` followed by 64 lowercase hex digits, and the prefix tells it from a
// plain SHA-256.
#define DIGEST_TEXT_SIZE 67
bool parseDigest(const char *text, u8 out[32]);
void digestText(char out[DIGEST_TEXT_SIZE], const u8 digest[32]);
// Returns false when it did not fit; dst is then truncated, never left unterminated.
bool appendf(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool vappendf(char *dst, size_t cap, const char *fmt, va_list args)
	__attribute__((format(printf, 3, 0)));

#endif // CODEC_H

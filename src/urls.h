// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef URLS_H
#define URLS_H

#include "result.h"

#include <stdbool.h>
#include <stddef.h>

// Reads the text of a scanned QR code as the URLs to fetch, the way FBI reads a payload: one per
// line, with `https://` before any URL that has no scheme. It refuses some things FBI lets
// through: a scheme other than http and https, a character no URL holds, a URL it would have to
// cut, and more URLs than it keeps. 1024 is FBI's DOWNLOAD_URL_MAX. A signed storage link runs to
// several hundred bytes.
#define URL_MAX 1024
#define URLS_MAX 16

typedef struct {
	size_t count;
	// Each URL is whole and NUL-ended. The array is 16 KiB, so it never goes on the stack.
	char url[URLS_MAX][URL_MAX];
} UrlList;

typedef enum { URLS_OK, URLS_EMPTY, URLS_BAD, URLS_TOO_MANY, URLS_TOO_LONG } UrlsVerdict;

// Reads `length` bytes. A trailing NUL, which some generators add, isn't part of the text. *out
// has count 0 unless the verdict is URLS_OK.
UrlsVerdict urlsParse(const char *text, size_t length, UrlList *out);
// Returns the text the screen shows for a refusal.
const char *urlsWhy(UrlsVerdict verdict);
// The scheme is read case-insensitively, because a QR code in capitals spells it that way.
bool urlIsHttps(const char *url);

// Writes the text the screen shows for a link that was not fetched or installed, chosen by the
// stable word its Operation carries (`why` is that Operation's Result, from which an HTTP status is
// read). Returns false, with `out` empty, for a word that has none. The card then shows the
// word itself.
bool fetchWhy(char *out, size_t cap, const char *detail, Result why);

#endif // URLS_H

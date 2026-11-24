// SPDX-License-Identifier: GPL-3.0-or-later
#include "urls.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

// A link written without a scheme is https. The QR route has no paired host key, so TLS is the only
// thing between the person scanning and whoever answers; a link written as `http://` keeps working
// exactly as it did, and nothing here ever falls back from https to http by itself.
#define IMPLIED_SCHEME "https://"

static bool isBlank(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

static bool isAlpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// A scheme may hold what RFC 3986 allows: letters, digits, `+`, `-` and `.`. Its first
// character must be a letter, but a digit there is read as part of a scheme too, so
// `3ds://x` is refused as not http instead of being fetched as a host named `3ds:`.
static bool isSchemeChar(char c) {
	return isAlpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

// Returns the length of a scheme followed by `://`, or 0 for a line with none.
// `host:8000/x` has a port, not a scheme, and a `://` further in is part of a query.
static size_t schemeLength(const char *line, size_t n) {
	size_t i = 0;
	while (i < n && isSchemeChar(line[i])) {
		i++;
	}
	return i + 3 <= n && memcmp(line + i, "://", 3) == 0 ? i : 0;
}

static UrlsVerdict addLine(const char *line, size_t n, UrlList *out) {
	for (size_t i = 0; i < n; i++) {
		if ((unsigned char)line[i] <= 0x20 || (unsigned char)line[i] >= 0x7F) {
			return URLS_BAD; // a space inside, a control character, or a byte past ASCII
		}
	}
	size_t scheme = schemeLength(line, n);
	if (scheme == 0 && n >= 3 && memcmp(line, "://", 3) == 0) {
		return URLS_BAD;
	}
	if (scheme != 0 && !(scheme == 4 && strncasecmp(line, "http", 4) == 0) &&
		!(scheme == 5 && strncasecmp(line, "https", 5) == 0)) {
		return URLS_BAD;
	}
	size_t prefix = scheme ? 0 : sizeof(IMPLIED_SCHEME) - 1;
	if (out->count == URLS_MAX) {
		return URLS_TOO_MANY;
	}
	if (prefix + n >= URL_MAX) {
		return URLS_TOO_LONG;
	}
	char *url = out->url[out->count++];
	memcpy(url, IMPLIED_SCHEME, prefix);
	memcpy(url + prefix, line, n);
	url[prefix + n] = '\0';
	return URLS_OK;
}

UrlsVerdict urlsParse(const char *text, size_t length, UrlList *out) {
	out->count = 0;
	while (length > 0 && text[length - 1] == '\0') {
		length--;
	}
	UrlsVerdict verdict = URLS_OK;
	for (size_t at = 0; at < length && verdict == URLS_OK;) {
		const char *end = memchr(text + at, '\n', length - at);
		size_t stop = end ? (size_t)(end - text) : length, from = at;
		at = stop + 1;
		while (from < stop && isBlank(text[from])) {
			from++;
		}
		while (stop > from && isBlank(text[stop - 1])) {
			stop--;
		}
		if (stop > from) {
			verdict = addLine(text + from, stop - from, out);
		}
	}
	if (verdict != URLS_OK) {
		out->count = 0;
		return verdict;
	}
	return out->count == 0 ? URLS_EMPTY : URLS_OK;
}

const char *urlsWhy(UrlsVerdict verdict) {
	switch (verdict) {
	case URLS_OK:
		return "";
	case URLS_EMPTY:
		return "The code holds no link.";
	case URLS_TOO_MANY:
		return "The code lists too many links.";
	case URLS_TOO_LONG:
		return "A link in the code is too long.";
	default:
		return "The code is not a list of http or https links.";
	}
}

bool urlIsHttps(const char *url) {
	return strncasecmp(url, "https://", 8) == 0;
}

// Words for the people who scan. The details are the ones the wire uses, terse so a
// script can read them.
static const struct {
	const char *detail, *words;
} FETCH_WORDS[] = {
	{"connect", "Could not reach the server. Check the link and the Wi-Fi."},
	{"timeout", "The server took too long to answer."},
	{"tls", "The secure connection could not be set up."},
	{"no-tls", "The console's SSL service did not start, so https links cannot be used. Restart "
			   "3Drop. Plain http links still work."},
	{"no-curl", "The download library did not start, so nothing can be downloaded. Restart 3Drop."},
	{"redirect", "The link loops, or sends the download somewhere 3Drop will not go (plain http "
				 "from https, for one)."},
	{"no-length", "The server did not say how big the file is, so it cannot be installed."},
	{"short", "The server ended the file early."},
	{"length", "The server sent more than it said it would."},
	{"net", "The connection failed while downloading."},
	{"canceled", "Canceled."},
	{"metadata", "That is not a CIA 3Drop can read."},
	{"worker-stall", "The card stopped taking data."},
	{"writer", "The install could not keep up with the download."},
	{"thread", "Out of memory for the download."},
	{"write", "The card could not be written."},
	{"bundle", "The certificate list could not be saved."},
	{"too-big", "That is more than a certificate list can be."},
	{"ring", "Not enough memory to hold the download."},
	{"plain-http", "Plain http links are turned off."},
	{"unmanaged", "3Drop does not install this kind of title."},
	{"self", "A patch or add-on of 3Drop itself is not installed this way."},
	{"am-info", "The console would not say what is installed."},
	{"exists-pending", "An earlier install of this title did not finish. Uninstall the title to "
					   "clear it."},
	// The person who scanned cannot fix this. The CIA author's advice (raise the
	// version) is the host's, in wire.sh.
	{"not-newer", "That version, or a newer one, is already installed."},
	{"space", "Not enough free space on the card."},
	// Only 3Drop's own CIA gets here. 3Drop is run from the Homebrew Launcher, no
	// CIA of it is installed, and an install of itself overwrites one.
	{"notfound", "3Drop is not installed as an app here, so it cannot update itself. Install its "
				 "CIA with FBI first."},
};

bool fetchWhy(char *out, size_t cap, const char *detail, Result why) {
	if (cap == 0) {
		return false;
	}
	out[0] = '\0';
	if (!detail) {
		return false;
	}
	if (strcmp(detail, "http") == 0) {
		int status = downloadHttpStatus(why);
		if (status != 0) {
			snprintf(out, cap, "The server answered HTTP %d.", status);
		} else {
			snprintf(out, cap, "The server refused the download.");
		}
		return true;
	}
	for (size_t i = 0; i < sizeof(FETCH_WORDS) / sizeof(FETCH_WORDS[0]); i++) {
		if (strcmp(detail, FETCH_WORDS[i].detail) == 0) {
			snprintf(out, cap, "%s", FETCH_WORDS[i].words);
			return true;
		}
	}
	return false;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CERTS_H
#define CERTS_H

#include "result.h"

#include <stdbool.h>
#include <stddef.h>

// The bundle Universal Updater keeps, shared with it, so whichever of the two fetches it first
// serves both. FS spells the path from the card's root, while stdio and curl spell it with `sdmc:`.
#define CACERT_DIR_FS "/config/ssl"
#define CACERT_PATH_FS CACERT_DIR_FS "/cacert.pem"
#define CACERT_TEMP_FS CACERT_PATH_FS ".tmp"
#define CACERT_ASIDE_FS CACERT_PATH_FS ".old"
#define CACERT_PATH "sdmc:" CACERT_PATH_FS
#define CACERT_TEMP "sdmc:" CACERT_TEMP_FS
#define CACERT_URL "https://curl.se/ca/cacert.pem"

// The one switch for plain http, which FBI and Universal Updater both take. 0 makes tlsPolicy
// refuse every http URL, and nothing else changes.
#ifndef TLS_ALLOW_PLAIN_HTTP // a test builds the strict mode by defining it
#define TLS_ALLOW_PLAIN_HTTP 1
#endif

// What a download does about TLS. Every https connection of it is checked against the bundle,
// whatever the link's own scheme. libcurl takes its options once for all hops, so an http link that
// is sent on to https is checked there too. Only the person scanning can turn that off, for this
// download alone (`skipCheck`). A plain http hop has no TLS to check.
typedef struct {
	bool allowed;	 // the URL may be fetched at all
	bool verify;	 // its https connections' chain and host are checked against CACERT_PATH
	bool needBundle; // and the bundle must be on the card first (an https link whose check is on)
} TlsPolicy;
TlsPolicy tlsPolicyFor(const char *url, bool plainAllowed, bool skipCheck);
static inline TlsPolicy tlsPolicy(const char *url, bool skipCheck) {
	return tlsPolicyFor(url, TLS_ALLOW_PLAIN_HTTP != 0, skipCheck);
}
// The list's own fetch is always checked against the card's list when it reads (a refresh replaces
// a list only by what that list vouches for), else against the bundle shipped inside 3Drop (the
// first use, a list that will not read, or a list too old to vouch for curl.se's certificate).
// Nothing fetches the list unchecked, so none reaches the card over a connection nobody verified.
// Returns the shipped bundle as libcurl takes it from memory (CURLOPT_CAINFO_BLOB). It is
// res/cacert.pem, linked in by src/caBundle.S, and is never changed or freed. *length is its size.
const void *certsShipped(size_t *length);
// Returns the protocols a redirect may lead on to. An https download never ends up on plain http.
// An http download may be sent on to https and back (libcurl keeps no order between hops), but not
// when the SSL service did not start (`tlsUp` false), in which case it stays on http.
const char *tlsRedirects(const char *url, bool tlsUp);

// What a failed check names, from the flags download.c's verify callback records (mbedTLS's x509
// flags; libcurl's own CURLINFO_SSL_VERIFYRESULT is always 0 with Mbed TLS).
typedef enum { TLS_CAUSE_ISSUER, TLS_CAUSE_HOST, TLS_CAUSE_OTHER } TlsCause;
TlsCause tlsCause(long verifyResult);
const char *tlsCauseWhy(TlsCause cause);

// The bundle exists and is not empty (Universal Updater asks only that it exists).
bool certsPresent(void);
bool certsMakeDir(void);
// Removes the temp, whether or not the fetch that wrote it finished. A fetch the person called off
// publishes nothing, and the next fetch would not want a half list left on the card.
void certsDiscard(void);
// CACERT_TEMP becomes the bundle, or nothing changes. A file with no certificate in it (a captive
// portal's page), one Mbed TLS cannot read (the markers of a bundle around something else), or one
// whose last certificate is cut (a download that stopped inside it) replaces nothing. The old
// bundle goes aside first and comes back if the new one cannot be moved in, because FS refuses a
// rename onto a name that exists. The temp is gone afterwards, whether the new bundle was kept or
// not. Returns false, with the reason in *why: STAGE_METADATA for a file that is not a list, or the
// card's own Result when the move into place fails.
bool certsPublish(Result *why);

#endif // CERTS_H

// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "result.h"
#include "urls.h"

#include <3ds.h>
#include <stdbool.h>

// One transfer by libcurl, run on a thread of its own. The main thread keeps the screen, the LED
// and the keys, which a lookup, a handshake or the bundle's parse would otherwise freeze for
// seconds. A TLS handshake also needs a deeper stack than main's 32768 bytes (FBI runs its curl on
// 64 KiB).
//
// A CIA goes into the ring, with this thread as its pump, just as transferReceive is when the CIA
// comes over the wire. AM stays on the main thread because that is where it is, so the thread stops
// twice and waits for main's answer.
//   DL_SIZED     the response headers are in, and size is Content-Length. Main arms the ring.
//   DL_PREFIXED  the CIA's prefix, and nothing past it, is in the ring's head. Main opens the
//                import.
// A file (the certificate bundle) is written whole to `path` with no stop.
typedef enum { DL_SINK_CIA, DL_SINK_FILE } DlSink;
typedef enum { DL_STARTING, DL_SIZED, DL_FETCHING, DL_PREFIXED, DL_RUNNING, DL_FINISHED } DlStage;
// What main has said at a stop. DL_WAITING means no answer yet, and it is also what every stage
// except the two stops holds.
typedef enum { DL_WAITING, DL_GO, DL_STOP } DlAnswer;
// Which kind of "tls" refusal it was. A check the person may overrule or fix is DL_TLS_CHECK (the
// site's certificate failed it, and verifyResult says how) or DL_TLS_BUNDLE (the list it checks
// against could not be read). DL_TLS_SETUP is a handshake that failed for another reason, which
// skipping the check does not help. libcurl's Mbed TLS backend does not store the flags anywhere
// that CURLINFO_SSL_VERIFYRESULT reads (curl 8.4.0 mbedtls.c:736), so the thread records them
// itself, in a verify callback it attaches to the TLS config through CURLOPT_SSL_CTX_FUNCTION.
typedef enum { DL_TLS_NONE, DL_TLS_CHECK, DL_TLS_BUNDLE, DL_TLS_SETUP } DlTls;

// cacert.pem is ~220 KB, so a server that sends more is not sending it.
#define DL_FILE_MAX (4u << 20)
#define DL_STACK 0x20000
// This priority is just above main's 0x30 and below the ring's writer (0x18), which runs above its
// pump.
#define DL_PRIORITY 0x2F

typedef struct {
	// Set by the caller before downloadStart.
	char url[URL_MAX];
	const char *userAgent;
	DlSink sink;
	const char *path; // DL_SINK_FILE writes the file here, removing it again if the transfer fails
	bool verify;	  // every https connection's chain and host are checked against the bundle
	// The bundle to check against: the card's list (CACERT_PATH) when NULL, otherwise these bytes
	// in memory (libcurl's CURLOPT_CAINFO_BLOB, which keeps the pointer, so they must outlive the
	// transfer).
	const void *caBlob;
	size_t caBlobLen;

	// Zeroed by downloadStart, from here to the end of the struct.
	// These belong to the thread and are read through the readers below while it runs. The stage
	// and the answer are one word (the stage in its low byte, the answer above it), so the thread's
	// move from a stop to the next stage clears the answer in the same store. That means main,
	// which answers only a stop it sees unanswered, can never answer one twice.
	int state;
	u64 size, received, prefixSize;
	bool cancel;

	// Final once the stage is DL_FINISHED.
	bool ok;			// every byte of the body went through its sink
	const char *detail; // a stable word for the refusal; NULL when ok
	Result why;			// the refusal's Result
	long http;			// the last response's status, 0 for none
	long verifyResult;	// mbedTLS's x509 flags as the verify callback left them
	DlTls tls;			// DL_TLS_NONE unless the refusal is "tls"
	void *easy;			// the thread's CURL handle
	void *file;			// DL_SINK_FILE's open file
	Thread thread;
} Download;

#define DL_STAGE_MASK 0xFF
static inline DlStage downloadStage(const Download *d) {
	return (DlStage)(__atomic_load_n(&d->state, __ATOMIC_ACQUIRE) & DL_STAGE_MASK);
}
// Returns the stop the thread is waiting at for downloadAnswer, DL_SIZED or DL_PREFIXED, or
// DL_STARTING (0) when it waits at none or has been answered. It reads the state once, so what the
// caller then does is for that stop.
static inline DlStage downloadAsking(const Download *d) {
	int state = __atomic_load_n(&d->state, __ATOMIC_ACQUIRE);
	int stage = state & DL_STAGE_MASK;
	return (stage == DL_SIZED || stage == DL_PREFIXED) && (state >> 8) == DL_WAITING
			   ? (DlStage)stage
			   : DL_STARTING;
}
static inline u64 downloadReceived(const Download *d) {
	return __atomic_load_n(&d->received, __ATOMIC_ACQUIRE);
}
static inline u64 downloadSize(const Download *d) {
	return __atomic_load_n(&d->size, __ATOMIC_ACQUIRE);
}
static inline u64 downloadPrefix(const Download *d) {
	return __atomic_load_n(&d->prefixSize, __ATOMIC_ACQUIRE);
}

// Starts sslc first (mbedTLS draws its entropy from it, so no handshake works before that), then
// libcurl. What did not start is left out for good. Without sslc, an https URL is refused
// ("no-tls") while an http one is held to http, redirects included. Without libcurl, every URL is
// refused ("no-curl"). The Result is libcurl's failure if there is one, otherwise sslc's.
Result downloadInit(void);
void downloadExit(void);
// Returns true when https can be fetched, which means both started.
bool downloadTlsUp(void);
// Gets the site a link names, "host" or "host:port", as libcurl's URL parser reads it. That is the
// part the person is trusting, whatever comes before it in the text. Returns false, with `out`
// unspecified, when libcurl cannot read the link, finds no host in it, or the name does not fit
// `cap` (it is never cut).
bool downloadSite(const char *url, char *out, size_t cap);
// Gets the path a link names, from the same parser and as the link writes it (a percent code is not
// decoded; a dot segment is resolved, as libcurl will request it). On github.com the owner and the
// repository are in it. *query says whether the link has a query string, which is not part of
// `out`, and nothing else of it is kept. Returns false, with `out` unspecified and *query false,
// when libcurl cannot read the link or the path does not fit `cap` (it is never cut).
bool downloadPath(const char *url, char *out, size_t cap, bool *query);

// Starts the thread. Everything from `state` on starts at zero, including the cancel field of the
// last download, so a Download can be started again as new; the caller's fields above stay. Returns
// false when the thread cannot be made, in which case *d is finished with DOWNLOAD_NO_THREAD.
bool downloadStart(Download *d);
// Main's reply at DL_SIZED and DL_PREFIXED: true goes on, false ends the transfer. It takes effect
// only while downloadAsking reports a stop; at any other time it changes nothing.
void downloadAnswer(Download *d, bool go);
// Ends the transfer at its next chance. The thread finishes whatever it was doing.
void downloadCancel(Download *d);
// Call this after DL_FINISHED. It joins and frees the thread.
void downloadJoin(Download *d);

#endif // DOWNLOAD_H

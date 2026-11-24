// SPDX-License-Identifier: GPL-3.0-or-later
#include "download.h"

#include "certs.h"
#include "cia.h"
#include "net.h" // SOC_FAST_RECV_SIZE, LISTENER_WINDOW_BYTES, WAIT_NS
#include "ring.h"

#include <3ds.h>
#include <ctype.h>
#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <stddef.h> // offsetof
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h> // MIN
#include <sys/socket.h>

#define CONNECT_TIMEOUT_S 15L
// A connection below one byte a second for this long is dead, as transfer.c treats an
// idle stall as a dead peer. The wire's limit is longer because a CIA there has a
// host that is known to be alive.
#define STALL_S 60L
#define MAX_REDIRECTS 8L

// These record what started, and so what may be asked of libcurl: without sslc there
// is no https, and without libcurl there is no download.
static bool tlsStarted, curlStarted;

Result downloadInit(void) {
	tlsStarted = R_SUCCEEDED(sslcInit(0));
	curlStarted = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
	return !curlStarted ? DOWNLOAD_NO_CURL : tlsStarted ? 0 : DOWNLOAD_NO_TLS;
}

void downloadExit(void) {
	if (curlStarted) {
		curl_global_cleanup();
		curlStarted = false;
	}
	if (tlsStarted) {
		sslcExit();
		tlsStarted = false;
	}
}

bool downloadTlsUp(void) {
	return curlStarted && tlsStarted;
}

// Uses libcurl's own URL parser, so what is shown is what it will connect to, and a `user@` before
// the host, a long prefix or a port cannot pass for the site. The URL API needs no global init.
bool downloadSite(const char *url, char *out, size_t cap) {
	CURLU *parsed = curl_url();
	char *host = NULL, *port = NULL;
	bool ok = parsed && curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
			  curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && host[0] != '\0';
	if (ok) {
		// Only an explicit port is included, because the scheme's own port is not
		// part of what the link says. If the parse cannot hand over a port (it ran
		// out of memory for the copy), that does not mean the link has none. A row
		// with no port would say the link names none, so the link is refused instead.
		CURLUcode asked = curl_url_get(parsed, CURLUPART_PORT, &port, 0);
		int n;
		if (asked == CURLUE_OK) {
			n = snprintf(out, cap, "%s:%s", host, port);
		} else if (asked == CURLUE_NO_PORT) {
			n = snprintf(out, cap, "%s", host);
		} else {
			n = -1;
		}
		ok = n >= 0 && (size_t)n < cap;
	}
	curl_free(host);
	curl_free(port);
	curl_url_cleanup(parsed);
	return ok;
}

// The path beside the site, from the same parser. CURLU_URLDECODE is not used, so a
// %2F stays as written instead of becoming a slash the person never sees. Of a
// query, only whether it exists is kept, and its text is freed unread. A bare `?`
// counts as a query (libcurl returns an empty query for it).
bool downloadPath(const char *url, char *out, size_t cap, bool *query) {
	CURLU *parsed = curl_url();
	char *path = NULL, *queryText = NULL;
	*query = false;
	bool ok = parsed && curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
			  curl_url_get(parsed, CURLUPART_PATH, &path, 0) == CURLUE_OK;
	if (ok) {
		int n = snprintf(out, cap, "%s", path);
		ok = n >= 0 && (size_t)n < cap;
	}
	if (ok) {
		// Only NO_QUERY means the link has no query. If the parse cannot hand over a
		// query (it ran out of memory for the copy), that does not mean the link has
		// none, and the row would say there is none.
		CURLUcode asked = curl_url_get(parsed, CURLUPART_QUERY, &queryText, 0);
		ok = asked == CURLUE_OK || asked == CURLUE_NO_QUERY;
		*query = ok && asked == CURLUE_OK;
	}
	curl_free(path);
	curl_free(queryText);
	curl_url_cleanup(parsed);
	return ok;
}

// Sets a stage with no answer in it (DL_WAITING is the zero in the high bytes).
static void setStage(Download *d, DlStage stage) {
	__atomic_store_n(&d->state, (int)stage, __ATOMIC_RELEASE);
}

static bool cancelled(const Download *d) {
	return __atomic_load_n(&d->cancel, __ATOMIC_ACQUIRE);
}

// Callers refuse only when nothing has been refused yet (classify and the
// post-transfer checks look first), so the first refusal is the only one. This
// always returns false, so a caller can return its result.
static bool refuse(Download *d, const char *detail, Result why) {
	d->detail = detail;
	d->why = why;
	return false;
}

// The thread stops at `at` and waits for main's answer. Taking the answer moves the
// thread to `next` and clears the answer in one store (the state word holds both),
// so a stop is answered once.
static bool awaitMain(Download *d, DlStage at, DlStage next) {
	setStage(d, at);
	for (;;) {
		int answer = __atomic_load_n(&d->state, __ATOMIC_ACQUIRE) >> 8;
		if (answer != DL_WAITING) {
			setStage(d, next);
			return answer == DL_GO || refuse(d, "stopped", 0);
		}
		if (cancelled(d)) {
			return refuse(d, "canceled", TRANSFER_CANCELED);
		}
		svcSleepThread(WAIT_NS);
	}
}

// Waits out a full ring, as transfer.c's deadlines wait out a busy worker, until its
// writer has done nothing for WORKER_STALL_MS.
static bool waitWriter(Download *d) {
	svcSleepThread(WAIT_NS);
	if (cancelled(d)) {
		return refuse(d, "canceled", TRANSFER_CANCELED);
	}
	return ringBusy() || refuse(d, "worker-stall", TRANSFER_WORKER_STALL);
}

static void addReceived(Download *d, size_t n) {
	__atomic_store_n(&d->received, d->received + n, __ATOMIC_RELEASE);
}

// The headers are in, so the final response's own Content-Length is known. After a
// redirect, that is the last response's.
static bool contentLength(Download *d, curl_off_t *length) {
	*length = -1;
	curl_easy_getinfo(d->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, length);
	return *length > 0;
}

static bool fileBody(Download *d, const char *data, size_t len) {
	FILE *file = d->file;
	if (downloadStage(d) == DL_STARTING) {
		curl_off_t length;
		if (contentLength(d, &length) && (u64)length > DL_FILE_MAX) {
			return refuse(d, "too-big", DOWNLOAD_TOO_BIG);
		}
		setStage(d, DL_RUNNING);
	}
	if (d->received + len > DL_FILE_MAX) {
		return refuse(d, "too-big", DOWNLOAD_TOO_BIG);
	}
	if (fwrite(data, 1, len, file) != len) {
		return refuse(d, "write", DOWNLOAD_NO_FILE);
	}
	addReceived(d, len);
	return true;
}

// The first bytes give the prefix's size, and the prefix is the one thing held back from
// the import. It is pumped whole, then main opens the import, then the body follows.
static bool sized(Download *d) {
	curl_off_t length;
	if (!contentLength(d, &length)) {
		return refuse(d, "no-length", DOWNLOAD_NO_LENGTH);
	}
	__atomic_store_n(&d->size, (u64)length, __ATOMIC_RELEASE);
	return awaitMain(d, DL_SIZED, DL_FETCHING);
}

static bool prefixed(Download *d) {
	u64 prefix;
	if (!ciaPrefixSize(ringTier1(), d->received, d->size, &prefix)) {
		return refuse(d, "metadata", STAGE_METADATA);
	}
	__atomic_store_n(&d->prefixSize, prefix, __ATOMIC_RELEASE);
	return true;
}

static bool ciaBody(Download *d, const u8 *data, size_t len) {
	if (downloadStage(d) == DL_STARTING && !sized(d)) {
		return false;
	}
	while (len > 0) {
		// Never take more than what is still owed to the next stop, or more than Content-Length.
		u64 limit = d->size - d->received;
		if (d->prefixSize == 0) {
			limit = MIN(limit, (u64)CIA_HEADER_PEEK - d->received);
		} else if (downloadStage(d) != DL_RUNNING) {
			limit = MIN(limit, d->prefixSize - d->received);
		}
		if (limit == 0) {
			return refuse(d, "length", TRANSFER_RECV_FAILED); // more than the server declared
		}
		unsigned char *buffer;
		size_t capacity;
		if (!ringAcquire(&buffer, &capacity)) {
			return refuse(d, "writer", 0);
		}
		if (capacity == 0) {
			if (!waitWriter(d)) {
				return false;
			}
			continue;
		}
		size_t n = (size_t)MIN((u64)MIN(len, capacity), limit);
		memcpy(buffer, data, n);
		if (!ringCommit(n)) {
			return refuse(d, "writer", 0); // the writer failed; installClose has the account of why
		}
		addReceived(d, n);
		data += n;
		len -= n;
		if (d->prefixSize == 0 && d->received >= CIA_HEADER_PEEK && !prefixed(d)) {
			return false;
		}
		if (d->prefixSize != 0 && downloadStage(d) != DL_RUNNING && d->received == d->prefixSize &&
			!awaitMain(d, DL_PREFIXED, DL_RUNNING)) {
			return false;
		}
	}
	return true;
}

static size_t onBody(char *data, size_t size, size_t count, void *context) {
	Download *d = context;
	size_t len = size * count;
	if (cancelled(d)) {
		refuse(d, "canceled", TRANSFER_CANCELED);
		return 0;
	}
	bool taken =
		d->sink == DL_SINK_FILE ? fileBody(d, data, len) : ciaBody(d, (const u8 *)data, len);
	return taken ? len : 0;
}

// libcurl calls this while it connects and shakes hands as well as during the body,
// so it is the one place a cancel reaches a transfer that has no byte to stop yet.
static int onProgress(void *context, curl_off_t total, curl_off_t now, curl_off_t upTotal,
					  curl_off_t upNow) {
	(void)total; // libcurl's signature; the write callback keeps its own byte counts
	(void)now;
	(void)upTotal;
	(void)upNow;
	return cancelled(context) ? 1 : 0;
}

// This runs before the connect, as net.c's listener sets the window before bind. soc:U
// gives a socket a small window of its own, and no window scaling raises it later, so a
// connection that wants a whole window of bytes in flight asks for it here. The value is
// the listener's, swept on the same stack, and a refusal changes nothing, as it does there.
static int onSocket(void *context, curl_socket_t fd, curlsocktype purpose) {
	(void)context;
	if (purpose == CURLSOCKTYPE_IPCXN) {
		int window = LISTENER_WINDOW_BYTES;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &window, sizeof(window));
	}
	return CURL_SOCKOPT_OK;
}

// An option libcurl refuses ends the setup with libcurl's own code, which the card then shows.
#define OPT(option, value)                                                                         \
	do {                                                                                           \
		CURLcode refused = curl_easy_setopt(easy, option, value);                                  \
		if (refused != CURLE_OK) {                                                                 \
			return refused;                                                                        \
		}                                                                                          \
	} while (0)

// Four decimal parts of one to three digits, 0 to 255, joined by dots, and nothing else.
static bool ipv4(const char *text, unsigned char ip[4]) {
	for (int part = 0; part < 4; part++) {
		char *end;
		unsigned long value = strtoul(text, &end, 10);
		// strtoul alone would take a sign or a space before the digits.
		if (!isdigit((unsigned char)*text) || end - text > 3 || value > 255) {
			return false;
		}
		ip[part] = (unsigned char)value;
		text = end;
		if (part < 3 && *text++ != '.') {
			return false;
		}
	}
	return *text == '\0';
}

// Finds the IPv4 address this connection was made for, when its host is one. The
// host is that of the URL libcurl is at while the handshake runs: a redirect moves
// it to the hop's URL before it connects (curl 8.4.0 transfer.c:1671, getinfo.c:99).
// It is the name libcurl gives Mbed TLS to check (vtls.c:1451, mbedtls.c:643-644),
// and libcurl's URL parser writes every IPv4 form as dotted decimal (urlapi.c:661),
// so a name, an IPv6 literal (in brackets) or anything else is not an IPv4 address.
static bool connectionIpv4(const Download *d, unsigned char ip[4]) {
	char *url = NULL, *host = NULL;
	curl_easy_getinfo(d->easy, CURLINFO_EFFECTIVE_URL, &url);
	CURLU *parsed = url ? curl_url() : NULL;
	bool ok = parsed && curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
			  curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && ipv4(host, ip);
	curl_free(host);
	curl_url_cleanup(parsed);
	return ok;
}

// A certificate made for an address, as mkcert and openssl make one, lists that address
// among its IP-address names. Mbed TLS compares the host with DNS names only
// (x509_crt.c:3016-3030), though libcurl gives it an IP literal as the host
// (mbedtls.c:642-649), so Mbed TLS flags the certificate. The host's flag is cleared
// when the host is an IPv4 address and one of those names is that address.
static void forgiveIpName(const Download *d, const mbedtls_x509_crt *crt, uint32_t *flags) {
	unsigned char ip[4];
	if (!(*flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) || !connectionIpv4(d, ip)) {
		return;
	}
	for (const mbedtls_x509_sequence *name = &crt->subject_alt_names; name; name = name->next) {
		// An iPAddress is [7] of a GeneralName, and four bytes of it are an IPv4 address.
		if (name->buf.tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_X509_SAN_IP_ADDRESS) &&
			name->buf.len == sizeof(ip) && memcmp(name->buf.p, ip, sizeof(ip)) == 0) {
			*flags &= ~(uint32_t)MBEDTLS_X509_BADCERT_CN_MISMATCH;
			return;
		}
	}
}

// Mbed TLS calls this for each certificate in the chain it checks (the site's is depth
// 0), with the flags that certificate has earned (for its dates, its issuer and its
// host). The date flags are always cleared, and the host flag where the rule above says
// so; every other flag stays as it is. Mbed TLS ORs what is left into its total after
// this returns, and libcurl decides on that total, refusing the connection on it as
// before. What is left is also collected here for the failure screen, because libcurl's
// Mbed TLS backend keeps what it reads from the handshake in a local variable and
// CURLINFO_SSL_VERIFYRESULT never gets it (curl 8.4.0 mbedtls.c:736). A connection with
// any flag ends the transfer, so only the last connection (a redirect's) can have any.
static int onCertificate(void *context, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
	Download *d = context;
	// No date is checked on any certificate of the chain, because the console keeps
	// its own local time, with no time zone and no network sync, and people set it
	// wrong or set it back for games.
	*flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED);
	if (depth == 0) {
		forgiveIpName(d, crt, flags);
	}
	d->verifyResult |= (long)*flags;
	return 0;
}

// libcurl's setup hook, called as each TLS connection's Mbed TLS config is made
// (mbedtls.c:685-693, before any handshake). It sets the callback on the config.
static CURLcode onTlsConfig(CURL *easy, void *config, void *context) {
	(void)easy;
	mbedtls_ssl_conf_verify(config, onCertificate, context);
	return CURLE_OK;
}

static CURLcode setup(Download *d, CURL *easy) {
	bool verify = d->verify;
	OPT(CURLOPT_URL, d->url);
	OPT(CURLOPT_USERAGENT, d->userAgent);
	OPT(CURLOPT_FOLLOWLOCATION, 1L);
	OPT(CURLOPT_MAXREDIRS, MAX_REDIRECTS);
	OPT(CURLOPT_FAILONERROR, 1L);
	OPT(CURLOPT_PROTOCOLS_STR, tlsStarted ? "http,https" : "http");
	OPT(CURLOPT_REDIR_PROTOCOLS_STR, tlsRedirects(d->url, tlsStarted));
	// No proxy is taken from the environment, because the certificate hook reads the
	// link's host, not a proxy's.
	OPT(CURLOPT_PROXY, "");
	OPT(CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_S);
	OPT(CURLOPT_LOW_SPEED_LIMIT, 1L);
	OPT(CURLOPT_LOW_SPEED_TIME, STALL_S);
	OPT(CURLOPT_NOSIGNAL, 1L);
	// Each read is as large as the window, because a smaller read re-opens the
	// window late and stalls the sender.
	OPT(CURLOPT_BUFFERSIZE, (long)SOC_FAST_RECV_SIZE);
	OPT(CURLOPT_SOCKOPTFUNCTION, onSocket);
	OPT(CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
	OPT(CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
	if (verify) {
		if (d->caBlob) {
			// NOCOPY spares setopt's copy of the bundle; setopt copies only the small
			// struct. curl still copies the whole bundle into each connection's TLS
			// config and each session-cache entry (8.4.0 vtls.c blobdup), freed with
			// them, and copies it once more, NUL-terminated, to parse it.
			struct curl_blob blob = {
				.data = (void *)d->caBlob, .len = d->caBlobLen, .flags = CURL_BLOB_NOCOPY};
			OPT(CURLOPT_CAINFO_BLOB, &blob);
		} else {
			OPT(CURLOPT_CAINFO, CACERT_PATH);
		}
		OPT(CURLOPT_SSL_CTX_FUNCTION, onTlsConfig);
		OPT(CURLOPT_SSL_CTX_DATA, d);
	}
	OPT(CURLOPT_NOPROGRESS, 0L);
	OPT(CURLOPT_XFERINFOFUNCTION, onProgress);
	OPT(CURLOPT_XFERINFODATA, d);
	OPT(CURLOPT_WRITEFUNCTION, onBody);
	OPT(CURLOPT_WRITEDATA, d);
	return CURLE_OK;
}
#undef OPT

// The URL libcurl was sent to when it refused a hop. libcurl makes the hop's URL the
// effective one before it checks the scheme (curl 8.4.0: transfer.c Curl_follow,
// then url.c findprotocol), so the refused scheme is that URL's.
static bool refusedHopIsHttps(CURL *easy) {
	char *hop = NULL;
	curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &hop);
	return hop && urlIsHttps(hop);
}

// Works out what libcurl's answer means to the person looking at the card. A refusal
// a callback already made stands, because it says why libcurl then stopped.
static void classify(Download *d, CURLcode rc) {
	if (d->detail) {
		return;
	}
	Result code = DOWNLOAD_CURL(rc);
	switch (rc) {
	case CURLE_HTTP_RETURNED_ERROR:
		refuse(d, "http", d->http >= 100 && d->http <= 599 ? DOWNLOAD_HTTP(d->http) : code);
		break;
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_COULDNT_CONNECT:
		refuse(d, "connect", code);
		break;
	case CURLE_OPERATION_TIMEDOUT:
		refuse(d, "timeout", code);
		break;
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_CONNECT_ERROR:
		// A handshake that ends on a certificate flag failed the check (the flags are
		// onCertificate's). A handshake that ends without a flag, and not on a bad
		// bundle, ended for another reason (a protocol or cipher the two do not share).
		d->tls = DL_TLS_SETUP;
		if (rc == CURLE_SSL_CACERT_BADFILE) {
			d->tls = DL_TLS_BUNDLE;
		} else if (rc == CURLE_PEER_FAILED_VERIFICATION || d->verifyResult != 0) {
			d->tls = DL_TLS_CHECK;
		}
		refuse(d, "tls", code);
		break;
	case CURLE_TOO_MANY_REDIRECTS:
	case CURLE_UNSUPPORTED_PROTOCOL:
		if (rc == CURLE_UNSUPPORTED_PROTOCOL && !tlsStarted && refusedHopIsHttps(d->easy)) {
			// An http link is held to http without sslc, and was sent on to https.
			// That is not a loop.
			refuse(d, "no-tls", DOWNLOAD_NO_TLS);
		} else {
			// This is a loop, or a hop to ftp or file, which sslc would not have allowed either.
			refuse(d, "redirect", code);
		}
		break;
	case CURLE_ABORTED_BY_CALLBACK:
		refuse(d, "canceled", TRANSFER_CANCELED);
		break;
	default:
		refuse(d, "net", code);
		break;
	}
}

static void worker(void *context) {
	Download *d = context;
	CURLcode rc;
	if (!curlStarted) {
		refuse(d, "no-curl", DOWNLOAD_NO_CURL);
	} else if (urlIsHttps(d->url) && !tlsStarted) {
		// This is not a failed check. No handshake is tried, so there is nothing for a Y to skip.
		refuse(d, "no-tls", DOWNLOAD_NO_TLS);
	} else if (!(d->easy = curl_easy_init())) {
		refuse(d, "net", DOWNLOAD_CURL(CURLE_OUT_OF_MEMORY));
	} else if (d->sink == DL_SINK_FILE && !(d->file = fopen(d->path, "wb"))) {
		refuse(d, "write", DOWNLOAD_NO_FILE); // nothing is fetched into the void
	} else if ((rc = setup(d, d->easy)) != CURLE_OK) {
		refuse(d, "net", DOWNLOAD_CURL(rc)); // an option libcurl refused, not a protocol error
	} else {
		rc = curl_easy_perform(d->easy);
		curl_easy_getinfo(d->easy, CURLINFO_RESPONSE_CODE, &d->http);
		if (rc != CURLE_OK) {
			classify(d, rc);
		} else if (!d->detail && d->sink == DL_SINK_CIA) {
			if (d->received != d->size) {
				refuse(d, "short", TRANSFER_SHORT_READ); // the server ended the body early, cleanly
			} else if (downloadStage(d) != DL_RUNNING) {
				// No prefix ever came, so this is too short to be a CIA.
				refuse(d, "metadata", STAGE_METADATA);
			}
		}
	}
	if (d->file) {
		if (fclose(d->file) != 0 && !d->detail) {
			refuse(d, "write", DOWNLOAD_NO_FILE);
		}
		d->file = NULL;
		if (d->detail) {
			remove(d->path); // half a file is not a bundle
		}
	}
	if (d->easy) {
		curl_easy_cleanup(d->easy);
	}
	d->ok = d->detail == NULL;
	setStage(d, DL_FINISHED);
}

bool downloadStart(Download *d) {
	// Everything on the thread's side starts over, including the cancel of the last
	// download. The zeros mean DL_STARTING, no refusal, DL_TLS_NONE and no handle.
	memset(&d->state, 0, sizeof(*d) - offsetof(Download, state));
	d->thread = threadCreate(worker, d, DL_STACK, DL_PRIORITY, 0, false);
	if (!d->thread) {
		refuse(d, "thread", DOWNLOAD_NO_THREAD);
		setStage(d, DL_FINISHED);
		return false;
	}
	return true;
}

// Only a stop that is waiting takes an answer. That is decided on the state word as
// it is when the answer lands, by a compare-and-swap, so a stop the thread has just
// left is not answered, and the next stop does not take that answer in its place.
void downloadAnswer(Download *d, bool go) {
	int state = __atomic_load_n(&d->state, __ATOMIC_ACQUIRE);
	for (;;) {
		int stage = state & DL_STAGE_MASK;
		if ((stage != DL_SIZED && stage != DL_PREFIXED) || (state >> 8) != DL_WAITING) {
			return;
		}
		int answered = stage | (go ? DL_GO : DL_STOP) << 8;
		if (__atomic_compare_exchange_n(&d->state, &state, answered, false, __ATOMIC_ACQ_REL,
										__ATOMIC_ACQUIRE)) {
			return;
		}
	}
}

void downloadCancel(Download *d) {
	__atomic_store_n(&d->cancel, true, __ATOMIC_RELEASE);
}

void downloadJoin(Download *d) {
	if (d->thread) {
		threadJoin(d->thread, UINT64_MAX);
		threadFree(d->thread);
		d->thread = NULL;
	}
}

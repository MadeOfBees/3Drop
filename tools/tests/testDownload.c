// cc: src/download.c src/certs.c src/urls.c src/cia.c src/codec.c src/fs.c
// cc: -lmbedx509
// Tests src/download.c against a scripted libcurl and ring. It covers the options a transfer is
// given, a CIA's hand-offs (headers, then the prefix and nothing past it), every refusal and its
// message, a cancel at each point where one can arrive, curl's codes as the card shows them, a ring
// that falls behind, and the file sink the certificate bundle uses. The thread is real; the worker
// waits for the main thread's answer.
#include "fakes.h"

#include "caRoots.h" // CA_BUNDLE: certs.c's two symbols, with the test's own two roots
#include "certs.h"
#include "ciaBuild.h"
#include "download.h"
#include "net.h"
#include "ring.h"

#include <assert.h>
#include <ctype.h>
#include <curl/curl.h>
#include <malloc.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- The ring: a flat buffer, 1 MiB units as in the real ring, and a writer that can fall behind
// ----
static u8 *tier;
static u64 pumped, armedTotal;
static unsigned armCalls;
static bool armFails, acquireFails, commitFails, writerBusy;
static volatile unsigned fullPolls, busyPolls;
u8 *ringTier1(void) {
	if (!tier) {
		tier = memalign(0x1000, RING_TIER1_BYTES);
	}
	return tier;
}
bool ringArm(u64 total, RingDir dir, bool overflow, u64 offset) {
	armCalls++;
	pumped = 0;
	armedTotal = total;
	return !armFails;
}
static size_t offered; // what the last acquire allowed: a commit may not exceed it
bool ringAcquire(unsigned char **buffer, size_t *capacity) {
	*buffer = tier + pumped;
	size_t unit = RING_UNIT - (size_t)(pumped % RING_UNIT);
	*capacity = armedTotal - pumped < unit ? (size_t)(armedTotal - pumped) : unit;
	if (fullPolls > 0) {
		fullPolls--;
		*capacity = 0;
	}
	offered = *capacity;
	return !acquireFails;
}
bool ringCommit(size_t size) {
	// The real ring's writer reads whole units. A copy past the acquired unit is a copy into the
	// next one, which the flat buffer here would hide.
	assert(size <= offered);
	offered = 0;
	pumped += size;
	return !commitFails;
}
bool ringBusy(void) {
	if (busyPolls > 0) {
		busyPolls--;
		return true;
	}
	return writerBusy;
}
void svcSleepThread(s64 ns) {
	usleep(100);
}

// ---- The socket option a connection is given: caught here, because host libc would clamp it ----
static struct { int calls, fd, level, name, value; } sockopts;
int setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
	assert(length == sizeof(int));
	sockopts.calls++;
	sockopts.fd = fd;
	sockopts.level = level;
	sockopts.name = name;
	sockopts.value = *(const int *)value;
	return 0;
}

// ---- libcurl: the script decides what a transfer does ----
typedef struct {
	CURLcode result;
	long http;
	// The flags Mbed TLS finds in the chain of the handshake that the transfer makes (`handshake`),
	// the site's own and the issuer's, as one set. They reach libcurl's caller only through the
	// verify callback that the setup hook put on the TLS config: libcurl's
	// CURLINFO_SSL_VERIFYRESULT is always 0 with Mbed TLS (curl 8.4.0 mbedtls.c:736 keeps them in a
	// local variable), and it is 0 here too.
	long verifyResult;
	// The transfer gets as far as a TLS handshake; earlierHandshake means a redirect's earlier
	// connection did too, before it, and came out clean.
	bool handshake, earlierHandshake;
	curl_off_t length;
	const u8 *body;
	size_t bodyLen, chunk;
	bool noBody;   // the transfer fails before a byte of body: connect, TLS, a 404
	bool skipXfer; // no progress callbacks (libcurl's come at least once a second, not per chunk)
	// The transfer waits for `gate` before its first progress callback, which fires while it is
	// connecting.
	bool holdConnect;
	size_t gateAt; // the transfer holds before this chunk until `gate` is set (0: none)
	volatile bool gate;
	// What the thread set.
	char url[2048], agent[64], caInfo[128], protocols[32], redirects[32], proxy[32];
	bool hasCaInfo, hasProtocols, hasProxy;
	// CAINFO_BLOB: the bytes the thread named and the flag it gave (libcurl copies only the
	// struct).
	bool hasBlob;
	const void *blobData;
	size_t blobLen;
	unsigned blobFlags;
	// The setup hook the thread named, and the data it gave it.
	curl_ssl_ctx_callback sslCtx;
	void *sslCtxData;
	long verifyPeer, verifyHost, follow, maxRedirs, failOnError, connectTimeout, lowLimit, lowTime,
		noSignal, noProgress, bufferSize;
	unsigned easies, cleanups, performs, globals, globalCleanups;
	bool globalFails; // makes curl_global_init return CURLE_FAILED_INIT
	// curl_easy_setopt returns failCode for failOption (0 means none), and counts its calls.
	CURLoption failOption;
	CURLcode failCode;
	unsigned setopts;
	// The URL libcurl reports as the effective one when it refuses a hop (hopKnown: whether it
	// reports one at all), and how often the thread asked.
	char hop[128];
	bool hopKnown;
	unsigned hopReads;
	size_t (*write)(char *, size_t, size_t, void *);
	void *writeData;
	curl_xferinfo_callback xfer;
	void *xferData;
	long xferCalls;
	curl_sockopt_callback sockopt;
} Script;
static Script script;

CURLcode curl_global_init(long flags) {
	script.globals++;
	return script.globalFails ? CURLE_FAILED_INIT : CURLE_OK;
}
void curl_global_cleanup(void) {
	script.globalCleanups++;
}
CURL *curl_easy_init(void) {
	script.easies++;
	return &script;
}
// cppcheck-suppress constParameter ; libcurl's signature
void curl_easy_cleanup(CURL *handle) {
	assert(handle == &script);
	script.cleanups++;
}
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...) {
	va_list ap;
	va_start(ap, option);
	long number = option < CURLOPTTYPE_OBJECTPOINT ? va_arg(ap, long) : 0;
	void *pointer = (option >= CURLOPTTYPE_OBJECTPOINT && option < CURLOPTTYPE_FUNCTIONPOINT) ||
							option >= CURLOPTTYPE_BLOB
						? va_arg(ap, void *)
						: NULL;
	void (*function)(void) = option >= CURLOPTTYPE_FUNCTIONPOINT && option < CURLOPTTYPE_OFF_T
								 ? va_arg(ap, void (*)(void))
								 : NULL;
	va_end(ap);
	script.setopts++;
	if (script.failCode != CURLE_OK && option == script.failOption) {
		return script.failCode;
	}
	switch (option) {
	case CURLOPT_URL:
		snprintf(script.url, sizeof(script.url), "%s", (char *)pointer);
		break;
	case CURLOPT_USERAGENT:
		snprintf(script.agent, sizeof(script.agent), "%s", (char *)pointer);
		break;
	case CURLOPT_CAINFO:
		script.hasCaInfo = true;
		snprintf(script.caInfo, sizeof(script.caInfo), "%s", (char *)pointer);
		break;
	case CURLOPT_CAINFO_BLOB: {
		const struct curl_blob *blob = pointer;
		script.hasBlob = true;
		script.blobData = blob->data;
		script.blobLen = blob->len;
		script.blobFlags = blob->flags;
		break;
	}
	case CURLOPT_SSL_CTX_FUNCTION:
		script.sslCtx = (curl_ssl_ctx_callback)function;
		break;
	case CURLOPT_SSL_CTX_DATA:
		script.sslCtxData = pointer;
		break;
	case CURLOPT_PROTOCOLS_STR:
		script.hasProtocols = true;
		snprintf(script.protocols, sizeof(script.protocols), "%s", (char *)pointer);
		break;
	case CURLOPT_REDIR_PROTOCOLS_STR:
		snprintf(script.redirects, sizeof(script.redirects), "%s", (char *)pointer);
		break;
	case CURLOPT_PROXY:
		script.hasProxy = true;
		snprintf(script.proxy, sizeof(script.proxy), "%s", (char *)pointer);
		break;
	case CURLOPT_SSL_VERIFYPEER:
		script.verifyPeer = number;
		break;
	case CURLOPT_SSL_VERIFYHOST:
		script.verifyHost = number;
		break;
	case CURLOPT_FOLLOWLOCATION:
		script.follow = number;
		break;
	case CURLOPT_MAXREDIRS:
		script.maxRedirs = number;
		break;
	case CURLOPT_FAILONERROR:
		script.failOnError = number;
		break;
	case CURLOPT_CONNECTTIMEOUT:
		script.connectTimeout = number;
		break;
	case CURLOPT_LOW_SPEED_LIMIT:
		script.lowLimit = number;
		break;
	case CURLOPT_LOW_SPEED_TIME:
		script.lowTime = number;
		break;
	case CURLOPT_NOSIGNAL:
		script.noSignal = number;
		break;
	case CURLOPT_NOPROGRESS:
		script.noProgress = number;
		break;
	case CURLOPT_BUFFERSIZE:
		script.bufferSize = number;
		break;
	case CURLOPT_SOCKOPTFUNCTION:
		script.sockopt = (curl_sockopt_callback)function;
		break;
	case CURLOPT_WRITEFUNCTION:
		script.write = (size_t(*)(char *, size_t, size_t, void *))function;
		break;
	case CURLOPT_WRITEDATA:
		script.writeData = pointer;
		break;
	case CURLOPT_XFERINFOFUNCTION:
		script.xfer = (curl_xferinfo_callback)function;
		break;
	case CURLOPT_XFERINFODATA:
		script.xferData = pointer;
		break;
	default:
		assert(!"an option the thread should not set");
	}
	return CURLE_OK;
}
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
	va_list ap;
	va_start(ap, info);
	if (info == CURLINFO_CONTENT_LENGTH_DOWNLOAD_T) {
		*va_arg(ap, curl_off_t *) = script.length;
	} else if (info == CURLINFO_RESPONSE_CODE) {
		*va_arg(ap, long *) = script.http;
	} else if (info == CURLINFO_SSL_VERIFYRESULT) {
		*va_arg(ap, long *) = 0; // libcurl's Mbed TLS backend always leaves 0 in it
	} else if (info == CURLINFO_EFFECTIVE_URL) {
		script.hopReads++;
		*va_arg(ap, char **) = script.hopKnown ? script.hop : NULL;
	} else {
		assert(!"an info the thread should not ask");
	}
	va_end(ap);
	return CURLE_OK;
}

// ---- Mbed TLS: the one call the setup hook makes, caught here (libmbedtls is not linked) ----
static int fakeConfig; // what libcurl hands its setup hook as the TLS config
static struct {
	int (*verify)(void *, mbedtls_x509_crt *, int, uint32_t *);
	void *context;
	unsigned sets;
} mbed;
void mbedtls_ssl_conf_verify(mbedtls_ssl_config *conf,
							 int (*f_vrfy)(void *, mbedtls_x509_crt *, int, uint32_t *),
							 void *p_vrfy) {
	assert((void *)conf == &fakeConfig);
	mbed.verify = f_vrfy;
	mbed.context = p_vrfy;
	mbed.sets++;
}

// One TLS connection as libcurl's Mbed TLS backend makes it (mbedtls.c:685-693, 715, 736). The
// setup hook gets the config first. Then the handshake checks the chain, which Mbed TLS reports to
// the verify callback one certificate at a time, from the root down to the site's own, each with
// its own flags. The issuer's flags belong to the root's certificate and the others to the site's;
// the date flags are on both, because Mbed TLS sets them on every certificate. libcurl itself
// decides on the total and keeps it where its caller can't read it. Mbed TLS ORs each certificate's
// flags into that total after the callback returns (2.28.8 x509_crt.c
// x509_crt_merge_flags_with_cb), so a flag the callback clears is cleared for libcurl too. These
// certificates are dated outside every span (the start in 2099, the end in 1999) and carry no name.
// The date flags must come back cleared, and each other flag as it went in (testCertCheck.sh has
// the real certificates and the real chain).
static mbedtls_x509_crt datedOut = {.valid_from = {2099, 1, 1, 0, 0, 0},
									.valid_to = {1999, 1, 1, 0, 0, 0}};
#define DATE_FLAGS (MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED)
static void tlsConnection(long flags) {
	mbed.verify = NULL;
	if (script.sslCtx) {
		assert(script.sslCtx(&script, &fakeConfig, script.sslCtxData) == CURLE_OK);
	}
	if (mbed.verify) {
		uint32_t root = (uint32_t)flags & (MBEDTLS_X509_BADCERT_NOT_TRUSTED | DATE_FLAGS);
		uint32_t site = (uint32_t)flags & ~(uint32_t)MBEDTLS_X509_BADCERT_NOT_TRUSTED;
		uint32_t rootBefore = root, siteBefore = site;
		assert(mbed.verify(mbed.context, &datedOut, 1, &root) == 0);
		assert(mbed.verify(mbed.context, &datedOut, 0, &site) == 0);
		assert(root == (rootBefore & ~(uint32_t)DATE_FLAGS));
		assert(site == (siteBefore & ~(uint32_t)DATE_FLAGS));
	}
}

CURLcode curl_easy_perform(CURL *handle) {
	script.performs++;
	while (script.holdConnect && !script.gate) {
		usleep(100);
	}
	// Call it before anything arrives, because libcurl's progress callback fires while it connects.
	if (script.xfer && script.xfer(script.xferData, 0, 0, 0, 0) != 0) {
		return CURLE_ABORTED_BY_CALLBACK;
	}
	if (script.handshake) {
		if (script.earlierHandshake) {
			tlsConnection(0);
		}
		tlsConnection(script.verifyResult);
	}
	if (script.noBody) {
		return script.result;
	}
	size_t at = 0, chunks = 0;
	while (at < script.bodyLen) {
		size_t n = script.bodyLen - at < script.chunk ? script.bodyLen - at : script.chunk;
		if (script.gateAt && chunks == script.gateAt) {
			while (!script.gate) {
				usleep(100);
			}
		}
		script.xferCalls++;
		if (!script.skipXfer && script.xfer &&
			script.xfer(script.xferData, script.length, (curl_off_t)at, 0, 0) != 0) {
			return CURLE_ABORTED_BY_CALLBACK;
		}
		if (script.write((char *)script.body + at, 1, n, script.writeData) != n) {
			return CURLE_WRITE_ERROR;
		}
		at += n;
		chunks++;
	}
	return script.result;
}

// ---- libcurl's URL API: the script decides what the parse of a link returns ----
static struct {
	bool noHandle, setFails, hostFails, pathFails;
	bool portFails, queryFails; // the getter's own error, not an absent part
	const char *host, *port;	// NULL: the link has none
	const char *path, *query;	// NULL: the link has no query (path: libcurl cannot say)
	char set[2048];				// the text it was given
	unsigned handles, cleanups, strings, frees;
} urls;
// What CURLU_URLDECODE does to an answer, as libcurl documents it: %XX becomes its byte.
static char *decoded(const char *text) {
	char *out = malloc(strlen(text) + 1), *at = out;
	for (; *text; text++) {
		if (text[0] == '%' && isxdigit((unsigned char)text[1]) &&
			isxdigit((unsigned char)text[2])) {
			char hex[3] = {text[1], text[2], '\0'};
			*at++ = (char)strtol(hex, NULL, 16);
			text += 2;
		} else {
			*at++ = *text;
		}
	}
	*at = '\0';
	return out;
}
CURLU *curl_url(void) {
	if (urls.noHandle) {
		return NULL;
	}
	urls.handles++;
	return (CURLU *)&urls;
}
// cppcheck-suppress constParameter ; libcurl's signature
void curl_url_cleanup(CURLU *handle) {
	urls.cleanups += handle != NULL;
}
// cppcheck-suppress constParameter ; libcurl's signature
CURLUcode curl_url_set(CURLU *handle, CURLUPart what, const char *part, unsigned int flags) {
	assert(handle == (CURLU *)&urls && what == CURLUPART_URL && flags == 0);
	snprintf(urls.set, sizeof(urls.set), "%s", part);
	return urls.setFails ? CURLUE_MALFORMED_INPUT : CURLUE_OK;
}
CURLUcode curl_url_get(const CURLU *handle, CURLUPart what, char **part, unsigned int flags) {
	assert((const void *)handle == (const void *)&urls);
	// Only the path may be asked for decoded, and libcurl then decodes it. The screen shows the
	// path as the link writes it, so asking for a decoded one changes the answer here as it does in
	// libcurl.
	assert((flags & ~(unsigned)CURLU_URLDECODE) == 0 && (what == CURLUPART_PATH || flags == 0));
	assert(what == CURLUPART_HOST || what == CURLUPART_PORT || what == CURLUPART_PATH ||
		   what == CURLUPART_QUERY);
	if ((what == CURLUPART_HOST && urls.hostFails) || (what == CURLUPART_PATH && urls.pathFails)) {
		return CURLUE_MALFORMED_INPUT;
	}
	if ((what == CURLUPART_PORT && urls.portFails) ||
		(what == CURLUPART_QUERY && urls.queryFails)) {
		return CURLUE_OUT_OF_MEMORY; // as the getter's own copy can fail; the part is not absent
	}
	const char *value = what == CURLUPART_HOST	 ? urls.host
						: what == CURLUPART_PORT ? urls.port
						: what == CURLUPART_PATH ? urls.path
												 : urls.query;
	if (!value) {
		return what == CURLUPART_PORT	 ? CURLUE_NO_PORT
			   : what == CURLUPART_QUERY ? CURLUE_NO_QUERY
										 : CURLUE_MALFORMED_INPUT;
	}
	*part = (flags & CURLU_URLDECODE) ? decoded(value) : strdup(value);
	urls.strings++;
	return CURLUE_OK;
}
void curl_free(void *p) {
	urls.frees += p != NULL;
	free(p);
}

// ---- The main thread's part, as src/qr.c plays it ----
// Threads are real, but creating one can be made to fail.
static bool noThread;
typedef struct {
	pthread_t thread;
	ThreadFunc entry;
	void *arg;
} HostThread;
static void *hostRun(void *t) {
	((HostThread *)t)->entry(((HostThread *)t)->arg);
	return NULL;
}
Thread threadCreate(ThreadFunc entry, void *arg, size_t stack, int prio, int core, bool detach) {
	assert(stack >= 0x10000 && prio == DL_PRIORITY && core == 0 && !detach);
	if (noThread) {
		return NULL;
	}
	HostThread *t = malloc(sizeof(*t));
	*t = (HostThread){.entry = entry, .arg = arg};
	return pthread_create(&t->thread, NULL, hostRun, t) == 0 ? (Thread)t : NULL;
}
Result threadJoin(Thread thread, u64 timeoutNs) {
	assert(timeoutNs == UINT64_MAX); // a join that waits for the thread, never a timeout
	return pthread_join(((HostThread *)thread)->thread, NULL);
}
void threadFree(Thread thread) {
	free(thread);
}

typedef struct {
	bool arm;		  // arm the ring at DL_SIZED
	bool open;		  // open the import at DL_PREFIXED
	bool mute;		  // answer neither, so the thread waits for a cancel
	DlStage cancelAt; // cancel when this stage is seen (DL_FINISHED: never)
	bool twice;		  // every answer is followed at once by its opposite, which must change nothing
	unsigned opens;	  // number of answers to the second stop
	// What it saw.
	unsigned seen; // bit per stage
	u64 sizeAtSized, receivedAtPrefix, pumpedAtPrefix, prefixAtPrefix;
	u8 headAtPrefix[64];
} Driver;

static Download dl;
static Driver driver;

static void start(DlSink sink, const char *url, bool verify) {
	memset(&dl, 0, sizeof(dl));
	snprintf(dl.url, sizeof(dl.url), "%s", url);
	dl.userAgent = "3Drop/test";
	dl.sink = sink;
	dl.verify = verify;
	pumped = armedTotal = armCalls = 0;
	armFails = acquireFails = commitFails = writerBusy = false;
	fullPolls = busyPolls = 0;
}

// Drives a started download to its end, answering as `driver` says.
static void drive(void) {
	assert(downloadStart(&dl));
	for (int spins = 0; spins < 200000; spins++) {
		DlStage stage = downloadStage(&dl);
		driver.seen |= 1u << stage;
		if (stage == driver.cancelAt) {
			downloadCancel(&dl);
			script.gate = true; // a transfer held at its connect goes on, into the cancel
			driver.cancelAt = DL_FINISHED;
		}
		if (stage == DL_FINISHED) {
			downloadJoin(&dl);
			return;
		}
		if (stage == DL_SIZED && downloadAsking(&dl) && !driver.mute) {
			driver.sizeAtSized = downloadSize(&dl);
			if (driver.arm) {
				assert(ringArm(downloadSize(&dl), RING_TO_HANDLE, true, 0));
			}
			downloadAnswer(&dl, driver.arm);
			if (driver.twice) {
				downloadAnswer(&dl, !driver.arm); // the first answer stands
			}
		}
		if (stage == DL_PREFIXED && downloadAsking(&dl)) {
			driver.opens++;
			driver.receivedAtPrefix = downloadReceived(&dl);
			driver.pumpedAtPrefix = pumped;
			driver.prefixAtPrefix = downloadPrefix(&dl);
			memcpy(driver.headAtPrefix, tier, sizeof(driver.headAtPrefix));
			downloadAnswer(&dl, driver.open);
			if (driver.twice) {
				downloadAnswer(&dl, !driver.open);
			}
		}
		usleep(100);
	}
	assert(!"the download never finished");
}

static void resetScript(void) {
	unsigned easies = script.easies, cleanups = script.cleanups, performs = script.performs;
	unsigned globals = script.globals, globalCleanups = script.globalCleanups;
	memset(&script, 0, sizeof(script));
	memset(&mbed, 0, sizeof(mbed));
	script.easies = easies;
	script.cleanups = cleanups;
	script.performs = performs;
	script.globals = globals;
	script.globalCleanups = globalCleanups;
	script.result = CURLE_OK;
	script.http = 200;
	script.chunk = 16384;
	memset(&driver, 0, sizeof(driver));
	driver.arm = driver.open = true;
	driver.cancelAt = DL_FINISHED;
}

static TestCIA cia;
// The whole CIA as the builder lays it out: its prefix, and the one content byte.
static void ciaBody(size_t chunk) {
	resetScript();
	script.body = cia.bytes;
	script.bodyLen = (size_t)cia.extent;
	script.length = (curl_off_t)cia.extent;
	script.chunk = chunk;
}
// A CIA claimed to run `more` bytes past its prefix (the builder's buffer is zeros there): the
// download reads the prefix's sizes and nothing else of the CIA, so this is a body of any length.
static void ciaBig(size_t chunk, size_t more) {
	ciaBody(chunk);
	script.bodyLen = (size_t)cia.prefix + more;
	script.length = (curl_off_t)script.bodyLen;
}

static void options(void) {
	// A verified https download uses the bundle, makes both checks, and allows only https after any
	// redirect.
	ciaBody(16384);
	start(DL_SINK_CIA, "https://example.com/3drop.cia", true);
	drive();
	assert(dl.ok && script.performs >= 1);
	assert(strcmp(script.url, "https://example.com/3drop.cia") == 0);
	assert(strcmp(script.agent, "3Drop/test") == 0);
	assert(script.follow == 1 && script.maxRedirs == 8 && script.failOnError == 1);
	assert(script.connectTimeout == 15 && script.lowLimit == 1 && script.lowTime == 60);
	assert(script.noSignal == 1 && script.noProgress == 0);
	assert(script.bufferSize == SOC_FAST_RECV_SIZE); // a whole window in one soc:U read
	assert(script.verifyPeer == 1 && script.verifyHost == 2);
	assert(script.hasCaInfo && strcmp(script.caInfo, CACERT_PATH) == 0);
	assert(script.hasProtocols && strcmp(script.protocols, "http,https") == 0);
	assert(strcmp(script.redirects, "https") == 0);
	// An empty proxy, so libcurl takes none from the environment (the certificate hook would read
	// the link's host, not the proxy's).
	assert(script.hasProxy && script.proxy[0] == '\0');
	assert(script.easies == script.cleanups); // the handle is always released
	// The connection asks for the listener's window before it connects, and only for a connection
	// it makes itself.
	assert(script.sockopt);
	sockopts.calls = 0;
	assert(script.sockopt(NULL, 7, CURLSOCKTYPE_IPCXN) == CURL_SOCKOPT_OK);
	assert(sockopts.calls == 1 && sockopts.fd == 7 && sockopts.level == SOL_SOCKET &&
		   sockopts.name == SO_RCVBUF && sockopts.value == LISTENER_WINDOW_BYTES);
	assert(script.sockopt(NULL, 7, CURLSOCKTYPE_ACCEPT) == CURL_SOCKOPT_OK && sockopts.calls == 1);

	// The same download with the check skipped: neither check is made, and no bundle is named.
	ciaBody(16384);
	start(DL_SINK_CIA, "https://example.com/3drop.cia", false);
	drive();
	assert(dl.ok && script.verifyPeer == 0 && script.verifyHost == 0 && !script.hasCaInfo);
	assert(strcmp(script.redirects, "https") ==
		   0); // skipping a check does not let https drop to http

	// Plain http: redirects may go to either, and libcurl takes its options once, for every hop, so
	// an https hop is checked against the bundle as an https link's own connection is.
	ciaBody(16384);
	start(DL_SINK_CIA, "http://192.168.1.5:8000/a.cia",
		  tlsPolicy("http://192.168.1.5:8000/a.cia", false).verify);
	drive();
	assert(dl.ok && strcmp(script.redirects, "http,https") == 0);
	assert(script.verifyPeer == 1 && script.verifyHost == 2);
	assert(script.hasCaInfo && strcmp(script.caInfo, CACERT_PATH) == 0);
	assert(strcmp(script.url, "http://192.168.1.5:8000/a.cia") == 0);
	// With Y for this download: the same link with no check, for this download only.
	ciaBody(16384);
	start(DL_SINK_CIA, "http://192.168.1.5:8000/a.cia",
		  tlsPolicy("http://192.168.1.5:8000/a.cia", true).verify);
	drive();
	assert(dl.ok && script.verifyPeer == 0 && script.verifyHost == 0 && !script.hasCaInfo);
	assert(strcmp(script.redirects, "http,https") == 0);
}

static void cias(void) {
	// Every way a body can be cut into callbacks, across both hand-off points and a ring unit.
	// (100000 divides no ring unit: some callback straddles two, and is taken in two parts.)
	static const size_t CHUNKS[] = {1, 3, 20, 21, 100, 4096, 65536, 100000, 1 << 20};
	for (size_t k = 0; k < sizeof(CHUNKS) / sizeof(CHUNKS[0]); k++) {
		if (CHUNKS[k] == 1) {
			// One byte at a time is slow, so this run goes only as far as the prefix, where it is
			// stopped.
			ciaBig(1, 70);
			start(DL_SINK_CIA, "https://h/a.cia", true);
			driver.open = false;
			drive();
			assert(driver.receivedAtPrefix == cia.prefix && driver.pumpedAtPrefix == cia.prefix);
			assert(memcmp(tier, cia.bytes, (size_t)cia.prefix) == 0);
			continue;
		}
		size_t more = CHUNKS[k] >= 65536 ? 2500000 : 300000; // past one 1 MiB unit, and a second
		ciaBig(CHUNKS[k], more);
		start(DL_SINK_CIA, "https://h/a.cia", true);
		drive();
		size_t total = (size_t)cia.prefix + more;
		assert(dl.ok && dl.detail == NULL && dl.why == 0);
		assert(driver.seen & (1u << DL_SIZED) && driver.seen & (1u << DL_PREFIXED));
		assert(driver.sizeAtSized == total && armCalls == 1 && armedTotal == total);
		assert(driver.opens == 1); // asked once, and not again once answered
		// The whole prefix, and nothing past it, was in the ring when the import was asked to open.
		assert(driver.prefixAtPrefix == cia.prefix && driver.receivedAtPrefix == cia.prefix);
		assert(driver.pumpedAtPrefix == cia.prefix);
		assert(memcmp(driver.headAtPrefix, cia.bytes, sizeof(driver.headAtPrefix)) == 0);
		// Then the body, byte for byte.
		assert(downloadReceived(&dl) == total && pumped == total);
		assert(memcmp(tier, cia.bytes, total) == 0);
		assert(downloadStage(&dl) == DL_FINISHED && dl.http == 200);
	}
}

static void answers(void) {
	// Main's answer to a stop is taken once: a second, opposite one at once after it changes
	// nothing, at either stop, and the stop is not asked about again.
	for (int arm = 1; arm >= 0; arm--) {
		ciaBig(4096, 50000);
		start(DL_SINK_CIA, "https://h/a.cia", true);
		driver.twice = true;
		driver.arm = arm;
		driver.open = true;
		drive();
		assert(driver.opens == (arm ? 1u : 0u) && armCalls == (arm ? 1u : 0u));
		assert(dl.ok == arm);
		assert(arm || strcmp(dl.detail, "stopped") == 0);
	}
	ciaBig(4096, 50000);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	driver.twice = true;
	driver.open = false;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "stopped") == 0 && driver.opens == 1);
	assert(pumped == cia.prefix); // refused at the prefix, whatever came after

	// The state word itself, with the stage below and the answer above: a stop is asked about until
	// it is answered, an answer lands on a waiting stop and on no other stage, and the first one
	// stands. (The store is a compare-and-swap, so an answer can never land on a stage the thread
	// has moved on to. The window for that is a few nanoseconds, and no test can hold it open.)
	static const int STOPS[] = {DL_SIZED, DL_PREFIXED};
	static const int OTHERS[] = {DL_STARTING, DL_FETCHING, DL_RUNNING, DL_FINISHED};
	Download probe;
	memset(&probe, 0, sizeof(probe));
	for (size_t i = 0; i < sizeof(STOPS) / sizeof(STOPS[0]); i++) {
		probe.state = STOPS[i];
		assert((int)downloadAsking(&probe) == STOPS[i] && (int)downloadStage(&probe) == STOPS[i]);
		downloadAnswer(&probe, true);
		assert(probe.state == (STOPS[i] | DL_GO << 8) && (int)downloadStage(&probe) == STOPS[i]);
		assert(downloadAsking(&probe) == DL_STARTING);
		downloadAnswer(&probe, false);
		assert(probe.state == (STOPS[i] | DL_GO << 8));
		probe.state = STOPS[i];
		downloadAnswer(&probe, false);
		assert(probe.state == (STOPS[i] | DL_STOP << 8) && downloadAsking(&probe) == DL_STARTING);
	}
	for (size_t i = 0; i < sizeof(OTHERS) / sizeof(OTHERS[0]); i++) {
		probe.state = OTHERS[i];
		assert(downloadAsking(&probe) == DL_STARTING);
		downloadAnswer(&probe, true);
		downloadAnswer(&probe, false);
		assert(probe.state == OTHERS[i]);
	}
}

static void refusals(void) {
	// With no Content-Length (chunked) the ring cannot be sized, and nothing is armed or pumped.
	ciaBody(4096);
	script.length = -1;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "no-length") == 0 && dl.why == DOWNLOAD_NO_LENGTH);
	assert(armCalls == 0 && pumped == 0 && !(driver.seen & (1u << DL_SIZED)));
	assert(script.easies == script.cleanups);
	// Nothing to fetch is not a CIA either.
	ciaBody(4096);
	script.length = 0;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "no-length") == 0 && armCalls == 0);

	// Main could not arm the ring, so the transfer ends with no byte taken.
	ciaBody(4096);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	driver.arm = false;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "stopped") == 0 && pumped == 0);
	assert(!(driver.seen & (1u << DL_PREFIXED)));

	// The import was refused at the prefix (a gate), so not a byte of the body goes in.
	ciaBody(4096);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	driver.open = false;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "stopped") == 0);
	assert(pumped == cia.prefix && downloadReceived(&dl) == cia.prefix);

	// A header that ciaParse would refuse is reported at once, before the prefix is wanted.
	TestCIA bad = build(0x00010004, 0x00010004, 0x0004000003D37200ULL, 1, 1);
	LE32(bad.bytes, 0x2000);
	resetScript();
	script.body = bad.bytes;
	script.bodyLen = (size_t)bad.extent;
	script.length = (curl_off_t)bad.extent;
	script.chunk = 4096;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "metadata") == 0 && dl.why == STAGE_METADATA);
	assert(!(driver.seen & (1u << DL_PREFIXED)) && pumped <= 4096);
	free(bad.bytes);

	// A CIA too short for its own header: the transfer ends and the ring's account is the failure.
	ciaBody(4096);
	script.bodyLen = 10;
	script.length = 10;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "metadata") == 0);
	// A body shorter than the Content-Length said, closed early by the server.
	ciaBig(4096, 1000);
	script.bodyLen = (size_t)cia.prefix + 100;
	script.result = CURLE_RECV_ERROR; // curl: 56, the connection ended with bytes still owed
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "net") == 0 && dl.why == DOWNLOAD_CURL(CURLE_RECV_ERROR));
	// A server that ends the body cleanly but short (no error from curl) is still a short body.
	ciaBig(4096, 1000);
	script.bodyLen = (size_t)cia.prefix + 100;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "short") == 0 && dl.why == TRANSFER_SHORT_READ);
	// A body longer than the Content-Length said (a server that lies) is stopped at the declared
	// size.
	ciaBig(4096, 1000);
	script.length = (curl_off_t)cia.prefix + 500;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "length") == 0 && pumped == cia.prefix + 500);
}

// When libcurl refuses an option, its own code is the Result the card shows, whichever option it
// was, and the options after it are not tried.
static void refusedOptions(void) {
	static const struct {
		CURLoption option;
		CURLcode code;
		unsigned position; // among the 24 a verified download sets
	} CASES[] = {
		{CURLOPT_URL, CURLE_OUT_OF_MEMORY, 1},
		{CURLOPT_PROTOCOLS_STR, CURLE_FAILED_INIT, 6},
		// A libcurl that cannot take the setup hook refuses it here, and the download says so
		// instead of failing every check without a cause.
		{CURLOPT_SSL_CTX_FUNCTION, CURLE_FAILED_INIT, 18},
		{CURLOPT_WRITEDATA, CURLE_WRITE_ERROR, 24},
	};
	for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
		unsigned performed = script.performs;
		ciaBody(4096);
		script.failOption = CASES[i].option;
		script.failCode = CASES[i].code;
		start(DL_SINK_CIA, "https://h/a.cia", true);
		drive();
		assert(!dl.ok && strcmp(dl.detail, "net") == 0 && dl.why == DOWNLOAD_CURL(CASES[i].code));
		assert(script.setopts == CASES[i].position && script.performs == performed);
		assert(script.easies == script.cleanups);
	}
}

static void cancelInBody(void);
static void cancels(void) {
	// A cancel before anything arrives: libcurl's progress callback is what stops a connect. This
	// transfer would have failed to connect, so only the callback can have made it a cancel; it is
	// held at the connect until the cancel is in.
	ciaBody(4096);
	script.noBody = true;
	script.result = CURLE_COULDNT_CONNECT;
	script.holdConnect = true;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	driver.cancelAt = DL_STARTING;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "canceled") == 0 && dl.why == TRANSFER_CANCELED);
	assert(armCalls == 0 && pumped == 0 && script.xferCalls == 0);

	// The same Download is started again, as qr.c starts its two again and again. Nothing it ended
	// with, its cancel least of all, carries into the next download. There is no start() this time,
	// because it would zero the very thing under test.
	ciaBody(16384);
	drive();
	assert(dl.ok && dl.detail == NULL && dl.why == 0);
	assert(downloadReceived(&dl) == (u64)cia.extent);

	// While the thread waits to be told the ring is armed or the import is open, only the cancel
	// can end it, because main answers neither.
	ciaBody(4096);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	driver.mute = true;
	driver.cancelAt = DL_SIZED;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "canceled") == 0 && dl.why == TRANSFER_CANCELED);
	assert(armCalls == 0 && pumped == 0);
	ciaBody(4096);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	driver.cancelAt = DL_PREFIXED; // seen after the arm; the open is never answered
	assert(downloadStart(&dl));
	for (int i = 0; i < 100000 && downloadStage(&dl) != DL_PREFIXED; i++) {
		if (downloadStage(&dl) == DL_SIZED && downloadAsking(&dl)) {
			assert(ringArm(downloadSize(&dl), RING_TO_HANDLE, true, 0));
			downloadAnswer(&dl, true);
		}
		usleep(100);
	}
	assert(downloadStage(&dl) == DL_PREFIXED);
	downloadCancel(&dl);
	while (downloadStage(&dl) != DL_FINISHED) {
		usleep(100);
	}
	downloadJoin(&dl);
	assert(!dl.ok && strcmp(dl.detail, "canceled") == 0 && pumped == cia.prefix);

	// While the pump waits on a ring that does not empty (its writer is busy, so no stall is ever
	// declared), only the cancel ends the wait.
	ciaBody(4096);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	fullPolls = 1000000;
	writerBusy = true;
	driver.cancelAt = DL_FETCHING;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "canceled") == 0 && dl.why == TRANSFER_CANCELED);
	assert(pumped == 0 && armCalls == 1);

	// In the body: the transfer holds at its 400th callback (well past the prefix) until the cancel
	// is in. This runs twice: once where libcurl's progress callback sees the cancel, and once with
	// no progress callbacks, where only the write callback's own check can.
	for (int viaProgress = 1; viaProgress >= 0; viaProgress--) {
		ciaBig(100, 100000);
		script.gateAt = 400;
		script.skipXfer = !viaProgress;
		start(DL_SINK_CIA, "https://h/a.cia", true);
		cancelInBody();
	}
}

// Starts the download set up in `dl`, runs to the body, cancels there, and checks that it ended
// there.
static void cancelInBody(void) {
	assert(downloadStart(&dl));
	for (int i = 0; i < 100000 && downloadStage(&dl) != DL_RUNNING; i++) {
		if (downloadStage(&dl) == DL_SIZED && downloadAsking(&dl)) {
			assert(ringArm(downloadSize(&dl), RING_TO_HANDLE, true, 0));
			downloadAnswer(&dl, true);
		}
		if (downloadStage(&dl) == DL_PREFIXED && downloadAsking(&dl)) {
			downloadAnswer(&dl, true);
		}
		usleep(100);
	}
	assert(downloadStage(&dl) == DL_RUNNING);
	downloadCancel(&dl);
	script.gate = true;
	while (downloadStage(&dl) != DL_FINISHED) {
		usleep(100);
	}
	downloadJoin(&dl);
	assert(!dl.ok && strcmp(dl.detail, "canceled") == 0 && dl.why == TRANSFER_CANCELED);
	assert(pumped < cia.prefix + 100000 && pumped > cia.prefix);
	assert(script.easies == script.cleanups);
}

static void codes(void) {
	static const struct {
		CURLcode code;
		long http;
		const char *detail;
		Result why;
	} CASES[] = {
		{CURLE_COULDNT_RESOLVE_HOST, 0, "connect", DOWNLOAD_CURL(CURLE_COULDNT_RESOLVE_HOST)},
		{CURLE_COULDNT_RESOLVE_PROXY, 0, "connect", DOWNLOAD_CURL(CURLE_COULDNT_RESOLVE_PROXY)},
		{CURLE_COULDNT_CONNECT, 0, "connect", DOWNLOAD_CURL(CURLE_COULDNT_CONNECT)},
		{CURLE_OPERATION_TIMEDOUT, 0, "timeout", DOWNLOAD_CURL(CURLE_OPERATION_TIMEDOUT)},
		{CURLE_PEER_FAILED_VERIFICATION, 0, "tls", DOWNLOAD_CURL(CURLE_PEER_FAILED_VERIFICATION)},
		{CURLE_SSL_CACERT_BADFILE, 0, "tls", DOWNLOAD_CURL(CURLE_SSL_CACERT_BADFILE)},
		{CURLE_SSL_CONNECT_ERROR, 0, "tls", DOWNLOAD_CURL(CURLE_SSL_CONNECT_ERROR)},
		{CURLE_TOO_MANY_REDIRECTS, 0, "redirect", DOWNLOAD_CURL(CURLE_TOO_MANY_REDIRECTS)},
		{CURLE_UNSUPPORTED_PROTOCOL, 0, "redirect", DOWNLOAD_CURL(CURLE_UNSUPPORTED_PROTOCOL)},
		{CURLE_GOT_NOTHING, 0, "net", DOWNLOAD_CURL(CURLE_GOT_NOTHING)},
		{CURLE_SEND_ERROR, 0, "net", DOWNLOAD_CURL(CURLE_SEND_ERROR)},
		{CURLE_HTTP_RETURNED_ERROR, 404, "http", DOWNLOAD_HTTP(404)},
		{CURLE_HTTP_RETURNED_ERROR, 403, "http", DOWNLOAD_HTTP(403)},
		{CURLE_HTTP_RETURNED_ERROR, 500, "http", DOWNLOAD_HTTP(500)},
		{CURLE_HTTP_RETURNED_ERROR, 599, "http", DOWNLOAD_HTTP(599)},
		{CURLE_HTTP_RETURNED_ERROR, 400, "http", DOWNLOAD_HTTP(400)},
		// A status outside 100..599 is not an HTTP status, so the code curl gave stands.
		{CURLE_HTTP_RETURNED_ERROR, 0, "http", DOWNLOAD_CURL(CURLE_HTTP_RETURNED_ERROR)},
		{CURLE_HTTP_RETURNED_ERROR, 99, "http", DOWNLOAD_CURL(CURLE_HTTP_RETURNED_ERROR)},
		{CURLE_HTTP_RETURNED_ERROR, 600, "http", DOWNLOAD_CURL(CURLE_HTTP_RETURNED_ERROR)},
	};
	for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
		ciaBody(4096);
		script.noBody = true;
		script.result = CASES[i].code;
		script.http = CASES[i].http;
		// Only a handshake has flags to record. A bad list (BADFILE) is refused before a handshake,
		// and the rest never get as far.
		script.handshake = CASES[i].code == CURLE_PEER_FAILED_VERIFICATION ||
						   CASES[i].code == CURLE_SSL_CONNECT_ERROR;
		script.verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
		start(DL_SINK_CIA, "https://h/a.cia", true);
		drive();
		assert(!dl.ok && strcmp(dl.detail, CASES[i].detail) == 0 && dl.why == CASES[i].why);
		assert(dl.http == CASES[i].http && armCalls == 0);
		assert(script.hopReads == 0); // a hop is asked about only with sslc down (startFailures)
		// The verify flags come from the callback: they are kept for a handshake, and only then.
		assert(dl.verifyResult == (script.handshake ? MBEDTLS_X509_BADCERT_NOT_TRUSTED : 0));
		assert(script.easies == script.cleanups);
	}
	// Every refusal's Result is a failure the card can show.
	assert(R_FAILED(DOWNLOAD_HTTP(404)) && R_FAILED(DOWNLOAD_CURL(CURLE_LAST - 1)));
	assert(R_DESCRIPTION(DOWNLOAD_HTTP(599)) == 0x200 + 499 &&
		   R_DESCRIPTION(DOWNLOAD_HTTP(100)) == 0x200);
}

// What the failed-check screen names reaches it only through the verify callback, because libcurl's
// own answer for it, CURLINFO_SSL_VERIFYRESULT, is 0 with Mbed TLS (the fake returns 0 too). Each
// cause is listed as the flags the callback is given, then certs.c's naming of them, which is the
// wording the screen shows.
static void tlsFlags(void) {
	static const struct {
		long flags;
		TlsCause cause;
		const char *words; // what the cause's sentence holds
	} CAUSES[] = {
		{MBEDTLS_X509_BADCERT_NOT_TRUSTED, TLS_CAUSE_ISSUER, "bundle"},
		{MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH, TLS_CAUSE_ISSUER,
		 "bundle"},
		{MBEDTLS_X509_BADCERT_CN_MISMATCH, TLS_CAUSE_HOST, "another"},
	};
	for (size_t i = 0; i < sizeof(CAUSES) / sizeof(CAUSES[0]); i++) {
		ciaBody(4096);
		script.noBody = true;
		script.result = CURLE_PEER_FAILED_VERIFICATION;
		script.handshake = true;
		script.verifyResult = CAUSES[i].flags;
		start(DL_SINK_CIA, "https://h/a.cia", true);
		drive();
		assert(!dl.ok && strcmp(dl.detail, "tls") == 0 && dl.tls == DL_TLS_CHECK);
		assert(script.sslCtx != NULL && mbed.sets == 1 && mbed.context == &dl);
		assert(dl.verifyResult == CAUSES[i].flags);
		assert(tlsCause(dl.verifyResult) == CAUSES[i].cause);
		assert(strstr(tlsCauseWhy(tlsCause(dl.verifyResult)), CAUSES[i].words));
	}
	// A date flag is not a failure and not a cause. The hook clears it, so a handshake that ends on
	// one alone records no flag, and one with another flag records only that flag.
	static const struct {
		long flags, left;
	} DATES[] = {
		{MBEDTLS_X509_BADCERT_EXPIRED, 0},
		{MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED, 0},
		{MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_NOT_TRUSTED,
		 MBEDTLS_X509_BADCERT_NOT_TRUSTED},
		{MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_CN_MISMATCH,
		 MBEDTLS_X509_BADCERT_CN_MISMATCH},
	};
	for (size_t i = 0; i < sizeof(DATES) / sizeof(DATES[0]); i++) {
		ciaBody(4096);
		script.noBody = true;
		script.result = CURLE_PEER_FAILED_VERIFICATION;
		script.handshake = true;
		script.verifyResult = DATES[i].flags;
		start(DL_SINK_CIA, "https://h/a.cia", true);
		drive();
		assert(dl.tls == DL_TLS_CHECK && dl.verifyResult == DATES[i].left);
	}
	// A redirect's first connection came out clean and its second did not: the flags recorded are
	// the second's.
	ciaBody(4096);
	script.noBody = true;
	script.result = CURLE_PEER_FAILED_VERIFICATION;
	script.handshake = script.earlierHandshake = true;
	script.verifyResult = MBEDTLS_X509_BADCERT_CN_MISMATCH;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	drive();
	assert(dl.tls == DL_TLS_CHECK && mbed.sets == 2); // a hook on each connection
	assert(dl.verifyResult == MBEDTLS_X509_BADCERT_CN_MISMATCH);
	// A handshake that ends on a flag was the check, though libcurl calls it a connect error. One
	// that ends on no flag was not the check, and a failed check that left no flag has no cause to
	// name it.
	static const struct {
		CURLcode code;
		long flags;
		DlTls tls;
	} ENDS[] = {
		{CURLE_SSL_CONNECT_ERROR, MBEDTLS_X509_BADCERT_NOT_TRUSTED, DL_TLS_CHECK},
		{CURLE_SSL_CONNECT_ERROR, 0, DL_TLS_SETUP},
		{CURLE_PEER_FAILED_VERIFICATION, 0, DL_TLS_CHECK},
	};
	for (size_t i = 0; i < sizeof(ENDS) / sizeof(ENDS[0]); i++) {
		ciaBody(4096);
		script.noBody = true;
		script.result = ENDS[i].code;
		script.handshake = true;
		script.verifyResult = ENDS[i].flags;
		start(DL_SINK_CIA, "https://h/a.cia", true);
		drive();
		assert(dl.tls == ENDS[i].tls && dl.verifyResult == ENDS[i].flags);
	}
	// A download with its check off records nothing: no hook, so nothing to name.
	ciaBody(4096);
	script.noBody = true;
	script.result = CURLE_SSL_CONNECT_ERROR;
	script.handshake = true;
	script.verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	start(DL_SINK_CIA, "https://h/a.cia", false);
	drive();
	assert(dl.tls == DL_TLS_SETUP && dl.verifyResult == 0 && script.sslCtx == NULL &&
		   mbed.sets == 0);
}

static void backPressure(void) {
	// The ring's writer is behind for a while: the pump waits it out, and loses nothing.
	ciaBody(4096);
	fullPolls = 0;
	start(DL_SINK_CIA, "https://h/a.cia", true);
	fullPolls = 50;
	busyPolls = 1000;
	drive();
	assert(dl.ok && pumped == cia.extent && memcmp(tier, cia.bytes, (size_t)cia.extent) == 0);

	// A writer that is not busy and still takes nothing is stuck.
	ciaBody(4096);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	fullPolls = 1000000;
	writerBusy = false;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "worker-stall") == 0 && dl.why == TRANSFER_WORKER_STALL);
	assert(pumped < cia.extent);

	// The ring failing under the pump (a write to AM failed) stops the pump, and installClose says
	// why. This runs once with the acquire failing and once with the commit failing; each alone
	// must end the transfer.
	for (int where = 0; where < 2; where++) {
		ciaBig(4096, 100000);
		start(DL_SINK_CIA, "https://h/a.cia", true);
		driver.arm = true;
		assert(downloadStart(&dl));
		for (int i = 0; i < 100000 && downloadStage(&dl) != DL_FINISHED; i++) {
			if (downloadStage(&dl) == DL_SIZED && downloadAsking(&dl)) {
				assert(ringArm(downloadSize(&dl), RING_TO_HANDLE, true, 0));
				downloadAnswer(&dl, true);
			}
			if (downloadStage(&dl) == DL_PREFIXED && downloadAsking(&dl)) {
				acquireFails = where == 0;
				commitFails = where == 1;
				downloadAnswer(&dl, true);
			}
			usleep(100);
		}
		downloadJoin(&dl);
		assert(!dl.ok && strcmp(dl.detail, "writer") == 0 && pumped < cia.prefix + 5000);
	}
}

static const char BUNDLE[] = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static void files(void) {
	mkdir("sdmc:/tmp", 0755);
	const char *path = "sdmc:/tmp/bundle.tmp";
	resetScript();
	script.body = (const u8 *)BUNDLE;
	script.bodyLen = sizeof(BUNDLE) - 1;
	script.length = (curl_off_t)script.bodyLen;
	script.chunk = 7;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(dl.ok && downloadReceived(&dl) == sizeof(BUNDLE) - 1 && armCalls == 0);
	assert(!(driver.seen & (1u << DL_SIZED)) && !(driver.seen & (1u << DL_PREFIXED)));
	FILE *f = fopen(path, "rb");
	char text[256] = "";
	assert(f && fread(text, 1, sizeof(text) - 1, f) == sizeof(BUNDLE) - 1 && fclose(f) == 0);
	assert(strcmp(text, BUNDLE) == 0);
	assert(script.verifyPeer == 0 && script.verifyHost == 0 && !script.hasCaInfo);
	assert(script.sslCtx == NULL && !script.hasBlob); // nothing to record: no check is made
	assert(remove(path) == 0);

	// A refresh of the list the console already has is checked like any download: peer and host
	// verification on, against that list (CAINFO is the card's list), while the body goes to the
	// path it is given (qr.c's CACERT_TEMP, which testQr pins), not over the list.
	resetScript();
	script.body = (const u8 *)BUNDLE;
	script.bodyLen = sizeof(BUNDLE) - 1;
	script.length = (curl_off_t)script.bodyLen;
	script.chunk = 7;
	start(DL_SINK_FILE, CACERT_URL, true);
	dl.path = path;
	drive();
	assert(dl.ok && exists(path));
	assert(script.verifyPeer == 1 && script.verifyHost == 2);
	assert(script.hasCaInfo && strcmp(script.caInfo, CACERT_PATH) == 0 && !script.hasBlob);
	assert(script.sslCtx != NULL && script.sslCtxData == &dl); // the hook that records the flags
	assert(remove(path) == 0);
	// The same fetch against the bundle shipped in the app, handed over in memory through libcurl's
	// own blob option: the bytes are kept by pointer (NOCOPY, never freed or changed by libcurl)
	// and no file is named.
	resetScript();
	script.body = (const u8 *)BUNDLE;
	script.bodyLen = sizeof(BUNDLE) - 1;
	script.length = (curl_off_t)script.bodyLen;
	script.chunk = 7;
	start(DL_SINK_FILE, CACERT_URL, true);
	dl.path = path;
	size_t shippedLen;
	dl.caBlob = certsShipped(&shippedLen);
	dl.caBlobLen = shippedLen;
	drive();
	assert(dl.ok && exists(path));
	assert(script.verifyPeer == 1 && script.verifyHost == 2 && !script.hasCaInfo);
	assert(script.hasBlob && script.blobData == CA_BUNDLE &&
		   script.blobLen == sizeof(CA_BUNDLE) - 1);
	assert(script.blobFlags == CURL_BLOB_NOCOPY);
	assert(script.sslCtx != NULL && script.sslCtxData == &dl);
	assert(remove(path) == 0);
	// A refresh that fails its check ends as a failed check and leaves no file, so nothing that
	// failed the check can be taken for the list.
	resetScript();
	script.noBody = true;
	script.result = CURLE_PEER_FAILED_VERIFICATION;
	script.handshake = true;
	script.verifyResult = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	start(DL_SINK_FILE, CACERT_URL, true);
	dl.path = path;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "tls") == 0 && dl.tls == DL_TLS_CHECK && !exists(path));
	assert(dl.verifyResult == MBEDTLS_X509_BADCERT_NOT_TRUSTED && script.verifyPeer == 1 &&
		   script.hasCaInfo);
	// A list on the card that will not read ends the fetch as an unreadable list, not a failed
	// check.
	resetScript();
	script.noBody = true;
	script.result = CURLE_SSL_CACERT_BADFILE;
	start(DL_SINK_FILE, CACERT_URL, true);
	dl.path = path;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "tls") == 0 && dl.tls == DL_TLS_BUNDLE && !exists(path));

	// A file needs no Content-Length (the bundle's server may chunk it), and its transfer may be
	// any length up to the cap.
	resetScript();
	script.body = (const u8 *)BUNDLE;
	script.bodyLen = sizeof(BUNDLE) - 1;
	script.length = -1;
	script.chunk = 1000;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(dl.ok && exists(path) && remove(path) == 0);

	// A failed transfer leaves no partial file behind.
	resetScript();
	script.body = (const u8 *)BUNDLE;
	script.bodyLen = 20;
	script.length = (curl_off_t)(sizeof(BUNDLE) - 1);
	script.result = CURLE_RECV_ERROR;
	script.chunk = 7;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "net") == 0 && !exists(path));
	// A 404 page is not a bundle.
	resetScript();
	script.noBody = true;
	script.result = CURLE_HTTP_RETURNED_ERROR;
	script.http = 404;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "http") == 0 && !exists(path));

	// The cap: if a server sends more than a bundle can be, the transfer ends, whatever the server
	// says.
	static u8 big[DL_FILE_MAX + 4096];
	memset(big, 'x', sizeof(big));
	resetScript();
	script.body = big;
	script.bodyLen = DL_FILE_MAX;
	script.length = -1;
	script.chunk = 65536;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(dl.ok && downloadReceived(&dl) == DL_FILE_MAX && remove(path) == 0); // exactly the cap
	script.bodyLen = DL_FILE_MAX + 1;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "too-big") == 0 && dl.why == DOWNLOAD_TOO_BIG &&
		   !exists(path));
	script.bodyLen = 100;
	script.length = DL_FILE_MAX + 1; // or says so up front
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(!dl.ok && strcmp(dl.detail, "too-big") == 0 && !exists(path));

	// A card that fills mid-fetch (the host's file-size limit stands in for a full card): the first
	// write that comes up short ends the fetch there, not at the end of the body, and the partial
	// file is removed.
	struct rlimit keep, tight;
	assert(getrlimit(RLIMIT_FSIZE, &keep) == 0);
	tight = keep;
	tight.rlim_cur = 10000;
	signal(SIGXFSZ, SIG_IGN); // a write past the limit is an error here, not a signal
	assert(setrlimit(RLIMIT_FSIZE, &tight) == 0);
	resetScript();
	script.body = big;
	script.bodyLen = 300000;
	script.length = -1;
	script.chunk = 16384;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	drive();
	assert(setrlimit(RLIMIT_FSIZE, &keep) == 0);
	assert(!dl.ok && strcmp(dl.detail, "write") == 0 && dl.why == DOWNLOAD_NO_FILE &&
		   !exists(path));
	assert(downloadReceived(&dl) <= 16384 && script.xferCalls <= 2);

	// A path that cannot be written is reported, and nothing is fetched into the void.
	resetScript();
	script.body = (const u8 *)BUNDLE;
	script.bodyLen = sizeof(BUNDLE) - 1;
	script.length = (curl_off_t)script.bodyLen;
	script.chunk = 7;
	start(DL_SINK_FILE, CACERT_URL, false);
	unsigned performed = script.performs;
	dl.path = "sdmc:/no/such/folder/bundle.tmp";
	drive();
	assert(!dl.ok && strcmp(dl.detail, "write") == 0 && script.performs == performed);
	// A cancel in the middle of a file ends it, and removes it.
	resetScript();
	script.body = (const u8 *)BUNDLE;
	script.bodyLen = sizeof(BUNDLE) - 1;
	script.length = (curl_off_t)script.bodyLen;
	script.chunk = 3;
	script.gateAt = 5;
	start(DL_SINK_FILE, CACERT_URL, false);
	dl.path = path;
	assert(downloadStart(&dl));
	while (downloadReceived(&dl) < 15) {
		usleep(100);
	}
	downloadCancel(&dl);
	script.gate = true;
	while (downloadStage(&dl) != DL_FINISHED) {
		usleep(100);
	}
	downloadJoin(&dl);
	assert(!dl.ok && strcmp(dl.detail, "canceled") == 0 && !exists(path));
}

static void lifecycle(void) {
	// If the thread cannot be made, that is reported at once, the download finishes, and nothing is
	// left to join.
	noThread = true;
	ciaBody(4096);
	start(DL_SINK_CIA, "https://h/a.cia", true);
	assert(!downloadStart(&dl) && downloadStage(&dl) == DL_FINISHED);
	assert(!dl.ok && strcmp(dl.detail, "thread") == 0 && dl.why == DOWNLOAD_NO_THREAD);
	assert(script.performs == 0 || script.easies == script.cleanups);
	downloadJoin(&dl);
	noThread = false;

	// libcurl and sslc start together and stop together.
	script.globals = script.globalCleanups = 0;
	fake.sslcInits = fake.sslcExits = 0;
	assert(downloadInit() == 0 && fake.sslcInits == 1 && script.globals == 1 && downloadTlsUp());
	downloadExit();
	assert(fake.sslcExits == 1 && script.globalCleanups == 1 && !downloadTlsUp());
}

// A refusal made before libcurl is touched: the handle is never made, nothing is performed.
static void refusedUnstarted(DlSink sink, const char *url, bool verify, const char *detail,
							 Result why) {
	unsigned easies = script.easies, performs = script.performs;
	ciaBody(4096);
	start(sink, url, verify);
	dl.path = "sdmc:/tmp/bundle.tmp";
	drive();
	assert(!dl.ok && strcmp(dl.detail, detail) == 0 && dl.why == why && dl.tls == DL_TLS_NONE);
	assert(script.easies == easies && script.performs == performs && !exists(dl.path));
}

// What the screen shows of a link is libcurl's reading of its host (and port, when the link names
// one), taken from libcurl's parse of the whole link, with nothing read from the text here.
static void siteIs(const char *link, const char *host, const char *port, const char *want,
				   size_t cap) {
	char out[160] = "unset";
	memset(&urls, 0, sizeof(urls));
	urls.host = host;
	urls.port = port;
	bool ok = downloadSite(link, out, cap);
	assert(strcmp(urls.set, link) == 0 || urls.handles == 0);
	assert(urls.handles == urls.cleanups && urls.strings == urls.frees); // nothing left allocated
	assert(want ? ok && strcmp(out, want) == 0 : !ok);
}

static void sites(void) {
	// The answer is the host libcurl names, whatever the link's text puts before it.
	siteIs("https://good.example@evil.example/x.cia", "evil.example", NULL, "evil.example", 160);
	assert(strcmp(urls.set, "https://good.example@evil.example/x.cia") == 0 && urls.handles == 1);
	// A port the link names is part of the site; the scheme's own, which libcurl does not
	// report, is not.
	siteIs("http://192.168.1.5:8000/a.cia", "192.168.1.5", "8000", "192.168.1.5:8000", 160);
	siteIs("[::1]:8000", "[::1]", "8000", "[::1]:8000", 160);
	// Never cut: a name that just fits is whole, and a name one character longer gets no answer.
	siteIs("https://h/x", "0123456789", NULL, "0123456789", 11);
	siteIs("https://h/x", "0123456789", NULL, NULL, 10);
	siteIs("https://h/x", "0123456789", "443", "0123456789:443", 15);
	siteIs("https://h/x", "0123456789", "443", NULL, 14);
	// If the parse cannot hand over a port, there is no site: a row without one would say the link
	// names none, and this link names 8443.
	memset(&urls, 0, sizeof(urls));
	urls.host = "h";
	urls.portFails = true;
	assert(!downloadSite("https://h:8443/x.cia", (char[32]){0}, 32));
	assert(urls.cleanups == urls.handles && urls.strings == urls.frees);
	// A link that libcurl cannot read, finds no host in, or returns an empty host for gives no
	// site; and no handle at all gives none either.
	urls.setFails = true;
	assert(!downloadSite("https://h/x", (char[16]){0}, 16) && urls.cleanups == urls.handles);
	siteIs("https://h/x", NULL, NULL, NULL, 160);
	siteIs("https://h/x", "", NULL, NULL, 160);
	memset(&urls, 0, sizeof(urls));
	urls.hostFails = true;
	assert(!downloadSite("https://h/x", (char[16]){0}, 16) && urls.cleanups == urls.handles);
	memset(&urls, 0, sizeof(urls));
	urls.noHandle = true;
	assert(!downloadSite("https://h/x", (char[16]){0}, 16) && urls.handles == 0);
}

// What the screen shows of a link's path: libcurl's reading of it, as written, and whether the link
// has a query, both taken from the parse of the whole link. The query's text is not part of the
// answer.
static void pathIs(const char *link, const char *path, const char *query, const char *want,
				   bool wantQuery, size_t cap) {
	char out[1100] = "unset";
	bool hasQuery = true; // a failed answer must clear it
	memset(&urls, 0, sizeof(urls));
	urls.path = path;
	urls.query = query;
	bool ok = downloadPath(link, out, cap, &hasQuery);
	assert(strcmp(urls.set, link) == 0 || urls.handles == 0);
	assert(urls.handles == urls.cleanups && urls.strings == urls.frees); // nothing left allocated
	assert(want ? ok && strcmp(out, want) == 0 && hasQuery == wantQuery : !ok && !hasQuery);
}

static void paths(void) {
	// The answer is the path libcurl names for the whole link; the owner and the repository of a
	// GitHub link are in it.
	static const char RELEASE[] = "/MadeOfBees/3Drop/releases/download/v1/3drop.cia";
	pathIs("https://github.com/MadeOfBees/3Drop/releases/download/v1/3drop.cia", RELEASE, NULL,
		   RELEASE, false, 1100);
	assert(strcmp(urls.set, "https://github.com/MadeOfBees/3Drop/releases/download/v1/3drop.cia") ==
			   0 &&
		   urls.handles == 1);
	// A query is noticed but never shown, and its text is not part of the answer. A bare `?` counts
	// as one: libcurl returns it as an empty query.
	pathIs("https://h/a.cia?token=SECRET", "/a.cia", "token=SECRET", "/a.cia", true, 1100);
	pathIs("https://h/a.cia?", "/a.cia", "", "/a.cia", true, 1100);
	// A percent code is shown as written, not decoded: %2F is not a slash and %70 is not the letter
	// p.
	pathIs("https://h/a%2Fb/%70.cia", "/a%2Fb/%70.cia", NULL, "/a%2Fb/%70.cia", false, 1100);
	pathIs("https://h/a%2Fb?x=%41", "/a%2Fb", "x=%41", "/a%2Fb", true, 1100);
	// Never cut: a path that just fits is whole; a path one character longer gets no answer, and no
	// query either.
	pathIs("https://h/x", "/123456789", NULL, "/123456789", false, 11);
	pathIs("https://h/x", "/123456789", "q", "/123456789", true, 11);
	pathIs("https://h/x", "/123456789", NULL, NULL, false, 10);
	pathIs("https://h/x", "/123456789", "q", NULL, false, 10);
	// A link that libcurl cannot read, or gives no path for, gets no answer and no query; and no
	// handle at all gives none either.
	pathIs("https://h/x", "/x", "q", "/x", true, 1100);
	memset(&urls, 0, sizeof(urls));
	urls.setFails = true;
	urls.path = "/x";
	urls.query = "q";
	bool hasQuery = true;
	assert(!downloadPath("https://h/x", (char[16]){0}, 16, &hasQuery) && !hasQuery);
	assert(urls.cleanups == urls.handles && urls.strings == urls.frees);
	// The same goes for the query's mark: a link whose query cannot be read is refused, not shown
	// as one without a query.
	memset(&urls, 0, sizeof(urls));
	urls.path = "/x";
	urls.queryFails = true;
	hasQuery = true;
	assert(!downloadPath("https://h/x?token=SECRET", (char[16]){0}, 16, &hasQuery) && !hasQuery);
	assert(urls.cleanups == urls.handles && urls.strings == urls.frees);
	memset(&urls, 0, sizeof(urls));
	urls.pathFails = true;
	urls.query = "q";
	hasQuery = true;
	assert(!downloadPath("https://h/x", (char[16]){0}, 16, &hasQuery) && !hasQuery);
	assert(urls.cleanups == urls.handles && urls.strings == urls.frees);
	memset(&urls, 0, sizeof(urls));
	urls.noHandle = true;
	hasQuery = true;
	assert(!downloadPath("https://h/x", (char[16]){0}, 16, &hasQuery) && !hasQuery);
	assert(urls.handles == 0);
}

static void startFailures(void) {
	// If sslc fails, https stays unavailable for good. An https link is refused with its own
	// message, not as a failed check (nothing is tried, so there is nothing for a Y to skip),
	// whether or not the check was skipped, and so is the bundle's own https fetch.
	fake.sslcInits = fake.sslcExits = 0;
	script.globals = script.globalCleanups = 0;
	fake.sslcResult = (Result)0xD8E0806A;
	assert(downloadInit() == DOWNLOAD_NO_TLS && script.globals == 1 && !downloadTlsUp());
	refusedUnstarted(DL_SINK_CIA, "https://example.com/a.cia", true, "no-tls", DOWNLOAD_NO_TLS);
	refusedUnstarted(DL_SINK_CIA, "https://example.com/a.cia", false, "no-tls", DOWNLOAD_NO_TLS);
	refusedUnstarted(DL_SINK_CIA, "HTTPS://example.com/a.cia", true, "no-tls", DOWNLOAD_NO_TLS);
	refusedUnstarted(DL_SINK_FILE, CACERT_URL, false, "no-tls", DOWNLOAD_NO_TLS);
	// Plain http goes on working, held to http: no https hop is allowed, and none is tried.
	ciaBody(16384);
	start(DL_SINK_CIA, "http://192.168.1.5:8000/a.cia", true);
	drive();
	assert(dl.ok && strcmp(script.protocols, "http") == 0 && strcmp(script.redirects, "http") == 0);
	// When the server sends an http download on to https, libcurl refuses the hop as an unsupported
	// protocol. The missing SSL service is what is reported, not a loop (codes() has the same code
	// with the service up, and that one is a redirect). A hop to ftp or file is refused with the
	// service up too, so it is reported as a redirect here as well. libcurl's effective URL says
	// where the hop went, and the thread asks for it once, on this error alone.
	static const struct {
		const char *hop; // NULL: libcurl returns nothing
		const char *detail;
		Result why;
	} HOPS[] = {
		{"ftp://192.168.1.5/a.cia", "redirect", DOWNLOAD_CURL(CURLE_UNSUPPORTED_PROTOCOL)},
		{"file:///sdmc/a.cia", "redirect", DOWNLOAD_CURL(CURLE_UNSUPPORTED_PROTOCOL)},
		{"https://192.168.1.5:8000/a.cia", "no-tls", DOWNLOAD_NO_TLS},
		{"", "redirect", DOWNLOAD_CURL(CURLE_UNSUPPORTED_PROTOCOL)},
		{NULL, "redirect", DOWNLOAD_CURL(CURLE_UNSUPPORTED_PROTOCOL)},
	};
	for (size_t i = 0; i < sizeof(HOPS) / sizeof(HOPS[0]); i++) {
		ciaBody(16384);
		script.noBody = true;
		script.result = CURLE_UNSUPPORTED_PROTOCOL;
		script.hopKnown = HOPS[i].hop != NULL;
		snprintf(script.hop, sizeof(script.hop), "%s", HOPS[i].hop ? HOPS[i].hop : "");
		start(DL_SINK_CIA, "http://192.168.1.5:8000/a.cia", true);
		drive();
		assert(!dl.ok && strcmp(dl.detail, HOPS[i].detail) == 0 && dl.why == HOPS[i].why);
		assert(dl.tls == DL_TLS_NONE && script.hopReads == 1);
	}
	// A loop is a loop wherever the last hop pointed, and is not asked about.
	ciaBody(16384);
	script.noBody = true;
	script.result = CURLE_TOO_MANY_REDIRECTS;
	script.hopKnown = true;
	snprintf(script.hop, sizeof(script.hop), "https://192.168.1.5:8000/a.cia");
	start(DL_SINK_CIA, "http://192.168.1.5:8000/a.cia", true);
	drive();
	assert(!dl.ok && strcmp(dl.detail, "redirect") == 0 && script.hopReads == 0);
	assert(dl.why == DOWNLOAD_CURL(CURLE_TOO_MANY_REDIRECTS));
	downloadExit(); // still stops libcurl, and sslc only if it started
	assert(fake.sslcExits == 0 && script.globalCleanups == 1);
	fake.sslcResult = 0;

	// A libcurl failure is reported ahead of sslc's, and it lets nothing in, not even plain http.
	script.globalFails = true;
	assert(downloadInit() == DOWNLOAD_NO_CURL && !downloadTlsUp());
	refusedUnstarted(DL_SINK_CIA, "http://192.168.1.5:8000/a.cia", true, "no-curl",
					 DOWNLOAD_NO_CURL);
	refusedUnstarted(DL_SINK_CIA, "https://example.com/a.cia", true, "no-curl", DOWNLOAD_NO_CURL);
	// Both failed (the script was reset since, so libcurl fails again), and libcurl's failure is
	// the one reported.
	fake.sslcResult = (Result)0xD8E0806A;
	script.globalFails = true;
	assert(downloadInit() == DOWNLOAD_NO_CURL);
	script.globalFails = false;
	unsigned cleanups = script.globalCleanups;
	downloadExit(); // libcurl never started: nothing for it to clean up
	assert(script.globalCleanups == cleanups);
	fake.sslcResult = 0;

	// Both services are up again, as the first scan of the next run finds them.
	assert(downloadInit() == 0 && downloadTlsUp());
}

int main(void) {
	cia = build(0x00010004, 0x00010004, 0x0004000003D37200ULL, 1, 1);
	ringTier1();
	assert(downloadInit() == 0); // as the first scan does: both services up before any link
	options();
	cias();
	answers();
	refusals();
	refusedOptions();
	cancels();
	codes();
	tlsFlags();
	backPressure();
	files();
	sites();
	paths();
	startFailures();
	lifecycle();
	TEST_PASS();
	return 0;
}

// cc: src/certs.c src/urls.c src/fs.c
// cc: -lmbedx509
// The TLS policy for a QR download and the certificate bundle it checks against (Universal
// Updater's sdmc:/config/ssl/cacert.pem). Covers which URLs are checked and which are not, what a
// failed check names, and keeping the bundle's file whole through a publish.
#include "fakes.h"

#include "caRoots.h" // PEM_OK's two real certificates
#include "certs.h"
#include "download.h" // DL_FILE_MAX
#include "fs.h"
#include "urls.h" // urlIsHttps

#include <assert.h>
#include <errno.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

// On the test's card the path is the card's root plus the FS spelling.
#define HOST(fsPath) "sdmc:" fsPath

// A bundle that Mbed TLS can read: two roots from Mozilla's bundle, with the comments curl.se's
// file puts before its first certificate. A marker pair around something else is a non-bundle
// (CA_JUNK), not a bundle, because the publish keeps only a list that the library can read.
#define PEM_OK "##\n## Bundle of CA Root Certificates\n##\n\n" CA_ROOTS

static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static char *slurp(const char *path) {
	static char text[8192];
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	text[n] = '\0';
	fclose(f);
	return text;
}

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static void policy(void) {
	// Plain http is allowed, as in FBI and Universal Updater. Any https connection that such a
	// download makes, including a redirect, is checked all the same, and only a Y turns that off.
	// An http link doesn't need the list on the card before it starts.
	TlsPolicy p = tlsPolicyFor("http://host/a.cia", true, false);
	assert(p.allowed && p.verify && !p.needBundle);
	p = tlsPolicyFor("HTTP://host/a.cia", true, false);
	assert(p.allowed && p.verify && !p.needBundle);
	p = tlsPolicyFor("HTTP://host/a.cia", true, true);
	assert(p.allowed && !p.verify && !p.needBundle);
	// Strict mode is this argument alone: with it, the same URL is refused.
	p = tlsPolicyFor("http://host/a.cia", false, false);
	assert(!p.allowed);
	p = tlsPolicyFor("https://host/a.cia", false, false); // strict mode never refuses https
	assert(p.allowed && p.verify && p.needBundle);
	// https is checked and needs the list at once, unless Y turned that off for this download.
	p = tlsPolicyFor("https://host/a.cia", true, false);
	assert(p.allowed && p.verify && p.needBundle);
	p = tlsPolicyFor("HTTPS://host/a.cia", true, false);
	assert(p.allowed && p.verify && p.needBundle);
	p = tlsPolicyFor("https://host/a.cia", true, true);
	assert(p.allowed && !p.verify && !p.needBundle);
	// What the build ships: plain http is allowed, and a single macro changes that.
	assert(TLS_ALLOW_PLAIN_HTTP == 1);
	p = tlsPolicy("http://host/a.cia", false);
	assert(p.allowed == (TLS_ALLOW_PLAIN_HTTP != 0) && p.verify);
	p = tlsPolicy("https://host/a.cia", false);
	assert(p.allowed && p.verify && p.needBundle);
	p = tlsPolicy("https://host/a.cia", true);
	assert(p.allowed && !p.verify && !p.needBundle);
	// The bundle's own fetch has no policy of its own to pin here, because it is always checked,
	// against the card's list or the shipped bundle (qr.c's fetchBundle sets verify, and testQr
	// pins it).
	assert(urlIsHttps(CACERT_URL));
	assert(strcmp(CACERT_PATH, "sdmc:/config/ssl/cacert.pem") == 0);
	// A redirect from https never leads to http; a redirect from http can go to either.
	assert(strcmp(tlsRedirects("https://host/a.cia", true), "https") == 0);
	assert(strcmp(tlsRedirects("HTTPS://host/a.cia", true), "https") == 0);
	assert(strcmp(tlsRedirects("http://host/a.cia", true), "http,https") == 0);
	// If the SSL service did not start, an http link is held to http, redirects included.
	assert(strcmp(tlsRedirects("http://host/a.cia", false), "http") == 0);
	assert(strcmp(tlsRedirects("https://host/a.cia", false), "https") == 0);
}

static void causes(void) {
	// The date flags name no cause. The hook clears them, so one that got here did not come from
	// the check.
	assert(tlsCause(MBEDTLS_X509_BADCERT_EXPIRED) == TLS_CAUSE_OTHER);
	assert(tlsCause(MBEDTLS_X509_BADCERT_FUTURE) == TLS_CAUSE_OTHER);
	assert(tlsCause(MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_NOT_TRUSTED) ==
		   TLS_CAUSE_ISSUER);
	assert(tlsCause(MBEDTLS_X509_BADCERT_NOT_TRUSTED) == TLS_CAUSE_ISSUER);
	assert(tlsCause(MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH) ==
		   TLS_CAUSE_ISSUER);
	assert(tlsCause(MBEDTLS_X509_BADCERT_CN_MISMATCH) == TLS_CAUSE_HOST);
	assert(tlsCause(0) == TLS_CAUSE_OTHER); // no flag, so the chain was not what failed
	assert(tlsCause(MBEDTLS_X509_BADCERT_REVOKED) == TLS_CAUSE_OTHER);
	// Every cause has a message, and each one names its own thing.
	for (int c = 0; c <= TLS_CAUSE_OTHER; c++) {
		assert(tlsCauseWhy((TlsCause)c)[0] != '\0');
	}
	assert(strstr(tlsCauseWhy(TLS_CAUSE_ISSUER), "bundle"));
	assert(strstr(tlsCauseWhy(TLS_CAUSE_HOST), "another"));
	assert(strcmp(tlsCauseWhy(TLS_CAUSE_HOST), tlsCauseWhy(TLS_CAUSE_ISSUER)) != 0);
}

static void bundle(void) {
	Result why;
	// No bundle yet. The folder doesn't exist either, and the first publish creates it.
	assert(!certsPresent());
	assert(!exists(HOST("/config")));
	// If a file has the folder's name, the folder is not created, and that is reported instead of
	// papered over.
	put(HOST("/config"), "a file\n");
	assert(!certsMakeDir());
	assert(remove(HOST("/config")) == 0);
	assert(certsMakeDir() && certsMakeDir()); // twice: an existing folder is not a failure
	assert(exists(HOST(CACERT_DIR_FS)));
	assert(!certsPresent());
	// An empty file is not a bundle. Universal Updater requires that the file exist, and an empty
	// file fails every handshake as an invalid bundle.
	put(CACERT_PATH, "");
	assert(!certsPresent());
	put(CACERT_PATH, PEM_OK);
	assert(certsPresent());
	assert(remove(CACERT_PATH) == 0);
	// A folder with the bundle's name is not a bundle either.
	assert(mkdir(CACERT_PATH, 0755) == 0 && !certsPresent() && rmdir(CACERT_PATH) == 0);

	// A fetched bundle is published whole, or not at all.
	put(CACERT_TEMP, PEM_OK "second\n");
	assert(certsPublish(&why) && why == 0);
	assert(!exists(CACERT_TEMP) && !exists(CACERT_PATH ".old"));
	assert(strcmp(slurp(CACERT_PATH), PEM_OK "second\n") == 0 && certsPresent());

	// When a bundle already exists (a refetch), FS refuses a rename onto an existing name, so the
	// old file moves aside first and the new one replaces it.
	put(CACERT_TEMP, PEM_OK "third\n");
	assert(certsPublish(&why));
	assert(strcmp(slurp(CACERT_PATH), PEM_OK "third\n") == 0);
	assert(!exists(CACERT_TEMP) && !exists(CACERT_PATH ".old"));
	// A leftover from a publish that died between its two renames does not stop the next.
	put(CACERT_PATH ".old", "left behind\n");
	put(CACERT_TEMP, PEM_OK "fourth\n");
	assert(certsPublish(&why) && strcmp(slurp(CACERT_PATH), PEM_OK "fourth\n") == 0);
	assert(!exists(CACERT_PATH ".old"));

	// Anything that is not a bundle replaces nothing: a captive portal's page, a cut file, an empty
	// file.
	static const char CUT[] = "-----BEGIN CERTIFICATE-----\nMIIB\n"; // cut before its end
	static const char *const NOT_BUNDLES[] = {
		"<html><body>Log in to the Wi-Fi</body></html>\n",
		// The markers of a bundle around something that is not one. The marker scan passes it and
		// the library refuses it, which is what keeps a list like this off the card.
		CA_JUNK,
		CUT,
		"-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\n", // in the wrong order
		"<html>\n-----END CERTIFICATE-----\n",						// an end with no beginning
		"",
		// A download that stopped inside its last certificate is cut, however many whole ones came
		// before it. A bundle missing its tail is not the bundle the person asked for.
		PEM_OK "-----BEGIN CERTIFICATE-----\nMIIB\n",
		PEM_OK PEM_OK "-----BEGIN CERTIFICATE-----\nMIIB",
		PEM_OK "-----BEGIN CERTIFICATE-----", // cut inside the marker's line, no newline
		// A certificate that begins inside another has lost the end of the first.
		"-----BEGIN CERTIFICATE-----\nMIIB\n-----BEGIN CERTIFICATE-----\nMIIB\n"
		"-----END CERTIFICATE-----\n",
		PEM_OK "-----BEGIN CERTIFICATE-----\nMIIB\n-----BEGIN CERTIFICATE-----\nMIIB\n"
			   "-----END CERTIFICATE-----\n",
		// An end after the last whole certificate belongs to no certificate.
		PEM_OK "-----END CERTIFICATE-----\n",
		PEM_OK "<html>\n-----END CERTIFICATE-----\n",
	};
	for (size_t i = 0; i < sizeof(NOT_BUNDLES) / sizeof(NOT_BUNDLES[0]); i++) {
		put(CACERT_TEMP, NOT_BUNDLES[i]);
		assert(!certsPublish(&why) && why == STAGE_METADATA);
		assert(strcmp(slurp(CACERT_PATH), PEM_OK "fourth\n") == 0); // the old one stands
		assert(!exists(CACERT_TEMP) && !exists(CACERT_PATH ".old"));
	}
	// Two whole certificates, with the comments and blank lines between them that curl.se's file
	// has, and a trailing note after the last, make a bundle. (Only two, because three of these
	// literals would exceed the length ISO C requires a compiler to support.)
	static const char *const BUNDLES[] = {
		PEM_OK PEM_OK,
		PEM_OK "\n## next\n" PEM_OK "\n\n## end of file\n",
		PEM_OK "\n-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----",
		// A certificate that this Mbed TLS cannot read, after ones it can. The load returns the
		// count, not an error, and curl accepts such a file, so the publish keeps it rather than
		// refusing a bundle that is one certificate short of what its author meant.
		PEM_OK CA_JUNK,
	};
	for (size_t i = 0; i < sizeof(BUNDLES) / sizeof(BUNDLES[0]); i++) {
		put(CACERT_TEMP, BUNDLES[i]);
		assert(certsPublish(&why) && strcmp(slurp(CACERT_PATH), BUNDLES[i]) == 0);
		assert(!exists(CACERT_TEMP) && !exists(CACERT_PATH ".old"));
	}
	put(CACERT_TEMP, PEM_OK "fourth\n");
	assert(certsPublish(&why));
	// A cut at the end of a long bundle is found too, even when the file is longer than one read
	// can hold.
	FILE *big = fopen(CACERT_TEMP, "wb");
	assert(big);
	for (int i = 0; i < 4000; i++) {
		fputs("## a comment line that is not a certificate, as the bundle has many\n", big);
	}
	fputs(PEM_OK, big);
	fputs(CUT, big);
	assert(fclose(big) == 0 && !certsPublish(&why));
	assert(strcmp(slurp(CACERT_PATH), PEM_OK "fourth\n") == 0 && !exists(CACERT_TEMP));
	// With no temp file there is nothing to publish, and the old bundle stays.
	assert(!certsPublish(&why) && strcmp(slurp(CACERT_PATH), PEM_OK "fourth\n") == 0);

	// One certificate is a list as much as a hundred are. What makes it a list is that the library
	// can read it.
	put(CACERT_TEMP, CA_ROOT_AMAZON3);
	assert(certsPublish(&why) && strcmp(slurp(CACERT_PATH), CA_ROOT_AMAZON3) == 0);
	assert(!exists(CACERT_TEMP) && !exists(CACERT_PATH ".old"));

	// A bundle larger than the scan's first page still passes, because its marker may be anywhere
	// in it.
	FILE *f = fopen(CACERT_TEMP, "wb");
	assert(f);
	for (int i = 0; i < 4000; i++) {
		fputs("## a comment line that is not a certificate, as the bundle has many\n", f);
	}
	fputs(PEM_OK, f);
	assert(fclose(f) == 0 && certsPublish(&why) && certsPresent());

	// If the temp file's move into place fails after the old bundle has moved aside, the old one is
	// put back and the temp file is dropped.
	assert(remove(CACERT_PATH) == 0);
	put(CACERT_PATH, PEM_OK "kept\n");
	put(CACERT_TEMP, PEM_OK "new\n");
	fake.renameResult = (Result)0xC8804464;
	fake.renameFrom = "cacert.pem.tmp";
	assert(!certsPublish(&why) && why == (Result)0xC8804464); // the card's own Result, not a page's
	assert(strcmp(slurp(CACERT_PATH), PEM_OK "kept\n") == 0);
	assert(!exists(CACERT_TEMP) && !exists(CACERT_PATH ".old"));
	fake.renameResult = 0;
	fake.renameFrom = NULL;
}

// ---- The bundle the app ships: res/cacert.pem, read from the tree ----
#define SHIPPED TESTS_DIR "/../../res/cacert.pem"
#define HEADER "##\n## Bundle of CA Root Certificates\n##\n## Certificate data from Mozilla as of: "
#define BEGIN_MARK "-----BEGIN CERTIFICATE-----"
#define END_MARK "-----END CERTIFICATE-----"

// Reads a whole file and adds the NUL after it that Mbed TLS's PEM reader wants (libcurl makes its
// own NUL-terminated copy of a blob for the same reason).
static char *readAll(const char *path, size_t *length) {
	FILE *f = fopen(path, "rb");
	assert(f && fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
	char *text = malloc((size_t)size + 1);
	assert(text && fread(text, 1, (size_t)size, f) == (size_t)size);
	text[size] = '\0';
	fclose(f);
	*length = (size_t)size;
	return text;
}

// Returns the same list with the text of every certificate replaced. The markers still balance, but
// Mbed TLS can read none of it.
static char *garbled(const char *pem, size_t length) {
	char *out = malloc(length + 1), *at = out;
	bool inside = false;
	for (const char *line = pem; *line;) {
		size_t n = strcspn(line, "\n");
		n += line[n] == '\n';
		bool end = strncmp(line, END_MARK, sizeof(END_MARK) - 1) == 0;
		if (inside && !end) {
			memcpy(at, "!!!!\n", 5); // never longer than the line it replaces
			at += 5;
		} else {
			memcpy(at, line, n);
			at += n;
		}
		inside = end ? false : inside || strncmp(line, BEGIN_MARK, sizeof(BEGIN_MARK) - 1) == 0;
		line += n;
	}
	*at = '\0';
	return out;
}

static void shipped(void) {
	Result why;
	size_t length, recorded;
	// certsShipped hands over the two symbols src/caBundle.S gives the app; here they are the
	// test's own.
	assert(certsShipped(&length) == CA_BUNDLE && length == sizeof(CA_ROOTS) - 1);

	// The file itself: its header is intact, the checksum file beside it matches it, and Mbed TLS
	// can read every certificate in it, so a refresh of it is a list the app's own check accepts.
	char *pem = readAll(SHIPPED, &length);
	assert(strncmp(pem, HEADER, sizeof(HEADER) - 1) == 0);
	unsigned char digest[32];
	assert(mbedtls_sha256_ret((const unsigned char *)pem, length, digest, 0) == 0);
	char hex[65];
	for (size_t i = 0; i < sizeof(digest); i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
	char *record = readAll(SHIPPED ".sha256", &recorded);
	assert(strncmp(record, hex, 64) == 0 && strcmp(record + 64, "  cacert.pem\n") == 0);
	free(record);
	unsigned certificates = 0;
	for (const char *at = pem; (at = strstr(at, BEGIN_MARK)) != NULL; at++) {
		certificates++;
	}
	assert(certificates > 100 && length > 100000 && length < DL_FILE_MAX);
	mbedtls_x509_crt chain;
	mbedtls_x509_crt_init(&chain);
	assert(mbedtls_x509_crt_parse(&chain, (const unsigned char *)pem, length + 1) == 0); // all
	mbedtls_x509_crt_free(&chain);

	// A refresh that is the same file is a list the publish accepts.
	assert(remove(CACERT_PATH) == 0);
	put(CACERT_TEMP, pem);
	assert(certsPublish(&why) && why == 0 && certsPresent());
	size_t published;
	char *card = readAll(CACERT_PATH, &published);
	assert(published == length && memcmp(card, pem, length) == 0);
	free(card);

	// A list whose markers balance but which Mbed TLS cannot read is never published, whether the
	// card has a list (which stays) or none (which stays without one). A list that will not read
	// would send the next fetch down a road the card's own list cannot vouch for.
	char *junk = garbled(pem, length);
	assert(strstr(junk, BEGIN_MARK) && strstr(junk, END_MARK));
	put(CACERT_TEMP, junk);
	assert(!certsPublish(&why) && why == STAGE_METADATA && !exists(CACERT_TEMP));
	card = readAll(CACERT_PATH, &published);
	assert(published == length && memcmp(card, pem, length) == 0);
	free(card);
	assert(remove(CACERT_PATH) == 0);
	put(CACERT_TEMP, junk);
	assert(!certsPublish(&why) && why == STAGE_METADATA);
	assert(!exists(CACERT_PATH) && !exists(CACERT_TEMP) && !certsPresent());
	free(junk);
	// The real list cut inside its last certificate is not whole either.
	char *lastEnd = pem;
	for (char *at = pem; (at = strstr(at, END_MARK)) != NULL; at++) {
		lastEnd = at;
	}
	assert(lastEnd != pem);
	*lastEnd = '\0';
	put(CACERT_TEMP, pem);
	assert(!certsPublish(&why) && why == STAGE_METADATA && !certsPresent());
	free(pem);
}

int main(void) {
	policy();
	causes();
	bundle();
	shipped();
	TEST_PASS();
	return 0;
}

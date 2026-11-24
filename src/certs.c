// SPDX-License-Identifier: GPL-3.0-or-later
#include "certs.h"

#include "fs.h"
#include "urls.h" // urlIsHttps

#include <mbedtls/x509.h>	  // the flags download.c's verify callback records
#include <mbedtls/x509_crt.h> // the list is published only if this reads it
#include <stdio.h>
#include <string.h>
#include <strings.h>

TlsPolicy tlsPolicyFor(const char *url, bool plainAllowed, bool skipCheck) {
	bool https = urlIsHttps(url);
	// An http link gets no list fetched up front, because it may never leave http. If it is sent on
	// to https with none on the card, the transfer ends as DL_TLS_BUNDLE and the person is asked.
	return (TlsPolicy){
		.allowed = https || plainAllowed, .verify = !skipCheck, .needBundle = https && !skipCheck};
}

extern const unsigned char CA_BUNDLE[]; // src/caBundle.S
extern const unsigned CA_BUNDLE_SIZE;

const void *certsShipped(size_t *length) {
	*length = CA_BUNDLE_SIZE;
	return CA_BUNDLE;
}

const char *tlsRedirects(const char *url, bool tlsUp) {
	return urlIsHttps(url) ? "https" : tlsUp ? "http,https" : "http";
}

TlsCause tlsCause(long verifyResult) {
	if (verifyResult & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
		return TLS_CAUSE_ISSUER;
	}
	return (verifyResult & MBEDTLS_X509_BADCERT_CN_MISMATCH) ? TLS_CAUSE_HOST : TLS_CAUSE_OTHER;
}

const char *tlsCauseWhy(TlsCause cause) {
	switch (cause) {
	case TLS_CAUSE_ISSUER:
		return "The site's certificate is not signed by anyone in the certificate bundle.";
	case TLS_CAUSE_HOST:
		return "The certificate is for another site than the link's.";
	default:
		return "The site's certificate could not be checked.";
	}
}

bool certsPresent(void) {
	bool isDir;
	u64 size;
	// fsStat reports a folder's size as 0, so only a file with something in it is a bundle.
	return R_SUCCEEDED(fsStat(fsArchive(), CACERT_PATH_FS, &isDir, &size)) && size > 0;
}

bool certsMakeDir(void) {
	return R_SUCCEEDED(fsMkdirs(fsArchive(), CACERT_DIR_FS));
}

// A fetch that is called off leaves no temp behind. The worker deletes the temp only when the fetch
// failed, and a fetch that finished just as the key was read is not published.
void certsDiscard(void) {
	(void)fsDelete(fsArchive(), CACERT_TEMP_FS, false, false);
}

// A bundle holds a certificate, and every certificate in it ends. Each BEGIN has its END before the
// next BEGIN and before the end of the file. A page that isn't a bundle holds no certificate, and a
// download cut inside its last certificate (a BEGIN with no END after it) isn't whole.
static bool holdsCertificate(void) {
	static const char BEGIN[] = "-----BEGIN CERTIFICATE-----", END[] = "-----END CERTIFICATE-----";
	char line[256];
	bool open = false, any = false, sound = true;
	FILE *file = fopen(CACERT_TEMP, "rb");
	if (!file) {
		return false;
	}
	while (sound && fgets(line, sizeof(line), file)) {
		if (strstr(line, BEGIN) != NULL) {
			sound = !open; // a BEGIN inside a certificate means one of them was cut
			open = true;
		} else if (strstr(line, END) != NULL) {
			sound = open; // an END with no BEGIN
			open = false;
			any = true;
		}
	}
	fclose(file);
	return sound && any && !open;
}

// Parses the list the way libcurl's Mbed TLS backend does, with `mbedtls_x509_crt_parse_file`. The
// backend returns CURLE_SSL_CACERT_BADFILE only when that call returns below zero. The call returns
// 0 when every certificate parsed, the number that did not when some did (curl accepts that file
// anyway), and a negative error when none could be parsed. So 0 or more means a chain with at least
// one certificate in it, and a list that passes here cannot lead a download to that error, or to
// the unchecked refresh it offers.
static bool parses(void) {
	mbedtls_x509_crt chain;
	mbedtls_x509_crt_init(&chain);
	int r = mbedtls_x509_crt_parse_file(&chain, CACERT_TEMP);
	mbedtls_x509_crt_free(&chain);
	return r >= 0;
}

bool certsPublish(Result *why) {
	FS_Archive sd = fsArchive();
	bool published = false;
	*why = STAGE_METADATA; // a page, or a file Mbed TLS can't read, isn't a list of certificates
	if (holdsCertificate() && parses()) {
		// A leftover from a publish that died would block the move aside.
		(void)fsDelete(sd, CACERT_ASIDE_FS, false, false);
		// There is no old bundle the first time.
		(void)fsRename(CACERT_PATH_FS, CACERT_ASIDE_FS, false);
		*why = fsRename(CACERT_TEMP_FS, CACERT_PATH_FS, false);
		published = R_SUCCEEDED(*why);
		if (published) {
			(void)fsDelete(sd, CACERT_ASIDE_FS, false, false);
		} else {
			// Put the old bundle back, if it was moved.
			(void)fsRename(CACERT_ASIDE_FS, CACERT_PATH_FS, false);
		}
	}
	(void)fsDelete(sd, CACERT_TEMP_FS, false, false);
	return published;
}

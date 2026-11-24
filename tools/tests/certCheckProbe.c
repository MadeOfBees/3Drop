// Runs the certificate hook of src/download.c the way Mbed TLS does: one real verification of a
// chain (mbedtls_x509_crt_verify_with_profile) with the hook as its callback, for the host of the
// URL the transfer is at. testCertCheck.sh makes the certificates and judges what this prints.
// The hook is static, so this file includes download.c and defines the few libcurl functions the
// hook reaches. None of the rest of download.c is linked.
//
//   certCheckProbe <trust.pem> <chain.pem> <first url> <url>
//     <url> is where the transfer is when the connection is made (libcurl's effective URL): the
//     first URL, or wherever a redirect led. Its host is the name Mbed TLS checks the site's
//     certificate against. Prints the flags Mbed TLS ends with, which libcurl decides on, and the
//     flags the failure screen is given, for example `flags=CN_MISMATCH screen=CN_MISMATCH`.
//     `ok` stands for no flags.
//   certCheckProbe issuer <cert.pem> <url>
//     A defensive white-box check of the hook's depth guard. It calls the hook by hand for <cert>
//     as an issuer (depth 1) with the host's flag set. Mbed TLS sets that flag only for the site's
//     certificate, so no chain it verifies reaches this case. Prints the same two sets of flags.
//   certCheckProbe ipv4 <host>
//     Calls the hook's strict IPv4 parse directly on a host text. Prints the address as dotted
//     decimal, or `no`.
#include "download.c"

#include <assert.h>
#include <stdarg.h>

// ---- libcurl: the effective URL, and the URL parser that gives the host ----
static char *effectiveUrl;
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
	va_list ap;
	va_start(ap, info);
	assert(info == CURLINFO_EFFECTIVE_URL);
	*va_arg(ap, char **) = effectiveUrl;
	va_end(ap);
	return CURLE_OK;
}

// A stand-in for libcurl's parser that handles only canonical hosts, which the real parser returns
// as written: everything after `://` up to the port, the path or the end, and an IPv6 literal
// keeps its brackets. What the real parser does with a malformed or noncanonical host is not
// claimed here (the `ipv4` mode tests the strict parse on its own).
static char *hostOf(const char *url) {
	const char *host = strstr(url, "://");
	assert(host);
	host += 3;
	return strndup(host, host[0] == '[' ? strcspn(host, "]") + 1 : strcspn(host, ":/"));
}

struct Curl_URL {
	char url[1024];
};
CURLU *curl_url(void) {
	return calloc(1, sizeof(struct Curl_URL));
}
void curl_url_cleanup(CURLU *handle) {
	free(handle);
}
CURLUcode curl_url_set(CURLU *handle, CURLUPart what, const char *part, unsigned int flags) {
	assert(what == CURLUPART_URL && flags == 0);
	snprintf(handle->url, sizeof(handle->url), "%s", part);
	return CURLUE_OK;
}
CURLUcode curl_url_get(const CURLU *handle, CURLUPart what, char **part, unsigned int flags) {
	assert(what == CURLUPART_HOST && flags == 0);
	*part = hostOf(handle->url);
	return CURLUE_OK;
}
void curl_free(void *p) {
	free(p);
}

// Prints `label=`, then the flags as words joined by `+`. Prints `ok` when there are none, and a
// flag with no word as its number.
static void say(const char *label, uint32_t flags) {
	static const struct {
		uint32_t flag;
		const char *word;
	} WORDS[] = {{MBEDTLS_X509_BADCERT_EXPIRED, "EXPIRED"},
				 {MBEDTLS_X509_BADCERT_FUTURE, "FUTURE"},
				 {MBEDTLS_X509_BADCERT_CN_MISMATCH, "CN_MISMATCH"},
				 {MBEDTLS_X509_BADCERT_NOT_TRUSTED, "NOT_TRUSTED"}};
	const char *join = "";
	printf("%s=", label);
	for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
		if (flags & WORDS[i].flag) {
			printf("%s%s", join, WORDS[i].word);
			flags &= ~WORDS[i].flag;
			join = "+";
		}
	}
	if (flags != 0) {
		printf("%s%#x", join, (unsigned)flags);
	} else if (join[0] == '\0') {
		printf("ok");
	}
}

static void load(mbedtls_x509_crt *crt, const char *path) {
	mbedtls_x509_crt_init(crt);
	assert(mbedtls_x509_crt_parse_file(crt, path) == 0);
}

int main(int argc, char **argv) {
	static Download d;
	mbedtls_x509_crt trust, chain;
	uint32_t flags = 0;
	d.easy = &d; // any handle works because the libcurl functions above answer for it
	if (argc == 3 && strcmp(argv[1], "ipv4") == 0) {
		unsigned char ip[4];
		if (ipv4(argv[2], ip)) {
			printf("%u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
		} else {
			printf("no\n");
		}
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "issuer") == 0) {
		load(&chain, argv[2]);
		effectiveUrl = argv[3];
		flags = MBEDTLS_X509_BADCERT_CN_MISMATCH;
		assert(onCertificate(&d, &chain, 1, &flags) == 0);
	} else {
		assert(argc == 5);
		load(&trust, argv[1]);
		load(&chain, argv[2]);
		snprintf(d.url, sizeof(d.url), "%s", argv[3]);
		effectiveUrl = argv[4];
		// The host as the URL writes it. An IPv6 literal keeps its brackets here, though libcurl
		// leaves them off, and no DNS name equals it either way.
		char *host = hostOf(effectiveUrl);
		const mbedtls_x509_crt_profile *profile = &mbedtls_x509_crt_profile_default;
		int verdict = mbedtls_x509_crt_verify_with_profile(&chain, &trust, NULL, profile, host,
														   &flags, onCertificate, &d);
		assert((verdict == 0) == (flags == 0)); // Mbed TLS fails the chain when any flags are left
		free(host);
	}
	say("flags", flags);
	printf(" ");
	say("screen", (uint32_t)d.verifyResult);
	printf("\n");
	return 0;
}

// cc: src/urls.c
// Tests the text of a scanned QR code as a list of URLs, the way FBI reads one (newline-separated,
// with `https://` added where there is no scheme), and the cases FBI lets through that this
// refuses.
#include "fakes.h"

#include "urls.h"

#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static UrlList list;

// The bytes are put at the very end of a page whose next page cannot be read, so a byte read past
// `length` crashes the test instead of finding a NUL that happened to be there. The text of a scan
// is not a string.
static UrlsVerdict parseBytes(const char *text, size_t length) {
	static char *page;
	static size_t size;
	if (!page) {
		size = (size_t)sysconf(_SC_PAGESIZE);
		page = mmap(NULL, 2 * size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(page != MAP_FAILED && mprotect(page + size, size, PROT_NONE) == 0);
	}
	assert(length <= size);
	char *at = page + size - length;
	memcpy(at, text, length);
	memset(&list, 0x5A, sizeof(list));
	return urlsParse(at, length, &list);
}

// Uses the text's own length, so a NUL ends nothing the parser was not told about.
static UrlsVerdict parse(const char *text) {
	return parseBytes(text, strlen(text));
}

static void expectUrls(const char *text, size_t count, const char *const *want) {
	assert(parse(text) == URLS_OK && list.count == count);
	for (size_t i = 0; i < count; i++) {
		if (strcmp(list.url[i], want[i]) != 0) {
			fprintf(stderr, "testUrls: `%s` gave `%s`, want `%s`\n", text, list.url[i], want[i]);
			abort();
		}
	}
}

static void one(const char *text, const char *want) {
	expectUrls(text, 1, &want);
}

static void ordinary(void) {
	one("https://example.com/3drop.cia", "https://example.com/3drop.cia");
	one("http://192.168.1.5:8000/a.cia", "http://192.168.1.5:8000/a.cia");
	// A payload with no scheme gets https://, not FBI's http://, because the QR route has no paired
	// key, so TLS is the only check there is. A port is not a scheme.
	one("example.com/a.cia", "https://example.com/a.cia");
	one("192.168.1.5:8000/a.cia", "https://192.168.1.5:8000/a.cia");
	// Schemes are case-insensitive, and an upper-case QR alphabet makes a smaller code, so no
	// `https://` is added in front of an upper-case scheme.
	one("HTTPS://EXAMPLE.COM/A.CIA", "HTTPS://EXAMPLE.COM/A.CIA");
	one("Http://Example.com/a", "Http://Example.com/a");
	// A `://` further in is part of a query, not a scheme.
	one("example.com/go?u=http://other/a.cia", "https://example.com/go?u=http://other/a.cia");
}

static void several(void) {
	const char *want[] = {"http://a/x.cia", "https://b/y.cia", "https://c/z.cia"};
	expectUrls("http://a/x.cia\nhttps://b/y.cia\nc/z.cia", 3, want);
	// They come in the order the code lists them, because a title's update and DLC may follow it.
	expectUrls("http://a/x.cia\nhttps://b/y.cia\nc/z.cia\n", 3, want);
	// CRLF, blank lines and the blanks around a URL are not part of it.
	expectUrls("http://a/x.cia\r\n\r\n  https://b/y.cia \t\r\n\nc/z.cia\r\n", 3, want);
}

static void refused(void) {
	assert(parse("") == URLS_EMPTY && list.count == 0);
	assert(parse("\n\r\n  \n") == URLS_EMPTY);
	// Only http and https are fetched; FBI would prefix `http://` to these and fail later.
	assert(parse("ftp://host/a.cia") == URLS_BAD);
	assert(parse("file:///3ds/a.cia") == URLS_BAD);
	assert(parse("gopher://host/a") == URLS_BAD);
	assert(parse("://host/a.cia") == URLS_BAD);
	// Every character a scheme may hold, at its ends and in both cases: a letter, a digit and the
	// three marks. None is http or https, so each is refused rather than given `https://`. A digit
	// first is refused too, because `3ds://x` is not a host named `3ds:`.
	assert(parse("az://h/a") == URLS_BAD && parse("za://h/a") == URLS_BAD);
	assert(parse("AZ://h/a") == URLS_BAD && parse("ZA://h/a") == URLS_BAD);
	assert(parse("a09://h/a") == URLS_BAD && parse("a+b://h/a") == URLS_BAD);
	assert(parse("a-b://h/a") == URLS_BAD && parse("a.b://h/a") == URLS_BAD);
	assert(parse("3ds://h/a") == URLS_BAD && parse("1http://h/a") == URLS_BAD);
	// A scheme that only starts like one: https is five letters, not four.
	assert(parse("httpx://host/a.cia") == URLS_BAD && parse("htt://host/a") == URLS_BAD);
	assert(parse("https2://host/a.cia") == URLS_BAD && parse("httpsx://host/a") == URLS_BAD);
	// One bad line spoils the code, because the rest may be a different file's text, and none of it
	// is kept.
	assert(parse("http://a/x.cia\nftp://b/y.cia") == URLS_BAD && list.count == 0);
	// Whichever line is bad, and whatever follows it, the first refusal is the answer.
	assert(parse("ftp://b/y.cia\nhttp://a/x.cia") == URLS_BAD && list.count == 0);
	assert(parse("http://a/b c\nhttp://a/x.cia\nhttp://a/y.cia") == URLS_BAD && list.count == 0);
	// These hold what no URL holds, or what the screen cannot draw.
	assert(parse("http://a/b c.cia") == URLS_BAD);
	assert(parse("http://a/b\x01.cia") == URLS_BAD);
	assert(parse("http://a/b\x7F") == URLS_BAD);
	assert(parse("http://a/b\xC3\xA9.cia") == URLS_BAD);
	assert(parse("WIFI:T:WPA;S:my net;P:secret word;;") == URLS_BAD);
}

static void nul(void) {
	// Some generators end the text with a NUL; one inside the text is not a URL's.
	const char terminated[] = "http://a/x.cia\0";
	assert(parseBytes(terminated, sizeof(terminated)) == URLS_OK && list.count == 1);
	assert(strcmp(list.url[0], "http://a/x.cia") == 0);
	const char inside[] = "http://a/\0x.cia";
	assert(parseBytes(inside, sizeof(inside) - 1) == URLS_BAD);
	// Only `length` bytes are read: text that is not a string stops where it was told to.
	const char unterminated[] = {'h', 't', 't', 'p', ':', '/', '/', 'a', 'X', 'X'};
	assert(parseBytes(unterminated, 8) == URLS_OK && strcmp(list.url[0], "http://a") == 0);
	// ...including a `://` that the text was cut in the middle of. The bytes past `length` are not
	// read for it, so what is left is not a scheme and gets the implied one.
	const char cut[] = "http://";
	assert(parseBytes(cut, 6) == URLS_OK && strcmp(list.url[0], "https://http:/") == 0);
}

static void limits(void) {
	char text[URLS_MAX * 16 + 64] = "";
	for (size_t i = 0; i < URLS_MAX; i++) {
		snprintf(text + strlen(text), sizeof(text) - strlen(text), "http://h/%zu\n", i);
	}
	assert(parse(text) == URLS_OK && list.count == URLS_MAX);
	assert(strcmp(list.url[URLS_MAX - 1], "http://h/15") == 0);
	snprintf(text + strlen(text), sizeof(text) - strlen(text), "http://h/extra\n");
	assert(parse(text) == URLS_TOO_MANY && list.count == 0); // FBI drops the rest without a word

	// A URL is kept whole or refused, never cut, because a cut URL names another file.
	static char big[URL_MAX + 64];
	memset(big, 'a', sizeof(big));
	memcpy(big, "https://h/", 10);
	big[URL_MAX - 1] = '\0';
	assert(parse(big) == URLS_OK && strlen(list.url[0]) == URL_MAX - 1);
	big[URL_MAX - 1] = 'a';
	big[URL_MAX] = '\0';
	assert(parse(big) == URLS_TOO_LONG);
	// The `https://` it gets is counted, so a code that fits before it is added can fail after.
	memset(big, 'a', sizeof(big));
	big[URL_MAX - 1 - 8] = '\0'; // 8 = strlen("https://")
	assert(parse(big) == URLS_OK && strlen(list.url[0]) == URL_MAX - 1);
	big[URL_MAX - 8] = '\0';
	big[URL_MAX - 1 - 8] = 'a';
	assert(parse(big) == URLS_TOO_LONG);
}

static void https(void) {
	assert(urlIsHttps("https://a/b") && urlIsHttps("HTTPS://A/B") && urlIsHttps("hTTpS://a"));
	assert(!urlIsHttps("http://a/b") && !urlIsHttps("https:/a") && !urlIsHttps("") &&
		   !urlIsHttps("xhttps://a"));
}

// The words under a failed link's card. Every detail that download.c and the install gates can give
// a link has some, a status is read from the Result, and a word that has none says so.
static void why(void) {
	char words[160];
	static const struct {
		const char *detail, *has;
	} CASES[] = {
		{"connect", "reach the server"},
		{"timeout", "too long"},
		{"tls", "secure connection"},
		{"no-tls", "SSL service"},
		{"no-curl", "download library"},
		{"redirect", "somewhere 3Drop will not go"},
		{"no-length", "how big"},
		{"short", "ended the file early"},
		{"length", "more than it said"},
		{"net", "connection failed"},
		{"canceled", "Canceled"},
		{"metadata", "not a CIA"},
		{"worker-stall", "card stopped"},
		{"writer", "keep up"},
		{"thread", "memory"},
		{"write", "card could not be written"},
		{"bundle", "certificate list"},
		{"too-big", "more than a certificate list"},
		{"unmanaged", "kind of title"},
		{"not-newer", "already installed"},
		{"space", "free space"},
		{"notfound", "cannot update itself"},
		{"ring", "memory"},
		{"plain-http", "turned off"},
		{"self", "3Drop itself"},
		{"am-info", "would not say"},
		{"exists-pending", "Uninstall the title"},
	};
	for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
		memset(words, 'x', sizeof(words));
		assert(fetchWhy(words, sizeof(words), CASES[i].detail, 0));
		if (!strstr(words, CASES[i].has)) {
			fprintf(stderr, "testUrls: `%s` says `%s`, want `%s` in it\n", CASES[i].detail, words,
					CASES[i].has);
			abort();
		}
	}
	// When the console already holds that version or a newer one, the message is for the person
	// scanning, not the CIA's author.
	assert(fetchWhy(words, sizeof(words), "not-newer", 0));
	assert(strcmp(words, "That version, or a newer one, is already installed.") == 0);
	// An HTTP error shows its status, read from the Result. One without a status says only that the
	// server refused.
	assert(fetchWhy(words, sizeof(words), "http", DOWNLOAD_HTTP(404)));
	assert(strcmp(words, "The server answered HTTP 404.") == 0);
	assert(fetchWhy(words, sizeof(words), "http", DOWNLOAD_HTTP(100)));
	assert(strcmp(words, "The server answered HTTP 100.") == 0);
	assert(fetchWhy(words, sizeof(words), "http", DOWNLOAD_HTTP(599)));
	assert(strcmp(words, "The server answered HTTP 599.") == 0);
	assert(fetchWhy(words, sizeof(words), "http", DOWNLOAD_CURL(22)));
	assert(strcmp(words, "The server refused the download.") == 0);
	assert(fetchWhy(words, sizeof(words), "http", 0));
	assert(strcmp(words, "The server refused the download.") == 0);
	assert(downloadHttpStatus(DOWNLOAD_HTTP(403)) == 403 && downloadHttpStatus(0) == 0 &&
		   downloadHttpStatus(DOWNLOAD_CURL(22)) == 0 &&
		   downloadHttpStatus(APP_RESULT(DOWNLOAD_HTTP_BASE + 500)) == 0 &&
		   downloadHttpStatus(APP_RESULT(DOWNLOAD_HTTP_BASE - 1)) == 0);
	// The wire's words and anything unknown have no words of their own, so the card shows the word
	// itself.
	memset(words, 'x', sizeof(words));
	assert(!fetchWhy(words, sizeof(words), "digest", 0) && words[0] == '\0');
	assert(!fetchWhy(words, sizeof(words), "", 0) && !fetchWhy(words, sizeof(words), "ok", 0));
	memset(words, 'x', sizeof(words));
	assert(!fetchWhy(words, sizeof(words), NULL, 0) && words[0] == '\0');
	// A small buffer is cut, not overrun, and NUL-ended; a buffer of size 0 gets nothing.
	char tiny[8];
	memset(tiny, 'x', sizeof(tiny));
	assert(fetchWhy(tiny, sizeof(tiny), "connect", 0) && strlen(tiny) == sizeof(tiny) - 1);
	assert(!fetchWhy(tiny, 0, "connect", 0));
}

int main(void) {
	ordinary();
	several();
	refused();
	nul();
	limits();
	https();
	why();
	TEST_PASS();
	return 0;
}

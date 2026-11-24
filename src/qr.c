// SPDX-License-Identifier: GPL-3.0-or-later
// The Y key. Each step has a file of its own (scan.c for the camera, certs.c for the trust policy,
// download.c for libcurl, install.c for AM). This file holds the order they run in and what the
// screen says between them. The main thread does all of it, as it does for a computer's install.
// The download is a thread, and the loop in fetch() answers its two stops. Every wait in it, and
// every question, calls serveBusy, which answers a computer's `stats` and `screenshot` at a screen
// and answers nothing while a download runs.
#include "qr.h"

#include "app.h"
#include "certs.h"
#include "codec.h" // appendf
#include "download.h"
#include "fs.h" // appendLog
#include "install.h"
#include "net.h" // WAIT_NS
#include "ring.h"
#include "scan.h"
#include "stats.h"
#include "ui.h"
#include "urls.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define USER_AGENT "3Drop/" APP_VERSION
// A camera frame comes every 33 ms, so B is heard within one frame.
#define FRAME_WAIT_NS 20000000LL
#define ANY_KEY 0xFFFFFFFFu

// The confirm screen lists every link by its site and then its path, which is what the person is
// trusting (on github.com the host names nobody, because the owner and the repository are in the
// path). Both are shown in rows of ROW_WIDTH characters (what the top screen holds even in wide
// letters), with ROWS_SHOWN rows at once and Up and Down for the rest. A path breaks after a slash
// where a row would cut a word. A site longer than SITE_MAX isn't a link to agree to, so the code
// is refused, not cut. A path of up to PATH_WHOLE characters is shown whole; a longer one shows its
// first PATH_HEAD and its last PATH_TAIL characters around "...", so the owner and the file name
// both show. The query is not shown, and QUERY_MARK ends the path when there is one.
#define ROW_WIDTH 30
#define ROWS_SHOWN 6
#define SITE_MAX 121 // 120 characters and the end; five rows at most with its number and mark
#define PATH_WHOLE 120
#define PATH_HEAD 90
#define PATH_TAIL 27
#define ELLIPSIS "..."
#define QUERY_MARK "?..."
#define PATH_SHOWN (PATH_WHOLE + sizeof(QUERY_MARK)) // the longest path as shown, and the end

static UrlList links; // 16 KiB, so it is never on the stack
static char site[URLS_MAX][SITE_MAX];
static char shownPath[URLS_MAX][PATH_SHOWN];
static char wholePath[URL_MAX]; // one path as libcurl read it, before it is cut for the screen
// Every link's rows, each ended by a newline. Per link it holds the site (with the number, two
// spaces and the mark, up to 24 more than the site), the path, and a newline for each row of
// either.
static char rows[URLS_MAX * (SITE_MAX + 24 + PATH_SHOWN + 10)];
static size_t rowTotal;
static bool anyPlain;
static Download dl, bundle;
static bool downloadUp;
static bool downloading; // a download thread runs, a link's or the list's (read by qrDownloading())

bool qrDownloading(void) {
	return downloading;
}

// libcurl and sslc start with the first download and stop with the app, so a console that never
// scans pays for neither. If sslc fails, https is unavailable and plain http still works
// (download.c refuses an https link, and keeps an http link off https). If libcurl fails, no
// download works. The log says so.
static void downloadStartUp(void) {
	if (!downloadUp) {
		Result why = downloadInit();
		downloadUp = true;
		if (R_FAILED(why)) {
			appendLog("qr-tls-init", why, 0);
		}
	}
}

void qrExit(void) {
	if (downloadUp) {
		downloadExit();
		downloadUp = false;
	}
}

// Shows why on a screen that ends the run, then waits for any key.
static void refused(const char *words, Result why) {
	char detail[220] = "";
	appendf(detail, sizeof(detail), "%s", words);
	if (R_FAILED(why)) {
		appendf(detail, sizeof(detail), "\n0x%08lX", (unsigned long)(u32)why);
	}
	uiAsk(UI_QR_REFUSED, detail, ANY_KEY, serveBusy);
}

// What the main thread owes the console while it waits on a thread: the LED, any client that has
// connected (none while a download runs), and the keys, including the ones that client's request
// saw as well. Returns true if B or START was pressed, or the app is being closed.
static bool stopRequested(void) {
	appStatusTick();
	serveBusy();
	return !appRunning() || (pressedKeys() & (KEY_B | KEY_START)) != 0;
}

// ---- The scan ----

typedef enum { LOOK_CODE, LOOK_CANCEL, LOOK_FAIL } Look;

// Runs the camera until it finds a code that holds links, B is pressed, or the camera fails. A code
// that decodes but isn't a list of links is reported and ends the scan, because the same code would
// be read again next frame.
static Look look(void) {
	Result why = scanOpen();
	if (R_FAILED(why)) {
		refused("The camera could not be started.", why);
		return LOOK_FAIL;
	}
	bool picture = uiScanOpen();
	uiNotice(UI_QR_SCAN, NULL);
	Look end = LOOK_CANCEL;
	UrlsVerdict verdict = URLS_OK;
	while (!stopRequested()) {
		ScanStep step = scanPoll(FRAME_WAIT_NS);
		if (step == SCAN_BROKEN) {
			end = LOOK_FAIL;
			break;
		}
		if (step != SCAN_IDLE && picture) {
			uiScanFrame(scanFrame());
		}
		if (step == SCAN_FOUND) {
			size_t length;
			const u8 *payload = scanPayload(&length);
			verdict = urlsParse((const char *)payload, length, &links);
			end = verdict == URLS_OK ? LOOK_CODE : LOOK_FAIL;
			break;
		}
	}
	why = scanResult();
	scanClose();
	uiScanClose();
	if (end == LOOK_FAIL) {
		if (verdict != URLS_OK) {
			refused(urlsWhy(verdict), 0);
		} else {
			refused("The camera stopped.", why);
		}
	}
	return end;
}

// ---- The confirm screen ----

// Returns how many characters of `text` make the next row: ROW_WIDTH. A path that this row would
// cut inside a segment (the cut is not before a slash) breaks after the last slash instead, when a
// row of its own holds the segment it cuts; a longer segment is cut as it always was.
static size_t rowLength(const char *text, bool path) {
	size_t length = ROW_WIDTH;
	if (strlen(text) <= length) {
		return strlen(text);
	}
	if (path && text[length] != '/') {
		size_t slash = length;
		while (slash > 0 && text[slash - 1] != '/') {
			slash--;
		}
		if (slash > 0 && strcspn(text + slash, "/") <= ROW_WIDTH) {
			length = slash;
		}
	}
	return length;
}

// Appends `text` to `out` as rows, a newline after each, and returns how many rows.
static size_t appendRows(char *out, size_t cap, const char *text, bool path) {
	size_t n = 0;
	for (size_t at = 0, length; text[at]; at += length, n++) {
		length = rowLength(text + at, path);
		appendf(out, cap, "%.*s\n", (int)length, text + at);
	}
	return n;
}

// Builds a path as the screen shows it: whole up to PATH_WHOLE characters, else its first PATH_HEAD
// and its last PATH_TAIL characters around "...", then QUERY_MARK when the link has a query.
// PATH_SHOWN holds the longest of these, so the buffer never cuts what this writes.
static void showPath(char *out, const char *path, bool query) {
	const char *mark = query ? QUERY_MARK : "";
	size_t length = strlen(path);
	if (length <= PATH_WHOLE) {
		snprintf(out, PATH_SHOWN, "%.*s%s", PATH_WHOLE, path, mark);
	} else {
		snprintf(out, PATH_SHOWN, "%.*s" ELLIPSIS "%.*s%s", PATH_HEAD, path, PATH_TAIL,
				 path + length - PATH_TAIL, mark);
	}
}

// Reads each link's site and path the way libcurl does. A code with a link that cannot be shown
// whole (a site that is too long) or cannot be read at all is not used, because consent to a name
// that is cut isn't consent.
static bool readLinks(void) {
	for (size_t i = 0; i < links.count; i++) {
		bool query = false;
		const char *why = NULL;
		if (!downloadSite(links.url[i], site[i], sizeof(site[i]))) {
			why = "its site cannot be read, or is too long.";
		} else if (!downloadPath(links.url[i], wholePath, sizeof(wholePath), &query)) {
			why = "its path cannot be read.";
		}
		if (why) {
			char words[96] = "";
			appendf(words, sizeof(words), "Link %u of %u: %s", (unsigned)i + 1,
					(unsigned)links.count, why);
			refused(words, 0);
			return false;
		}
		showPath(shownPath[i], wholePath, query);
	}
	return true;
}

// Lays out every link as rows: its number, a mark when it is plain http, its whole site, then its
// path.
static void layLinks(void) {
	rows[0] = '\0';
	rowTotal = 0;
	anyPlain = false;
	for (size_t i = 0; i < links.count; i++) {
		bool plain = !urlIsHttps(links.url[i]);
		char entry[SITE_MAX + 24]; // a number of up to ten digits, two spaces, the mark, the site
		snprintf(entry, sizeof(entry), "%u  %s%.*s", (unsigned)i + 1, plain ? "http://" : "",
				 SITE_MAX - 1, site[i]);
		rowTotal += appendRows(rows, sizeof(rows), entry, false);
		rowTotal += appendRows(rows, sizeof(rows), shownPath[i], true);
		anyPlain = anyPlain || plain;
	}
}

// Writes the ROWS_SHOWN rows from `top`, then where they are when more exist, then, for the bottom
// screen after the form feed, what plain http means.
static void windowOf(char *out, size_t cap, size_t top) {
	out[0] = '\0';
	const char *row = rows;
	for (size_t i = 0; i < top; i++) {
		row += strcspn(row, "\n") + 1;
	}
	size_t shown = 0;
	for (; shown < ROWS_SHOWN && top + shown < rowTotal; shown++) {
		int length = (int)strcspn(row, "\n");
		appendf(out, cap, "%.*s\n", length, row);
		row += length + 1;
	}
	if (rowTotal > ROWS_SHOWN) {
		appendf(out, cap, "Up/Down: rows %u-%u of %u", (unsigned)top + 1, (unsigned)(top + shown),
				(unsigned)rowTotal);
	}
	if (anyPlain) {
		appendf(out, cap,
				"\f\nhttp:// is not encrypted: someone on the network can replace the file.");
	}
}

// A installs; B, or the app closing, does not (START is not asked for here). Up and Down move the
// rows when they do not all fit. Nothing else asks the person anything on the way to the first
// byte.
static bool confirmLinks(void) {
	size_t top = 0;
	for (;;) {
		char shown[320];
		windowOf(shown, sizeof(shown), top);
		bool scrolls = rowTotal > ROWS_SHOWN;
		u32 keys = KEY_A | KEY_B | (scrolls ? KEY_UP | KEY_DOWN : 0);
		u32 key = uiAsk(UI_QR_CONFIRM, shown, keys, serveBusy);
		if (key == 0 || (key & KEY_B)) {
			return false;
		}
		if (key & KEY_A) {
			return true;
		}
		if ((key & KEY_UP) && top > 0) {
			top--;
		} else if ((key & KEY_DOWN) && top + ROWS_SHOWN < rowTotal) {
			top++;
		}
	}
}

// ---- The certificate list ----

// The list is fetched from curl.se into a temp file, and the temp replaces the list only whole. The
// fetch is always checked, peer and host, and a fetch that fails the check replaces nothing. It is
// checked against the list on the card when `onCard` is set, so a fresh list replaces the one the
// console trusts only if that one vouches for it. Otherwise, when there is no list that reads (the
// first use) or the one there is too old to vouch for curl.se's current certificate, it is checked
// against the bundle shipped inside 3Drop. So no fetch goes unchecked, unlike Universal Updater's
// UpdateCACert, which does that. Returns false, with the reason in *op as for a refused link: the
// download's own detail when it failed (the network, the server, the size), "canceled" for a B, and
// "bundle" when the card will not hold the list or what came is not a list. A B leaves no failed
// check in bundle.tls, whatever the worker marked.
static bool fetchBundle(bool onCard, Operation *op) {
	bool stopped = false; // a B read in the fetch's loop, whatever the fetch then reports
	// A fetch that never starts has no check to have failed.
	bundle.tls = DL_TLS_NONE;
	if (!certsMakeDir()) {
		opSet(op, OUTCOME_REJECTED, "bundle", DOWNLOAD_NO_FILE);
		return false;
	}
	snprintf(bundle.url, sizeof(bundle.url), "%s", CACERT_URL);
	bundle.userAgent = USER_AGENT;
	bundle.sink = DL_SINK_FILE;
	bundle.path = CACERT_TEMP;
	bundle.verify = true;
	bundle.caBlob = onCard ? NULL : certsShipped(&bundle.caBlobLen);
	uiNotice(UI_QR_BUNDLE, NULL);
	downloading = true;
	if (downloadStart(&bundle)) {
		while (downloadStage(&bundle) != DL_FINISHED) {
			if (stopRequested()) {
				downloadCancel(&bundle);
				stopped = true;
			}
			svcSleepThread(WAIT_NS);
		}
		downloadJoin(&bundle);
	}
	downloading = false;
	if (stopped) {
		// A list fetched while the person said no isn't a list, so the temp goes and the card's
		// list stays.
		certsDiscard();
		// The worker marks a failed check before its tail, so a B read there finds one marked. The
		// B counts, so there is no second fetch (it would never see this B) and no refusal for the
		// check.
		bundle.tls = DL_TLS_NONE;
		installCancel(op);
		return false;
	}
	if (!bundle.ok) {
		opSet(op, OUTCOME_REJECTED, bundle.detail, bundle.why);
		return false;
	}
	Result why;
	if (!certsPublish(&why)) {
		opSet(op, OUTCOME_REJECTED, "bundle", why);
		return false;
	}
	return true;
}

typedef enum { CHECK_RETRY, CHECK_SKIP, CHECK_CANCEL } CheckAnswer;

// Appends the cause of a failed check, as the screens say it, on a line of its own.
static void appendCause(char *out, size_t cap, const Download *d) {
	appendf(out, cap, "\n%s", tlsCauseWhy(tlsCause(d->verifyResult)));
}

// Asks about a failed check the way Universal Updater does (download.cpp:898-925), but for this
// link alone. It shows the link (number, site and path, on the top screen), the cause, and the
// three ways on (on the bottom screen). The top screen's part is under 280 bytes (ui.c's body holds
// 320); with the form feed and the longest cause, it is under 350.
static CheckAnswer askAboutCheck(const Download *d, size_t index) {
	char words[512] = "";
	appendf(words, sizeof(words), "Link %u of %u\n", (unsigned)index + 1, (unsigned)links.count);
	appendRows(words, sizeof(words), site[index], false);
	appendRows(words, sizeof(words), shownPath[index], true);
	appendf(words, sizeof(words), "\f");
	if (d->tls == DL_TLS_BUNDLE) {
		appendf(words, sizeof(words), "\nThe certificate list on the card could not be read.");
	} else {
		appendCause(words, sizeof(words), d);
	}
	u32 key = uiAsk(UI_QR_CHECK, words, KEY_A | KEY_Y | KEY_B, serveBusy);
	if (key == 0 || (key & KEY_B)) {
		return CHECK_CANCEL;
	}
	return (key & KEY_Y) ? CHECK_SKIP : CHECK_RETRY;
}

// A refetch the person asked for. A failure is reported and the question stands. `failed` is the
// download that asked for it. A check that ran against the list on the card (DL_TLS_CHECK) shows
// that the list reads, so the refetch is checked by it first. If that list cannot vouch for curl.se
// (it is too old, so its own fetch fails the check too) or could not be read (DL_TLS_BUNDLE), the
// bundle shipped inside 3Drop checks the refetch, so a card whose list has gone stale can still
// renew it.
static bool refetchBundle(const Download *failed) {
	Operation failure = OP_REJECTED;
	bool onCard = failed->tls == DL_TLS_CHECK && certsPresent();
	bool fetched = fetchBundle(onCard, &failure);
	if (!fetched && onCard && bundle.tls == DL_TLS_CHECK) {
		failure = OP_REJECTED;
		fetched = fetchBundle(false, &failure);
	}
	if (fetched) {
		return true;
	}
	if (bundle.tls == DL_TLS_CHECK) {
		refused("The new list failed the check against the certificates built into 3Drop, so the "
				"card's list was not changed.",
				failure.result);
	} else {
		refused("The certificate list could not be fetched.", failure.result);
	}
	return false;
}

// ---- One download and its install ----

static void progress(void) {
	gApp.wireBytes = downloadReceived(&dl);
	gApp.uiWrittenBytes = gApp.uiSunkProgress ? ringDone() : gApp.wireBytes;
	uiProgressTick();
}

// The writer can still be canceled after the download thread has finished. The first stop is
// recorded and sent to the worker once. The worker then ends after its current AM call, not after
// the whole buffered ring (24 MiB and more) has gone into an import that is canceled anyway.
static void waitTick(void *context) {
	Operation *op = context;
	bool stop = stopRequested(); // called every tick, for the LED and the keys
	if (stop && !installCanceled(op)) {
		installCancel(op);
		ringStop();
	}
	gApp.uiWrittenBytes = ringDone();
	uiProgressTick();
}

// The download thread stops twice for the main thread, and this function is the main thread's half
// of that. At DL_SIZED the ring is armed for Content-Length. At DL_PREFIXED the CIA's prefix is in
// the ring's head and nothing past it. There the import opens, with the same parse, route and gates
// a computer's install has, or the thread is told to stop before it fetches a byte of the body.
static void fetch(const char *url, bool verify, InstallRun *run) {
	Operation *op = &gApp.last;
	bool armed = false, opened = false;
	u64 progressAt = 0;
	downloading = true;
	snprintf(dl.url, sizeof(dl.url), "%s", url);
	dl.userAgent = USER_AGENT;
	dl.sink = DL_SINK_CIA;
	dl.verify = verify;
	gApp.wireBytes = gApp.wireTotal = gApp.uiWrittenBytes = 0;
	gApp.uiSunkProgress = false;
	appSetStatus(SERVER_RECEIVING);
	uiProgressTick();
	if (downloadStart(&dl)) {
		while (downloadStage(&dl) != DL_FINISHED) {
			if (stopRequested()) {
				downloadCancel(&dl);
				// The key edge this just read is gone. The worker may already have finished, so the
				// operation says canceled here, or the close below would commit after the press.
				installCancel(op);
			}
			DlStage asked = downloadAsking(&dl);
			if (asked == DL_SIZED) {
				u64 size = downloadSize(&dl);
				armed = ringArm(size, RING_TO_HANDLE, true, 0);
				if (armed) {
					gApp.wireTotal = size;
					gApp.uiSunkProgress = true;
				} else {
					opSet(op, OUTCOME_REJECTED, "ring", OUT_OF_MEMORY);
				}
				downloadAnswer(&dl, armed);
			} else if (asked == DL_PREFIXED) {
				osTickCounterStart(&run->gap);
				opened = installOpen(run, downloadSize(&dl), downloadPrefix(&dl), op);
				// The import owns the ring from here, opened or not, because its refusal or its
				// close is what lets go of it.
				armed = false;
				downloadAnswer(&dl, opened && run->started);
			}
			u64 now = uptimeMs();
			if (now - progressAt >= UI_REDRAW_MIN_MS) { // a faster tick would draw nothing
				progressAt = now;
				progress();
			}
			svcSleepThread(WAIT_NS);
		}
		downloadJoin(&dl);
	}
	gApp.wireBytes = downloadReceived(&dl);
	if (opened) {
		// A stop after the download's last look still cancels, because the writer drains next.
		waitTick(op);
		installClose(run, dl.ok, dl.detail, dl.why, waitTick, op, op);
	} else {
		if (armed) {
			ringRelease(); // armed and never opened, so this flow has to let go of the ring
		}
		if (!op->detail) { // nothing above said why, so the transfer ended first
			opSet(op, OUTCOME_REJECTED, dl.detail ? dl.detail : "net", dl.why);
		}
	}
	downloading = false;
}

// The list's first fetch failed its check against the bundle shipped in 3Drop. This reports it with
// its cause, as a link's failed check is reported, but with no way on, because there is no check on
// the list to skip.
static void refusedListCheck(void) {
	char words[200] = "The certificate list failed its check.";
	appendCause(words, sizeof(words), &bundle);
	refused(words, bundle.why);
}

// Handles one link, from its TLS policy to its outcome in gApp.last, which is also logged. Returns
// true if the link installed. *self is true when that was 3Drop itself.
static bool oneLink(size_t index, size_t installed, bool *self) {
	const char *url = links.url[index];
	Operation *op = &gApp.last;
	InstallRun run = {0};
	bool skipCheck = false;
	appTransferStart();
	statsReset("qr");
	gApp.viaUrl = true;
	gApp.batchActive = links.count > 1;
	gApp.batchIndex = (u32)index + 1;
	gApp.batchTotal = (u32)links.count;
	gApp.batchInstalled = (u32)installed;
	for (;;) { // once, and again when the person answers a failed check
		*op = OP_REJECTED;
		run = (InstallRun){0};
		TlsPolicy policy = tlsPolicy(url, skipCheck);
		if (!policy.allowed) {
			opSet(op, OUTCOME_REJECTED, "plain-http", 0);
			break;
		}
		if (urlIsHttps(url) && !downloadTlsUp()) {
			// This is a setup failure and is reported as one, before a list is fetched for a check
			// that cannot run. It is not a failed check, which a Y could skip.
			opSet(op, OUTCOME_REJECTED, "no-tls", DOWNLOAD_NO_TLS);
			break;
		}
		// With no list on the card (the first use), the fetch is checked against the shipped
		// bundle.
		if (policy.needBundle && !certsPresent() && !fetchBundle(false, op)) {
			if (bundle.tls == DL_TLS_CHECK) {
				refusedListCheck();
			}
			break;
		}
		fetch(url, policy.verify, &run);
		if (!policy.verify || (dl.tls != DL_TLS_CHECK && dl.tls != DL_TLS_BUNDLE)) {
			break;
		}
		CheckAnswer answer;
		do {
			answer = askAboutCheck(&dl, index);
		} while (answer == CHECK_RETRY && !refetchBundle(&dl));
		if (answer == CHECK_CANCEL) {
			installCancel(op);
			break;
		}
		// The skip applies only within this call, so the next link in a batch is checked again.
		skipCheck = skipCheck || answer == CHECK_SKIP;
	}
	statsFinish();
	bool committed = appInstallDone(op);
	char event[32];
	snprintf(event, sizeof(event), "qr-%s", op->detail ? op->detail : "unknown");
	appendLog(event, op->result, gApp.wireBytes);
	*self = committed && run.route && run.route->self;
	return committed;
}

void qrRun(After *after) {
	if (look() != LOOK_CODE || !readLinks()) {
		return;
	}
	layLinks();
	if (!confirmLinks()) {
		return;
	}
	downloadStartUp();
	size_t installed = 0;
	for (size_t i = 0; i < links.count && !ringStranded(); i++) {
		bool self = false;
		bool ok = oneLink(i, installed, &self);
		installed += ok;
		// A refusal stops the rest, because they may depend on this one and its card needs a look.
		// 3Drop's own install stops them too, because it relaunches; the rest wait for the next
		// scan. Either way the log says how many were left.
		if (!ok || self) {
			if (i + 1 < links.count) {
				appendLog("qr-skipped", 0, links.count - i - 1);
			}
			if (self) {
				aptSetChainloaderToSelf();
				*after = AFTER_SELF_UPDATED;
			}
			return;
		}
	}
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmdFs.h"

#include "app.h"
#include "auth.h"
#include "fs.h"
#include "hash.h"
#include "layeredFs.h"
#include "path.h"
#include "ring.h"
#include "sdXfer.h"
#include "stats.h"
#include "sysinfo.h"
#include "titles.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

// Carries a mutation's arguments from its check to its run. It is static because one request is
// served at a time.
static struct {
	Target at, to;
	bool isDir, recursive, system; // system is the request's word for a boot file
	u64 had;					   // the current size of the file at the path
	Result found;
	SdPut put;
	u8 kept[32]; // what a range's temp must already hold
} arg;

static bool argTarget(VerbContext *c, size_t i, Target *t, bool reading) {
	if (!targetParse(c->field[i], t) || strcmp(t->path, "/") == 0) {
		return replyFail(c, "path", 0);
	}
	if (t->kind == TARGET_SD && pathProtected(t->path, reading)) {
		return replyFail(c, "protected", 0);
	}
	return true;
}

bool argPath(VerbContext *c, size_t i, Target *t, bool reading) {
	if (!argTarget(c, i, t, reading)) {
		return false;
	}
	if (t->kind != TARGET_SD) {
		return replyFail(c, "path", 0);
	}
	return true;
}

int argWord(VerbContext *c, size_t i, const char *const words[], size_t n) {
	const char *field = i < c->n ? c->field[i] : "";
	for (size_t w = 0; w < n; w++) {
		if (strcmp(field, words[w]) == 0) {
			return (int)w;
		}
	}
	replyFail(c, "grammar", 0);
	return -1;
}

bool argIdle(VerbContext *c) {
	if (ringStranded()) {
		return replyFail(c, "stranded", 0);
	}
	return true;
}

// Parses `range <offset> <length>` for a get or a put: two byte counts, and the length must be at
// least one.
static bool parseRange(char *const field[2], u64 *offset, u64 *length) {
	return parseU64(field[0], offset) && parseU64(field[1], length) && *length > 0;
}

bool argDigest(VerbContext *c, const char *text, u8 out[32]) {
	return parseDigest(text, out) || replyFail(c, "grammar", 0);
}

// Applies to writes only. A read changes nothing, so a read of a save or of extdata is answered
// with whatever FS finds there.
static bool argWritable(VerbContext *c, const Target *t, bool tree, bool system) {
	return replyRefusal(c, pathWriteRefusal(t, tree, system), 0);
}

// Looks up what is at the path now and stores the result in arg.found, arg.isDir and arg.had. An
// archive's title must be installed. AM is asked about that, so the caller refuses `stranded`
// first.
static bool argStat(VerbContext *c, const Target *t) {
	TitleFacts facts;
	FS_Archive archive;
	Result why = 0;
	const char *refusal =
		t->kind == TARGET_SD ? NULL : titleCheck(targetTitle(t), true, false, &facts, &why);
	if (!replyRefusal(c, refusal, why)) {
		return false;
	}
	if (R_FAILED(why = fsTargetOpen(t, &archive))) {
		return replyFail(c, "notfound", why);
	}
	arg.found = fsStat(archive, t->path, &arg.isDir, &arg.had);
	fsArchiveClose(archive);
	return true;
}

// A put on the card needs to know only whether a folder already has the path's name, which it
// refuses. This makes one call on the path. argStat makes two (a file's open, then a folder's),
// which searches a new file's folder twice.
static bool argFolder(VerbContext *c, const Target *t) {
	FS_Archive archive;
	Result why = fsTargetOpen(t, &archive);
	if (R_FAILED(why)) {
		return replyFail(c, "notfound", why);
	}
	arg.isDir = fsIsDir(archive, t->path);
	// A file isn't looked up here because the publish finds it.
	arg.found = arg.isDir ? 0 : FSERROR_NOT_FOUND;
	arg.had = 0;
	fsArchiveClose(archive);
	return true;
}

static bool argFound(VerbContext *c, const Target *t) {
	if (!argStat(c, t)) {
		return false;
	}
	if (R_FAILED(arg.found)) {
		return replyFail(c, "notfound", arg.found);
	}
	return true;
}

// ---- ls <id> <path> [d|t|dt|r|rt] -----------------------------------------------------------

// A walk that reads the whole card, or sends slowly, still ends.
#define LS_WALK_MS 120000

// A walk that ends early says why in a `rejected` trailer, so a partial listing is never taken for
// a whole one: `read` (FS failed, with its Result), `deadline` (LS_WALK_MS ran out), `depth` (a
// directory FS_WALK_DEPTH levels down was listed, not entered) or `name` (a name left out that
// UTF-8 cannot carry, or a path past SD_PATH_UNITS).
void lsPath(VerbContext *c, const char *flags) {
	Target t;
	// No path check, because a listing shows only names and sizes.
	if (!targetParse(c->field[2], &t)) {
		replyFail(c, "path", 0);
		return;
	}
	bool times = strchr(flags, 't') && t.kind == TARGET_SD;
	static FsWalk walk;
	Result r = fsWalkOpen(&walk, &t, strchr(flags, 'd') == NULL, strchr(flags, 'r') != NULL);
	if (R_FAILED(r)) {
		replyFail(c, "notfound", r);
		return;
	}
	u64 deadline = uptimeMs() + LS_WALK_MS;
	bool late = false, sending = listingBegin(c, "");
	while (sending && !late && fsWalkNext(&walk)) {
		appStatusTick(); // the walk can take LS_WALK_MS
		const FsEntry *e = &walk.entry;
		char mtime[24] = "-", hex[sizeof(e->name) * 2 + 1], line[sizeof(hex) + 64];
		u64 seconds;
		if (times && R_SUCCEEDED(fsWalkMtime(&walk, &seconds))) {
			snprintf(mtime, sizeof(mtime), "%llu", (unsigned long long)seconds);
		}
		hexEncode(hex, (const u8 *)e->name, strlen(e->name));
		int n = e->isDir ? snprintf(line, sizeof(line), "d - %s %s\n", mtime, hex)
						 : snprintf(line, sizeof(line), "f %llu %s %s\n",
									(unsigned long long)e->size, mtime, hex);
		sending = listingLine(line, (size_t)n);
		late = uptimeMs() >= deadline;
	}
	fsWalkClose(&walk);
	if (R_FAILED(walk.result)) {
		listingEnd(OUTCOME_REJECTED, "read", walk.result);
	} else if (late || walk.tooDeep || walk.skipped) {
		listingEnd(OUTCOME_REJECTED, late ? "deadline" : walk.tooDeep ? "depth" : "name", 0);
	} else {
		listingEnd(OUTCOME_COMMITTED, "ok", 0);
	}
}

// Shows the transfer on the screen's file row, and returns the status to restore once it is done.
static ServerStatus showFileRow(const char *path, bool sending, bool gzip) {
	ServerStatus before = gApp.status;
	const char *name = strrchr(path, '/') + 1;
	size_t n = strlen(name);
	gApp.fileSending = sending;
	gApp.fileGzip = gzip;
	if (n >= sizeof(gApp.fileName)) { // cut at a character's start, never inside its UTF-8 bytes
		n = sizeof(gApp.fileName) - 1;
		while (n > 0 && ((u8)name[n] & 0xC0) == 0x80) {
			n--;
		}
	}
	memcpy(gApp.fileName, name, n);
	gApp.fileName[n] = '\0';
	appSetStatus(SERVER_FILE);
	return before;
}

// ---- get <id> <path> [gzip | range <offset> <length>] --------------------------------------

// A range's header also includes the file's size and mtime, so a host joining ranges sees a change.
void cmdGet(VerbContext *c) {
	enum { GET_PLAIN, GET_GZIP, GET_RANGE }; // each word's index in the arrays below
	static const char *const OPTIONS[] = {
		[GET_PLAIN] = "", [GET_GZIP] = "gzip", [GET_RANGE] = "range"};
	static const size_t FIELDS[] = {[GET_PLAIN] = 3, [GET_GZIP] = 4, [GET_RANGE] = 6};
	Target t;
	SdGet get = {.length = UINT64_MAX};
	Operation op;
	char mtime[24] = "-";
	u64 seconds;
	int option = argWord(c, 3, OPTIONS, sizeof(OPTIONS) / sizeof(OPTIONS[0]));
	if (option < 0) {
		return;
	}
	if (c->n != FIELDS[option] ||
		(option == GET_RANGE && !parseRange(c->field + 4, &get.offset, &get.length))) {
		replyFail(c, "grammar", 0);
		return;
	}
	bool ranged = option == GET_RANGE;
	if (!argTarget(c, 2, &t, true) || !argIdle(c)) { // the file streams through tier 1
		return;
	}
	statsStart(c); // `stats` describes this get from here on
	if (!sdGetStart(&t, option == GET_GZIP, &get, &op)) {
		replyFail(c, op.detail, op.result);
		return;
	}
	// Only a range that isn't the whole file has its time looked up, so that a host joining it to
	// other ranges can see the file change between them. Other gets skip it because the time is a
	// call that takes the path, which FAT looks up in its whole folder.
	if (ranged && t.kind == TARGET_SD && !get.covers && R_SUCCEEDED(fsMtime(t.path, &seconds))) {
		snprintf(mtime, sizeof(mtime), "%llu", (unsigned long long)seconds);
	}
	bool framed =
		ranged ? REPLY_OK(c, " %llu raw %llu %s", (unsigned long long)get.length,
						  (unsigned long long)get.size, mtime)
			   : REPLY_OK(c, " %llu %s", (unsigned long long)get.length, get.gzip ? "gzip" : "raw");
	ServerStatus before = showFileRow(t.path, true, get.gzip);
	sdGetSend(c->client, framed, &get, &op);
	appSetStatus(before);
	if (framed) {
		bodyTrailer(c, &op, get.digest, get.wireBytes);
	}
}

// ---- hash <id> <path> ---------------------------------------------------------------------------

// Reports what a get of the file would end with, its size and wire digest, by reading the file
// without sending anything. It takes no nonce, as with get. A key file's digest is never given,
// because it would let someone test guesses of the key offline.
void cmdHash(VerbContext *c) {
	Target t;
	Operation op;
	u64 size;
	u8 digest[32];
	char text[DIGEST_TEXT_SIZE];
	if (!argTarget(c, 2, &t, true) || !argIdle(c)) { // the units go through tier 1
		return;
	}
	statsStart(c); // `stats` times its reads and digests
	if (!sdHash(&t, &size, digest, &op)) {
		replyFail(c, op.detail, op.result);
		return;
	}
	digestText(text, digest);
	REPLY_OK(c, " %llu %s", (unsigned long long)size, text);
}

// ---- put <id> <nonce> <path> ... (its grammar: cmd.c's put) ------------------------------------
// A range's digest covers the file from its start through the end of the body, and <kept> is the
// digest of what the temp file holds before the body. The body of a `system` put is the whole file,
// raw.

// FAT32's largest file bounds a put. A size of `-` is held as that maximum, which bounds the range.
// A range of whole units never ends at that size, so it never publishes.
#define FAT32_FILE_MAX 4294967295u
_Static_assert(FAT32_FILE_MAX == UINT32_MAX, "FAT32 counts a file's size in 32 bits");
_Static_assert(FAT32_FILE_MAX % DIGEST_UNIT != 0,
			   "a range of whole units would end the file at `-`");

// An archive put is held whole in tier 1, as is a /boot.firm read back to be checked. An extdata
// file cannot be resized. No file replaces a directory.
static const char *putRefusal(void) {
	bool found = R_SUCCEEDED(arg.found);
	if (((arg.at.kind != TARGET_SD || arg.put.firm) && arg.put.size > RING_TIER1_BYTES) ||
		(found && arg.at.kind == TARGET_EXTDATA && arg.had != arg.put.size)) {
		return "size";
	}
	return found && arg.isDir ? "exists" : NULL;
}

bool checkPutFile(VerbContext *c) {
	enum { PUT_PLAIN, PUT_GZIP, PUT_RANGE, PUT_SYSTEM }; // each word's index in the arrays below
	static const char *const OPTIONS[] = {
		[PUT_PLAIN] = "", [PUT_GZIP] = "gzip", [PUT_RANGE] = "range", [PUT_SYSTEM] = "system"};
	static const size_t FIELDS[] = {
		[PUT_PLAIN] = 6, [PUT_GZIP] = 8, [PUT_RANGE] = 10, [PUT_SYSTEM] = 7};
	char **f = c->field;
	int option = argWord(c, 6, OPTIONS, sizeof(OPTIONS) / sizeof(OPTIONS[0]));
	if (option < 0) {
		return false;
	}
	if (c->n != FIELDS[option]) {
		return replyFail(c, "grammar", 0);
	}
	arg.system = option == PUT_SYSTEM;
	if (!argTarget(c, 3, &arg.at, false) || !argWritable(c, &arg.at, false, arg.system)) {
		return false;
	}
	arg.put.gzip = option == PUT_GZIP;
	arg.put.offset = 0;
	arg.put.size = FAT32_FILE_MAX; // what `-` stands for
	bool unsized = option == PUT_RANGE && strcmp(f[4], "-") == 0, sd = arg.at.kind == TARGET_SD,
		 ok = (unsized || (parseU64(f[4], &arg.put.size) && arg.put.size <= FAT32_FILE_MAX)) &&
			  (sd || option == PUT_PLAIN || arg.system); // an archive put is whole and raw
	arg.put.firm = sd && pathBootFirm(arg.at.path);
	arg.put.length = arg.put.wireSize = arg.put.size;
	if (ok && arg.put.gzip) {
		ok = parseU64(f[7], &arg.put.wireSize) && arg.put.wireSize < arg.put.size;
	}
	// The digest is of the file's units, so a range starts on a unit boundary and only the last
	// range ends off one.
	if (ok && option == PUT_RANGE) {
		ok = parseRange(f + 7, &arg.put.offset, &arg.put.length) &&
			 arg.put.offset % DIGEST_UNIT == 0 &&
			 (arg.put.length % DIGEST_UNIT == 0 ||
			  (!unsized && arg.put.offset + arg.put.length >= arg.put.size));
		arg.put.wireSize = arg.put.length;
	}
	if (!ok) {
		return replyFail(c, "grammar", 0);
	}
	if (arg.put.length > arg.put.size ||
		arg.put.offset > arg.put.size - arg.put.length) { // there is no sum that could wrap
		return replyFail(c, "size", 0);
	}
	if (!argDigest(c, f[5], arg.put.digest) ||
		(option == PUT_RANGE && !argDigest(c, f[9], arg.kept))) {
		return false;
	}
	// This length check also covers the backup's path.
	if (sd && (strlen(arg.at.path) + strlen(SD_PUT_TEMP) >= SD_PATH_MAX ||
			   pathUnits(arg.at.path) + strlen(SD_PUT_TEMP) > SD_PATH_UNITS)) {
		return replyFail(c, "path", 0);
	}
	if (sd && !replyRefusal(c, layeredFsWriteRefusal(arg.at.path, arg.put.size), 0)) {
		return false;
	}
	// A power cut between the publish's two renames leaves no /boot.firm, and a battery can run
	// out.
	if (arg.put.firm && !sysinfoOnAc()) {
		return replyFail(c, "system", ON_BATTERY);
	}
	// The body goes through tier 1.
	if (!argIdle(c) || !(arg.at.kind == TARGET_SD ? argFolder(c, &arg.at) : argStat(c, &arg.at))) {
		return false;
	}
	Result why = 0;
	const char *refusal = putRefusal();
	// This goes last because it may read the temp back for minutes. A replayed request is refused
	// first.
	if (!refusal && arg.put.offset > 0 && sdPutDigests()) {
		refusal =
			nonceIs(f[2]) ? sdPutResume(arg.at.path, arg.put.offset, arg.kept, &why) : "challenge";
	}
	return replyRefusal(c, refusal, why);
}

void runPutFile(VerbContext *c) {
	Operation op;
	char digest[DIGEST_TEXT_SIZE];
	ServerStatus before = showFileRow(arg.at.path, false, arg.put.gzip);
	if (arg.at.kind == TARGET_SD) {
		sdPut(c->client, arg.at.path, &arg.put, &op);
	} else {
		archivePut(c->client, &arg.at, &arg.put, &op);
	}
	appSetStatus(before);
	digestText(digest, arg.put.digest);
	// The reply covers the bytes the path, or a range's temp, now holds.
	if (op.outcome == OUTCOME_COMMITTED) {
		reply(c, op.outcome, op.detail, op.result, " %llu %s",
			  (unsigned long long)(arg.put.offset + arg.put.length),
			  arg.at.kind == TARGET_SD && !sdPutDigests() ? "digest=skipped" : digest);
	} else {
		reply(c, op.outcome, op.detail, op.result, "");
	}
}

// ---- delete <id> <nonce> <path> [r|system] ------------------------------------------------------

bool checkDeletePath(VerbContext *c, bool recursive, bool system) {
	arg.recursive = recursive;
	arg.system = system;
	if (!argTarget(c, 3, &arg.at, false)) {
		return false;
	}
	if (arg.at.kind == TARGET_SD) {
		if (!replyRefusal(c, pathRemoveRefusal(arg.at.path, arg.system), 0)) {
			return false;
		}
	} else if (arg.recursive) { // one request never removes a subtree of a game's data
		return replyFail(c, "refused", 0);
	} else if (!argWritable(c, &arg.at, false, false) || !argIdle(c)) { // the title check asks AM
		return false;
	}
	if (!argFound(c, &arg.at)) {
		return false;
	}
	if (arg.system && arg.isDir) { // the `system` word is for single files
		return replyFail(c, "system", 0);
	}
	return true;
}

void runDeletePath(VerbContext *c) {
	Operation op = OP_REJECTED;
	FS_Archive archive;
	sdPutForget();
	Result r = fsTargetOpen(&arg.at, &archive);
	if (R_FAILED(r)) {
		replyFail(c, "open", r); // nothing was touched, as for a put
		return;
	}
	BENCH(u64 fsAt = svcGetSystemTick());
	r = fsDelete(archive, arg.at.path, arg.isDir, arg.recursive);
	BENCH(benchFsStep(FS_DELETE, &fsAt));
	if (R_SUCCEEDED(r)) {
		if (arg.at.kind == TARGET_SD && !arg.isDir) {
			sdDropBackup(arg.at.path); // once the file is gone, because a failed delete keeps both
			BENCH(benchFsStep(FS_DROP, &fsAt));
		}
		fsTargetCommit(&arg.at, archive, &op);
	} else {
		// The outcome is unknown, except for a single SD entry, because extdata has no commit,
		// saves are unmeasured, and a tree can go part way.
		bool untouched = arg.at.kind == TARGET_SD && !arg.recursive;
		opSet(&op, untouched ? OUTCOME_REJECTED : OUTCOME_UNKNOWN, "delete", r);
	}
	fsArchiveClose(archive);
	reply(c, op.outcome, op.detail, op.result, "");
}

// ---- mkdir <id> <nonce> <path> -----------------------------------------------------------------

bool checkMkdir(VerbContext *c) {
	return argPath(c, 3, &arg.at, false) && argWritable(c, &arg.at, false, false);
}

void runMkdir(VerbContext *c) {
	Result r = fsMkdirs(fsArchive(), arg.at.path);
	if (R_FAILED(r)) {
		replyFail(c, r == FSERROR_ALREADY_EXISTS ? "exists" : "mkdir", r);
	} else {
		REPLY_OK(c, "");
	}
}

// ---- rename <id> <nonce> <from> <to> -----------------------------------------------------------

bool checkRename(VerbContext *c) {
	bool isDir;
	u64 size;
	if (!argPath(c, 3, &arg.at, false) || !argPath(c, 4, &arg.to, false) || !argFound(c, &arg.at)) {
		return false;
	}
	// Moving it away takes it from where it was.
	if (!replyRefusal(c, pathRemoveRefusal(arg.at.path, false), 0)) {
		return false;
	}
	if (!argWritable(c, &arg.to, arg.isDir, false)) {
		return false;
	}
	Result why = 0;
	const char *refusal = arg.isDir ? layeredFsFolderRefusal(arg.at.path, arg.to.path, &why)
									: layeredFsWriteRefusal(arg.to.path, arg.had);
	if (!replyRefusal(c, refusal, why)) {
		return false;
	}
	if (R_SUCCEEDED(fsStat(fsArchive(), arg.to.path, &isDir, &size))) {
		return replyFail(c, "exists", 0);
	}
	return true;
}

void runRename(VerbContext *c) {
	sdPutForget();
	BENCH(u64 fsAt = svcGetSystemTick());
	Result r = fsRename(arg.at.path, arg.to.path, arg.isDir);
	BENCH(benchFsStep(FS_RENAME, &fsAt));
	if (R_FAILED(r)) {
		replyFail(c, "rename", r);
		return;
	}
	if (!arg.isDir) {
		sdDropBackup(arg.at.path); // once the file has moved, because a failed rename keeps both
		BENCH(benchFsStep(FS_DROP, &fsAt));
	}
	REPLY_OK(c, "");
}

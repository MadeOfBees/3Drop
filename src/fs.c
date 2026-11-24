// SPDX-License-Identifier: GPL-3.0-or-later
// The SD card, saves and extdata through FS (fs.h): archives opened for one request, the file and
// folder calls, a walk of a folder's tree, the event log, the card's free space and title seeds.
#include "fs.h"

#include "path.h"
#include "result.h"

#include <3ds.h>
#include <3ds/util/utf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

FS_Archive fsArchive(void) {
	static FS_Archive archive;
	FS_Archive opened; // libctru writes the handle even from an error reply
	if (!archive &&
		R_SUCCEEDED(FSUSER_OpenArchive(&opened, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
		archive = opened;
	}
	return archive;
}

Result fsTargetOpen(const Target *t, FS_Archive *archive) {
	const u32 lowPath[3] = {MEDIATYPE_SD, (u32)t->id, (u32)(t->id >> 32)};
	if (t->kind == TARGET_SD) {
		*archive = fsArchive();
		return 0;
	}
	return FSUSER_OpenArchive(archive,
							  t->kind == TARGET_SAVE ? ARCHIVE_USER_SAVEDATA : ARCHIVE_EXTDATA,
							  (FS_Path){PATH_BINARY, sizeof(lowPath), lowPath});
}

void fsArchiveClose(FS_Archive archive) {
	if (archive != fsArchive()) {
		FSUSER_CloseArchive(archive);
	}
}

void fsTargetCommit(const Target *t, FS_Archive archive, Operation *op) {
	// The secure value is (SecureValueSlot << 32) | (unique id << 8) | variation, where the last
	// two together are the title id's low word.
	u64 secureValue = (u64)SECUREVALUE_SLOT_SD << 32 | (u32)t->id;
	u8 existed = 0; // a refused call may write nothing
	opSet(op, OUTCOME_COMMITTED, "ok", 0);
	if (t->kind != TARGET_SAVE) {
		return;
	}
	Result r = FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &secureValue, sizeof(secureValue),
										&existed, 1);
	appendLog("secure-value", r, existed); // whether FS found one to delete
	if (R_FAILED(r)) {
		opSet(op, OUTCOME_REJECTED, "secure-value", r); // so nothing is committed
	} else if (R_FAILED(r = FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0,
												  NULL, 0))) {
		opSet(op, OUTCOME_UNKNOWN, "commit", r);
	}
}

void appendLog(const char *event, Result result, u64 bytes) {
	appendLogTail(event, result, bytes, "");
}

void appendLogTail(const char *event, Result result, u64 bytes, const char *tail) {
	struct stat st;
	if (stat(LOG_PATH, &st) == 0 && st.st_size >= LOG_MAX_BYTES) {
		remove(LOG_OLD_PATH);
		if (rename(LOG_PATH, LOG_OLD_PATH) != 0) {
			return; // lose this line rather than the card's space
		}
	}
	FILE *file = fopen(LOG_PATH, "a");
	if (!file) {
		return;
	}

	fprintf(file, "%s result=0x%08lX bytes=%llu%s\n", event, (unsigned long)(u32)result,
			(unsigned long long)bytes, tail);
	fclose(file);
}

FS_Path fsPathUtf16(u16 units[SD_PATH_MAX], const char *path) {
	ssize_t n = utf8_to_utf16(units, (const u8 *)path, SD_PATH_MAX - 1);
	if (n < 0 || n > SD_PATH_UNITS) {
		return (FS_Path){PATH_INVALID, 0, NULL};
	}
	units[n] = 0;
	return (FS_Path){PATH_UTF16, (u32)(n + 1) * sizeof(u16), units};
}

Result fsStat(FS_Archive archive, const char *path, bool *isDir, u64 *size) {
	u16 units[SD_PATH_MAX];
	FS_Path p = fsPathUtf16(units, path);
	Handle handle;
	*isDir = false;
	*size = 0;
	Result r = FSUSER_OpenFile(&handle, archive, p, FS_OPEN_READ, 0);
	if (R_SUCCEEDED(r)) {
		r = FSFILE_GetSize(handle, size);
		FSFILE_Close(handle);
		return r;
	}
	if (R_FAILED(FSUSER_OpenDirectory(&handle, archive, p))) {
		return r; // the file's error, either not found or why it could not be opened
	}
	FSDIR_Close(handle);
	*isDir = true;
	return 0;
}

bool fsIsDir(FS_Archive archive, const char *path) {
	u16 units[SD_PATH_MAX];
	Handle handle;
	if (R_FAILED(FSUSER_OpenDirectory(&handle, archive, fsPathUtf16(units, path)))) {
		return false; // a file, nothing there, or a card that fails; only a folder is asked for
	}
	FSDIR_Close(handle);
	return true;
}

Result fsOpenRead(FS_Archive archive, const char *path, Handle *h) {
	u16 units[SD_PATH_MAX];
	Result r = FSUSER_OpenFile(h, archive, fsPathUtf16(units, path), FS_OPEN_READ, 0);
	if (R_FAILED(r)) {
		*h = 0;
	}
	return r;
}

Result fsDelete(FS_Archive archive, const char *path, bool isDir, bool recursive) {
	u16 units[SD_PATH_MAX];
	FS_Path p = fsPathUtf16(units, path);
	if (p.type == PATH_INVALID) {
		// FS's answer to an invalid path has not been measured, so FS is never asked to delete one.
		return PATH_TOO_LONG;
	}
	if (!isDir) {
		return FSUSER_DeleteFile(archive, p);
	}
	return recursive ? FSUSER_DeleteDirectoryRecursively(archive, p)
					 : FSUSER_DeleteDirectory(archive, p);
}

Result fsRename(const char *from, const char *to, bool isDir) {
	u16 fromUnits[SD_PATH_MAX], toUnits[SD_PATH_MAX];
	FS_Path a = fsPathUtf16(fromUnits, from), b = fsPathUtf16(toUnits, to);
	if (a.type == PATH_INVALID || b.type == PATH_INVALID) {
		return PATH_TOO_LONG; // FS is never asked to move from or onto an invalid path either
	}
	return isDir ? FSUSER_RenameDirectory(fsArchive(), a, fsArchive(), b)
				 : FSUSER_RenameFile(fsArchive(), a, fsArchive(), b);
}

// A folder that already exists is not a failure. The SD card reports it as FSERROR_ALREADY_EXISTS;
// a save or an extdata may report it with a code of its own. So when a create fails with any other
// code, FS is asked about the folder, and the code is never used to judge it. A put on the SD card
// makes no extra FS call.
static bool folderThere(FS_Archive archive, const char *path) {
	bool isDir;
	u64 size;
	return R_SUCCEEDED(fsStat(archive, path, &isDir, &size)) && isDir;
}

Result fsMkdirs(FS_Archive archive, const char *path) {
	char partial[SD_PATH_MAX];
	u16 units[SD_PATH_MAX];
	size_t len = strlen(path);
	if (len >= sizeof(partial) || fsPathUtf16(units, path).type == PATH_INVALID) {
		return PATH_TOO_LONG;
	}
	memcpy(partial, path, len + 1);
	for (size_t i = 1; i <= len; i++) {
		if (partial[i] == '/' || partial[i] == '\0') {
			char saved = partial[i];
			partial[i] = '\0';
			Result r = FSUSER_CreateDirectory(archive, fsPathUtf16(units, partial), 0);
			if (R_FAILED(r) && r != FSERROR_ALREADY_EXISTS && !folderThere(archive, partial)) {
				return r;
			}
			partial[i] = saved;
		}
	}
	bool isDir;
	u64 size;
	Result r = fsStat(archive, path, &isDir, &size);
	return R_FAILED(r) || isDir ? r : FSERROR_ALREADY_EXISTS; // a file holds the name
}

bool fsMkParents(FS_Archive archive, const char *path) {
	char parent[SD_PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", path);
	*strrchr(parent, '/') = '\0';
	return parent[0] && R_SUCCEEDED(fsMkdirs(archive, parent));
}

// ---- The directory walk (ls) ------------------------------------------------------------------

#define WALK_BATCH 8 // FSDIR_Read entries per IPC

static size_t len16(const u16 *s) {
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static Result fsWalkPush(FsWalk *w, size_t len) {
	FsWalkLevel *l = &w->level[w->depth];
	*l = (FsWalkLevel){.len = len, .batch = calloc(WALK_BATCH, sizeof(FS_DirectoryEntry))};
	Result r = l->batch ? FSUSER_OpenDirectory(&l->dir, w->archive, fsMakePath(PATH_UTF16, w->path))
						: OUT_OF_MEMORY;
	if (R_FAILED(r)) {
		free(l->batch);
		return r;
	}
	w->depth++;
	return 0;
}

static void fsWalkPop(FsWalk *w) {
	FsWalkLevel *l = &w->level[--w->depth];
	FSDIR_Close(l->dir);
	free(l->batch);
}

Result fsWalkOpen(FsWalk *w, const Target *t, bool children, bool recursive) {
	const char *path = t->path;
	bool isDir;
	memset(w, 0, sizeof(*w));
	if (fsPathUtf16(w->path, path).type == PATH_INVALID) {
		return PATH_TOO_LONG;
	}
	size_t n = len16(w->path);
	Result r = fsTargetOpen(t, &w->archive);
	if (R_FAILED(r)) {
		return r;
	}
	if (R_FAILED(r = fsStat(w->archive, path, &isDir, &w->entry.size))) {
		goto close;
	}
	w->entry.isDir = isDir;
	if (!isDir || !children) { // the only entry is the file, or the directory itself
		const char *slash = strrchr(path, '/');
		snprintf(w->entry.name, sizeof(w->entry.name), "%s", slash[1] ? slash + 1 : "/");
		w->single = true;
		return 0;
	}
	w->recursive = recursive;
	w->root = strcmp(path, "/") == 0 ? 0 : n;
	if (R_SUCCEEDED(r = fsWalkPush(w, w->root))) {
		return 0;
	}
close:
	fsArchiveClose(w->archive);
	return r;
}

bool fsWalkNext(FsWalk *w) {
	if (w->single) { // returns its one entry, then the end, because a single walk pushes no level
		w->single = false;
		return true;
	}
	while (w->depth > 0) {
		FsWalkLevel *l = &w->level[w->depth - 1];
		if (l->at == l->got) {
			l->at = 0;
			w->result = FSDIR_Read(l->dir, &l->got, WALK_BATCH, l->batch);
			if (R_FAILED(w->result)) {
				return false; // a read error ends the walk but is never mistaken for its end
			}
			if (l->got == 0) {
				fsWalkPop(w);
				continue;
			}
		}
		const FS_DirectoryEntry *e = &l->batch[l->at++];
		size_t n = len16(e->name);
		if (l->len + 1 + n > SD_PATH_UNITS) {
			w->skipped++; // a path FS would not open; the rest of the walk goes on
			continue;
		}
		w->path[l->len] = '/';
		memcpy(w->path + l->len + 1, e->name, (n + 1) * sizeof(u16));
		w->entry.isDir = (e->attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
		w->entry.size = w->entry.isDir ? 0 : e->fileSize;
		ssize_t out =
			utf16_to_utf8((u8 *)w->entry.name, w->path + w->root + 1, sizeof(w->entry.name) - 1);
		if (out < 0 || (size_t)out >= sizeof(w->entry.name)) {
			w->skipped++; // a name UTF-8 cannot carry (an unpaired surrogate)
			continue;
		}
		w->entry.name[out] = '\0';
		if (w->recursive && w->entry.isDir) {
			if (w->depth == FS_WALK_DEPTH) {
				w->tooDeep = true; // listed, not entered
			} else if (R_FAILED(w->result = fsWalkPush(w, l->len + 1 + n))) {
				return false;
			}
		}
		return true;
	}
	return false;
}

void fsWalkClose(FsWalk *w) {
	while (w->depth > 0) {
		fsWalkPop(w);
	}
	fsArchiveClose(w->archive);
}

static Result mtimeOf(FS_Archive archive, const u16 *path, u64 *unixSeconds) {
	// Milliseconds since 2000-01-01, as libctru's archive_getmtime reads them.
	u64 ms = 0;
	Result r = FSUSER_ControlArchive(archive, ARCHIVE_ACTION_GET_TIMESTAMP, (void *)path,
									 (len16(path) + 1) * sizeof(u16), &ms, sizeof(ms));
	*unixSeconds = ms / 1000 + 946684800;
	return r;
}

Result fsWalkMtime(const FsWalk *w, u64 *unixSeconds) {
	return mtimeOf(w->archive, w->path, unixSeconds);
}

Result fsMtime(const char *path, u64 *unixSeconds) {
	u16 units[SD_PATH_MAX];
	FS_Path p = fsPathUtf16(units, path);
	return p.type == PATH_INVALID ? PATH_TOO_LONG : mtimeOf(fsArchive(), units, unixSeconds);
}

// ---- Free space --------------------------------------------------------------------------------

Result sdFreeSpace(u64 *freeBytes, u32 *clusterSize, u32 *totalClusters) {
	FS_ArchiveResource resource = {0};
	Result result = FSUSER_GetSdmcArchiveResource(&resource);
	if (R_FAILED(result)) {
		return result;
	}
	if (resource.clusterSize == 0) {
		return SD_BAD_RESOURCE; // every caller divides by it
	}
	*freeBytes = (u64)resource.freeClusters * resource.clusterSize;
	*clusterSize = resource.clusterSize;
	*totalClusters = resource.totalClusters;
	return 0;
}

// ---- The seed database -------------------------------------------------------------------------

// libctru wraps neither call, so they are sent the way FBI sends AddSeed (3dbrew FS:AddSeed,
// FS:GetSeed).
static Result seedCall(u32 header, u64 titleID, const u8 *in, u8 *out) {
	u32 *cmdbuf = getThreadCommandBuffer();
	cmdbuf[0] = header;
	cmdbuf[1] = (u32)titleID;
	cmdbuf[2] = (u32)(titleID >> 32);
	if (in) {
		memcpy(&cmdbuf[3], in, 16);
	}
	Result r = svcSendSyncRequest(*fsGetSessionHandle());
	if (R_SUCCEEDED(r)) {
		r = (Result)cmdbuf[1];
	}
	if (R_SUCCEEDED(r) && out) {
		memcpy(out, &cmdbuf[2], 16);
	}
	return r;
}

Result seedGet(u64 titleID, u8 seed[16]) {
	return seedCall(IPC_MakeHeader(0x87B, 2, 0), titleID, NULL, seed);
}

Result seedAdd(u64 titleID, const u8 seed[16]) {
	return seedCall(IPC_MakeHeader(0x87A, 6, 0), titleID, seed, NULL);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FS_H
#define FS_H

// Access to the SD card, saves and extdata through FS: opening a target's archive, then stat, read,
// delete, rename, mkdir and a walk of a folder's tree. It also covers the event log, the card's
// free space and the title seeds. Paths are archive-relative, with no "sdmc:" prefix.

#include "app.h"
#include "path.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

FS_Archive fsArchive(void);
// Opened per request, because FS allows 32 archive handles. A GBA VC save opens only through FSPXI,
// so it cannot be opened here.
Result fsTargetOpen(const Target *t, FS_Archive *archive);
void fsArchiveClose(FS_Archive archive);
// A save's secure value is deleted before its commit, so no commit lands beside a stale one, which
// the game would take for a rolled-back save and delete. If the secure value cannot be deleted,
// nothing is committed. FS switches a save's partition tables at the commit (3dbrew: "presumably"
// atomic).
void fsTargetCommit(const Target *t, FS_Archive archive, Operation *op);

// The error code the SD card gives for a name that already exists. A save or an extdata may use a
// different one, which is why fsMkdirs asks about the folder.
#define FSERROR_ALREADY_EXISTS ((Result)0xC82044BE)
// FS's two error codes for a name that does not exist; libctru maps both to ENOENT (archive_dev.c).
#define FSERROR_NOT_FOUND ((Result)0xC8804478)
#define FSERROR_PATH_NOT_FOUND ((Result)0xC92044FA)

// Every path is passed to FS as UTF-16, because under PATH_ASCII each byte of a UTF-8 name would be
// a character of its own. A path that will not convert, or is past SD_PATH_UNITS (pathNormalize
// lets no path that long through), is PATH_INVALID, which fsDelete, fsRename and fsMkdirs refuse on
// their own.
FS_Path fsPathUtf16(u16 units[SD_PATH_MAX], const char *path);

Result fsStat(FS_Archive archive, const char *path, bool *isDir, u64 *size);
// Whether path is a folder, in one call that names it (fsStat makes two for a path not there).
bool fsIsDir(FS_Archive archive, const char *path);
// After a failed open, *h is 0 and no handle is held.
Result fsOpenRead(FS_Archive archive, const char *path, Handle *h);
Result fsDelete(FS_Archive archive, const char *path, bool isDir, bool recursive);
// FS refuses a target that exists.
Result fsRename(const char *from, const char *to, bool isDir);
// Creates every folder of the path, from the top. A folder that already exists is not a failure, in
// any archive. Returns 0 once the path is a folder, or the error.
Result fsMkdirs(FS_Archive archive, const char *path);
// Used after a create that found no parent. Returns true once the parents exist, and false at the
// root, where a retry cannot help.
bool fsMkParents(FS_Archive archive, const char *path);

// How many directories deep a recursive walk enters. A directory this far down is listed but not
// entered (tooDeep).
#define FS_WALK_DEPTH 16
typedef struct {
	bool isDir;
	u64 size;
	char name[SD_PATH_MAX * 3]; // UTF-8, relative to the listed directory
} FsEntry;
typedef struct {
	Handle dir;
	size_t len; // this level's directory path, in UTF-16 units
	u32 got, at;
	FS_DirectoryEntry *batch;
} FsWalkLevel;
typedef struct {
	FsEntry entry;
	FS_Archive archive;
	u16 path[SD_PATH_MAX]; // UTF-16, so a name FAT holds in any script can be entered and printed
	FsWalkLevel level[FS_WALK_DEPTH];
	int depth;
	size_t root; // the listed directory's length in path
	bool single, recursive;
	bool tooDeep;  // a directory FS_WALK_DEPTH down was listed but not entered
	u32 skipped;   // a name UTF-8 cannot carry, or a path past SD_PATH_UNITS
	Result result; // 0 when the walk reached its end
} FsWalk;
Result fsWalkOpen(FsWalk *w, const Target *t, bool children, bool recursive);
bool fsWalkNext(FsWalk *w);
void fsWalkClose(FsWalk *w);
// SD walks only, because FAT keeps modification times.
Result fsWalkMtime(const FsWalk *w, u64 *unixSeconds);
Result fsMtime(const char *path, u64 *unixSeconds); // the modification time of an SD path

void appendLog(const char *event, Result result, u64 bytes);
// Logs the same line as appendLog, with `tail` (more ` name=value` fields, like a request's) after
// the bytes.
void appendLogTail(const char *event, Result result, u64 bytes, const char *tail);

Result sdFreeSpace(u64 *freeBytes, u32 *clusterSize, u32 *totalClusters);

// A seeded title will not boot until FS's seed database holds its seed.
Result seedGet(u64 titleID, u8 seed[16]);
Result seedAdd(u64 titleID, const u8 seed[16]);
// What GetSeed returns for a title with no seed (3dbrew). It is matched exactly: any other result
// is a failure.
#define SEED_ABSENT ((Result)0xC8804464)

#endif // FS_H

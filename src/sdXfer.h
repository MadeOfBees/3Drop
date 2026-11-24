// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SDXFER_H
#define SDXFER_H

// This header declares the SD-card get and put bodies, which run on the ring's worker. A get
// reads a file out to the wire. A put writes a file in from the wire through a temp file that
// is renamed over the old one (a /boot.firm is checked first), and each comes with its digest.
// The same put also goes into a save or SD extdata, and a file's hash is read off the card.

#include "knobs.h" // KNOB
#include "path.h"  // Target
#include "result.h"

#include <3ds.h>
#include <stdbool.h>

// The caller refuses `stranded` first, because that worker may still hold the ring
// and the open file.

typedef struct {
	u64 offset,
		length; // the bytes sent: at most `length` (as asked) from `offset`, as many as there are
	u64 size;	// the file's size
	bool gzip;
	bool covers; // the range is the whole file, so no other range is joined to it
	u64 wireBytes;
	u8 digest[32]; // the digest of the bytes sent, once they are sent whole
} SdGet;

// An offset past the file's end is refused with `size`.
bool sdGetStart(const Target *t, bool gzip, SdGet *get, Operation *op);
// Called after every sdGetStart that succeeded, framed or not. It stops the reader
// and closes the file.
void sdGetSend(int client, bool framed, SdGet *get, Operation *op);

typedef struct {
	u64 size;			// the file's size
	u64 offset, length; // the bytes of the file in the body: all of them, except in a range
	u64 wireSize;		// the body's size on the wire
	bool gzip;
	bool firm;	   // a /boot.firm of at most tier 1's size; it publishes only as a FIRM (sdXfer.c)
	u8 digest[32]; // the digest of the file's first offset + length bytes, however they came
} SdPut;
// Returns true in every release build. A bench `nohash 1` measures a put's ceiling
// without the hash.
static inline bool sdPutDigests(void) {
	return KNOB(noHash) == 0;
}
// A put that dies between its two publish renames leaves the old file at the backup, and the next
// put to the path restores it first. A range puts its body into the temp after the bytes the temp
// keeps, and publishes once it ends the file. A range that fails is cut off again, so the temp
// keeps whole ranges only. The temp is removed only by a put that starts it again, or by hand.
void sdPut(int client, const char *path, const SdPut *put, Operation *op);
// Checks a range before the spend: that the temp beside path holds the file's first
// `offset` bytes with this digest, as kept or as read back from the card (~11 MB/s).
// Returns NULL, or the refusal: `size` for a temp shorter than that, `digest` for
// other bytes, `read` or `hash` with the Result that stopped the read back, `canceled`
// or `exit` when START or an APT quit stops it, and `ring` when there is no tier 1.
const char *sdPutResume(const char *path, u64 offset, const u8 digest[32], Result *why);
// Gets a file's size and wire digest, reading it on the card a unit at a time and
// recording the time in `stats`. Refusals are as for sdPutResume, with `notfound`
// for no file and `isdir` for a folder. The caller refuses `stranded` first.
bool sdHash(const Target *t, u64 *size, u8 digest[32], Operation *op);
// Call this before a delete or rename, which may change a kept temp, so its bytes
// are read back next time.
void sdPutForget(void);
// Call this before path is deleted or moved away, because the next put to it would
// restore the backup.
void sdDropBackup(const char *path);
// The caller bounds the body by tier 1; nothing here checks.
// The whole body is held and checked before the archive opens, because SD extdata has
// no commit to fall back on, so a dropped sender or a wrong body changes nothing.
void archivePut(int client, const Target *t, const SdPut *put, Operation *op);

#endif // SDXFER_H

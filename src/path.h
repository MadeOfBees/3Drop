// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PATH_H
#define PATH_H

#include <3ds.h>
#include <stdbool.h>

// FS takes the archive-relative spelling and stdio takes the `sdmc:` one.
#define APP_PATH "/3ds/3drop"
#define APP_DIR "sdmc:" APP_PATH
#define LOG_PATH APP_DIR "/3drop.log"
// At this size the log becomes this older file, replacing the previous one, because a peer that
// replays a captured read and cuts it adds a line each time, and must not fill the SD card.
#define LOG_MAX_BYTES (1024 * 1024)
#define LOG_OLD_PATH APP_DIR "/3drop.old.log"
#define KEY_PATH APP_DIR "/key.txt"
// With this cache, only first boot and a regen pay the PBKDF2 stretch.
#define KEY_CACHE_PATH APP_DIR "/key.cache"

#define SD_PATH_MAX 256 // NUL included
// FSUSER opens an SD path of at most this many UTF-16 units, however its components divide it (a
// 254-unit path returns e0e046bf, as measured). That is fewer than SD_PATH_MAX holds in ASCII.
#define SD_PATH_UNITS 253
// These go beside a put's path (sdXfer.c). SD_PATH_MAX and SD_PATH_UNITS must have room for them,
// and that is checked before the spend.
#define SD_PUT_TEMP ".3drop.tmp"
#define SD_PUT_BACKUP ".3drop.bak"
_Static_assert(sizeof(SD_PUT_TEMP) == sizeof(SD_PUT_BACKUP), "the check measures both by the temp");

// The scheme is read on the raw field, so a `hex:` path never spells one.
typedef enum { TARGET_SD, TARGET_SAVE, TARGET_EXTDATA } TargetKind;
typedef struct {
	TargetKind kind;
	u64 id; // the save's title id, or the extdata id
	char path[SD_PATH_MAX];
} Target;
bool targetParse(const char *field, Target *t);
// Returns 0 for extdata that no SD application holds (0x14D1 badges, 0x165C theme layouts). HOME
// Menu reads those, so nothing may write them.
u64 targetTitle(const Target *t);

// Each SD path gets one spelling because FAT drops a trailing `.` or space, so "/a." would name
// "/a". The output is never truncated. Its bytes are well-formed UTF-8 with no C0 control or DEL.
bool pathNormalize(const char *field, char out[SD_PATH_MAX]);
// Returns the length of a well-formed UTF-8 path as FS counts it, in UTF-16 units (two for a
// character past U+FFFF).
size_t pathUnits(const char *path);

// Each rule below compares components with names. For a change, a component below the root that
// holds any non-ASCII character counts as every name it is compared with. At the root, such a
// component counts as each name with as many characters, a non-ASCII character standing for any
// character.

// Reading a key file would hand out the MAC key; replacing or moving one would re-key the console.
// A read compares ASCII names only, so reads stay open everywhere else, because no one spells a
// look-alike of the key by accident.
bool pathProtected(const char *path, bool reading);

// The console's data and what it boots from are refused, because 3Drop could not undo their loss
// remotely. The rest of /luma stays open because a LayeredFS sync undoes itself with a delete
// there. The request's word `system` opens one file directly in /luma/payloads or
// /luma/sysmodules, spelled exactly, to a put or a delete, and /boot.firm to a put. `system` opens
// nothing else. This is for a path that passed the gate above (cmdFs.c's argTarget), which has
// already answered `/` and /3ds.
const char *pathRemoveRefusal(const char *path, bool system);

// Luma patches any title that /luma/titles or /luma/plugins names, and a bad patch can stop a
// boot or 3Drop.
const char *pathWriteRefusal(const Target *t, bool tree, bool system);
// Returns true for /boot.firm spelled exactly, in either case. A put there publishes only a FIRM
// that boot9strap boots.
bool pathBootFirm(const char *path);
// Returns true for /luma/titles/<title id>/<file> as FAT matches it, ignoring ASCII case.
// That is a file Luma's loader reads.
bool pathLumaTitleFile(const char *path, const char *file);

#endif // PATH_H

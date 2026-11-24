// SPDX-License-Identifier: GPL-3.0-or-later
// Refuses up front whatever would make Luma3DS's loader stop the console (see layeredFs.h).
#include "layeredFs.h"

#include "fs.h"
#include "path.h"

#include <stdio.h>

static const char EXHEADER[] = "exheader.bin";

// FS has two Result codes for a name that is not there.
static bool absent(Result r) {
	return r == FSERROR_NOT_FOUND || r == FSERROR_PATH_NOT_FOUND;
}

// Checks for a folder where Luma looks for one (patcher.c, dirCheck). Only an absent result lets a
// launch go ahead. Any other result, including one from a failing card, counts as a folder, because
// a launch past one costs a hard crash and a refusal costs the user a word.
const char *layeredFsRefusal(u64 titleID, Result *why) {
	char path[48];
	u16 units[SD_PATH_MAX];
	Handle dir;
	snprintf(path, sizeof(path), "/luma/titles/%016llx/romfs", (unsigned long long)titleID);
	*why = FSUSER_OpenDirectory(&dir, fsArchive(), fsPathUtf16(units, path));
	if (R_SUCCEEDED(*why)) {
		FSDIR_Close(dir);
	}
	return absent(*why) ? NULL : "layeredfs";
}

// Luma's loader accepts an exheader.bin that is an ExHeader_Info or a whole ExHeader (patcher.c,
// loadTitleExheaderInfo). It stops on any other size.
static bool exheaderSize(u64 size) {
	return size == 0x400 || size == 0x800;
}

const char *layeredFsWriteRefusal(const char *path, u64 size) {
	return pathLumaTitleFile(path, EXHEADER) && !exheaderSize(size) ? "layeredfs" : NULL;
}

// A folder moved onto a title's directory brings its exheader.bin along, and FS finds the one in
// the folder in any letter case, as FAT matches names. If the card will not say, this refuses, with
// FS's Result in `why`.
const char *layeredFsFolderRefusal(const char *from, const char *to, Result *why) {
	char landing[SD_PATH_MAX + sizeof(EXHEADER)], inside[sizeof(landing)];
	bool isDir;
	u64 size;
	snprintf(landing, sizeof(landing), "%s/%s", to, EXHEADER);
	if (!pathLumaTitleFile(landing, EXHEADER)) {
		return NULL;
	}
	snprintf(inside, sizeof(inside), "%s/%s", from, EXHEADER);
	*why = fsStat(fsArchive(), inside, &isDir, &size);
	if (absent(*why)) {
		return NULL;
	}
	return R_FAILED(*why) || !exheaderSize(size) ? "layeredfs" : NULL;
}

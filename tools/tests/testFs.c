// cc: src/fs.c src/path.c src/codec.c
// SD path rules and fs.c's directory creation. How the verbs use them is tested in testDispatch.
#include "fakes.h"

#include "fs.h"
#include "path.h"

#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *sdWrite(const char *path, bool tree) {
	Target t = {.kind = TARGET_SD};
	snprintf(t.path, sizeof(t.path), "%s", path);
	return pathWriteRefusal(&t, tree, false);
}

static void paths(void) {
	char out[SD_PATH_MAX];
	// A decoded "hex:" is text and is never decoded again, because a second decode would write to
	// a path the sender never named.
	assert(!pathNormalize("hex:6865783a32663631", out));
	assert(pathNormalize("hex:2f4e696e74656e646f203344532f612062", out));
	assert(strcmp(out, "/Nintendo 3DS/a b") == 0);
	assert(pathNormalize("sdmc:/3ds/x", out) && strcmp(out, "/3ds/x") == 0);
	assert(pathNormalize("/", out) && strcmp(out, "/") == 0);
	assert(pathNormalize("/foo/bar", out) && strcmp(out, "/foo/bar") == 0);
	static const char *const BAD[] = {"hex:2f1f", "hex:2f0a",  "hex:2f0061", "hex:2f7f", "hex:2F4E",
									  "hex:2f4",  "hex:",	   "hex:zz",	 "a/b",		 "/a//b",
									  "/a/",	  "/a/.",	   "/a/..",		 "/a./b",	 "/a\\b",
									  "/c:d",	  "hex:2f6120"}; // "/a "
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		assert(!pathNormalize(BAD[i], out));
	}
	// UTF-8, hex or plain: well-formed only, and no C0 control or DEL. Refused: a lone continuation
	// byte, overlong 'd' and '/', an encoded surrogate, a code point past U+10FFFF, a lead byte
	// past F4, a sequence cut short. A plain field's bytes are refused alike.
	static const char *const MALFORMED[] = {
		"hex:2f80",			"hex:2f33c1a473", "hex:2fc0af", "hex:2f61eda080", "hex:2f61f4908080",
		"hex:2f61f5808080", "hex:2f61e381",	  "/a\xc1\xa4", "/a\xed\xa0\x80", "/a\x7f"};
	for (size_t i = 0; i < sizeof(MALFORMED) / sizeof(MALFORMED[0]); i++) {
		assert(!pathNormalize(MALFORMED[i], out));
	}
	// Accepted: a C1 control (ls shows it as '?', so a name made elsewhere can still be fetched),
	// CJK, and four-byte characters.
	assert(pathNormalize("hex:2f61c29b", out) && strcmp(out, "/a\xc2\x9b") == 0);
	assert(pathNormalize("hex:2fe99fb3e6a5bd2f78", out) &&
		   strcmp(out, "/\xe9\x9f\xb3\xe6\xa5\xbd/x") == 0);
	assert(pathNormalize("hex:2ff09f9880", out) && strcmp(out, "/\xf0\x9f\x98\x80") == 0);
	// FS counts a 4-byte character as two UTF-16 units and any other character as one (the
	// SD_PATH_UNITS edge is tested in testPathRules). For wider characters, the SD_PATH_MAX
	// byte limit is reached first.
	assert(pathUnits("/\xe9\x9f\xb3\xe6\xa5\xbd/\xf0\x9f\x98\x80") == 6); // /音楽/😀
	char longPath[SD_PATH_MAX + 1] = "/";
	for (size_t i = 1; i < SD_PATH_MAX; i += 2) {
		memcpy(longPath + i, "\xc3\xa9", 2);
	}
	longPath[SD_PATH_MAX - 1] = '\0'; // 255 bytes, 128 units
	assert(pathNormalize(longPath, out));
	memcpy(longPath + SD_PATH_MAX - 1, "a", 2);
	assert(!pathNormalize(longPath, out));
	// A hex: path that would decode past its buffer is refused before any byte is decoded.
	char longHex[4 + 2 * 300 + 1] = "hex:2f";
	for (int i = 1; i < 300; i++) {
		strcat(longHex, "61");
	}
	assert(!pathNormalize(longHex, out));

	static const char *const ORDINARY[] = {"/", "/3ds/3drop/log.txt", "/3ds/other/key.txt",
										   "/3ds/3drop/tmp/key.txt", "/3ds/3dropx/key.txt"};
	for (int reading = 0; reading < 2; reading++) { // ASCII names alike, for a read or a change
		assert(pathProtected("/3DS/3Drop/KEY~1.TXT", reading) &&
			   pathProtected("/3ds/3drop", reading) && pathProtected("/3ds", reading) &&
			   pathProtected("/3ds/3drop/key.cache/x", reading));
		// The key files where 3Drop keeps them, however that directory is spelled.
		assert(pathProtected(APP_PATH, reading) &&
			   pathProtected(&KEY_PATH[strlen("sdmc:")], reading) &&
			   pathProtected(&KEY_CACHE_PATH[strlen("sdmc:")], reading));
		for (size_t i = 0; i < sizeof(ORDINARY) / sizeof(ORDINARY[0]); i++) {
			assert(!pathProtected(ORDINARY[i], reading));
		}
	}

	// /Nintendo 3DS under every short name FAT may give it.
	static const char *const KEEP[] = {"/Nintendo 3DS", "/nintendo 3ds/x/y", "/NINTEN~1/x",
									   "/NINTEN~2/x",	"/ninten~9",		 "/NI4F2A~5/dbs"};
	for (size_t i = 0; i < sizeof(KEEP) / sizeof(KEEP[0]); i++) {
		const char *got = pathRemoveRefusal(KEEP[i], false); // NULL for a path that may go
		assert(got && strcmp(got, "refused") == 0);
	}
	static const char *const GO[] = {"/3ds/app",		 "/luma/titles/0004000000123400",
									 "/luma/config.ini", "/Nintendo",
									 "/MYFILE~1",		 "/nix/a~b"};
	for (size_t i = 0; i < sizeof(GO) / sizeof(GO[0]); i++) {
		assert(pathRemoveRefusal(GO[i], false) == NULL);
	}
	// What the console boots from, however FAT spells it (a hashed alias once four BOOT~n exist).
	static const char *const BOOT[] = {"/boot.firm",
									   "/BOOT.FIRM",
									   "/BOOT~1.FIR",
									   "/BO1A2B~5.FIR",
									   "/luma",
									   "/luma/payloads/x.firm",
									   "/Luma/SysModules/y.cxi",
									   "/luma/SYSMOD~1/y"};
	for (size_t i = 0; i < sizeof(BOOT) / sizeof(BOOT[0]); i++) {
		const char *got = pathRemoveRefusal(BOOT[i], false);
		assert(got && strcmp(got, "system") == 0);
		got = sdWrite(BOOT[i], false);
		assert(strcmp(BOOT[i], "/luma") == 0 || (got && strcmp(got, "system") == 0));
	}

	// Under /luma/titles and /luma/plugins only a managed SD application's directory takes a write,
	// however FAT spells it. Every other title answers with titleRefusal's words. A name that is
	// not a title id, or a tree moved onto the two, gets `refused`.
	static const struct {
		const char *path;
		bool tree;
		const char *refusal;
	} WRITES[] = {
		{"/luma/titles/0004000003d37200", false, "self"},
		{"/LUMA/TITLES/0004000003D37200/romfs/x", false, "self"},
		{"/luma/plugins/0004000003d37200/plugin.3gx", false, "self"},
		{"/luma", true, "refused"},
		{"/luma/titles", true, "refused"},
		{"/luma/titles/0004003000008f02/code.bin", false, "unmanaged"}, // HOME Menu
		{"/luma/titles/0004003000008F02", false, "unmanaged"},
		{"/luma/plugins/0004003000008f02/plugin.3gx", false, "unmanaged"},
		{"/luma/titles/0004000100001234/code.bin", false, "unmanaged"},			 // 00040001: a typo
		{"/luma/titles/000400000f800102/code.bin", false, "unmanaged"},			 // variation 0x02
		{"/luma/titles/0004000e00175e00/code.bin", false, "not-application"},	 // a patch
		{"/luma/plugins/0004008c00030002/plugin.3gx", false, "not-application"}, // DLC
		{"/luma/titles/0004000003d372001", false, "refused"},
		{"/luma/titles/000400~1/code.bin", false, "refused"},
		{"/luma/plugins/default.3gx", false, "refused"},
		{"/luma", false, NULL},
		{"/luma/titles", false, NULL},
		{"/luma/titles/000400000f800100/code.bin", true, NULL},
		{"/luma/titles/000400000F800100/romfs", false, NULL},
		{"/luma/config.ini", false, NULL},
		{"/luma/titlesx/0004003000008f02", false, NULL},
		{"/3ds/app.3dsx", true, NULL},
		{"/Nintendo 3DS/x/y", false, "refused"}, // the console's own data
		{"/NINTEN~1", true, "refused"},
		{"/NINTEN~2/x", false, "refused"},
		{"/NI4F2A~5/x", false, "refused"},
		{"/nintendo 3dsx/y", false, NULL},
	};
	for (size_t i = 0; i < sizeof(WRITES) / sizeof(WRITES[0]); i++) {
		const char *want = WRITES[i].refusal, *got = sdWrite(WRITES[i].path, WRITES[i].tree);
		assert(want ? got && strcmp(got, want) == 0 : got == NULL);
	}
	// FS gets every path as UTF-16: a character past the BMP is a surrogate pair, and bytes that
	// do not decode, or units past SD_PATH_UNITS, give PATH_INVALID. fsMkdirs makes the
	// UTF-8 names on the card.
	u16 units[SD_PATH_MAX];
	FS_Path wide = fsPathUtf16(units, "/\xe9\x9f\xb3\xe6\xa5\xbd/\xf0\x9f\x98\x80"); // /音楽/😀
	static const u16 WANT[] = {'/', 0x97F3, 0x697D, '/', 0xD83D, 0xDE00, 0};
	assert(wide.type == PATH_UTF16 && wide.size == sizeof(WANT) &&
		   memcmp(units, WANT, sizeof(WANT)) == 0);
	assert(fsPathUtf16(units, "/a\xc1\xa4").type == PATH_INVALID);
	unsigned calls =
		fake.fsCalls; // a path that will not convert is never handed to a delete or move
	assert(fsDelete(fsArchive(), "/a\xc1\xa4", true, true) == PATH_TOO_LONG &&
		   fsRename("/a", "/a\xc1\xa4", true) == PATH_TOO_LONG && fake.fsCalls == calls);
	char wideEdge[SD_PATH_MAX + 1];
	memset(wideEdge, 'a', sizeof(wideEdge) - 1);
	wideEdge[SD_PATH_MAX] = '\0';
	assert(fsPathUtf16(units, wideEdge).type == PATH_INVALID);
	wideEdge[SD_PATH_UNITS + 1] = '\0';
	assert(fsPathUtf16(units, wideEdge).type == PATH_INVALID);
	wideEdge[SD_PATH_UNITS] = '\0';
	assert(fsPathUtf16(units, wideEdge).type == PATH_UTF16);
	struct stat st;
	assert(fsMkdirs(fsArchive(), "/\xe9\x9f\xb3\xe6\xa5\xbd/\xe3\x82\xa2") == 0 &&
		   stat("sdmc:/\xe9\x9f\xb3\xe6\xa5\xbd/\xe3\x82\xa2", &st) == 0 && S_ISDIR(st.st_mode));
	// fsMkdirs takes at most SD_PATH_UNITS characters, and asks FS nothing for more.
	char edge[SD_PATH_MAX + 1];
	memset(edge, 'a', sizeof(edge) - 1);
	edge[0] = '/';
	edge[SD_PATH_MAX] = '\0';
	assert(fsMkdirs(fsArchive(), edge) == PATH_TOO_LONG);
	edge[SD_PATH_UNITS + 1] = '\0';
	calls = fake.fsCalls;
	assert(fsMkdirs(fsArchive(), edge) == PATH_TOO_LONG && fake.fsCalls == calls);
	edge[SD_PATH_UNITS] = '\0';
	assert(fsMkdirs(fsArchive(), edge) == 0);
}

// foldersIn: a folder that already exists is not a failure in the archive, whatever result the
// archive reports it with (own: the fake's dirExists). A file in the way is still a failure,
// reported with the create's own result.
static void foldersIn(const Target *target, const char *dir, Result own) {
	static const Result REFUSED = (Result)0xC86044D2; // the result for any other failure
	FS_Archive archive;
	u16 units[SD_PATH_MAX];
	char path[64];
	struct stat st;
	mkdir(dir, 0755);
	assert(R_SUCCEEDED(fsTargetOpen(target, &archive)));
	fake.dirExists = own;
	assert(fsMkdirs(archive, "/a/b/c") == 0);
	// /a and /a/b already exist, so a second new folder under them goes through at one stat each.
	unsigned calls = fake.fsCalls;
	snprintf(path, sizeof(path), "%s/a/b/d", dir);
	assert(fsMkdirs(archive, "/a/b/d") == 0 && stat(path, &st) == 0);
	assert(fake.fsCalls - calls == 3 + 2 * 2 + 2); // creates, two stats asked, the last stat
	assert(fsMkdirs(archive, "/a/b/c") == 0);
	// If a file holds the name of the last folder, or of one on the way, fsMkdirs returns the
	// create's own result.
	assert(R_SUCCEEDED(FSUSER_CreateFile(archive, fsPathUtf16(units, "/a/f"), 0, 4)));
	assert(fsMkdirs(archive, "/a/f") == own && fsMkdirs(archive, "/a/f/x") == own);
	snprintf(path, sizeof(path), "%s/a/f/x", dir);
	assert(stat(path, &st) != 0);
	// When the archive refuses a create and no folder is there, fsMkdirs ends after the create and
	// one stat.
	fake.sdGone = REFUSED;
	calls = fake.fsCalls;
	assert(fsMkdirs(archive, "/n/m") == REFUSED && fake.fsCalls - calls == 1 + 2);
	fake.sdGone = 0;
	fake.dirExists = 0;
	fsArchiveClose(archive);
}

// A save and an extdata report an existing folder with two different results, neither the SD
// card's, and with two different descriptions (0xBE, 0x9F). Only the stat, never the result,
// lets a create go on. The SD card, which says FSERROR_ALREADY_EXISTS, isn't asked about the
// folders it has.
static void archiveFolders(void) {
	static const Target SAVE = {.kind = TARGET_SAVE, .id = 0x000400000F800100ULL};
	static const Target EXTDATA = {.kind = TARGET_EXTDATA, .id = 0x55D};
	foldersIn(&SAVE, "save:000400000f800100", (Result)0xC8E044BE);
	foldersIn(&EXTDATA, "extdata:0000055d", (Result)0xC8A0449F);
	// The card takes three creates and the last stat, whether every folder is there or none is.
	assert(fsMkdirs(fsArchive(), "/a/b/c") == 0);
	unsigned calls = fake.fsCalls;
	assert(fsMkdirs(fsArchive(), "/a/b/c") == 0 && fake.fsCalls - calls == 3 + 2);
}

// Inside a save or extdata only a managed SD application that is not 3Drop takes a write, never
// HOME Menu or the system. The refusals are titleRefusal's words, with applicationOnly set.
static void targets(void) {
	Target t;
	assert(targetParse("save:000400000f800100/a/b", &t) && t.kind == TARGET_SAVE &&
		   t.id == 0x000400000F800100ULL && strcmp(t.path, "/a/b") == 0);
	assert(targetParse("save:000400000f800100hex:2f6120", &t) == false); // "/a ": not a path
	assert(targetParse("save:000400000f800100hex:2f612062", &t) && strcmp(t.path, "/a b") == 0);
	assert(targetParse("extdata:0000055d/", &t) && t.kind == TARGET_EXTDATA && t.id == 0x55D &&
		   strcmp(t.path, "/") == 0);
	assert(targetParse("/x", &t) && t.kind == TARGET_SD && t.id == 0);
	static const char *const BAD[] = {"save:000400000F800100/a",
									  "save:000400000f80010/a",
									  "save:000400000f800100",
									  "save:000400000f800100a",
									  "save:000400000f800100sdmc:/a",
									  "extdata:55d/a",
									  "extdata:0000055d/../a",
									  "save:",
									  "hex:736176653a30303034303030303066383030313030"};
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		assert(!targetParse(BAD[i], &t));
	}
	static const struct {
		const char *field, *refusal;
		u64 title;
	} WRITES[] = {
		{"save:000400000f800100/a", NULL, 0x000400000F800100ULL},
		{"save:0004000003d37200/a", "self", 0x0004000003D37200ULL},
		{"save:0004008c00030000/a", "not-application", 0x0004008C00030000ULL}, // DLC
		{"save:0004000e00175e00/a", "not-application", 0x0004000E00175E00ULL}, // a patch
		{"save:0004800400123400/a", "unmanaged", 0x0004800400123400ULL},	   // DSiWare
		{"save:000400000f800102/a", "unmanaged", 0x000400000F800102ULL},	   // variation 0x02
		{"extdata:0000055d/a", NULL, 0x0004000000055D00ULL},
		{"extdata:000fffff/a", NULL, 0x000400000FFFFF00ULL},
		{"extdata:0003d372/a", "self", 0x0004000003D37200ULL},
		{"extdata:0000008f/a", "unmanaged", 0}, // HOME Menu, USA
		{"extdata:000000a9/a", "unmanaged", 0}, // HOME Menu, KOR
		{"extdata:000002ff/a", "unmanaged", 0},
		{"extdata:000014d1/a", "unmanaged", 0}, // badges
		{"extdata:0000165c/a", "unmanaged", 0}, // saved theme layouts
		{"extdata:00100000/a", "unmanaged", 0},
		{"extdata:f000000b/a", "unmanaged", 0}, // NAND shared extdata
	};
	for (size_t i = 0; i < sizeof(WRITES) / sizeof(WRITES[0]); i++) {
		assert(targetParse(WRITES[i].field, &t) && targetTitle(&t) == WRITES[i].title);
		const char *want = WRITES[i].refusal, *got = pathWriteRefusal(&t, false, false);
		assert(want ? got && strcmp(got, want) == 0 : got == NULL);
	}
}

// A refused SD open keeps no handle, so a later call opens the card again.
static void sdOpenRefused(void) {
	struct stat st;
	fake.sdOpenResult = (Result)0xC8804464;
	assert(fsArchive() == 0);
	assert(fsMkdirs(fsArchive(), "/late") == 0 && stat("sdmc:/late", &st) == 0 &&
		   rmdir("sdmc:/late") == 0);
}

// A put on the card asks whether a folder is there. It asks in one call that names the folder,
// where fsStat makes two calls (a file's open, then a folder's) and so searches a path that is not
// there twice.
static void folderProbe(void) {
	struct stat st;
	assert(fsMkdirs(fsArchive(), "/probe/dir") == 0);
	FILE *f = fopen("sdmc:/probe/file", "wb");
	assert(f && fclose(f) == 0);
	unsigned calls = fake.fsCalls;
	assert(fsIsDir(fsArchive(), "/probe/dir") && fake.fsCalls - calls == 1);
	calls = fake.fsCalls;
	assert(!fsIsDir(fsArchive(), "/probe/file") && fake.fsCalls - calls == 1);
	calls = fake.fsCalls;
	assert(!fsIsDir(fsArchive(), "/probe/none") && fake.fsCalls - calls == 1);
	assert(fake.dirs == 0); // the folder it opened is closed
	fake.sdGone = (Result)0xC8804471;
	assert(!fsIsDir(fsArchive(), "/probe/dir")); // a card that fails has no folder to refuse for
	fake.sdGone = 0;
	assert(remove("sdmc:/probe/file") == 0 && rmdir("sdmc:/probe/dir") == 0 &&
		   rmdir("sdmc:/probe") == 0 && stat("sdmc:/probe", &st) != 0);
}

int main(void) {
	sdOpenRefused(); // first, because fsArchive keeps the card's handle from its first open
	paths();
	archiveFolders();
	targets();
	folderProbe();
	TEST_PASS();
	return 0;
}

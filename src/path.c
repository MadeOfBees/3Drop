// SPDX-License-Identifier: GPL-3.0-or-later
#include "path.h"

#include "codec.h"
#include "titles.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

// Checks for well-formed UTF-8 (Unicode's table 3-7) with no C0 control or DEL, because libctru's
// decoder passes an encoded surrogate, which FS would take as a lone unit, and an overlong form
// hides a byte the compares below never see.
static bool utf8Valid(const u8 *s, size_t n) {
	for (size_t i = 0; i < n;) {
		u8 lead = s[i];
		size_t len = lead < 0x80   ? 1
					 : lead < 0xC2 ? 0
					 : lead < 0xE0 ? 2
					 : lead < 0xF0 ? 3
					 : lead < 0xF5 ? 4
								   : 0;
		if (len == 0 || len > n - i || lead < 0x20 || lead == 0x7F) {
			return false;
		}
		// After E0, ED, F0 and F4 the second byte's range shuts out overlongs, surrogates and
		// anything past U+10FFFF.
		u8 lo = lead == 0xE0 ? 0xA0 : lead == 0xF0 ? 0x90 : 0x80;
		u8 hi = lead == 0xED ? 0x9F : lead == 0xF4 ? 0x8F : 0xBF;
		for (size_t k = 1; k < len; k++, lo = 0x80, hi = 0xBF) {
			if (s[i + k] < lo || s[i + k] > hi) {
				return false;
			}
		}
		i += len;
	}
	return true;
}

size_t pathUnits(const char *path) {
	size_t n = 0;
	for (const u8 *s = (const u8 *)path; *s; s++) {
		// Counts a character's first byte, and a 4-byte character's first byte twice.
		n += ((*s & 0xC0) != 0x80) + (*s >= 0xF0);
	}
	return n;
}

bool pathNormalize(const char *field, char out[SD_PATH_MAX]) {
	char decoded[SD_PATH_MAX];
	const char *p = field;
	size_t n = strlen(field);
	if (strncmp(field, "hex:", 4) == 0) {
		// Decoded only once. A decoded "hex:" holds a ':' and is refused, so it is never
		// decoded again.
		n = strlen(field + 4) / 2;
		if (n == 0 || n >= sizeof(decoded) || !hexDecode((u8 *)decoded, field + 4, n)) {
			return false;
		}
		decoded[n] = '\0';
		p = decoded;
	}
	// Every decoded byte, including a NUL, is checked before any compare.
	if (!utf8Valid((const u8 *)p, n)) {
		return false;
	}
	if (strncmp(p, "sdmc:", 5) == 0) {
		p += 5;
	}
	size_t len = strlen(p);
	if (p[0] != '/' || len >= SD_PATH_MAX || pathUnits(p) > SD_PATH_UNITS) {
		return false;
	}
	memcpy(out, p, len + 1);
	if (len == 1) {
		return true; // "/"
	}
	// Every segment must be non-empty and must not end in '.' or ' ' (which also refuses "." and
	// ".."). It must also not have dots beside non-ASCII characters and no other ASCII but spaces.
	// If FS were to trim or drop those characters, a dot segment would move every name after it.
	size_t dots = 0, wide = 0, other = 0; // the segment's bytes so far, by kind
	for (size_t i = 1, start = 1; i <= len; i++) {
		if (out[i] == '/' || out[i] == '\0') {
			bool dotLike = dots > 0 && wide > 0 && other == 0;
			if (i == start || out[i - 1] == '.' || out[i - 1] == ' ' || dotLike) {
				return false;
			}
			start = i + 1;
			dots = wide = other = 0;
		} else if (out[i] == '\\' || out[i] == ':') {
			return false;
		} else if ((u8)out[i] >= 0x80) {
			wide++;
		} else if (out[i] == '.') {
			dots++;
		} else if (out[i] != ' ') {
			other++;
		}
	}
	return true;
}

bool targetParse(const char *field, Target *t) {
	static const struct {
		const char *prefix;
		TargetKind kind;
		size_t bytes; // of the id, as fixed-width lowercase hex
	} SCHEMES[] = {{"save:", TARGET_SAVE, 8}, {"extdata:", TARGET_EXTDATA, 4}};
	*t = (Target){.kind = TARGET_SD};
	for (size_t i = 0; i < sizeof(SCHEMES) / sizeof(SCHEMES[0]); i++) {
		size_t n = strlen(SCHEMES[i].prefix), digits = 2 * SCHEMES[i].bytes;
		char hex[17];
		u8 id[8];
		if (strncmp(field, SCHEMES[i].prefix, n) != 0) {
			continue;
		}
		field += n;
		if (strnlen(field, digits) < digits) {
			return false;
		}
		memcpy(hex, field, digits);
		hex[digits] = '\0';
		if (!hexDecode(id, hex, SCHEMES[i].bytes)) {
			return false;
		}
		t->id = readBE(id, SCHEMES[i].bytes);
		t->kind = SCHEMES[i].kind;
		field += digits;
		if (field[0] != '/' && strncmp(field, "hex:", 4) != 0) {
			return false; // no `sdmc:` inside an archive
		}
		break;
	}
	return pathNormalize(field, t->path);
}

u64 targetTitle(const Target *t) {
	if (t->kind == TARGET_SAVE) {
		return t->id;
	}
	bool application = t->kind == TARGET_EXTDATA && titleUidIsApplication((u32)t->id) &&
					   t->id != 0x14D1 && t->id != 0x165C;
	return application ? 0x0004000000000000ULL | t->id << 8 : 0;
}

// Compares ignoring case, as FAT does.
static bool nameIs(const char *c, size_t n, const char *name, bool prefix) {
	size_t nameLen = strlen(name);
	return (prefix ? n >= nameLen : n == nameLen) && strncasecmp(c, name, nameLen) == 0;
}

// Any non-ASCII byte matches too. Below the root, a look-alike counts as every name it may stand
// for, whatever its length, because no legitimate name there needs one.
static bool componentIs(const char *c, size_t n, const char *name, bool prefix) {
	for (size_t i = 0; i < n; i++) {
		if ((u8)c[i] >= 0x80) {
			return true;
		}
	}
	return nameIs(c, n, name, prefix);
}

// Matches the root's names only. The console probes found that FS folds, drops and trims none of
// the look-alikes tried, so a non-ASCII character stands for one character of the name, and
// /音楽 is not a name.
static bool rootIs(const char *c, size_t n, const char *name, bool prefix) {
	size_t i = 0, k = 0;
	for (; i < n && name[k] != '\0'; k++) {
		if ((u8)c[i] < 0x80 && tolower((u8)c[i]) != tolower((u8)name[k])) {
			return false;
		}
		do { // past the character's continuation bytes
			i++;
		} while (i < n && ((u8)c[i] & 0xC0) == 0x80);
	}
	return name[k] == '\0' && (prefix || i == n);
}

bool pathProtected(const char *path, bool reading) {
	// Anything under a key name is protected too, because a directory made at key.cache/x would
	// stop the cache being written, and the stretch would run at every boot.
	static const char *const DIRS[] = {"3ds", "3drop"}; // APP_PATH's directories, as testFs checks
	bool (*is)(const char *, size_t, const char *, bool) = reading ? nameIs : componentIs;
	const char *c = path;
	for (int depth = 0; *c == '/' && c[1] != '\0'; depth++) {
		const char *name = ++c;
		c += strcspn(c, "/");
		size_t n = (size_t)(c - name);
		if (depth == 2) {
			return is(name, n, "key", true);
		}
		bool (*match)(const char *, size_t, const char *, bool) =
			depth == 0 && !reading ? rootIs : is;
		if (!match(name, n, DIRS[depth], false)) {
			return false;
		}
		if (*c == '\0') {
			return true; // /3ds or /3ds/3drop, since moving either would take the keys out of reach
		}
	}
	return false;
}

// The root component is the `n` bytes at `name`. Any root `~` name starting "ni" could be
// /Nintendo 3DS's 8.3 alias (NINTEN~1 to ~4, then hashed), so no numbering can miss it.
static bool consoleData(const char *name, size_t n) {
	return rootIs(name, n, "nintendo 3ds", false) ||
		   (rootIs(name, n, "ni", true) && memchr(name, '~', n) != NULL);
}

// Any `~` name that could alias a boot input counts, as it does for the console's data. So does
// the backup of /boot.firm, which the next put there restores before any check, so only a put's
// renames write it.
static bool bootInput(const char *name, size_t n) {
	// What Luma loads unsigned at boot; with external FIRMs on, a *.firm here replaces NAND's.
	static const char *const LUMA[] = {"payloads", "sysmodules", "native.firm",	   "twl.firm",
									   "agb.firm", "safe.firm",	 "sysupdater.firm"};
	if (rootIs(name, n, "boot.firm", false) || rootIs(name, n, "boot.firm" SD_PUT_BACKUP, false) ||
		(rootIs(name, n, "bo", true) && memchr(name, '~', n) != NULL)) {
		return true;
	}
	if (!rootIs(name, n, "luma", false) || name[n] == '\0') {
		return false;
	}
	name += n + 1;
	n = strcspn(name, "/");
	for (size_t i = 0; i < sizeof(LUMA) / sizeof(LUMA[0]); i++) {
		if (componentIs(name, n, LUMA[i], false)) {
			return true;
		}
	}
	return memchr(name, '~', n) != NULL;
}

bool pathBootFirm(const char *path) {
	return nameIs(path + 1, strlen(path + 1), "boot.firm", false);
}

// What `system` opens: a file directly in /luma/payloads or /luma/sysmodules, and, for a put,
// /boot.firm. Every name must be spelled exactly, since a look-alike or an alias could be any
// boot input.
static bool systemFile(const char *path, bool put) {
	const char *name = path + 1;
	size_t n = strcspn(name, "/");
	if (put && pathBootFirm(path)) {
		return true;
	}
	if (!nameIs(name, n, "luma", false) || name[n] == '\0') {
		return false;
	}
	name += n + 1;
	n = strcspn(name, "/");
	return (nameIs(name, n, "payloads", false) || nameIs(name, n, "sysmodules", false)) &&
		   name[n] == '/' && strchr(name + n + 1, '/') == NULL;
}

const char *pathRemoveRefusal(const char *path, bool system) {
	const char *name = path + 1;
	size_t n = strcspn(name, "/");
	// "/" and "/3ds" never get here: argTarget answers them first.
	if (consoleData(name, n)) {
		return "refused";
	}
	// /luma itself would take its payloads and sysmodules with it.
	bool luma = name[n] == '\0' && rootIs(name, n, "luma", false);
	bool opened = system && systemFile(path, false);
	return (bootInput(name, n) && !opened) || luma ? "system" : NULL;
}

// Parses the title id a path component spells, as FAT matches it: 16 hex digits of either case.
static bool componentTid(const char *c, u64 *titleID) {
	char tid[17];
	if (strcspn(c, "/") != 16) {
		return false;
	}
	for (size_t i = 0; i < 16; i++) {
		tid[i] = (char)tolower((unsigned char)c[i]);
	}
	tid[16] = '\0';
	return parseTid(tid, titleID);
}

// A file there would be taken for a put's temp or backup and replaced, and a folder would stop
// that put's renames. This applies to any component, which a put or a mkdir makes. FAT folds
// ASCII case only.
static bool putSibling(const char *path) {
	size_t k = strlen(SD_PUT_TEMP);
	for (const char *c = path; *c == '/';) {
		const char *name = ++c;
		c += strcspn(c, "/");
		size_t n = (size_t)(c - name);
		if (n >= k && (strncasecmp(name + n - k, SD_PUT_TEMP, k) == 0 ||
					   strncasecmp(name + n - k, SD_PUT_BACKUP, k) == 0)) {
			return true;
		}
	}
	return false;
}

const char *pathWriteRefusal(const Target *t, bool tree, bool system) {
	const char *path = t->path, *name = path + 1;
	size_t n = strcspn(name, "/");
	u64 titleID;
	// Inside a save or SD extdata, the refusal is its title's, as for a Luma directory.
	if (t->kind != TARGET_SD) {
		return titleRefusal(targetTitle(t), true);
	}
	if (consoleData(name, n)) {
		return "refused";
	}
	if (bootInput(name, n) && !(system && systemFile(path, true))) {
		return "system";
	}
	if (putSibling(path)) {
		return "reserved";
	}
	// These are where Luma loads per-title files: LayeredFS code and romfs (titles), and Rosalina's
	// plugins. A tree moved onto /luma or onto either directory could carry any title's patch.
	if (!rootIs(name, n, "luma", false)) {
		return NULL;
	}
	if (name[n] == '\0') {
		return tree ? "refused" : NULL;
	}
	name += n + 1;
	n = strcspn(name, "/");
	if (!componentIs(name, n, "titles", false) && !componentIs(name, n, "plugins", false)) {
		return NULL;
	}
	if (name[n] == '\0') {
		return tree ? "refused" : NULL;
	}
	if (!componentTid(name + n + 1, &titleID)) {
		return "refused"; // Luma patches by title id, and an 8.3 alias may spell one
	}
	return titleRefusal(titleID, true);
}

bool pathLumaTitleFile(const char *path, const char *file) {
	static const char *const DIRS[] = {"luma", "titles"};
	const char *c = path + 1;
	u64 titleID;
	for (size_t i = 0; i < sizeof(DIRS) / sizeof(DIRS[0]); i++) {
		size_t n = strcspn(c, "/");
		if (!nameIs(c, n, DIRS[i], false) || c[n] != '/') {
			return false;
		}
		c += n + 1;
	}
	return componentTid(c, &titleID) && c[16] == '/' && strcasecmp(c + 17, file) == 0;
}

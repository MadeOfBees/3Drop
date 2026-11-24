// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CMD_FS_H
#define CMD_FS_H

#include "path.h"
#include "protocol.h"

// Takes field i as an SD path and refuses a key file's path. A change also refuses a look-alike of
// one (pathProtected).
bool argPath(VerbContext *c, size_t i, Target *t, bool reading);
// Returns field i's index among words (a field that wasn't sent reads as ""), or -1 after replying
// `grammar`.
int argWord(VerbContext *c, size_t i, const char *const words[], size_t n);
// Replies `stranded` while a worker can still reach tier 1, the hash engine or an import.
bool argIdle(VerbContext *c);
bool argDigest(VerbContext *c, const char *text, u8 out[32]);
void lsPath(VerbContext *c, const char *flags);
void cmdGet(VerbContext *c);
void cmdHash(VerbContext *c);
bool checkPutFile(VerbContext *c);
void runPutFile(VerbContext *c);
bool checkDeletePath(VerbContext *c, bool recursive, bool system); // the flag cmd.c read
void runDeletePath(VerbContext *c);
bool checkMkdir(VerbContext *c);
void runMkdir(VerbContext *c);
bool checkRename(VerbContext *c);
void runRename(VerbContext *c);

#endif // CMD_FS_H

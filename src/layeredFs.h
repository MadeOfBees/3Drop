// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LAYEREDFS_H
#define LAYEREDFS_H

// Luma3DS's loader stops on an assertion (svcBreak) when it starts an app that has a LayeredFS
// romfs folder and none of the Nintendo SDK's file-system code, which only a game built with the
// SDK holds, and the console crashes. FS does not show an app any other title's code, so 3Drop
// cannot tell the two apart; the user says which a title is. These functions answer `layeredfs`.

#include <3ds.h>
#include <stdbool.h>

// Called before a launch the user did not call a game. Returns NULL when the title has
// no romfs folder, else `layeredfs`, with FS's Result in `why` when the card would not
// say whether there is one.
const char *layeredFsRefusal(u64 titleID, Result *why);

// Called before a write of `size` bytes at `path`. Returns `layeredfs` for an
// exheader.bin Luma would stop on.
const char *layeredFsWriteRefusal(const char *path, u64 size);

// Called before a folder moves from `from` onto `to`. It gives the same answer for the
// exheader.bin in the folder when `to` is a title's own directory. `why` carries FS's Result
// when the card would not say.
const char *layeredFsFolderRefusal(const char *from, const char *to, Result *why);

#endif // LAYEREDFS_H

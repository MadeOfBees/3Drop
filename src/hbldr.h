// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HBLDR_H
#define HBLDR_H

// Luma turns one installed placeholder title into whatever 3dsx hb:ldr was last told to load.

#include <3ds.h>
#include <stdbool.h>

// The layout is from Luma's private luma_shared_config.h. Its pm copies the selected tid over the
// live one when an application ends, so the selected one counts. Without Luma the page holds
// something else, so hb:ldr must answer too.
typedef struct {
	u64 hbldr3dsxTid;
	u64 selectedHbldr3dsxTid;
	bool useHbldr;
} LumaSharedConfig;
#define LUMA_SHARED_CONFIG ((volatile LumaSharedConfig *)(OS_SHAREDCFG_VADDR + 0x800))

bool hbldrPresent(void);
// The target goes last, so a refusal leaves none behind for the next load.
Result hbldrArm(const char *path, const char *extra);

#endif // HBLDR_H

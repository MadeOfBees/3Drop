// SPDX-License-Identifier: GPL-3.0-or-later
// Sends hb:ldr's commands the way hbmenu's loaders/rosalina.c does, because libctru has no wrapper
// for them.
#include "hbldr.h"

#include "path.h" // SD_PATH_MAX

#include <stdio.h>
#include <string.h>

// Luma copies this many bytes of argv whatever the descriptor says, so the whole buffer is sent. It
// holds the argument count, then "sdmc:" and the path, then the extra argument if there is one,
// each NUL-ended. Luma lays out its own default the same way (loader/source/hbldr.c, 3dsx.h).
#define ARGV_SIZE 0x400
// The path is the one long part, and the extra argument is sized by what is left after it.
_Static_assert(sizeof(u32) + sizeof("sdmc:") + SD_PATH_MAX < ARGV_SIZE,
			   "a path leaves room in the buffer for the extra argument");

static Result hbldrSend(Handle hbldr, u32 command, u32 buffer, const void *data, size_t size) {
	u32 *cmdbuf = getThreadCommandBuffer();
	cmdbuf[0] = IPC_MakeHeader(command, 0, 2);
	cmdbuf[1] = IPC_Desc_StaticBuffer(size, buffer);
	cmdbuf[2] = (u32)(uintptr_t)data;
	Result r = svcSendSyncRequest(hbldr);
	return R_SUCCEEDED(r) ? (Result)cmdbuf[1] : r;
}

bool hbldrPresent(void) {
	Handle hbldr;
	if (R_FAILED(svcConnectToPort(&hbldr, "hb:ldr"))) {
		return false;
	}
	svcCloseHandle(hbldr);
	return true;
}

Result hbldrArm(const char *path, const char *extra) {
	static u32 argv[ARGV_SIZE / sizeof(u32)];
	char *at = (char *)&argv[1];
	Handle hbldr;
	memset(argv, 0, sizeof(argv));
	argv[0] = extra ? 2 : 1;
	at += snprintf(at, ARGV_SIZE - sizeof(u32), "sdmc:%s", path) + 1;
	if (extra) {
		snprintf(at, (size_t)((char *)argv + ARGV_SIZE - at), "%s", extra);
	}
	Result r = svcConnectToPort(&hbldr, "hb:ldr");
	if (R_FAILED(r)) {
		return r;
	}
	r = hbldrSend(hbldr, 3, 1, argv, sizeof(argv)); // SetArgv, static buffer 1
	if (R_SUCCEEDED(r)) {
		r = hbldrSend(hbldr, 2, 0, path, strlen(path) + 1); // SetTarget, static buffer 0
	}
	svcCloseHandle(hbldr);
	return r;
}

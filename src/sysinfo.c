// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo.h"

#include <3ds.h>
#include <stdio.h>

static u8 modelId = 0xFF; // CFG_SystemModel
static char model[8] = "-", facts[128] = " model=- new3ds=- sysver=- luma=- region=-";

void sysinfoInit(void) {
	static const char *const MODELS[] = {"o3ds", "o3dsxl", "n3ds", "o2ds", "n3dsxl", "n2dsxl"};
	static const char *const REGIONS[] = {"JPN", "USA", "EUR", "AUS", "CHN", "KOR", "TWN"};
	u8 region = 0xFF; // CFG_Region
	bool isNew = false;
	char sysver[24], luma[24] = "-";
	s64 version;
	if (R_SUCCEEDED(cfguInit())) {
		if (R_FAILED(CFGU_GetSystemModel(&modelId))) {
			modelId = 0xFF;
		}
		if (R_FAILED(CFGU_SecureInfoGetRegion(&region))) {
			region = 0xFF;
		}
		cfguExit();
	}
	if (modelId < sizeof(MODELS) / sizeof(MODELS[0])) {
		snprintf(model, sizeof(model), "%s", MODELS[modelId]);
	}
	Result newRc = APT_CheckNew3DS(&isNew);
	// 0 is its only success.
	if (osGetSystemVersionDataString(NULL, NULL, sysver, sizeof(sysver)) != 0) {
		snprintf(sysver, sizeof(sysver), "-");
	}
	// Only Luma's kernel extension answers type 0x10000; any other kernel refuses it.
	if (R_SUCCEEDED(svcGetSystemInfo(&version, 0x10000, 0))) {
		u32 v = (u32)version;
		snprintf(luma, sizeof(luma), "%lu.%lu.%lu", (unsigned long)GET_VERSION_MAJOR(v),
				 (unsigned long)GET_VERSION_MINOR(v), (unsigned long)GET_VERSION_REVISION(v));
	}
	snprintf(facts, sizeof(facts), " model=%s new3ds=%s sysver=%s luma=%s region=%s", model,
			 R_FAILED(newRc) ? "-"
			 : isNew		 ? "1"
							 : "0",
			 sysver, luma, region < sizeof(REGIONS) / sizeof(REGIONS[0]) ? REGIONS[region] : "-");
}

u8 sysinfoModelId(void) {
	return modelId;
}

const char *sysinfoModel(void) {
	return model;
}

void sysinfoFormat(char *out, size_t cap) {
	u8 battery = OS_SharedConfig->led_battery; // bits laid out as sysinfoOnAc documents
	// lin_free is the linear heap's free bytes, as the bench's `mem` reply calls them. malloc draws
	// on the application heap, which is another pool.
	snprintf(out, cap, " wifi=%u bat=%u charging=%u ac=%u lin_free=%lu%s", osGetWifiStrength(),
			 (battery >> 2) & 7u, (battery >> 1) & 1u, battery & 1u,
			 (unsigned long)linearSpaceFree(), facts);
}

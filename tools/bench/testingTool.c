// SPDX-License-Identifier: GPL-3.0-or-later
// The Testing Tool is content 0 of every fixture. It logs what the MCU shows of the LED, so the
// LED's device checks read numbers, not eyes. It also shows what hb:ldr and Luma say of the 3dsx
// loader, and it returns to 3Drop.
#include <3ds.h>

#include "app.h"   // uptimeMs, and led.h through it
#include "hbldr.h" // LUMA_SHARED_CONFIG only (the tool links testingTool.c, led.c and ledRead.c)
#include "path.h"  // APP_DIR

#include <stdio.h>

#define TOOL_NAME "3Drop Testing Tool"
#define LOG APP_DIR "/led-probe.log"
// The host puts the marker before `run`. A run consumes it, and the evidence comes from that run
// alone.
#define MARKER APP_DIR "/testing-tool.auto"
#define EVIDENCE APP_DIR "/testing-tool.txt"
// A sample is one logged read of the MCU's LED register; led.h's LED_SAMPLE_MS is a pattern step.
#define SAMPLE_EVERY_MS 100
// This is longer than the lit pattern's 15.9 s (led.h), so a phase sees it end.
#define PHASE_MS 20000
// The length of the R key's sampling run, and the number of opens SELECT's script times.
#define SAMPLE_RUN_MS 60000
#define OPEN_COUNT 100
// libctru's console takes ESC [ row ; column H. This one is near the bottom of the top screen.
#define BREATHING_AT "\x1b[29;4H"
#define PURPLE 0xFF00FF

// These are four probe colors of the tool's own, because it tests the driver, not 3Drop's table.
// The label is the legend.
static const struct {
	u32 key, rgb;
	const char *label;
} PROBES[] = {
	{KEY_A, PURPLE, "A purple"},
	{KEY_X, 0x0000FF, "X blue"},
	{KEY_Y, 0x00FF00, "Y green"},
	{KEY_B, 0xFF0000, "B red"},
};
#define PROBE_COUNT (sizeof(PROBES) / sizeof(PROBES[0]))

static FILE *logFile;

// The framebuffer is BGR8 and single-buffered (set in main), so blue goes first and no swap
// follows.
static void fill(u32 rgb) {
	u16 w, h;
	u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
	u32 c[3];
	ledChannels(rgb, c);
	for (u32 i = 0; i < (u32)w * h; i++) {
		fb[i * 3] = (u8)c[2];
		fb[i * 3 + 1] = (u8)c[1];
		fb[i * 3 + 2] = (u8)c[0];
	}
	gfxFlushBuffers();
}

// The evidence (EVIDENCE, and LOG for a run by hand) consists of these lines: `build <tool>
// <version>`, then `title <16 hex>`; `# <heading>` per phase; per sample `<ms> <rrggbb>`, stamped
// at the end of its read, or `<ms> err <result>`; a phase's first write as `write <result>`, each
// refresh as `<ms> write <result>`, stamped at its start; per open `<n> <result> <us> us`; `done`
// last. probe.sh reads build, title, the samples and done.
static void sample(u64 start) {
	u8 rgb[3];
	Result r = ledRead(rgb);
	if (R_SUCCEEDED(r)) {
		fprintf(logFile, "%llu %02x%02x%02x\n", uptimeMs() - start, rgb[0], rgb[1], rgb[2]);
	} else {
		fprintf(logFile, "%llu err %08lx\n", uptimeMs() - start, (unsigned long)r);
	}
}

// A phase writes the LED (again every LED_REFRESH_MS, if refresh) and samples it for ms. With rgb 0
// it only samples: it writes nothing, and the LED shows what it was last given.
static void phase(const char *what, Led led, bool refresh, u64 ms) {
	unsigned long rgb = led.rgb;
	u64 start = uptimeMs(), written = start;
	fprintf(logFile, "# %s", what);
	if (rgb) {
		fprintf(logFile, " rgb=%06lx breathes=%d refresh=%d\n", rgb, led.breathes, refresh);
		fprintf(logFile, "write %08lx\n", (unsigned long)ledWrite(led));
	} else {
		fprintf(logFile, " write=none\n");
	}
	while (uptimeMs() - start < ms && aptMainLoop()) {
		if (refresh && uptimeMs() - written >= LED_REFRESH_MS) {
			written = uptimeMs();
			fprintf(logFile, "%llu write %08lx\n", written - start, (unsigned long)ledWrite(led));
		}
		sample(start);
		svcSleepThread(SAMPLE_EVERY_MS * 1000000LL);
	}
	fflush(logFile);
}

// SELECT runs the timing checks, hands-free. mcu::HWC is also opened back to back, because a port
// can still be full just after a close (0xD0401834).
static void script(void) {
	phase("one-shot", (Led){PURPLE, false}, false, PHASE_MS);
	phase("refreshed", (Led){PURPLE, false}, true, PHASE_MS);
	phase("breathing", (Led){PURPLE, true}, true, PHASE_MS);
	fprintf(logFile, "# opens\n");
	for (int i = 0; i < OPEN_COUNT; i++) {
		u64 tick = svcGetSystemTick();
		Result r = ledOpen();
		u64 us = (svcGetSystemTick() - tick) * 1000000 / SYSCLOCK_ARM11; // times the open alone
		if (R_SUCCEEDED(r)) {
			mcuHwcExit();
		}
		fprintf(logFile, "%d %08lx %llu us\n", i, (unsigned long)r, us);
	}
	(void)ledWrite(LED_OFF);
	fflush(logFile);
}

int main(void) {
	bool returnTo3Drop = false, breathes = false;
	// The marker is removed first because a run that dies must not make every later launch
	// unattended.
	bool unattended = remove(MARKER) == 0;

	gfxInitDefault();
	// Stay awake with the lid closed, as 3Drop does. An app that allows sleep (aptInit's default)
	// sleeps when the lid closes, and the LED would show the system's sleep and wake patterns
	// instead.
	aptSetSleepAllowed(false);
	consoleInit(GFX_TOP, NULL);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	logFile = fopen(unattended ? EVIDENCE : LOG, unattended ? "w" : "a");
	if (!logFile) { // no SD card to log to, so the samples go to the screen
		logFile = stdout;
	}
	// Every fixture carries this build under its own id, shown so the title can be uninstalled.
	u64 titleID = 0;
	u8 media = 0;
	(void)APT_GetAppletInfo((NS_APPID)envGetAptAppId(), &titleID, &media, NULL, NULL, NULL);
	if (unattended) {
		fprintf(logFile, "build " TOOL_NAME " %s\n", APP_VERSION);
		fprintf(logFile, "title %016llx\n", (unsigned long long)titleID);
	}
	// This sample comes before the baseline, so it shows what HOME Menu or the last exit left on
	// the LED.
	fprintf(logFile, "# launch\n");
	sample(uptimeMs());
	// A probe begins from a known baseline even when launched directly from HOME. 3Drop's own
	// startup never does this, because it would erase a self-update's green handoff.
	(void)ledWrite(LED_OFF);

	printf("\n\n   " TOOL_NAME "\n");
	printf("\n   Title %016llx\n", (unsigned long long)titleID);
	printf("\n   LED:");
	for (size_t i = 0; i < PROBE_COUNT; i++) {
		printf(" %s%s", i ? " " : "", PROBES[i].label);
	}
	printf("\n        L breathe on/off\n");
	printf("        R sample %d s   SELECT timing checks\n", SAMPLE_RUN_MS / 1000);
	printf("        log " LOG "\n");
	printf("\n   Press START to open 3Drop.\n");
	// A read-only check of what `run <path.3dsx>` depends on. It doesn't call hbldrPresent(), which
	// drops the Result shown below, and the tool links no hbldr.c.
	Handle hbldr;
	Result port = svcConnectToPort(&hbldr, "hb:ldr");
	if (R_SUCCEEDED(port)) {
		svcCloseHandle(hbldr);
	}
	printf("\n   hb:ldr %08lx  use_hbldr %d\n", (unsigned long)port, LUMA_SHARED_CONFIG->useHbldr);
	printf("   3dsx title %016llx\n   selected   %016llx\n",
		   (unsigned long long)LUMA_SHARED_CONFIG->hbldr3dsxTid,
		   (unsigned long long)LUMA_SHARED_CONFIG->selectedHbldr3dsxTid);
	gfxFlushBuffers();
	gfxSwapBuffers();

	// A closing HOME leaves no done line, so that the host does not read a cut script as a whole
	// one.
	if (unattended) {
		script();
		if (aptMainLoop()) {
			fprintf(logFile, "done\n");
			returnTo3Drop = true;
		}
	}
	while (!returnTo3Drop && aptMainLoop()) {
		gspWaitForVBlank();
		hidScanInput();
		u32 pressed = hidKeysDown();
		if (pressed & KEY_START) {
			returnTo3Drop = true;
			break;
		}
		if (pressed & KEY_L) {
			breathes = !breathes;
			printf(BREATHING_AT "breathing %s ", breathes ? "on" : "off");
			gfxFlushBuffers();
		}
		for (size_t i = 0; i < PROBE_COUNT; i++) {
			if (pressed & PROBES[i].key) {
				fill(PROBES[i].rgb);
				fprintf(logFile, "# probe rgb=%06lx breathes=%d write %08lx\n",
						(unsigned long)PROBES[i].rgb, breathes,
						(unsigned long)ledWrite((Led){PROBES[i].rgb, breathes}));
			}
		}
		if (pressed & KEY_R) {
			phase("sample", LED_OFF, false, SAMPLE_RUN_MS);
		}
		if (pressed & KEY_SELECT) {
			script();
		}
	}

	// When closed from HOME, the tool leaves its pattern, so the launch sample of the next run
	// shows what HOME Menu did to it. Otherwise it leaves the LED off, as 3Drop does.
	if (!aptShouldClose()) {
		(void)ledWrite(LED_OFF);
	}
	if (logFile != stdout) {
		fclose(logFile);
	}
	if (returnTo3Drop) {
		aptSetChainloader(SELF_TITLE_ID, MEDIATYPE_SD); // goes back to 3Drop from aptExit
	} else {
		aptSetSleepAllowed(
			true); // not before a chainload, because with the lid closed APT would sleep first
	}
	gfxExit();
	return 0;
}

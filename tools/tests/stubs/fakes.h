// A test overrides any fake by defining the function; otherwise it scripts and reads this state.
#ifndef TEST_STUBS_FAKES_H
#define TEST_STUBS_FAKES_H

#include <3ds.h>
#include <stdio.h>
#include <string.h>

// A test's asserts do its work. With them compiled out, a test would pass having checked nothing.
#ifdef NDEBUG
#error "the tests build without NDEBUG"
#endif

// The line run.sh requires of a C test. A test that returns from main early never prints it.
#define TEST_PASS() testPass(__BASE_FILE__)
void testPass(const char *file);

typedef struct {
	u64 nowMs, stepMs; // the uptime: a read of the clock returns nowMs, then advances it by stepMs
	s64 wallMs;		   // osGetTime, the system clock, reads this far from the uptime: its steps
	u64 bootMs;		   // the system tick counts from here on the same clock
	u32 keysDown;
	bool homeBlocked, powerPressed;
	bool rendererFails; // C3D_Init answers false
	bool textureFails;	// C3D_TexInit answers false: no memory for the scan's picture
	bool linearFull;	// linear memory holds tier 1 alone; a smaller linearMemAlign returns NULL
	// AM: a scripted title, 3Drop itself (none when selfTitleID is 0), a pending import, a ticket.
	u64 titleID, size, selfTitleID, pendingID, ticketID;
	u32 moreTitles; // applications AM lists after those, with no info for any unless moreInfo
	bool moreInfo;
	u16 version, selfVersion;
	bool installed;
	bool importTicket; // an import's pipe committed the title's ticket ahead of its TMD
	u64 titleDeleted, pendingDeleted, ticketDeleted;
	u64 chainload; // the title libctru's chainloader was last set to (self: selfTitleID)
	Result fsFail; // non-zero: FSUSER_DeleteFile answers it, for a path ending in fsFailOn if set
	const char *fsFailOn;
	Result treeFail; // non-zero: a recursive delete answers it, past its first treeFailAfter files
	unsigned treeFailAfter;
	Result sdGone; // non-zero: every path call on the card answers it and touches nothing
	// Non-zero: a name that is not there answers it instead of FSERROR_NOT_FOUND (FS has two codes
	// for this).
	Result notFound;
	// Non-zero: FSUSER_CreateDirectory on a save or an extdata answers it for a folder that is
	// already there, instead of FSERROR_ALREADY_EXISTS, which is the SD card's code. FS's other
	// archives may have codes of their own.
	Result dirExists;
	int dirs;  // directories opened with FSUSER_OpenDirectory and not yet closed
	int files; // files opened with FSUSER_OpenFile and not yet closed
	// Non-zero: opening a save or extdata archive answers it, once archiveOpensOk more opens (a
	// check's) have gone through; archives counts those open.
	Result archiveResult;
	unsigned archiveOpensOk;
	Result sdOpenResult; // non-zero: the next SD archive open answers it, once
	int archives;
	Result commitResult, secureValueResult;
	unsigned commits, secureDeletes;
	u64 secureValue;
	Result writeResult; // non-zero: FSFILE_Write answers it
	bool shortWrite;	// FSFILE_Write moves one byte fewer than asked and succeeds
	u64 writeMs;		// each FSFILE_Write moves the clock this far, as a slow card would
	// Non-zero: FSFILE_Flush, FSFILE_SetSize, FSDIR_Read and the SD archive resource answer it.
	Result fileFlushResult, setSizeResult, dirReadResult, sdmcResourceResult;
	// Non-zero: a rename whose source ends with renameFrom answers renameResult.
	Result renameResult;
	const char *renameFrom;
	Result hbldrResult, argvResult, targetResult;
	int hbldrOpen;
	// srv: srvIpcResult fails every IPC, and srvOpenIpcResult fails only GetServiceHandle.
	// srvUnknown is an sm that does not match the name asked. srvResult refuses GetServiceHandle
	// (a full port). srvNoHandle makes GetServiceHandle answer 0 with no handle. srvName holds the
	// last GetServiceHandle's 8 name bytes, which Luma's kernel names the session by, and srvFlags
	// holds its flags word.
	Result srvResult, srvIpcResult, srvOpenIpcResult;
	bool srvUnknown, srvNoHandle;
	char srvName[9];
	u32 srvFlags;
	unsigned srvAsks, srvOpens; // IsServiceRegistered and GetServiceHandle requests
	u32 hbldrArgv[0x400 / 4];
	char hbldrTarget[256];
	// Non-zero: svcFlushProcessDataCache and the hardware SHA engine answer it. The engine does so
	// from its shaFailAt-th call on, counted from when shaFailAt was set (0: from the first).
	Result flushResult, shaResult;
	unsigned shaFailAt;
	// Where the last FSFILE_Write ended, and where it stood at each engine call (shaCalls of them):
	// a unit hashed after its own write shows as an end past that unit's start.
	u64 writtenTo, shaWrittenTo[8];
	unsigned shaCalls;
	// The first svcFlushProcessDataCache calls (flushes of them): each range, and writtenTo as it
	// stood then, so a range flushed before any write shows an end of 0.
	u32 flushAddr[8], flushSize[8];
	u64 flushWrittenTo[8];
	unsigned flushes;
	Result psResult; // non-zero: PS_GenerateRandomBytes answers it
	// FS's seed database, holding one title's seed. GetSeed answers seedGetResult when set, else
	// the seed or SEED_ABSENT. AddSeed answers seedAddResult, and when that is 0 it takes the seed
	// and counts it.
	u64 seedTitleID;
	u8 seed[16];
	Result seedGetResult, seedAddResult;
	unsigned seedAdds;
	u8 needsCleanup;
	// Calls to AM_InitializeExternalTitleDatabase, and those with overwrite set.
	unsigned titleDbInits, titleDbOverwrites;
	u32 pending, freeClusters; // freeClusters: of the SD, the directory ./sdmc: of the test
	// AM's answers when set; cleanupResult is NeedsCleanup's, infoResult the scripted title's.
	Result infoResult, pendingInfoResult, cleanupResult, deletePendingResult, deleteTitleResult;
	Result ticketResult, startResult, cancelResult, finishResult, ticketCountResult;
	Result numTicketsResult;
	bool finishInstalls; // AM_FinishCiaInstall leaves fake.titleID installed, whatever it answers
	// luma 0 means another kernel, which refuses the query.
	s64 luma;
	unsigned mcuOpens;
	u8 mcuInfo[0x13];
	Result mcuReadResult;
	u8 shell;
	Result shellResult;
	unsigned amCalls, amMutations, starts, overwriteStarts, cancels, titleDeletes, pendingDeletes,
		ticketDeletes;
	unsigned cleanups, replies;
	unsigned fsCalls; // FS IPCs: path calls, FSFILE calls on an open file, the SHA engine
	// A card folder, given as its host path ("sdmc:/dir"). `watched` counts each name in it that
	// a path call gives FS to look up (a rename gives two), and FAT searches the whole folder for
	// every one.
	const char *watch;
	unsigned watched;
	// FSFILE_Read and FSFILE_Write finish nothing until a thread join begins, as with a stuck card.
	volatile bool cardHeld;
	Result joinResult; // non-zero: a thread join answers it and joins nothing, which strands a ring
	unsigned threads;  // threads created and not yet freed
	int threadFailPrio; // threadCreate refuses this priority only
	int threadHoldPrio; // a thread of this priority starts only once the card is let go
	// sendStr keeps a request's first reply line in reply, its latest in last, each MAC beside it.
	char reply[2048], replyMac[132], last[2048], lastMac[132], body[16384], log[64];
	bool sendFails;		// sendStr answers false (the line may or may not have gone out)
	bool bodySendFails; // sendBytes, and a pump's send(), take nothing and fail
	size_t bodyLen;
	Result logResult;
	u64 logBytes;
	unsigned logs;
	Result sslcResult; // non-zero: sslcInit answers it
	unsigned sslcInits, sslcExits;
} Fake;

extern Fake fake;

// Records what the fake appendLog records. A test with its own appendLog calls it too.
void fakeLog(const char *event, Result result, u64 bytes);

// The wire's digest of a body (src/hash.h), and its text (`u:` and 64 hex digits), valid until
// the next call.
void refDigest(const void *body, size_t n, u8 out[32]);
const char *refDigestText(const void *body, size_t n);

#if THREEDROP_TEST_VERBS
#include "knobs.h" // benchRun, and with it Verb and VerbContext

// Sends one request through benchRun, as dispatch would hand it: the verb `bench`, `id`, then
// `args` split on spaces, under the verb row src/cmd.c has (2 to 8 fields). The caller scripts the
// reply beforehand and afterwards reads it from wherever its own fake kept it.
static inline void benchAs(const char *id, const char *args) {
	static const Verb BENCH = {"bench", 2, 8, false, NULL, benchRun, NOT_BUSY};
	static char line[160];
	char *field[9] = {"bench", (char *)id}, *at = line; // field[8] stays NULL
	size_t n = 2;
	snprintf(line, sizeof(line), "%s", args);
	while (*line && n < 8 && (field[n] = strsep(&at, " ")) != NULL) {
		n++;
	}
	VerbContext c = {.client = 7, .request = "bench", .id = field[1], .field = field, .n = n};
	c.verb = &BENCH;
	benchRun(&c);
}
#endif

#endif // TEST_STUBS_FAKES_H

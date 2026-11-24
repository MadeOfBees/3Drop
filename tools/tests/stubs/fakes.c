// Weak defaults, so a test's own definition of any of these replaces it.
#include "fakes.h"

#include "app.h"
#include "cia.h"
#include "cmdFs.h"
#include "discover.h"
#include "fs.h"
#include "net.h"
#include "ring.h"
#include "screenshot.h"
#include "stats.h"
#include "sysinfo.h"
#include "titles.h"
#include "transfer.h"

#include <3ds/util/utf.h>
#include <arpa/inet.h>
#include <assert.h>
#include <citro2d.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <mbedtls/sha256.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEAK __attribute__((weak))
#define FAKE(type, name, params, value)                                                            \
	WEAK type name params {                                                                        \
		return value;                                                                              \
	}
#define NOOP(name, params)                                                                         \
	WEAK void name params {}

Fake fake = {.nowMs = 1000,
			 .titleID = 0x000400000F800100ULL,
			 .version = 1,
			 .selfTitleID = SELF_TITLE_ID,
			 .selfVersion = 64341,
			 .freeClusters = 0x100000,
			 .luma = SYSTEM_VERSION(13, 1, 2)};
WEAK App gApp;
WEAK InstallStats gStats;

// Atomic because a ring's worker makes FS calls too.
#define FS_CALL() __atomic_add_fetch(&fake.fsCalls, 1, __ATOMIC_SEQ_CST)

// Threads are real, so a ring's worker runs alongside the pump. The clock steps atomically because
// a worker reads it too. A phase's TickCounter reads the clock without stepping it, so a test's
// steps count only the timers' reads.
WEAK u64 osGetTime(void) {
	return __atomic_fetch_add(&fake.nowMs, fake.stepMs, __ATOMIC_SEQ_CST) + (u64)fake.wallMs;
}
WEAK u64 svcGetSystemTick(void) {
	return (__atomic_fetch_add(&fake.nowMs, fake.stepMs, __ATOMIC_SEQ_CST) - fake.bootMs) *
		   (SYSCLOCK_ARM11 / 1000);
}
static u64 tickNow(void) {
	return (__atomic_load_n(&fake.nowMs, __ATOMIC_SEQ_CST) - fake.bootMs) * (SYSCLOCK_ARM11 / 1000);
}
NOOP(svcSleepThread, (s64 ns))
WEAK Result svcFlushProcessDataCache(Handle process, u32 addr, u32 size) {
	unsigned call = __atomic_fetch_add(&fake.flushes, 1, __ATOMIC_SEQ_CST);
	if (call < sizeof(fake.flushAddr) / sizeof(fake.flushAddr[0])) {
		fake.flushAddr[call] = addr;
		fake.flushSize[call] = size;
		fake.flushWrittenTo[call] = __atomic_load_n(&fake.writtenTo, __ATOMIC_SEQ_CST);
	}
	return fake.flushResult;
}
// sslc is mbedTLS's entropy source on the console, and it is brought up before any handshake.
FAKE(Result, sslcInit, (Handle session), (fake.sslcInits++, fake.sslcResult))
WEAK void sslcExit(void) {
	fake.sslcExits++;
}
FAKE(Result, svcWaitSynchronization, (Handle handle, s64 nanoseconds), (Result)0x09401BFE)
typedef struct {
	pthread_t thread;
	ThreadFunc entry;
	void *arg;
	int prio;
} HostThread;
static void *hostThreadRun(void *arg) {
	HostThread *t = arg;
	while (fake.threadHoldPrio && t->prio == fake.threadHoldPrio && fake.cardHeld) {
		usleep(100); // simulates a thread the scheduler never runs while the card is stuck
	}
	t->entry(t->arg);
	return NULL;
}
WEAK Thread threadCreate(ThreadFunc entry, void *arg, size_t stack, int prio, int core,
						 bool detach) {
	if (prio == fake.threadFailPrio) {
		return NULL;
	}
	HostThread *t = malloc(sizeof(*t));
	if (!t) {
		return NULL;
	}
	*t = (HostThread){.entry = entry, .arg = arg, .prio = prio};
	if (pthread_create(&t->thread, NULL, hostThreadRun, t) != 0) {
		free(t);
		return NULL;
	}
	__atomic_add_fetch(&fake.threads, 1, __ATOMIC_SEQ_CST);
	return (Thread)t;
}
// Waits for the thread, after releasing a held card, unless joinResult strands the worker.
WEAK Result threadJoin(Thread thread, u64 timeoutNs) {
	if (fake.joinResult) {
		return fake.joinResult;
	}
	fake.cardHeld = false;
	return pthread_join(((HostThread *)thread)->thread, NULL);
}
WEAK void threadFree(Thread thread) {
	__atomic_sub_fetch(&fake.threads, 1, __ATOMIC_SEQ_CST);
	free(thread);
}
WEAK void LightSemaphore_Init(LightSemaphore *s, s16 initial, s16 max) {
	s->count = initial;
}
WEAK int LightSemaphore_TryAcquire(LightSemaphore *s, s32 n) {
	return __atomic_sub_fetch(&s->count, n, __ATOMIC_SEQ_CST) < 0 &&
		   __atomic_add_fetch(&s->count, n, __ATOMIC_SEQ_CST) >= 0;
}
WEAK void LightSemaphore_Acquire(LightSemaphore *s, s32 n) {
	while (LightSemaphore_TryAcquire(s, n)) {
		usleep(100);
	}
}
WEAK void LightSemaphore_Release(LightSemaphore *s, s32 n) {
	__atomic_add_fetch(&s->count, n, __ATOMIC_SEQ_CST);
}
WEAK void osTickCounterStart(TickCounter *counter) {
	counter->reference = tickNow();
}
WEAK void osTickCounterUpdate(TickCounter *counter) {
	u64 now = tickNow();
	counter->elapsed = now - counter->reference;
	counter->reference = now;
}
FAKE(double, osTickCounterRead, (const TickCounter *c), (double)c->elapsed / CPU_TICKS_PER_MSEC)
WEAK void *memalign(size_t alignment, size_t size) {
	void *p = NULL;
	return posix_memalign(&p, alignment, size) == 0 ? p : NULL;
}
WEAK void *linearMemAlign(size_t size, size_t alignment) {
	return fake.linearFull && size < RING_TIER1_BYTES ? NULL : memalign(alignment, size);
}
FAKE(void *, linearAlloc, (size_t size), memalign(0x1000, size))
WEAK void linearFree(void *mem) {
	free(mem);
}
FAKE(Result, GSPGPU_FlushDataCache, (const void *addr, u32 size), 0)
// libctru answers an empty input itself, with no IPC. Every input starts on a page, which is the
// only path measured; an unaligned buffer's partial pages go through the kernel's copy.
WEAK Result FSUSER_UpdateSha256Context(const void *data, u32 size, u8 *hash) {
	if (size == 0) {
		return mbedtls_sha256_ret(data, 0, hash, 0);
	}
	assert(((uintptr_t)data & 0xFFF) == 0);
	FS_CALL();
	unsigned call = __atomic_fetch_add(&fake.shaCalls, 1, __ATOMIC_SEQ_CST);
	if (call < sizeof(fake.shaWrittenTo) / sizeof(fake.shaWrittenTo[0])) {
		fake.shaWrittenTo[call] = __atomic_load_n(&fake.writtenTo, __ATOMIC_SEQ_CST);
	}
	unsigned left = fake.shaFailAt ? __atomic_sub_fetch(&fake.shaFailAt, 1, __ATOMIC_SEQ_CST) : 0;
	return fake.shaResult && left == 0 ? fake.shaResult : mbedtls_sha256_ret(data, size, hash, 0);
}
// The wire's digest, computed separately from src/hash.c so an expectation
// cannot share the kit's bug.
void refDigest(const void *body, size_t n, u8 out[32]) {
	mbedtls_sha256_context all;
	u8 unit[32];
	mbedtls_sha256_init(&all);
	mbedtls_sha256_starts_ret(&all, 0);
	for (size_t at = 0; at < n; at += 1u << 20) {
		mbedtls_sha256_ret((const u8 *)body + at, n - at < 1u << 20 ? n - at : 1u << 20, unit, 0);
		mbedtls_sha256_update_ret(&all, unit, sizeof(unit));
	}
	mbedtls_sha256_finish_ret(&all, out);
	mbedtls_sha256_free(&all);
}
const char *refDigestText(const void *body, size_t n) {
	static char text[DIGEST_TEXT_SIZE] = "u:";
	u8 digest[32];
	refDigest(body, n, digest);
	for (size_t i = 0; i < sizeof(digest); i++) {
		snprintf(text + 2 + 2 * i, 3, "%02x", digest[i]);
	}
	return text;
}

u8 sharedPageFake[0x1000];
static u32 commandBuffer[64];
FAKE(u32 *, getThreadCommandBuffer, (void), commandBuffer)
FAKE(Result, svcConnectToPort, (Handle * out, const char *port),
	 strcmp(port, "hb:ldr") != 0 || fake.hbldrResult ? (fake.hbldrResult ? fake.hbldrResult : -1)
													 : (*out = 0x4B, fake.hbldrOpen++, 0))
WEAK Result svcCloseHandle(Handle handle) {
	fake.hbldrOpen -= handle == 0x4B;
	return 0;
}
// A command buffer word holds a 32-bit address. Every buffer sent is static, within 2 GiB of this
// one, so the nearest host address with those low bits is the one sent. This buffer's high word
// alone isn't enough, because ASLR can put a 4 GiB boundary between two statics.
static const void *commandAddress(u32 word) {
	uintptr_t here = (uintptr_t)commandBuffer;
	return (const void *)(here + (uintptr_t)(intptr_t)(s32)(word - (u32)here));
}
// fs:USER (handle 0x5E) answers its two seed calls as 3dbrew documents them: the title id is in
// words 1-2, AddSeed's seed in words 3-6, and the seed GetSeed returns in words 2-5.
static Handle fsSession = 0x5E;
FAKE(Handle *, fsGetSessionHandle, (void), &fsSession)
static void fsSeedRequest(u32 *c) {
	u64 titleID = c[1] | (u64)c[2] << 32;
	Result r = (Result)0xD9001830;
	FS_CALL();
	if (c[0] == IPC_MakeHeader(0x87B, 2, 0)) {
		bool held = fake.seedTitleID != 0 && titleID == fake.seedTitleID;
		r = fake.seedGetResult ? fake.seedGetResult : held ? 0 : SEED_ABSENT;
		if (r == 0) {
			memcpy(&c[2], fake.seed, sizeof(fake.seed));
		}
	} else if (c[0] == IPC_MakeHeader(0x87A, 6, 0)) {
		r = fake.seedAddResult;
		if (r == 0) {
			fake.seedTitleID = titleID;
			memcpy(fake.seed, &c[3], sizeof(fake.seed));
			fake.seedAdds++;
		}
	}
	c[1] = (u32)r;
}
// srv: (handle 0x5C) knows one service, ndm:u, and matches a name the way Luma's sm does
// (sysmodules/sm/source/services.c): by the length asked, which doesn't count the NUL, and then by
// where the registered name ends. Its session is 0x6D. sm holds a GetServiceHandle request for an
// unknown name until that name registers (srv.c's replay), with no timeout, so such a request
// stops the test.
static Handle srvSession = 0x5C;
FAKE(Handle *, srvGetSessionHandle, (void), &srvSession)
static void srvRequest(u32 *c) {
	static const char registered[8] = "ndm:u";
	const char *name = (const char *)&c[1];
	u32 size = c[3];
	bool found = !fake.srvUnknown && size >= 1 && size <= 8 && strnlen(name, size) == size &&
				 strncmp(registered, name, size) == 0 && (size == 8 || registered[size] == '\0');
	if (c[0] == IPC_MakeHeader(0xE, 3, 0)) { // IsServiceRegistered
		fake.srvAsks++;
		c[1] = 0;
		c[2] = found;
		return;
	}
	assert(c[0] == IPC_MakeHeader(0x5, 4, 0) && found);
	fake.srvOpens++;
	memcpy(fake.srvName, name, 8);
	fake.srvName[8] = '\0';
	fake.srvFlags = c[4];
	c[1] = (u32)fake.srvResult;
	c[3] = fake.srvResult == 0 && !fake.srvNoHandle ? 0x6D : 0;
}
// hb:ldr (handle 0x4B) checks each request as Luma's hbldr.c does; anything else gets 0xD9001830.
WEAK Result svcSendSyncRequest(Handle session) {
	u32 *c = commandBuffer, size = c[1] >> 14;
	Result r;
	if (session == fsSession) {
		fsSeedRequest(c);
		return 0;
	}
	if (session == srvSession) {
		// A failed IPC writes no reply here, so the request's words stay. That is a worst case for
		// GetServiceHandle, whose cmd[3] Luma's kernel zeroes on failure (SendSyncRequest.c).
		r = fake.srvIpcResult;
		if (r == 0 && c[0] == IPC_MakeHeader(0x5, 4, 0)) {
			r = fake.srvOpenIpcResult;
		}
		if (r == 0) {
			srvRequest(c);
		}
		return r;
	}
	if (session != 0x4B) {
		return -1;
	}
	if (c[0] == IPC_MakeHeader(2, 0, 2) && (c[1] & 0x3FFF) == 0x0002 && size <= 256) {
		r = fake.targetResult;
		if (!r) {
			memcpy(fake.hbldrTarget, commandAddress(c[2]), size);
		}
	} else if (c[0] == IPC_MakeHeader(3, 0, 2) && (c[1] & 0x3FFF) == (0x2 | 1 << 10) &&
			   size == sizeof(fake.hbldrArgv)) {
		r = fake.argvResult;
		if (!r) {
			memcpy(fake.hbldrArgv, commandAddress(c[2]), size);
		}
	} else {
		r = (Result)0xD9001830;
	}
	c[1] = (u32)r;
	return 0;
}
WEAK int getpeername(int fd, struct sockaddr *address, socklen_t *size) {
	struct sockaddr_in peer = {.sin_family = AF_INET};
	peer.sin_addr.s_addr = inet_addr("192.0.2.73");
	memcpy(address, &peer, sizeof(peer));
	*size = sizeof(peer);
	return 0;
}
// aptMainLoop follows a POWER press to HOME whatever HOME's setting, as libctru's does.
FAKE(bool, aptMainLoop, (void), !fake.powerPressed)
FAKE(bool, aptIsHomeAllowed, (void), !fake.homeBlocked)
WEAK void aptSetHomeAllowed(bool allowed) {
	fake.homeBlocked = !allowed;
}
FAKE(bool, aptShouldClose, (void), false)
WEAK void aptSetChainloader(u64 programID, u8 mediatype) {
	assert(mediatype == MEDIATYPE_SD);
	fake.chainload = programID;
}
WEAK void aptSetChainloaderToSelf(void) {
	fake.chainload = fake.selfTitleID;
}
NOOP(aptSetChainloaderArgs, (const void *arg, size_t size, const void *hmac))
// The regular heap size, as the console reads it, is not capped at 25165824 bytes.
FAKE(u32, envGetHeapSize, (void), 31621120)
FAKE(u32, envGetLinearHeapSize, (void), 32u << 20)
FAKE(Result, cfguInit, (void), 0)
NOOP(cfguExit, (void))
FAKE(Result, CFGU_GetSystemModel, (u8 * model), (*model = CFG_MODEL_3DSXL, 0))
FAKE(Result, CFGU_SecureInfoGetRegion, (u8 * region), (*region = 1, 0))
FAKE(Result, APT_CheckNew3DS, (bool *out), (*out = false, 0))
FAKE(Result, osGetSystemVersionDataString, (OS_VersionBin * n, OS_VersionBin *c, char *s, u32 max),
	 (snprintf(s, max, "11.17.0-50U"), 0))
FAKE(Result, svcGetSystemInfo, (s64 * out, u32 type, s32 param),
	 type == 0x10000 && param == 0 && fake.luma ? (*out = fake.luma, 0) : (Result)0xF8C007F4)
FAKE(u32, linearSpaceFree, (void), 0)
FAKE(u8, sysinfoModelId, (void), CFG_MODEL_3DSXL) // for ui.c where sysinfo.c is not linked
// The name and probe answers where src/discover.c isn't linked: discovery is closed and has
// nothing to say.
FAKE(bool, discoverTick, (void), false)
NOOP(discoverDrain, (void))
NOOP(discoverClose, (void))
NOOP(discoverFormat, (char *dst, size_t cap))
NOOP(srvSetBlockingPolicy, (bool nonBlocking))
FAKE(Result, mcuHwcInit, (void), (fake.mcuOpens++, 0))
NOOP(mcuHwcExit, (void))
FAKE(Result, MCUHWC_SetInfoLedPattern, (const InfoLedPattern *pattern), 0)
WEAK Result MCUHWC_ReadRegister(u8 reg, void *data, u32 size) {
	if (reg == 0x7F && size <= sizeof(fake.mcuInfo)) {
		memcpy(data, fake.mcuInfo, size);
	}
	return fake.mcuReadResult;
}
FAKE(Result, ptmuInit, (void), 0)
NOOP(ptmuExit, (void))
FAKE(Result, PTMU_GetShellState, (u8 * out), (*out = fake.shell, fake.shellResult))
// led_battery (3dbrew BATTERY_LEDSTATE): bit 0 is the adapter and bit 1 is charging, so a swap
// between the two shows.
osSharedConfig_s osSharedConfigFake = {.wifi_strength = 2, .led_battery = 4 << 2 | 1};
NOOP(hidScanInput, (void))
FAKE(u32, hidKeysDown, (void), fake.keysDown)
NOOP(gspWaitForVBlank, (void))
NOOP(gfxSetDoubleBuffering, (gfxScreen_t screen, bool enable))
FAKE(Result, psInit, (void), 0)
NOOP(psExit, (void))
// Bytes that never repeat a run, so a minted key uses the whole alphabet and a replayed nonce
// passes only by reuse.
WEAK Result PS_GenerateRandomBytes(void *out, size_t len) {
	static u64 state = 1;
	if (fake.psResult) {
		return fake.psResult;
	}
	for (size_t i = 0; i < len; i++) {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL; // Knuth's MMIX LCG
		((u8 *)out)[i] = (u8)(state >> 56);
	}
	return 0;
}

// The scripted title answers first, so a test may script 3Drop's own id. Every call that names a
// medium names the SD, because deleting, installing or cleaning up a NAND title can brick the
// console.
#define AM(mutates) (fake.amCalls++, fake.amMutations += (mutates))
#define AM_SD(media, mutates) (assert((media) == MEDIATYPE_SD), AM(mutates))
static bool selfListed(void) {
	return fake.selfTitleID != 0 && !(fake.installed && fake.titleID == fake.selfTitleID);
}
// The moreTitles' ids, as AM_GetTitleList lists them.
static u64 moreTitleID(u32 i) {
	return 0x0004000000100000ULL + ((u64)i << 8);
}
// What AM says of one title: the scripted one (infoResult is its failure), 3Drop itself, and the
// moreTitles, which have an answer only when moreInfo is set (their version is 1 + their index).
static Result titleInfoOf(u64 id, AM_TitleInfo *info) {
	if (id == fake.titleID && (fake.infoResult || fake.installed)) {
		*info = (AM_TitleInfo){.titleID = fake.titleID, .size = fake.size, .version = fake.version};
		return fake.infoResult;
	}
	if (id == fake.selfTitleID && fake.selfTitleID != 0) {
		*info = (AM_TitleInfo){.titleID = fake.selfTitleID, .version = fake.selfVersion};
		return 0;
	}
	if (fake.moreInfo && id >= moreTitleID(0) && id < moreTitleID(fake.moreTitles)) {
		u32 i = (u32)((id - moreTitleID(0)) >> 8);
		*info = (AM_TitleInfo){.titleID = id, .size = 4096ULL * (i + 1), .version = i + 1};
		return 0;
	}
	return TITLE_ABSENT;
}
// A batch has one result, that of the first title that fails (AM's own is unmeasured, so a
// test of src/ treats any failure alike). The infos before the failing title are written.
// cppcheck-suppress constParameter ; libctru's signature
WEAK Result AM_GetTitleInfo(FS_MediaType media, u32 count, u64 *ids, AM_TitleInfo *info) {
	AM_SD(media, 0);
	for (u32 i = 0; i < count; i++) {
		Result r = titleInfoOf(ids[i], &info[i]);
		if (r) {
			return r;
		}
	}
	return 0;
}
FAKE(Result, AM_GetTitleCount, (FS_MediaType m, u32 *count),
	 (AM_SD(m, 0), *count = fake.installed + selfListed() + fake.moreTitles, 0))
WEAK Result AM_GetTitleList(u32 *read, FS_MediaType m, u32 count, u64 *ids) {
	AM_SD(m, 0);
	*read = 0;
	if (fake.installed && *read < count) {
		ids[(*read)++] = fake.titleID;
	}
	if (selfListed() && *read < count) {
		ids[(*read)++] = fake.selfTitleID;
	}
	for (u32 i = 0; i < fake.moreTitles && *read < count; i++) {
		ids[(*read)++] = moreTitleID(i);
	}
	return 0;
}
FAKE(Result, AM_GetTicketCount, (u32 * count),
	 (AM(0), *count = (fake.ticketID != 0) + fake.importTicket, fake.ticketCountResult))
FAKE(Result, AM_GetNumPendingTitles, (u32 * count, FS_MediaType m),
	 (AM_SD(m, 0), *count = fake.pending + (fake.pendingID != 0), 0))
// With no import pending the answer is TITLE_ABSENT, as the console answers the query (for the
// delete it may also be success, which src reads the same way). Any other answer fails closed.
static Result pendingAnswer(u64 id, Result scripted) {
	return scripted ? scripted : fake.pendingID && id == fake.pendingID ? 0 : TITLE_ABSENT;
}
FAKE(Result, AM_GetPendingTitleInfo, (u32 n, FS_MediaType m, u64 *ids, AM_PendingTitleInfo *info),
	 (AM_SD(m, 0), pendingAnswer(ids[0], fake.pendingInfoResult)))
// AM answers TITLE_ABSENT, not 0, for a title with no ticket, and libctru copies the count out
// anyway. The count from a failed call is noise; it is 1 here, so a caller that reads it shows up.
WEAK Result AMNET_GetTitleNumTickets(u32 *count, u64 id) {
	AM(0);
	*count = (id == fake.ticketID) + (fake.importTicket && id == fake.titleID);
	if (fake.numTicketsResult || *count == 0) {
		*count = 1;
		return fake.numTicketsResult ? fake.numTicketsResult : TITLE_ABSENT;
	}
	return 0;
}
FAKE(Result, AM_NeedsCleanup, (u8 * needs, FS_MediaType m),
	 (AM_SD(m, 0), *needs = fake.needsCleanup, fake.cleanupResult))
FAKE(Result, AM_QueryAvailableExternalTitleDatabase, (bool *available),
	 (AM(0), *available = true, 0))
FAKE(Result, AM_InitializeExternalTitleDatabase, (bool overwrite),
	 (AM(1), fake.titleDbInits++, fake.titleDbOverwrites += overwrite, 0))
FAKE(Result, AM_DoCleanup, (FS_MediaType m), (AM_SD(m, 1), fake.cleanups++, 0))
// AM_DeleteUserTitle refuses a system title (its Result is unmeasured, so src reads any failure
// alike). AM_DeletePendingTitle takes either. AM_DeleteTicket deletes every ticket of the title,
// with no guard.
static bool systemTitle(u64 id) {
	return (id >> 32) & 0x10; // the category's system bit: 00040010, 00040030 (HOME Menu), ...
}
WEAK Result AM_DeleteUserTitle(FS_MediaType m, u64 id) {
	AM_SD(m, 1);
	fake.titleDeletes++;
	fake.titleDeleted = id;
	if (fake.deleteTitleResult || systemTitle(id)) {
		return fake.deleteTitleResult ? fake.deleteTitleResult : (Result)0xE0E0802C;
	}
	fake.installed = fake.installed && id != fake.titleID;
	return 0;
}
WEAK Result AM_DeletePendingTitle(FS_MediaType m, u64 id) {
	AM_SD(m, 1);
	fake.pendingDeletes++;
	fake.pendingDeleted = id;
	Result r = pendingAnswer(id, fake.deletePendingResult);
	fake.pendingID = r == 0 ? 0 : fake.pendingID; // r is 0 only for fake.pendingID's id
	return r;
}
WEAK Result AM_DeleteTicket(u64 id) {
	AM(1);
	fake.ticketDeletes++;
	fake.ticketDeleted = id;
	if (fake.ticketResult == 0) {
		fake.ticketID = id == fake.ticketID ? 0 : fake.ticketID;
		fake.importTicket = fake.importTicket && id != fake.titleID;
	}
	return fake.ticketResult;
}
FAKE(Result, AM_StartCiaInstall, (FS_MediaType m, Handle *cia),
	 (AM_SD(m, 1), fake.starts++, *cia = 0x55, fake.importTicket = fake.startResult == 0,
	  fake.startResult))
FAKE(Result, AM_StartCiaInstallOverwrite, (Handle * cia, FS_MediaType m),
	 (AM_SD(m, 1), fake.overwriteStarts++, *cia = 0x55, fake.importTicket = fake.startResult == 0,
	  fake.startResult))
// A failed Finish may still leave the title installed, because AM commits inside it.
FAKE(Result, AM_FinishCiaInstall, (Handle cia),
	 (AM(1), fake.installed = fake.installed || fake.finishInstalls, fake.finishResult))
FAKE(Result, AM_CancelCIAInstall, (Handle cia), (AM(1), fake.cancels++, fake.cancelResult))

// The SD archive is ./sdmc:, a save is ./save:<title-id>, and extdata is ./extdata:<id8>. The SD
// archive's handle is never its id, so an id passed as a handle finds nothing, as on the console.
#define FAKE_SD_ARCHIVE ((FS_Archive)0x5D)
// Returns "" for a path that names nothing: an invalid one, or one that will not convert. FS's
// answer to a non-ASCII byte under PATH_ASCII is unmeasured, so here such a path names nothing,
// and a call that still sends one fails.
static const char *hostPath(FS_Archive archive, FS_Path path) {
	static char buf[2][1024];
	static int next;
	char name[SD_PATH_MAX * 3] = "";
	if (path.type == PATH_UTF16) {
		ssize_t n = utf16_to_utf8((uint8_t *)name, path.data, sizeof(name) - 1);
		if (n < 0 || (size_t)n >= sizeof(name)) {
			return "";
		}
		name[n] = '\0';
	} else if (path.type == PATH_ASCII) {
		for (const char *c = path.data; *c; c++) {
			if ((uint8_t)*c >= 0x80) {
				return "";
			}
		}
		snprintf(name, sizeof(name), "%s", (const char *)path.data);
	} else {
		return "";
	}
	next ^= 1;
	snprintf(buf[next], sizeof(buf[next]), "%s%s",
			 archive == FAKE_SD_ARCHIVE ? "sdmc:"
			 : archive == ARCHIVE_SDMC	? "no-archive:"
										: (const char *)(uintptr_t)archive,
			 name);
	return buf[next];
}
// Counts a name FS is given to look up when it is in the folder under watch.
static void watchName(const char *at) {
	size_t n = fake.watch ? strlen(fake.watch) : 0;
	if (n > 0 && strncmp(at, fake.watch, n) == 0 && at[n] == '/' && !strchr(at + n + 1, '/')) {
		__atomic_add_fetch(&fake.watched, 1, __ATOMIC_SEQ_CST);
	}
}
static const char *host(FS_Archive archive, FS_Path path) {
	FS_CALL();
	const char *at = hostPath(archive, path);
	watchName(at);
	return fake.sdGone ? "/nonexistent/3drop-card-gone" : at;
}
// The reverse of libctru's archive_dev.c error table. An errno that table does not name gets a
// Result of its own, never one of the exact codes src compares with.
static Result fsResult(int ok) {
	if (!ok && fake.sdGone) {
		return fake.sdGone;
	}
	switch (ok ? 0 : errno) {
	case 0:
		return 0;
	case ENOENT:
	case ENOTDIR: // a path through a file, where FS finds nothing either
		return fake.notFound ? fake.notFound : (Result)0xC8804478;
	case EEXIST:
		return FSERROR_ALREADY_EXISTS;
	case EINVAL:
		return (Result)0xE0E046BE;
	case ENAMETOOLONG:
		return (Result)0xE0E046BF;
	default:
		return (Result)(0xC8804000 | (errno & 0x3FF));
	}
}
FAKE(FS_Path, fsMakePath, (FS_PathType type, const void *path),
	 ((FS_Path){type, type == PATH_ASCII ? (u32)strlen(path) + 1 : 1, path}))
// libctru's converters (3ds/util/utf.h) return the number of units the whole input needs, writing
// them while they fit in len, or -1 for input that does not decode. Like libctru's decoder,
// this one lets an encoded surrogate through and writes it as a lone unit.
WEAK ssize_t utf8_to_utf16(uint16_t *out, const uint8_t *in, size_t len) {
	static const uint8_t MASK[] = {0x7F, 0x1F, 0x0F, 0x07};
	static const uint32_t LEAST[] = {0, 0x80, 0x800, 0x10000};
	size_t n = 0;
	while (*in) {
		uint8_t lead = *in++;
		int more = lead < 0x80	 ? 0
				   : lead < 0xC2 ? -1
				   : lead < 0xE0 ? 1
				   : lead < 0xF0 ? 2
				   : lead < 0xF5 ? 3
								 : -1;
		if (more < 0) {
			return -1;
		}
		uint32_t code = lead & MASK[more];
		for (int i = 0; i < more; i++, in++) {
			if ((*in & 0xC0) != 0x80) {
				return -1;
			}
			code = code << 6 | (*in & 0x3F);
		}
		if (code < LEAST[more] || code > 0x10FFFF) {
			return -1;
		}
		size_t units = code > 0xFFFF ? 2 : 1;
		if (n + units <= len) {
			if (units == 2) {
				out[n] = (uint16_t)(0xD800 | (code - 0x10000) >> 10);
				out[n + 1] = (uint16_t)(0xDC00 | (code & 0x3FF));
			} else {
				out[n] = (uint16_t)code;
			}
		}
		n += units;
	}
	return (ssize_t)n;
}
WEAK ssize_t utf16_to_utf8(uint8_t *out, const uint16_t *in, size_t len) {
	static const uint8_t LEAD[] = {0, 0, 0xC0, 0xE0, 0xF0}; // indexed by a character's byte count
	size_t n = 0;
	for (; *in; in++) {
		uint32_t code = *in;
		if (code >= 0xDC00 && code <= 0xDFFF) {
			return -1; // a lone low surrogate
		}
		if (code >= 0xD800 && code <= 0xDBFF) {
			if (in[1] < 0xDC00 || in[1] > 0xDFFF) {
				return -1; // a lone high one
			}
			code = 0x10000 + ((code - 0xD800) << 10 | (*++in - 0xDC00));
		}
		size_t bytes = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
		for (size_t i = 0; n + bytes <= len && i < bytes; i++) {
			size_t shift = 6 * (bytes - 1 - i);
			out[n + i] =
				(uint8_t)(i == 0 ? LEAD[bytes] | code >> shift : 0x80 | (code >> shift & 0x3F));
		}
		n += bytes;
	}
	return (ssize_t)n;
}
WEAK Result FSUSER_OpenArchive(FS_Archive *archive, FS_ArchiveID id, FS_Path path) {
	const u32 *low = path.data; // {media type, id low, id high}
	char dir[32];
	struct stat st;
	FS_CALL();
	if (id == ARCHIVE_SDMC) {
		Result r = fake.sdOpenResult;
		fake.sdOpenResult = 0;
		// libctru reads the handle words of a refused reply, which still hold the request's path
		// type and size.
		*archive = r ? path.type | (u64)path.size << 32 : FAKE_SD_ARCHIVE;
		return r;
	}
	if (id == ARCHIVE_USER_SAVEDATA) {
		snprintf(dir, sizeof(dir), "save:%08x%08x", low[2], low[1]);
	} else {
		snprintf(dir, sizeof(dir), "extdata:%08x", low[1]);
	}
	if (fake.archiveResult && fake.archiveOpensOk == 0) {
		return fake.archiveResult;
	}
	fake.archiveOpensOk -= fake.archiveOpensOk != 0;
	if (low[0] != MEDIATYPE_SD || stat(dir, &st) != 0) {
		return (Result)0xC8804478;
	}
	*archive = (FS_Archive)(uintptr_t)strdup(dir);
	fake.archives++;
	return 0;
}
FAKE(FS_Archive, fsArchive, (void), FAKE_SD_ARCHIVE)			  // for tests that do not link fs.c
FAKE(Result, fsMkdirs, (FS_Archive archive, const char *path), 0) // main.c's and bench.c's, no fs.c
WEAK Result FSUSER_CloseArchive(FS_Archive archive) {
	if (archive != FAKE_SD_ARCHIVE && archive != ARCHIVE_SDMC) {
		free((void *)(uintptr_t)archive);
		fake.archives--;
	}
	return 0;
}
// A handle reads only if opened to read, as FS's does.
WEAK Result FSUSER_OpenFile(Handle *out, FS_Archive archive, FS_Path path, u32 flags, u32 attr) {
	int access = !(flags & FS_OPEN_WRITE) ? O_RDONLY : (flags & FS_OPEN_READ) ? O_RDWR : O_WRONLY;
	int mode = access | ((flags & FS_OPEN_CREATE) ? O_CREAT : 0);
	int fd = open(host(archive, path), mode, 0644);
	struct stat st;
	// FS doesn't open a directory as a file.
	if (fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		close(fd);
		fd = -1;
		errno = ENOENT;
	}
	*out = (Handle)(fd + 1);
	if (fd >= 0) {
		__atomic_add_fetch(&fake.files, 1, __ATOMIC_SEQ_CST);
	}
	return fsResult(fd >= 0);
}
FAKE(Result, FSUSER_OpenFileDirectly,
	 (Handle * out, FS_ArchiveID id, FS_Path archive, FS_Path path, u32 flags, u32 attr),
	 FSUSER_OpenFile(out, FAKE_SD_ARCHIVE, path, flags, attr))
WEAK Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path) {
	const char *at = hostPath(archive, path);
	size_t n = strlen(at), k = fake.fsFailOn ? strlen(fake.fsFailOn) : 0;
	if (fake.fsFail && n >= k && strcmp(at + n - k, k ? fake.fsFailOn : "") == 0) {
		return fake.fsFail;
	}
	return fsResult(unlink(host(archive, path)) == 0);
}
// FS refuses a target that exists, where POSIX rename would replace it.
WEAK Result FSUSER_RenameFile(FS_Archive a, FS_Path src, FS_Archive b, FS_Path dst) {
	struct stat st;
	const char *to = host(b, dst), *from = hostPath(a, src); // hostPath keeps its last two answers
	size_t n = strlen(from), k = fake.renameFrom ? strlen(fake.renameFrom) : 0;
	watchName(from); // its target was counted by host()
	if (lstat(to, &st) == 0) {
		return FSERROR_ALREADY_EXISTS;
	}
	if (fake.renameResult && k && n >= k && strcmp(from + n - k, fake.renameFrom) == 0) {
		return fake.renameResult;
	}
	return fsResult(rename(from, to) == 0);
}
FAKE(Result, FSUSER_RenameDirectory, (FS_Archive a, FS_Path src, FS_Archive b, FS_Path dst),
	 FSUSER_RenameFile(a, src, b, dst))
// An existing folder answers FSERROR_ALREADY_EXISTS, as on the SD card; in a save or an extdata
// it answers fake.dirExists when that is set.
WEAK Result FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attr) {
	Result r = fsResult(mkdir(host(archive, path), 0755) == 0);
	if (r == FSERROR_ALREADY_EXISTS && archive != FAKE_SD_ARCHIVE && fake.dirExists) {
		return fake.dirExists;
	}
	return r;
}
WEAK Result FSUSER_OpenDirectory(Handle *out, FS_Archive archive, FS_Path path) {
	*out = (Handle)opendir(host(archive, path));
	if (*out) {
		__atomic_add_fetch(&fake.dirs, 1, __ATOMIC_SEQ_CST);
	}
	return fsResult(*out != 0);
}
WEAK Result FSDIR_Read(Handle dir, u32 *read, u32 count, FS_DirectoryEntry *entries) {
	struct dirent *d;
	struct stat st;
	if (fake.dirReadResult) {
		*read = 0;
		return fake.dirReadResult;
	}
	for (*read = 0; *read < count && (d = readdir((DIR *)dir)) != NULL;) {
		if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0 &&
			fstatat(dirfd((DIR *)dir), d->d_name, &st, 0) == 0) {
			FS_DirectoryEntry *e = &entries[(*read)++];
			*e = (FS_DirectoryEntry){.attributes = S_ISDIR(st.st_mode) ? FS_ATTRIBUTE_DIRECTORY : 0,
									 .fileSize = (u64)st.st_size};
			utf8_to_utf16(e->name, (const uint8_t *)d->d_name, 0x105);
		}
	}
	return 0;
}
WEAK Result FSDIR_Close(Handle dir) {
	__atomic_sub_fetch(&fake.dirs, 1, __ATOMIC_SEQ_CST);
	return closedir((DIR *)dir);
}
FAKE(Result, FSUSER_DeleteDirectory, (FS_Archive archive, FS_Path path),
	 fsResult(rmdir(host(archive, path)) == 0))
static unsigned treeRemoved; // counts the files a recursive delete has reached, for fake.treeFail
static int removeTree(const char *path) {
	DIR *dir = opendir(path);
	struct dirent *d;
	struct stat st;
	char child[512];
	while (dir && (d = readdir(dir)) != NULL) {
		if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0) {
			snprintf(child, sizeof(child), "%s/%s", path, d->d_name);
			if (lstat(child, &st) != 0 ||
				(!S_ISDIR(st.st_mode) && fake.treeFail && treeRemoved++ == fake.treeFailAfter) ||
				(S_ISDIR(st.st_mode) ? removeTree(child) : unlink(child))) {
				closedir(dir);
				return -1;
			}
		}
	}
	return dir ? (closedir(dir), rmdir(path)) : -1;
}
WEAK Result FSUSER_DeleteDirectoryRecursively(FS_Archive archive, FS_Path path) {
	treeRemoved = 0;
	Result r = fsResult(removeTree(host(archive, path)) == 0);
	return r && fake.treeFail ? fake.treeFail : r;
}
// Commits a save, or returns the mtime in ms since 2000, as FS answers it.
WEAK Result FSUSER_ControlArchive(FS_Archive archive, u32 action, void *input, u32 inputSize,
								  void *output, u32 outputSize) {
	struct stat st;
	if (action == ARCHIVE_ACTION_COMMIT_SAVE_DATA) {
		FS_CALL();
		fake.commits += fake.commitResult == 0;
		return fake.commitResult;
	}
	int ok = stat(host(archive, (FS_Path){PATH_UTF16, inputSize, input}), &st) == 0;
	*(u64 *)output = ok ? ((u64)st.st_mtime - 946684800) * 1000 : 0;
	return fsResult(ok);
}
// Accepts only the DELETE action libctru documents; anything else is refused as FS refuses a
// malformed request.
WEAK Result FSUSER_ControlSecureSave(FS_SecureSaveAction action, void *input, u32 inputSize,
									 void *output, u32 outputSize) {
	if (action != SECURESAVE_ACTION_DELETE || inputSize != 8 || outputSize != 1) {
		return (Result)0xE0E046BE;
	}
	fake.secureValue = *(const u64 *)input;
	fake.secureDeletes += fake.secureValueResult == 0;
	return fake.secureValueResult;
}
// FS refuses a file that exists, and one whose directory does not.
WEAK Result FSUSER_CreateFile(FS_Archive archive, FS_Path path, u32 attributes, u64 size) {
	int fd = open(host(archive, path), O_RDWR | O_CREAT | O_EXCL, 0644);
	Result r = fsResult(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
	if (fd >= 0) {
		close(fd);
	}
	return r;
}
WEAK Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource *resource) {
	*resource = (FS_ArchiveResource){512, 0x8000, 0x200000, fake.freeClusters};
	return fake.sdmcResourceResult;
}
static Result fsIo(ssize_t n, u32 *count) {
	FS_CALL();
	*count = n > 0 ? (u32)n : 0;
	return fsResult(n >= 0);
}
// Blocks until the transfer has given up and joins its worker, however fast the host runs.
static void cardWait(void) {
	while (fake.cardHeld) {
		usleep(100);
	}
}
WEAK Result FSFILE_Read(Handle file, u32 *read, u64 offset, void *buffer, u32 size) {
	cardWait();
	return fsIo(pread((int)file - 1, buffer, size, (off_t)offset), read);
}
WEAK Result FSFILE_Write(Handle file, u32 *written, u64 offset, const void *buffer, u32 size,
						 u32 flags) {
	cardWait();
	__atomic_add_fetch(&fake.nowMs, fake.writeMs, __ATOMIC_SEQ_CST);
	if (fake.writeResult) {
		*written = 0;
		return fake.writeResult;
	}
	Result r = fsIo(pwrite((int)file - 1, buffer, size - (fake.shortWrite && size), (off_t)offset),
					written);
	__atomic_store_n(&fake.writtenTo, offset + *written, __ATOMIC_SEQ_CST);
	return r;
}
WEAK Result FSFILE_GetSize(Handle file, u64 *size) {
	FS_CALL();
	struct stat st;
	int ok = fstat((int)file - 1, &st) == 0;
	*size = ok ? (u64)st.st_size : 0;
	return fsResult(ok);
}
FAKE(Result, FSFILE_SetSize, (Handle file, u64 size),
	 (FS_CALL(), fake.setSizeResult ? fake.setSizeResult
									: fsResult(ftruncate((int)file - 1, (off_t)size) == 0)))
FAKE(Result, FSFILE_Flush, (Handle file), (FS_CALL(), fake.fileFlushResult))
WEAK Result FSFILE_Close(Handle file) {
	if (!file) {
		return 0;
	}
	FS_CALL();
	__atomic_sub_fetch(&fake.files, 1, __ATOMIC_SEQ_CST);
	return (Result)close((int)file - 1);
}

FAKE(ServerStatus, appStatus, (void), gApp.status)
WEAK void appSetStatus(ServerStatus status) {
	gApp.status = status;
}
WEAK void appSetIdle(void) {
	gApp.status = SERVER_READY; // the status of a listening console
}
NOOP(appStatusTick, (void))
FAKE(Led, appLed, (void), LED_OFF)
NOOP(uiProgressTick, (void))
void testPass(const char *file) {
	const char *name = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
	printf("%.*s: PASS\n", (int)strcspn(name, "."), name);
}
void fakeLog(const char *event, Result result, u64 bytes) {
	snprintf(fake.log, sizeof(fake.log), "%s", event);
	fake.logResult = result;
	fake.logBytes = bytes;
	fake.logs++;
}
WEAK void appendLog(const char *event, Result result, u64 bytes) {
	fakeLog(event, result, bytes);
}
WEAK void appendLogTail(const char *event, Result result, u64 bytes, const char *tail) {
	fakeLog(event, result, bytes);
}
WEAK bool sendStr(int client, const char *s) {
	size_t line = strcspn(s, "\n");
	const char *mac = s[line] ? s + line + 1 : "";
	snprintf(fake.last, sizeof(fake.last), "%.*s", (int)line, s);
	snprintf(fake.lastMac, sizeof(fake.lastMac), "%.*s", (int)strcspn(mac, "\n"), mac);
	if (fake.replies++ == 0) {
		snprintf(fake.reply, sizeof(fake.reply), "%s", fake.last);
		snprintf(fake.replyMac, sizeof(fake.replyMac), "%s", fake.lastMac);
	}
	return !fake.sendFails;
}
NOOP(gracefulClose, (int client))
NOOP(macResponse, (uint8_t out[64], const char *request, const char *response))
FAKE(bool, macRequest, (uint8_t out[64], const char *request), false)
FAKE(const char *, nonceLive, (void),
	 "0000000000000000000000000000000000000000000000000000000000000000")
FAKE(bool, nonceIs, (const char *hex), true)
FAKE(bool, nonceSpend, (const char *hex), true)
NOOP(nonceExpireBefore, (u64 ms))
NOOP(macTrailer, (uint8_t out[64], const char *request, const char *response, const char *t))
WEAK void transferSessionInit(TransferSession *s, int client, u64 size, TransferProgressFn f,
							  bool ring) {
	*s = (TransferSession){.client = client};
}
FAKE(u64, transferAbsoluteDuration, (u64 totalSize), 0)
FAKE(bool, setNonblocking, (int fd), true)
FAKE(int, openListener, (u16 port), -1) // it never comes up
FAKE(bool, wouldBlock, (void), errno == EAGAIN || errno == EWOULDBLOCK)
FAKE(bool, startPressed, (void), false)
NOOP(keepKeys, (bool keep))
// net.c's version, for a test without it. No request has kept a key, so the scan's own keys are
// the only ones.
WEAK u32 pressedKeys(void) {
	hidScanInput();
	return hidKeysDown();
}
FAKE(bool, idleStartTaken, (void), false)
FAKE(LinesRead, readRequestLines, (int c, char *buf, size_t cap, char **second, u64 timeoutMs),
	 LINES_GONE)
WEAK bool sendBytes(int client, const unsigned char *data, size_t len) {
	size_t room = sizeof(fake.body) - 1 - fake.bodyLen;
	if (fake.bodySendFails) {
		errno = ECONNRESET;
		return false;
	}
	memcpy(fake.body + fake.bodyLen, data, len < room ? len : room);
	fake.bodyLen += len < room ? len : room;
	fake.body[fake.bodyLen] = '\0';
	return true;
}
WEAK ssize_t send(int fd, const void *data, size_t len, int flags) {
	return sendBytes(fd, data, len) ? (ssize_t)len : -1;
}
static u8 framebuffer[240 * 2 * 3];
WEAK u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height) {
	*width = 240;
	*height = 2;
	return memset(framebuffer, 0x5A, sizeof(framebuffer));
}
FAKE(bool, C3D_FrameBegin, (u8 flags), true)
NOOP(C3D_FrameEnd, (u8 flags))
FAKE(const char *, C2D_TextParse, (C2D_Text * text, C2D_TextBuf buf, const char *str), NULL)
NOOP(C2D_DrawText, (const C2D_Text *text, u32 flags, float x, float y, float depth, float scaleX,
					float scaleY, ...))
NOOP(C2D_DrawCircleSolid, (float x, float y, float z, float radius, u32 color))
// A texture is the memory it holds, 2 bytes a texel (RGB565 is the only format here).
WEAK bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format) {
	tex->data = fake.textureFails ? NULL : calloc((size_t)width * height, 2);
	tex->width = width;
	tex->height = height;
	return tex->data != NULL;
}
NOOP(C3D_TexSetFilter,
	 (C3D_Tex * tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter))
WEAK void C3D_TexDelete(C3D_Tex *tex) {
	free(tex->data);
	tex->data = NULL;
}
FAKE(bool, C2D_DrawImageAt,
	 (C2D_Image img, float x, float y, float depth, const void *tint, float scaleX, float scaleY),
	 true)
FAKE(bool, uiShowsKey, (void), false)
FAKE(bool, ringStranded, (void), false)
WEAK int gListener = -1;					 // net.c's variable, for tests that don't link net.c
FAKE(bool, argIdle, (VerbContext * c), true) // for bench.c where cmdFs.c is not linked
FAKE(const char *, authDisplayKey, (void), NULL)
FAKE(bool, authRegenerateKey, (void), false)

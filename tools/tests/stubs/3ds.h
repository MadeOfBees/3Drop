// Declarations only; behavior comes from fakes.c's weak defaults or from the test itself.
#ifndef TEST_STUBS_3DS_H
#define TEST_STUBS_3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uintptr_t Handle;
typedef int32_t Result;

#define BIT(n) (1u << (n))
#define R_FAILED(r) ((Result)(r) < 0)
#define R_SUCCEEDED(r) ((Result)(r) >= 0)
#define R_DESCRIPTION(r) ((r)&0x3FF)
#define R_MODULE(r) (((r) >> 10) & 0xFF)
#define MAKERESULT(level, summary, module, description)                                            \
	((u32)(level) << 27 | (u32)(summary) << 21 | (u32)(module) << 10 | (u32)(description))
#define RL_PERMANENT 27
#define RS_INVALIDSTATE 5
#define RS_INTERNAL 11
#define RM_AM 32
#define RM_APPLICATION 254
#define RD_NOT_FOUND 1018
#define RD_TIMEOUT 1022
#define SYSCLOCK_ARM11 268111856ULL
#define CPU_TICKS_PER_MSEC (SYSCLOCK_ARM11 / 1000.0)
#define CUR_PROCESS_HANDLE 0xFFFF8001

typedef struct Thread_tag *Thread;
typedef void (*ThreadFunc)(void *);
typedef struct {
	s32 count;
} LightSemaphore;
typedef struct {
	u64 elapsed, reference;
} TickCounter;
u64 osGetTime(void);
u64 svcGetSystemTick(void);
void svcSleepThread(s64 ns);
Result svcFlushProcessDataCache(Handle process, u32 addr, u32 size);
Thread threadCreate(ThreadFunc entry, void *arg, size_t stackSize, int prio, int core,
					bool detached);
Result threadJoin(Thread thread, u64 timeoutNs);
void threadFree(Thread thread);
void LightSemaphore_Init(LightSemaphore *semaphore, s16 initial, s16 max);
void LightSemaphore_Acquire(LightSemaphore *semaphore, s32 count);
int LightSemaphore_TryAcquire(LightSemaphore *semaphore, s32 count);
void LightSemaphore_Release(LightSemaphore *semaphore, s32 count);
void osTickCounterStart(TickCounter *counter);
void osTickCounterUpdate(TickCounter *counter);
double osTickCounterRead(const TickCounter *counter);
void *linearAlloc(size_t size);
void *linearMemAlign(size_t size, size_t alignment);
void linearFree(void *mem);
Result GSPGPU_FlushDataCache(const void *addr, u32 size);

bool aptMainLoop(void);
void aptSetSleepAllowed(bool allowed);
bool aptIsHomeAllowed(void);
void aptSetHomeAllowed(bool allowed);
bool aptShouldClose(void);
void aptSetChainloader(u64 programID, u8 mediatype);
void aptSetChainloaderToSelf(void);
void aptSetChainloaderArgs(const void *deliverArg, size_t deliverArgSize, const void *hmac);
Result APT_ReceiveDeliverArg(void *param, size_t paramSize, void *hmac, u64 *sender,
							 bool *received);
Result aptSendCommand(u32 *cmdbuf);
u32 *getThreadStaticBuffers(void);
#define IPC_Desc_StaticBuffer(size, id) ((u32)(((size) << 14) | ((id) << 10) | 2))
#define IPC_MakeHeader(command, normal, translate)                                                 \
	((u32)(((command) << 16) | (((normal)&0x3F) << 6) | ((translate)&0x3F)))
u32 *getThreadCommandBuffer(void);
Result svcConnectToPort(Handle *out, const char *portName);
Result svcSendSyncRequest(Handle session);
Result svcCloseHandle(Handle handle);
// The shared configuration page; Luma keeps its own config at +0x800 (hbldr.h).
extern u8 sharedPageFake[0x1000];
#define OS_SHAREDCFG_VADDR ((uintptr_t)sharedPageFake)
u32 envGetHeapSize(void);
u32 envGetLinearHeapSize(void);
enum { KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3), KEY_UP = BIT(6) };
enum { KEY_DOWN = BIT(7) };
enum { KEY_X = BIT(10) };
void hidScanInput(void);
u32 hidKeysDown(void);
u32 hidKeysHeld(void);
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
void gspWaitForVBlank(void);
void gfxInitDefault(void);
void gfxExit(void);
u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height);
void gfxSetDoubleBuffering(gfxScreen_t screen, bool enable);
void gfxSetWide(bool enable);
bool gfxIsWide(void);
Result psInit(void);
void psExit(void);
Result PS_GenerateRandomBytes(void *out, size_t len);

typedef enum { CFG_MODEL_3DSXL = 1, CFG_MODEL_2DS = 3, CFG_MODEL_N2DSXL = 5 } CFG_SystemModel;
Result cfguInit(void);
void cfguExit(void);
Result CFGU_GetSystemModel(u8 *model);
Result CFGU_SecureInfoGetRegion(u8 *region);
Result APT_CheckNew3DS(bool *out);
typedef struct {
	u8 build, minor, mainver, reserved_x3;
	char region;
	u8 reserved_x5[3];
} OS_VersionBin;
Result osGetSystemVersionDataString(OS_VersionBin *nver, OS_VersionBin *cver, char *sysverstr,
									u32 sysverstr_maxsize);
Result svcGetSystemInfo(s64 *out, u32 type, s32 param);
#define SYSTEM_VERSION(major, minor, revision)                                                     \
	(((major) << 24) | ((minor) << 16) | ((revision) << 8))
#define GET_VERSION_MAJOR(version) ((version) >> 24)
#define GET_VERSION_MINOR(version) (((version) >> 16) & 0xFF)
#define GET_VERSION_REVISION(version) (((version) >> 8) & 0xFF)
u32 linearSpaceFree(void);
void srvSetBlockingPolicy(bool nonBlocking);
Result mcuHwcInit(void);
void mcuHwcExit(void);
typedef struct InfoLedPattern {
	u8 delay, smoothing, loopDelay, blinkSpeed;
	u8 redPattern[32], greenPattern[32], bluePattern[32];
} InfoLedPattern;
Result MCUHWC_SetInfoLedPattern(const InfoLedPattern *pattern);
Result MCUHWC_ReadRegister(u8 reg, void *data, u32 size);
Result ptmuInit(void);
void ptmuExit(void);
Result PTMU_GetShellState(u8 *out); // 0 means the lid is closed, 1 means it is open
typedef struct {
	u8 wifi_strength, led_battery;
} osSharedConfig_s;
extern osSharedConfig_s osSharedConfigFake;
#define OS_SharedConfig (&osSharedConfigFake)
static inline u8 osGetWifiStrength(void) {
	return OS_SharedConfig->wifi_strength;
}
Result ndmuInit(void);
void ndmuExit(void);
typedef enum {
	NDM_EXCLUSIVE_STATE_NONE = 0,
	NDM_EXCLUSIVE_STATE_INFRASTRUCTURE = 1
} ndmExclusiveState;
Result NDMU_EnterExclusiveState(u32 state);
Result NDMU_LeaveExclusiveState(void);
Result NDMU_GetExclusiveState(ndmExclusiveState *state);
Result NDMU_LockState(void);
Result NDMU_UnlockState(void);
Handle *srvGetSessionHandle(void);
struct in_addr;
Result socInit(u32 *buffer, u32 size);
Result socExit(void);
Result SOCU_GetIPInfo(struct in_addr *ip, struct in_addr *netmask, struct in_addr *broadcast);

typedef u64 FS_Archive;
typedef enum { MEDIATYPE_NAND = 0, MEDIATYPE_SD = 1, MEDIATYPE_GAME_CARD = 2 } FS_MediaType;
typedef enum {
	PATH_INVALID = 0,
	PATH_EMPTY = 1,
	PATH_BINARY = 2,
	PATH_ASCII = 3,
	PATH_UTF16 = 4
} FS_PathType;
#define ARCHIVE_ACTION_COMMIT_SAVE_DATA 0
#define ARCHIVE_ACTION_GET_TIMESTAMP 1
typedef enum { SECURESAVE_ACTION_DELETE = 0 } FS_SecureSaveAction;
#define SECUREVALUE_SLOT_SD 0x1000
typedef enum {
	ARCHIVE_EXTDATA = 6,
	ARCHIVE_SDMC = 9,
	ARCHIVE_USER_SAVEDATA = 0x567890B2
} FS_ArchiveID;
#define FS_OPEN_READ BIT(0)
#define FS_OPEN_WRITE BIT(1)
#define FS_OPEN_CREATE BIT(2)
#define FS_ATTRIBUTE_DIRECTORY BIT(0)
#define FS_WRITE_FLUSH BIT(0)
typedef struct {
	FS_PathType type;
	u32 size;
	const void *data;
} FS_Path;
typedef struct {
	u32 sectorSize, clusterSize, totalClusters, freeClusters;
} FS_ArchiveResource;
typedef struct {
	u16 name[0x106];
	u32 attributes;
	u64 fileSize;
} FS_DirectoryEntry;
FS_Path fsMakePath(FS_PathType type, const void *path);
Handle *fsGetSessionHandle(void);
Result FSUSER_OpenArchive(FS_Archive *archive, FS_ArchiveID id, FS_Path path);
Result FSUSER_CloseArchive(FS_Archive archive);
Result FSUSER_OpenFile(Handle *out, FS_Archive archive, FS_Path path, u32 flags, u32 attributes);
Result FSUSER_OpenFileDirectly(Handle *out, FS_ArchiveID id, FS_Path archivePath, FS_Path filePath,
							   u32 flags, u32 attributes);
Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path);
Result FSUSER_RenameFile(FS_Archive srcArchive, FS_Path src, FS_Archive dstArchive, FS_Path dst);
Result FSUSER_RenameDirectory(FS_Archive srcArchive, FS_Path src, FS_Archive dstArchive,
							  FS_Path dst);
Result FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attributes);
Result FSUSER_DeleteDirectory(FS_Archive archive, FS_Path path);
Result FSUSER_DeleteDirectoryRecursively(FS_Archive archive, FS_Path path);
Result FSUSER_ControlArchive(FS_Archive archive, u32 action, void *input, u32 inputSize,
							 void *output, u32 outputSize);
Result FSUSER_OpenDirectory(Handle *out, FS_Archive archive, FS_Path path);
Result FSUSER_CreateFile(FS_Archive archive, FS_Path path, u32 attributes, u64 fileSize);
Result FSUSER_ControlSecureSave(FS_SecureSaveAction action, void *input, u32 inputSize,
								void *output, u32 outputSize);
Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource *resource);
Result FSUSER_UpdateSha256Context(const void *data, u32 size, u8 *hash);
Result FSFILE_Read(Handle file, u32 *read, u64 offset, void *buffer, u32 size);
Result FSFILE_Write(Handle file, u32 *written, u64 offset, const void *buffer, u32 size, u32 flags);
Result FSFILE_GetSize(Handle file, u64 *size);
Result FSFILE_SetSize(Handle file, u64 size);
Result FSFILE_Flush(Handle file);
Result FSFILE_Close(Handle file);
Result FSDIR_Read(Handle dir, u32 *read, u32 count, FS_DirectoryEntry *entries);
Result FSDIR_Close(Handle dir);

typedef struct {
	u64 titleID, size;
	u16 version;
} AM_TitleInfo;
typedef AM_TitleInfo AM_PendingTitleInfo;
Result amInit(void);
void amExit(void);
Result AM_GetTitleCount(FS_MediaType media, u32 *count);
Result AM_GetTicketCount(u32 *count);
Result AM_GetTitleList(u32 *read, FS_MediaType media, u32 count, u64 *titleIds);
Result AM_GetTitleInfo(FS_MediaType media, u32 count, u64 *titleIds, AM_TitleInfo *info);
Result AM_GetNumPendingTitles(u32 *count, FS_MediaType media);
Result AM_GetPendingTitleInfo(u32 count, FS_MediaType media, u64 *titleIds,
							  AM_PendingTitleInfo *info);
Result AM_NeedsCleanup(u8 *needsCleanup, FS_MediaType media);
Result AM_DoCleanup(FS_MediaType media);
Result AM_DeleteUserTitle(FS_MediaType media, u64 titleID);
Result AM_DeletePendingTitle(FS_MediaType media, u64 titleID);
Result AM_DeleteTicket(u64 ticketID);
Result AMNET_GetTitleNumTickets(u32 *count, u64 titleID);
Result AM_StartCiaInstall(FS_MediaType media, Handle *ciaHandle);
Result AM_StartCiaInstallOverwrite(Handle *ciaHandle, FS_MediaType media);
Result AM_FinishCiaInstall(Handle ciaHandle);
Result AM_CancelCIAInstall(Handle ciaHandle);
Result AM_QueryAvailableExternalTitleDatabase(bool *available);
Result AM_InitializeExternalTitleDatabase(bool overwrite);

// For the QR download: sslc seeds mbedTLS's entropy and the camera is polled.
enum { KEY_Y = BIT(11) };
Result sslcInit(Handle session_handle);
void sslcExit(void);
Result svcWaitSynchronization(Handle handle, s64 nanoseconds);
enum { PORT_CAM1 = BIT(0) };
enum { SELECT_NONE = 0, SELECT_OUT1 = BIT(0), SELECT_IN1 = BIT(1) };
typedef enum { CONTEXT_A = BIT(0) } CAMU_Context;
typedef enum { SIZE_CTR_TOP_LCD = 7 } CAMU_Size;
typedef enum { FRAME_RATE_30 = 8 } CAMU_FrameRate;
typedef enum { PHOTO_MODE_LETTER = 4 } CAMU_PhotoMode;
typedef enum { OUTPUT_RGB_565 = 1 } CAMU_OutputFormat;
Result camInit(void);
void camExit(void);
Result CAMU_SetSize(u32 select, CAMU_Size size, CAMU_Context context);
Result CAMU_SetOutputFormat(u32 select, CAMU_OutputFormat format, CAMU_Context context);
Result CAMU_SetFrameRate(u32 select, CAMU_FrameRate frameRate);
Result CAMU_SetNoiseFilter(u32 select, bool noiseFilter);
Result CAMU_SetAutoExposure(u32 select, bool autoExposure);
Result CAMU_SetAutoWhiteBalance(u32 select, bool autoWhiteBalance);
Result CAMU_SetPhotoMode(u32 select, CAMU_PhotoMode photoMode);
Result CAMU_SetSharpness(u32 select, s8 sharpness);
Result CAMU_Activate(u32 select);
Result CAMU_GetBufferErrorInterruptEvent(Handle *event, u32 port);
Result CAMU_SetTrimming(u32 port, bool trimming);
Result CAMU_GetMaxBytes(u32 *maxBytes, s16 width, s16 height);
Result CAMU_SetTransferBytes(u32 port, u32 bytes, s16 width, s16 height);
Result CAMU_ClearBuffer(u32 port);
Result CAMU_SetReceiving(Handle *event, void *dst, u32 port, u32 imageSize, s16 transferUnit);
Result CAMU_StartCapture(u32 port);
Result CAMU_StopCapture(u32 port);
Result CAMU_IsBusy(bool *busy, u32 port);

#endif // TEST_STUBS_3DS_H

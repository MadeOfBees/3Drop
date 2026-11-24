// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdXfer.h"

#include "app.h"
#include "codec.h" // readLE
#include "fs.h"
#include "hash.h"
#include "net.h" // startPressed
#include "ring.h"
#include "stats.h"
#include "sysinfo.h"
#include "transfer.h"
#include "ui.h"

#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h> // MIN
#include <zlib.h>

// One transfer at a time; a stranded worker keeps these until it returns (fileAbandon).
static Handle file;
static FS_Archive archive;
static char temp[SD_PATH_MAX];

// What a range put keeps between requests. The temp for `path` holds the file's first `size` bytes,
// and `chain` is the digest of those bytes so far. Only one temp is held at a time; a temp that is
// not held is read back from the card.
static struct {
	char path[SD_PATH_MAX];
	u64 size;
	DigestChain chain;
} kept;

void sdPutForget(void) {
	kept.path[0] = '\0';
}

static void keptSet(const char *path, u64 size, DigestChain chain) {
	snprintf(kept.path, sizeof(kept.path), "%s", path);
	kept.size = size;
	kept.chain = chain;
}

// The path's temp starts over, so none of its bytes are kept yet.
static void keptReset(const char *path) {
	DigestChain none;
	digestChainStart(&none);
	keptSet(path, 0, none);
}

static void fileEnd(void) {
	if (file) {
		FSFILE_Close(file);
		file = 0;
	}
	if (archive) {
		fsArchiveClose(archive);
		archive = 0;
	}
	if (temp[0]) {
		fsDelete(fsArchive(), temp, false, false);
		temp[0] = '\0';
		sdPutForget();
	}
}

static void fileAbandon(const RingStats *stats) {
	appendLog("stranded-file-closed", 0, stats->written);
	fileEnd();
}

// zlib's widest window is 15, and adding 16 selects a gzip wrapper instead of a zlib one.
#define GZIP_WBITS (15 + 16)

// For a get: deflate's thread runs at a lower priority than the reader that feeds it and the send
// loop it feeds.
#define GZIP_PRIORITY 0x32
// For a get: the number of chunks sent stored between two tries of deflate. A later try may find
// that the data shrinks again.
#define GZIP_PROBE_CHUNKS 16

static struct {
	z_stream z;
	TransferSession *session;
	unsigned char in[64 * 1024]; // get: a chunk of raw bytes out of the ring, waiting for deflate
	bool inEnd;					 // get: the ring has given its last byte
	u64 raw;					 // put: raw bytes inflated so far
	bool ended;					 // put: the stream's end was inflated
	bool failed;
	Result why; // OUT_OF_MEMORY if zlib ran out of memory, otherwise 0
	// get: deflate's thread fills `out` while the send loop sends the last one it took.
	Thread thread;
	LightSemaphore full, empty; // `out` is ready to take; `out` is free to fill
	unsigned char out[64 * 1024];
	size_t outLen;
	bool outEnd;			 // `out` ends the stream
	int zError;				 // deflate's failure, for the send loop to report
	bool broken;			 // the ring failed, or the thread was stopped
	volatile bool starved;	 // the reader is behind deflate, so its wait counts as the ring's
	volatile bool stop;		 // the send loop is done with the body
	bool chunkDone;			 // the chunk is deflated and its block flushed, so it can be measured
	uLong chunkIn, chunkOut; // the chunk's raw size, and total_out when it went in
	int level;				 // Z_BEST_SPEED, or Z_NO_COMPRESSION for stored blocks
	unsigned stored;		 // chunks stored since the level last changed
} gz;
_Static_assert(sizeof(gz.out) <= RECV_BUFFER_SIZE, "a deflated buffer fits the send's staging");

// zlib's state is on the regular heap, which no stat reads, so a shortage shows in the log.
static bool gzipFail(int z) {
	gz.failed = true;
	gz.why = z == Z_MEM_ERROR ? OUT_OF_MEMORY : 0;
	if (gz.why) {
		appendLog("gzip-no-memory", gz.why, 0);
	}
	return false;
}

// ---- get ----------------------------------------------------------------------------------------

// Sets the level from how the last chunk did, deciding chunk by chunk as the data goes. The level
// becomes stored once deflate's output stops shrinking, and deflate again when a probe chunk
// shrinks.
static bool gzipLevel(void) {
	int level = gz.level;
	if (level == Z_NO_COMPRESSION) { // a stored chunk's size says nothing of what deflate would do
		level = ++gz.stored == GZIP_PROBE_CHUNKS ? Z_BEST_SPEED : Z_NO_COMPRESSION;
	} else if (gz.chunkIn > 0 && gz.z.total_out - gz.chunkOut >= gz.chunkIn) {
		level = Z_NO_COMPRESSION;
	}
	if (level == gz.level) {
		return true;
	}
	// The chunk's block is flushed, so deflateParams changes the level with nothing to write.
	int z = deflateParams(&gz.z, level, Z_DEFAULT_STRATEGY);
	gz.zError = z == Z_OK ? 0 : z;
	gz.level = level;
	gz.stored = 0;
	return z == Z_OK;
}

// Takes the ring's next chunk, after setting the level from how the last chunk did.
static bool gzipTake(void) {
	size_t n = 0;
	if (!gzipLevel()) {
		return false;
	}
	while (n == 0 && !gz.inEnd) {
		if (gz.stop || !ringFill(gz.in, sizeof(gz.in), &n, &gz.inEnd)) {
			return false;
		}
		gz.starved = n == 0 && !gz.inEnd;
		if (gz.starved) {
			svcSleepThread(WAIT_NS);
		}
	}
	gz.z.next_in = gz.in;
	gz.z.avail_in = (uInt)n;
	gz.chunkIn = n;
	gz.chunkOut = gz.z.total_out;
	gz.chunkDone = false;
	return true;
}

// Deflates into `out` until it is full or the stream has ended. Each chunk ends in a flushed block
// (Z_BLOCK, no bytes added), so its output can be measured against its input.
static bool gzipFill(void) {
	int z = Z_OK;
	gz.z.next_out = gz.out;
	gz.z.avail_out = sizeof(gz.out);
	while (gz.z.avail_out > 0 && z != Z_STREAM_END) {
		if (gz.chunkDone && !gzipTake()) {
			return false;
		}
		int flush = gz.z.avail_in > 0 ? Z_NO_FLUSH : gz.inEnd ? Z_FINISH : Z_BLOCK;
		z = deflate(&gz.z, flush);
		if (z != Z_OK && z != Z_STREAM_END) {
			gz.zError = z;
			return false;
		}
		gz.chunkDone = flush == Z_BLOCK && gz.z.avail_out > 0; // with room left, the flush is whole
	}
	gz.outLen = sizeof(gz.out) - gz.z.avail_out;
	gz.outEnd = z == Z_STREAM_END;
	return true;
}

static void gzipWorker(void *arg) {
	(void)arg;
	for (bool done = false; !done;) {
		LightSemaphore_Acquire(&gz.empty, 1);
		gz.broken = gz.stop || !gzipFill();
		done = gz.broken || gz.outEnd;
		LightSemaphore_Release(&gz.full, 1);
	}
}

// Deflate's thread starts a buffer ahead, so a send waits on it only when deflate is the slower.
static bool gzipStart(void) {
	LightSemaphore_Init(&gz.full, 0, 1);
	LightSemaphore_Init(&gz.empty, 1, 1);
	gz.level = Z_BEST_SPEED;
	gz.chunkDone = true;
	gz.thread = threadCreate(gzipWorker, NULL, 16 * 1024, GZIP_PRIORITY, 0, false);
	return gz.thread != NULL;
}

// Deflate's thread stops after its current call; it waits on nothing but this and the ring.
static void gzipJoin(void) {
	gz.stop = true;
	LightSemaphore_Release(&gz.empty, 1); // wakes it if it is waiting for its buffer to be emptied
	threadJoin(gz.thread, UINT64_MAX);
	threadFree(gz.thread);
	gz.thread = NULL;
}

static bool gzipSource(void *context, unsigned char *dst, size_t cap, size_t *got, bool *end) {
	(void)context;
	(void)cap; // unused, since the static assert on `out` covers it
	*got = 0;
	*end = false;
	// Check START before taking a ready buffer as well, because the worker can finish before this
	// call begins.
	if (!transferPoll(gz.session)) {
		return false;
	}
	// Poll START, the screen and the deadlines while deflate is the slower stage. The deadlines end
	// a stuck reader here too, because this loop never waits on the deflate thread's flag for that.
	while (LightSemaphore_TryAcquire(&gz.full, 1) != 0) {
		if (gz.starved) {
			return true; // the reader is behind, so transferSend waits on it as for a plain get
		}
		svcSleepThread(WAIT_NS);
		if (!appRunning() || !transferPoll(gz.session)) {
			return false;
		}
	}
	if (gz.zError) {
		return gzipFail(gz.zError);
	}
	if (gz.broken) {
		return false; // the ring failed, and its stats say how
	}
	memcpy(dst, gz.out, gz.outLen);
	*got = gz.outLen;
	*end = gz.outEnd;
	LightSemaphore_Release(&gz.empty, 1);
	return true;
}

// Mirrors compressionWorthwhile in tools/wire.sh, so a file's put and get agree on whether to
// compress. The decision uses the name and size alone, because a file's head mispredicts its
// body both ways.
#define GZIP_MIN_BYTES 1024 // below this, gzip's ~18-byte envelope dominates
static const char *const GZIP_SKIP_EXTS[] = {
	"cia", "gz",  "tgz",  "zip", "7z",	 "rar", "xz",  "bz2",	"zst",
	"png", "jpg", "jpeg", "gif", "webp", "mp3", "ogg", "opus",	"m4a",
	"mp4", "mkv", "3dsx", "cxi", "cci",	 "3ds", "app", "bcstm", "bcwav",
};

static bool gzipWorthwhile(const char *path, u64 size) {
	const char *dot = strrchr(path, '.');
	for (size_t i = 0;
		 dot && !strchr(dot, '/') && i < sizeof(GZIP_SKIP_EXTS) / sizeof(GZIP_SKIP_EXTS[0]); i++) {
		if (strcasecmp(dot + 1, GZIP_SKIP_EXTS[i]) == 0) {
			return false;
		}
	}
	return size >= GZIP_MIN_BYTES;
}

// FS can't open a folder as a file. Say so with `isdir`, because `notfound` would send the user
// looking for it.
static const char *openRefusal(FS_Archive from, const char *path) {
	bool isDir = false;
	u64 size;
	return R_SUCCEEDED(fsStat(from, path, &isDir, &size)) && isDir ? "isdir" : "notfound";
}

bool sdGetStart(const Target *t, bool gzip, SdGet *get, Operation *op) {
	*op = OP_REJECTED;
	const char *refusal = "notfound";
	Result r = fsTargetOpen(t, &archive);
	if (R_FAILED(r)) {
		archive = 0; // a failed open holds no archive
	} else if (R_FAILED(r = fsOpenRead(archive, t->path, &file))) {
		refusal = openRefusal(archive, t->path);
	} else {
		r = FSFILE_GetSize(file, &get->size);
	}
	if (R_FAILED(r) || get->offset > get->size) {
		fileEnd();
		opSet(op, OUTCOME_REJECTED, R_FAILED(r) ? refusal : "size", r);
		return false;
	}
	get->length = MIN(get->length, get->size - get->offset);
	get->covers = get->length == get->size;
	get->gzip = gzip && gzipWorthwhile(t->path, get->length);
	if (!ringArm(get->length, RING_FROM_HANDLE, false, get->offset) ||
		!ringStart(file, true, fileAbandon)) {
		fileEnd(); // a thread that never started holds nothing
		opSet(op, OUTCOME_REJECTED, "ring", OUT_OF_MEMORY);
		return false;
	}
	memset(&gz, 0, sizeof(gz));
	// Deflate is set up before the header promises gzip. With no memory for deflate's state, the
	// body goes raw. Level 1, because at the default level the console's CPU deflates slower than
	// its wire sends.
	int z = Z_OK;
	if (get->gzip) {
		z = deflateInit2(&gz.z, Z_BEST_SPEED, Z_DEFLATED, GZIP_WBITS, 8, Z_DEFAULT_STRATEGY);
		if (z == Z_OK && !gzipStart()) {
			deflateEnd(&gz.z);
			z = Z_MEM_ERROR; // no memory for the thread's stack
		}
	}
	if (z != Z_OK) {
		get->gzip = gzipFail(z);
	}
	return true;
}

void sdGetSend(int client, bool framed, SdGet *get, Operation *op) {
	TransferSession session;
	RingStats stats;
	transferSessionInit(&session, client, get->length, uiProgressTick, true);
	gz.session = &session;
	bool sent = framed && transferSend(&session, get->gzip ? gzipSource : NULL, NULL);
	if (get->gzip) {
		gzipJoin();
		deflateEnd(&gz.z);
	}
	get->wireBytes = session.acceptedSize;
	if (!ringBodyEnd(sent, PHASE_SD_IO, transferWaitTick, &session, &stats, op)) {
		return; // the file stays open for fileAbandon
	}
	fileEnd();
	if (!framed) {
		opSet(op, OUTCOME_REJECTED, "send", TRANSFER_SEND_FAILED); // no body after a lost header
	} else if (stats.failure) {
		opSet(op, OUTCOME_REJECTED, "read", stats.failure);
	} else if (stats.hashFailure) {
		opSet(op, OUTCOME_REJECTED, "hash", stats.hashFailure);
	} else if (get->gzip && gz.failed) {
		opSet(op, OUTCOME_REJECTED, "gzip", gz.why);
	} else if (!sent) {
		transferFailOp(op, &session);
	} else {
		memcpy(get->digest, stats.digest, sizeof(get->digest));
		opSet(op, OUTCOME_COMMITTED, "ok", 0);
	}
}

// ---- put ----------------------------------------------------------------------------------------

// Reads `n` bytes of the file from `at`, one unit per call, the way the ring reads.
static Result readAt(Handle h, u64 at, u8 *into, u32 n) {
	for (u32 done = 0; done < n;) {
		u32 step = MIN(n - done, RING_UNIT), got = 0;
		Result r = FSFILE_Read(h, &got, at + done, into + done, step);
		if (R_FAILED(r = shortIo(r, got, step))) {
			return r;
		}
		done += step;
	}
	return 0;
}

// From 3dbrew "FIRM": the header is 0x200 bytes and starts with "FIRM". The ARM11 and ARM9 entry
// points are at 0x08 and 0x0C. From 0x40 come four section headers of 0x30 bytes each, holding the
// offset in the file, load address, size, copy method and SHA-256.
#define FIRM_HEADER 0x200u
#define FIRM_ARM11_ENTRY 0x08u
#define FIRM_ARM9_ENTRY 0x0Cu
#define FIRM_SECTIONS 0x40u
#define FIRM_SECTION 0x30u
#define FIRM_SECTION_HASH 0x10u
#define FIRM_SECTOR 0x200u // boot9strap takes a section's offset and size in whole sectors only
#define FIRM_WORD 4u	   // and a load address in whole words only

// Whether [address, end) lies in memory boot9strap lets a FIRM load into (the windows in inRange
// of boot9strap's firm.c): ARM9 RAM, VRAM, DSP and AXI WRAM less its last 0x400 bytes, and FCRAM.
// boot9strap bars FCRAM when the header asks to boot before the lockout (byte 0x10 bit 1), unless
// boot9strap 1.4 finds the lockout done (from CFG9 bits the console cannot read). Here FCRAM is
// allowed whatever the header says.
static bool firmRegion(u64 address, u64 end) {
	static const struct {
		u32 from, to;
	} REGIONS[] = {
		{0x08000000, 0x08100000},
		{0x18000000, 0x18600000},
		{0x1FF00000, 0x1FFFFC00},
		{0x20000000, 0x28000000},
	};
	for (size_t i = 0; i < sizeof(REGIONS) / sizeof(REGIONS[0]); i++) {
		if (address >= REGIONS[i].from && end <= REGIONS[i].to) {
			return true;
		}
	}
	return false;
}

// These checks are drawn from boot9strap's firm.c and main.c, but not all of its checks are made.
// Checked: the magic, an ARM9 entry point, and each non-empty section: whole sectors inside the
// file, with the SHA-256 its header gives (a section at 0 would hold its own hash) and a
// whole-word load address inside the memory boot9strap allows. Also checked: each entry point in
// a section, and a file at least as long as the header plus every section. Not checked, so a FIRM
// that passes may still not boot: that no section overlaps the buffer boot9strap reads the file
// into (0x20001000, but 0x18000000 when it boots before the lockout, and 0x24000000 in 1.0), and,
// before the lockout, its bar on FCRAM and its 3 MB cap. "Before the lockout" means the header
// asks for it and CFG9 says the lockout has not come. The console cannot read CFG9, and a guess
// could refuse a good FIRM. Returns `system` for a file that fails the other checks, or `read` or
// `hash` with the Result that stopped it.
static const char *firmCheck(Handle h, u32 size, Result *why) {
	u8 header[FIRM_HEADER], hash[32];
	u8 *section = ringTier1(); // a page's start: the engine's only measured input
	Result r;
	if (!section) {
		*why = OUT_OF_MEMORY;
		return "ring";
	}
	*why = STAGE_METADATA;
	if (size < FIRM_HEADER) {
		return "system";
	}
	if (R_FAILED(r = readAt(h, 0, header, FIRM_HEADER))) {
		*why = r;
		return "read";
	}
	u64 arm11 = readLE(header + FIRM_ARM11_ENTRY, 4), arm9 = readLE(header + FIRM_ARM9_ENTRY, 4);
	bool arm9In = false, arm11In = arm11 == 0; // a zero ARM11 entry starts nothing there
	u64 wanted = FIRM_HEADER;				   // the bytes boot9strap reads: header and sections
	if (memcmp(header, "FIRM", 4) != 0 || arm9 == 0) {
		return "system";
	}
	for (const u8 *entry = header + FIRM_SECTIONS;
		 entry < header + FIRM_SECTIONS + 4 * FIRM_SECTION; entry += FIRM_SECTION) {
		u64 offset = readLE(entry, 4), address = readLE(entry + 4, 4),
			length = readLE(entry + 8, 4);
		if (length == 0) {
			continue; // boot9strap skips an empty one, wherever it says it starts
		}
		// 64 bits, not 32, so a section that would wrap 32 bits still ends past every region.
		u64 end = address + length;
		if ((offset | length) % FIRM_SECTOR != 0 || offset + length > size ||
			address % FIRM_WORD != 0 || !firmRegion(address, end)) {
			return "system";
		}
		wanted += length;
		arm9In = arm9In || (arm9 >= address && arm9 < end);
		arm11In = arm11In || (arm11 >= address && arm11 < end);
		if (R_FAILED(r = readAt(h, offset, section, (u32)length))) {
			*why = r;
			return "read";
		}
		if (R_FAILED(r = sha256Of(section, (u32)length, hash))) {
			*why = r;
			return "hash";
		}
		if (!digestEqual(hash, entry + FIRM_SECTION_HASH)) {
			return "system";
		}
	}
	if (!arm9In || !arm11In || size < wanted) {
		return "system";
	}
	*why = 0;
	return NULL;
}

// Checks the bytes read back from the closed temp, which are the ones that get published.
static const char *firmRefusal(u32 size, Result *why) {
	Handle h;
	if (R_FAILED(*why = fsOpenRead(fsArchive(), temp, &h))) {
		return "read";
	}
	const char *refusal = firmCheck(h, size, why);
	FSFILE_Close(h);
	return refusal;
}

static bool inflateSink(void *context, const unsigned char *buffer, size_t size) {
	const SdPut *put = context;
	gz.z.next_in = (Bytef *)(uintptr_t)buffer;
	gz.z.avail_in = (uInt)size;
	while (gz.z.avail_in > 0) {
		unsigned char *dst, past;
		size_t cap = 1;
		if (gz.ended) {
			return gzipFail(Z_DATA_ERROR); // bytes after the stream's end
		}
		if (gz.raw == put->length) {
			dst = &past; // the size is reached, so a byte inflated here is one too many
		} else if (!transferRingAcquire(gz.session, &dst, &cap)) {
			return false;
		}
		gz.z.next_out = dst;
		gz.z.avail_out = (uInt)cap;
		int z = inflate(&gz.z, Z_NO_FLUSH);
		size_t made = cap - gz.z.avail_out;
		if ((z != Z_OK && z != Z_STREAM_END) || (dst == &past && made > 0)) {
			return gzipFail(z);
		}
		gz.ended = z == Z_STREAM_END;
		gz.raw += made;
		gApp.inflatedBytes = gz.raw;
		if (made > 0 && !ringCommit(made)) {
			return false;
		}
	}
	return true;
}

static Result openTemp(void) {
	u16 units[SD_PATH_MAX];
	FS_Path p = fsPathUtf16(units, temp);
	Result r = FSUSER_OpenFile(&file, fsArchive(), p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	if (R_FAILED(r) && fsMkParents(fsArchive(), temp)) {
		r = FSUSER_OpenFile(&file, fsArchive(), p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	}
	if (R_FAILED(r)) {
		file = 0; // a failed open holds no handle
	}
	return r;
}

// Renames the temp to the path first, since FS refuses a target that exists. Only after such a
// refusal does the old file move to the backup. If the publish then fails, the old file is renamed
// back, and `missing` is set when that cannot be done.
static Result publish(const char *path, const char *backup, bool *missing) {
	BENCH(u64 fsAt = svcGetSystemTick());
	Result r = fsRename(temp, path, false);
	BENCH(benchFsStep(FS_PUBLISH, &fsAt));
	if (r != FSERROR_ALREADY_EXISTS) {
		return r;
	}
	r = fsRename(path, backup, false);
	if (r == FSERROR_ALREADY_EXISTS) { // the backup of an earlier overwrite, whose delete failed
		fsDelete(fsArchive(), backup, false, false);
		r = fsRename(path, backup, false);
	}
	BENCH(benchFsStep(FS_ASIDE, &fsAt));
	if (R_FAILED(r)) {
		return r;
	}
	r = fsRename(temp, path, false);
	BENCH(benchFsStep(FS_SWAP, &fsAt));
	if (R_FAILED(r)) {
		Result back = fsRename(backup, path, false); // moves the old file back
		if (R_FAILED(back)) {
			appendLog("publish-restore-failed", back, 0);
			*missing = true;
		}
		return r;
	}
	// A backup whose delete fails stays beside a live file, where it is never restored.
	fsDelete(fsArchive(), backup, false, false);
	BENCH(benchFsStep(FS_DROP, &fsAt));
	return 0;
}

static bool receiveBody(TransferSession *session, const SdPut *put) {
	memset(&gz, 0, sizeof(gz));
	gz.session = session;
	if (!put->gzip) {
		return transferReceive(session, put->length, NULL, NULL);
	}
	int z = inflateInit2(&gz.z, GZIP_WBITS);
	if (z != Z_OK) {
		return gzipFail(z);
	}
	bool received = transferReceive(session, put->wireSize, inflateSink, (void *)put);
	inflateEnd(&gz.z);
	if (received && (!gz.ended || gz.raw != put->length)) {
		return gzipFail(Z_DATA_ERROR);
	}
	return received;
}

// The path holds what it held before. A temp that kept bytes before this body is cut back to them,
// unless that fails; any other temp is deleted.
static void putFailed(const SdPut *put, Operation *op, const char *detail, Result why) {
	if (put->offset > 0 && (!file || R_SUCCEEDED(FSFILE_SetSize(file, put->offset)))) {
		temp[0] = '\0';
	}
	fileEnd();
	opSet(op, OUTCOME_REJECTED, detail, why);
}

void sdPut(int client, const char *path, const SdPut *put, Operation *op) {
	char backup[SD_PATH_MAX];
	TransferSession session;
	RingStats stats;
	u8 through[32];
	*op = OP_REJECTED;
	snprintf(temp, sizeof(temp), "%s" SD_PUT_TEMP, path);
	snprintf(backup, sizeof(backup), "%s" SD_PUT_BACKUP, path);
	BENCH(u64 fsAt = svcGetSystemTick());
	fsRename(backup, path, false); // fails harmlessly if there is no orphan or the path exists
	BENCH(benchFsStep(FS_ORPHAN, &fsAt));
	if (put->offset == 0) {
		keptReset(path);
	}
	Result r = openTemp();
	BENCH(benchFsStep(FS_OPEN, &fsAt));
	if (R_FAILED(r)) {
		putFailed(put, op, "open", r);
		return;
	}
	// Set the size up front. A full card then fails before any byte moves, and anything past the
	// kept bytes, whether from a lost put or a failed range, is cut.
	r = FSFILE_SetSize(file, put->offset + put->length);
	BENCH(benchFsStep(FS_SETSIZE, &fsAt));
	if (R_FAILED(r)) {
		putFailed(put, op, "space", r);
		return;
	}
	if (!ringArm(put->length, RING_TO_HANDLE, false, put->offset) ||
		!ringStart(file, sdPutDigests(), fileAbandon)) {
		putFailed(put, op, "ring", OUT_OF_MEMORY);
		return;
	}
	// Inflate and the card spend their time on the file's bytes, which a gzip wire understates.
	transferSessionInit(&session, client, MAX(put->wireSize, put->length), uiProgressTick, true);
	gApp.wireTotal = put->length; // the bar counts the file's bytes, not the (gzip) wire's
	bool received = receiveBody(&session, put);
	if (!ringBodyEnd(received, PHASE_SD_IO, transferWaitTick, &session, &stats, op)) {
		return; // the file stays open for fileAbandon
	}
	DigestChain chain = kept.chain;
	digestChainAdd(&chain);
	digestChainPeek(chain, through);
	const char *detail = NULL;
	Result why = 0;
	bool missing = false;
	if (stats.failure) {
		detail = "write";
		why = stats.failure;
	} else if (stats.hashFailure) {
		detail = "hash";
		why = stats.hashFailure;
	} else if (gz.failed) {
		detail = "gzip";
		why = gz.why;
	} else if (!received) {
		detail = transferFailDetail(session.failure);
		why = session.failure;
	} else if (sdPutDigests() && !digestEqual(through, put->digest)) {
		detail = "digest"; // a captured request replayed with another body publishes nothing
		why = STAGE_DIGEST;
	} else {
		BENCH(fsAt = svcGetSystemTick());
		why = FSFILE_Flush(file);
		BENCH(benchFsStep(FS_FLUSH, &fsAt));
		detail = R_FAILED(why) ? "write" : NULL;
	}
	bool whole = put->offset + put->length == put->size;
	if (!detail) {
		FSFILE_Close(file);
		BENCH(benchFsStep(FS_CLOSE, &fsAt));
		file = 0;
		detail = whole && put->firm ? firmRefusal((u32)put->size, &why) : NULL;
	}
	if (!detail && whole && put->firm && !sysinfoOnAc()) {
		detail = "system"; // the charger left mid-transfer, so nothing is renamed on a battery
		why = ON_BATTERY;
	}
	if (!detail && whole) {
		why = publish(path, backup, &missing);
		detail = R_FAILED(why) ? "publish" : NULL;
	}
	if (missing) { // as a power cut leaves it: the old file at the backup, the new one kept
		temp[0] = '\0';
		opSet(op, OUTCOME_UNKNOWN, detail, why);
		return;
	}
	if (detail) {
		putFailed(put, op, detail, why);
		return;
	}
	temp[0] = '\0';
	// Once published, nothing is kept. An unhashed put (bench nohash) keeps nothing either, since
	// its chain is not the digest of these bytes.
	keptSet(whole || !sdPutDigests() ? "" : path, put->offset + put->length, chain);
	opSet(op, OUTCOME_COMMITTED, "ok", 0);
}

// Reads the file's first `bytes`, one unit per call, and feeds them to a digest that digestStart
// starts. Reading 1073741824 bytes takes a minute and a half. When `timed`, each read and digest
// is added to `stats`. A put's check, which comes before the put's own, must leave `stats` alone.
// Returns NULL, or the refusal with the Result in `*why`.
static const char *digestRead(Handle h, u64 bytes, bool timed, Result *why) {
	u8 *unit = ringTier1();
	if (!unit) {
		*why = OUT_OF_MEMORY;
		return "ring";
	}
	digestStart();
	for (u64 at = 0; at < bytes; at += DIGEST_UNIT) {
		u32 n = (u32)MIN(bytes - at, DIGEST_UNIT);
		TickCounter tick;
		appStatusTick();
		// Check START and APT once per unit, as a get's pump does, because a 4 GB file takes
		// minutes to read.
		*why = startPressed() ? TRANSFER_CANCELED : !appRunning() ? TRANSFER_EXITED : 0;
		if (*why) {
			return transferFailDetail(*why);
		}
		osTickCounterStart(&tick);
		if (R_FAILED(*why = readAt(h, at, unit, n))) {
			return "read";
		}
		if (timed) {
			phaseSince(PHASE_SD_IO, &tick);
		}
		if (R_FAILED(*why = digestUnits(unit, n))) {
			return "hash";
		}
		if (timed) {
			phaseSince(PHASE_HASH, &tick);
		}
	}
	*why = 0;
	return NULL;
}

// Hashes the temp's first `bytes` into the kept chain, the way a body is hashed.
static const char *readBack(Handle h, const char *path, u64 bytes, Result *why) {
	sdPutForget();
	const char *refusal = digestRead(h, bytes, false, why);
	if (!refusal) {
		DigestChain chain;
		digestChainStart(&chain);
		digestChainAdd(&chain);
		keptSet(path, bytes, chain);
	}
	return refusal;
}

const char *sdPutResume(const char *path, u64 offset, const u8 digest[32], Result *why) {
	char name[SD_PATH_MAX];
	Handle h;
	u64 size = 0;
	u8 held[32];
	const char *refusal = NULL;
	snprintf(name, sizeof(name), "%s" SD_PUT_TEMP, path);
	if (R_FAILED(*why = fsOpenRead(fsArchive(), name, &h))) {
		return "size"; // no temp, so none of the bytes are kept
	}
	if (R_FAILED(*why = FSFILE_GetSize(h, &size)) || size < offset) {
		FSFILE_Close(h);
		return "size";
	}
	if (strcmp(kept.path, path) != 0 || kept.size != offset) {
		refusal = readBack(h, path, offset, why);
	}
	FSFILE_Close(h);
	if (refusal) {
		return refusal;
	}
	digestChainPeek(kept.chain, held);
	return digestEqual(held, digest) ? NULL : "digest";
}

bool sdHash(const Target *t, u64 *size, u8 digest[32], Operation *op) {
	FS_Archive from;
	Handle h = 0;
	const char *refusal = "notfound";
	Result why = fsTargetOpen(t, &from);
	if (R_SUCCEEDED(why)) {
		if (R_FAILED(why = fsOpenRead(from, t->path, &h))) {
			refusal = openRefusal(from, t->path);
		} else if (R_SUCCEEDED(why = FSFILE_GetSize(h, size)) &&
				   !(refusal = digestRead(h, *size, true, &why))) {
			TickCounter tick;
			osTickCounterStart(&tick);
			why = digestFinish(digest);
			phaseSince(PHASE_HASH, &tick);
			refusal = R_FAILED(why) ? "hash" : NULL;
		}
		if (h) {
			FSFILE_Close(h);
		}
		fsArchiveClose(from);
	}
	opSet(op, refusal ? OUTCOME_REJECTED : OUTCOME_COMMITTED, refusal ? refusal : "ok", why);
	return !refusal;
}

// ---- put into a save or SD extdata -------------------------------------------------------------

// Creates the file at its final size because a DIFF container cannot be resized. Checkpoint also
// creates at size.
static Result createAt(FS_Archive to, const char *path, u64 size) {
	u16 units[SD_PATH_MAX];
	FS_Path p = fsPathUtf16(units, path);
	Result r = FSUSER_CreateFile(to, p, 0, size);
	if (R_FAILED(r) && fsMkParents(to, path)) {
		r = FSUSER_CreateFile(to, p, 0, size);
	}
	return r;
}

// Flushes the data cache first, as ring.c does before its writes, because recv() wrote the body
// through the data cache and the digest flushed only the units it gave the engine (hash.c), never a
// body under ENGINE_FLOOR.
static Result writeAll(FS_Archive to, const char *path, const unsigned char *body, u32 size) {
	Handle h;
	u16 units[SD_PATH_MAX];
	Result r = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)body, size);
	if (R_SUCCEEDED(r)) {
		r = FSUSER_OpenFile(&h, to, fsPathUtf16(units, path), FS_OPEN_WRITE, 0);
	}
	if (R_FAILED(r)) {
		return r;
	}
	for (u32 at = 0, n, written; R_SUCCEEDED(r) && at < size; at += n) {
		appStatusTick(); // up to 24 of these, each a card write
		n = MIN(size - at, RING_UNIT);
		r = FSFILE_Write(h, &written, at, body + at, n, 0);
		r = shortIo(r, written, n);
	}
	if (R_SUCCEEDED(r)) {
		r = FSFILE_Flush(h);
	}
	FSFILE_Close(h);
	return r;
}

void archivePut(int client, const Target *t, const SdPut *put, Operation *op) {
	u8 digest[32];
	TransferSession session;
	FS_Archive to;
	bool isDir;
	u64 size;
	*op = OP_REJECTED;
	// No worker is started, because nothing is written until the whole body is in and checked.
	if (!ringArm(put->size, RING_TO_HANDLE, false, 0)) {
		opSet(op, OUTCOME_REJECTED, "ring", OUT_OF_MEMORY);
		return;
	}
	transferSessionInit(&session, client, put->size, uiProgressTick, false);
	if (!transferReceive(&session, put->size, NULL, NULL)) {
		transferFailOp(op, &session);
		return;
	}
	const u8 *body = ringTier1();
	Result r = digestOf(body, (u32)put->size, digest);
	if (R_FAILED(r)) {
		opSet(op, OUTCOME_REJECTED, "hash", r);
		return;
	}
	if (!digestEqual(digest, put->digest)) {
		opSet(op, OUTCOME_REJECTED, "digest", STAGE_DIGEST); // a request replayed with another body
		return;
	}
	if (R_FAILED(r = fsTargetOpen(t, &to))) {
		opSet(op, OUTCOME_REJECTED, "open", r);
		return;
	}
	// From here extdata changes as it is written and has no commit, so its failure is `unknown`.
	// A save changes only at its commit (measured: failed writes left it byte-identical).
	// The step that failed is logged. A failure after a delete that went through is what shows, on
	// a console, FS dropping a save's uncommitted change.
	const char *step = "archive-delete-failed";
	bool exists = R_SUCCEEDED(fsStat(to, t->path, &isDir, &size));
	if (exists && t->kind == TARGET_SAVE) {
		r = fsDelete(to, t->path, false, false);
	}
	if (R_SUCCEEDED(r) && (!exists || t->kind == TARGET_SAVE)) {
		step = "archive-create-failed";
		r = createAt(to, t->path, put->size);
	}
	if (R_SUCCEEDED(r)) {
		step = "archive-write-failed";
		r = writeAll(to, t->path, body, (u32)put->size);
	}
	if (R_FAILED(r)) {
		appendLog(step, r, 0);
		opSet(op, t->kind == TARGET_SAVE ? OUTCOME_REJECTED : OUTCOME_UNKNOWN, "write", r);
	} else {
		fsTargetCommit(t, to, op);
	}
	fsArchiveClose(to);
}

void sdDropBackup(const char *path) {
	char backup[SD_PATH_MAX];
	if (snprintf(backup, sizeof(backup), "%s" SD_PUT_BACKUP, path) < (int)sizeof(backup)) {
		Result r = fsDelete(fsArchive(), backup, false, false);
		// Most paths have none; one that stays is restored by the next put to the path.
		if (R_FAILED(r) && r != FSERROR_NOT_FOUND && r != FSERROR_PATH_NOT_FOUND) {
			appendLog("backup-drop-failed", r, 0);
		}
	}
}

// cc: src/auth.c src/codec.c src/hash.c
// The real auth.c: PBKDF2 as key.cache stores it, fail-closed MACs, a key.txt that is replaced only
// when missing, minted keys that use the whole alphabet, MACs pinned to vectors, and a nonce that
// is spent only once. Also the real hash.c: the wire's digest, pinned to vectors.
#include "fakes.h"

#include "auth.h"
#include "codec.h"
#include "hash.h"
#include "path.h"
#include "result.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

// The console's FS can refuse a read outright, whereas the host's fopen will even open a directory.
// With writeFails set, a file opened for writing is created and emptied, as on a full card, but
// accepts no bytes.
static int keyReadErrno;
static bool writeFails;
FILE *fopen(const char *path, const char *mode) {
	if (keyReadErrno && mode[0] == 'r' && strcmp(path, KEY_PATH) == 0) {
		errno = keyReadErrno;
		return NULL;
	}
	int fd = open(path, mode[0] == 'r' ? O_RDONLY : O_RDWR | O_CREAT | O_TRUNC, 0644);
	return fd < 0 ? NULL : fdopen(fd, writeFails && mode[0] == 'w' ? "r" : mode);
}

// With renameFailTo set, the next rename onto that name fails, as a rename on FS can.
static const char *renameFailTo;
int rename(const char *from, const char *to) {
	if (renameFailTo && strcmp(to, renameFailTo) == 0) {
		renameFailTo = NULL;
		errno = EIO;
		return -1;
	}
	return renameat(AT_FDCWD, from, AT_FDCWD, to);
}

static const char *hexOf(const uint8_t *bytes, size_t n) {
	static char hex[MAC_HEX + 1];
	hexEncode(hex, bytes, n);
	return hex;
}

static bool loadKey(const char *key) {
	FILE *file = fopen(KEY_PATH, "w");
	assert(file && fputs(key, file) >= 0 && fclose(file) == 0);
	return authLoadKey();
}

static const char *keyFile(void) {
	static char line[KEY_MAX + 8];
	FILE *file = fopen(KEY_PATH, "r");
	line[0] = '\0';
	assert(file && (fgets(line, sizeof(line), file) || feof(file)) && fclose(file) == 0);
	return line;
}

// With no key, then with keys that must not load, nothing is signed and key.txt is never replaced.
static void failClosed(void) {
	uint8_t mac[MAC_SIZE];
	memset(mac, 0xAA, sizeof(mac));
	char tag[NAME_TAG_HEX + 1];
	assert(!authKeyLoaded() && !authDisplayKey() && !macRequest(mac, "stats 00") &&
		   !authNameTag(tag));
	for (size_t i = 0; i < MAC_SIZE; i++) {
		assert(mac[i] == 0);
	}
	static const char *const REFUSED[] = {"", "\n", "---\n", "-\r\n"};
	for (size_t i = 0; i < sizeof(REFUSED) / sizeof(REFUSED[0]); i++) {
		assert(!loadKey(REFUSED[i]) && !authKeyLoaded() && !strcmp(keyFile(), REFUSED[i]));
	}
	// A key.txt that cannot be read is an error, not a missing key, so nothing is minted over it.
	static const int UNREADABLE[] = {EIO, EACCES};
	for (size_t i = 0; i < sizeof(UNREADABLE) / sizeof(UNREADABLE[0]); i++) {
		keyReadErrno = UNREADABLE[i];
		assert(!authLoadKey() && !authKeyLoaded());
		keyReadErrno = 0;
		assert(!strcmp(keyFile(), "-\r\n"));
	}
	// A key longer than KEY_MAX is refused, never cut to fit.
	char key[KEY_MAX + 3];
	memset(key, 'k', KEY_MAX + 1);
	strcpy(key + KEY_MAX + 1, "\n");
	assert(!loadKey(key) && !authKeyLoaded() && !strcmp(keyFile(), key));
	// Only a missing key.txt mints one, and it loads.
	assert(remove(KEY_PATH) == 0);
	assert(authLoadKey() && authKeyLoaded() && strlen(keyFile()) == 15 &&
		   !strncmp(keyFile(), authDisplayKey(), 14));
	// KEY_MAX characters load.
	strcpy(key + KEY_MAX, "\n");
	assert(loadKey(key) && strlen(authDisplayKey()) == KEY_MAX);
}

static bool present(const char *path) {
	return access(path, F_OK) == 0;
}

// A key file is replaced whole or not at all. A write that fails after its open (a full card or a
// power cut) leaves either the old key, which still loads, or no key, which the next boot mints. It
// never leaves an empty key, which no boot re-mints. The same holds for key.cache, and no temp file
// is left behind.
static void wholeWrites(void) {
	char before[KEY_MAX + 8], cache[256] = "";
	assert(loadKey("abcd-efgh-2345\n"));
	snprintf(before, sizeof(before), "%s", keyFile());
	writeFails = true;
	assert(!authRegenerateKey());
	writeFails = false;
	assert(!strcmp(keyFile(), before) && authLoadKey() &&
		   !strcmp(authDisplayKey(), "abcd-efgh-2345"));
	assert(remove(KEY_PATH) == 0); // a first boot on a full card
	writeFails = true;
	assert(!authLoadKey());
	writeFails = false;
	assert(!present(KEY_PATH) && authLoadKey() && strlen(keyFile()) == 15);
	// A new key whose cache write fails still loads; the old cache stays whole.
	assert(loadKey("abcd-efgh-2345\n"));
	FILE *f = fopen(KEY_CACHE_PATH, "r");
	assert(f && fread(cache, 1, sizeof(cache) - 1, f) > 0 && fclose(f) == 0);
	f = fopen(KEY_PATH, "w");
	assert(f && fputs("wxyz-2345-abcd\n", f) >= 0 && fclose(f) == 0);
	writeFails = true;
	assert(authLoadKey() && !strcmp(authDisplayKey(), "wxyz-2345-abcd"));
	writeFails = false;
	char now[256] = "";
	f = fopen(KEY_CACHE_PATH, "r");
	assert(f && fread(now, 1, sizeof(now) - 1, f) > 0 && fclose(f) == 0 && !strcmp(now, cache));
	// A power cut that left a temp half written changes nothing; the next write replaces it.
	f = fopen(KEY_PATH ".tmp", "w");
	assert(f && fputs("wxyz-23", f) >= 0 && fclose(f) == 0);
	assert(authLoadKey() && !strcmp(authDisplayKey(), "wxyz-2345-abcd"));
	assert(authRegenerateKey() && strlen(keyFile()) == 15);
	assert(!present(KEY_PATH ".tmp") && !present(KEY_CACHE_PATH ".tmp"));
}

// A regeneration that fails keeps the key in use past a restart: a new key that cannot take
// key.txt's name, or that will not read back, gives way to the old one, and a cut between the
// renames leaves the old one aside for the next boot. Nothing is left beside the key.
static void regenFailures(void) {
	assert(loadKey("abcd-efgh-2345\n"));
	renameFailTo = KEY_PATH;
	assert(!authRegenerateKey() && !strcmp(keyFile(), "abcd-efgh-2345\n"));
	keyReadErrno = EIO;
	assert(!authRegenerateKey());
	keyReadErrno = 0;
	assert(!strcmp(keyFile(), "abcd-efgh-2345\n") && authLoadKey() &&
		   !strcmp(authDisplayKey(), "abcd-efgh-2345"));
	assert(rename(KEY_PATH, KEY_PATH ".old") == 0);
	FILE *f = fopen(KEY_PATH ".tmp", "w");
	assert(f && fputs("wxyz-2345-abcd\n", f) >= 0 && fclose(f) == 0);
	assert(authLoadKey() && !strcmp(authDisplayKey(), "abcd-efgh-2345"));
	assert(authRegenerateKey() && strlen(keyFile()) == 15);
	assert(!present(KEY_PATH ".tmp") && !present(KEY_PATH ".old") &&
		   !present(KEY_CACHE_PATH ".old"));
}

// Minted keys use every symbol of the alphabet between them, so a key carries its full 60 bits.
static void mintedKeys(void) {
	static const char ALPHABET[] = "0123456789abcdefghjkmnpqrstvwxyz";
	bool used[32] = {false};
	unsigned symbols = 0;
	for (int n = 0; n < 40; n++) {
		assert(authRegenerateKey());
		const char *key = keyFile();
		assert(strlen(key) == 15 && key[4] == '-' && key[9] == '-' && key[14] == '\n');
		for (int i = 0; i < 14; i++) {
			const char *at = strchr(ALPHABET, key[i]);
			assert(i == 4 || i == 9 || (at && key[i]));
			if (i != 4 && i != 9 && !used[at - ALPHABET]) {
				used[at - ALPHABET] = true;
				symbols++;
			}
		}
	}
	assert(symbols == 32);
}

// A nonce is re-announced for the first half of its life, then replaced. If it is spent at the end
// of its life, it is refused however it was kept.
static void nonceLife(void) {
	char first[NONCE_HEX + 1], second[NONCE_HEX + 1];
	snprintf(first, sizeof(first), "%s", nonceLive()); // main()'s last nonce, minted at this nowMs
	fake.nowMs += 150000 - 1;
	assert(strcmp(nonceLive(), first) == 0);
	fake.nowMs += 1;
	snprintf(second, sizeof(second), "%s", nonceLive());
	assert(strcmp(second, first) != 0 && !nonceSpend(first));
	fake.nowMs += 300000 - 1;
	assert(nonceSpend(second));
	snprintf(first, sizeof(first), "%s", nonceLive());
	fake.nowMs += 300000;
	assert(!nonceSpend(first));
	// Its age comes from the uptime, so a step of the system clock in either direction neither ends
	// it nor keeps it.
	snprintf(first, sizeof(first), "%s", nonceLive());
	fake.wallMs -= 36; // the console's clock stepped back this much once
	assert(nonceIs(first) && !strcmp(nonceLive(), first));
	fake.wallMs += 3600000; // an hour on
	assert(nonceIs(first) && !strcmp(nonceLive(), first));
	fake.wallMs -= 7200000; // an hour back
	fake.nowMs += 300000;
	assert(!nonceIs(first));
	fake.wallMs = 0;
}

// A failing CSPRNG mints no key and hands out no nonce, so no mutation can be spent.
static void failingRng(void) {
	char before[KEY_MAX + 8];
	snprintf(before, sizeof(before), "%s", keyFile());
	assert(nonceSpend(nonceLive()));	// spending it leaves none live
	fake.psResult = (Result)0xE0E01BF5; // any failure
	assert(!authRegenerateKey() && strcmp(keyFile(), before) == 0);
	assert(nonceLive() == NULL && !nonceSpend(""));
	fake.psResult = 0;
}

static const char *digestHex(const u8 digest[32]) {
	static char text[67] = "u:";
	hexEncode(text + 2, digest, 32);
	return text;
}

// The wire's digest on an engine that starts over at every call, as the console's does. testWire.sh
// pins tools/wire.sh's unitDigest to the same vectors. Each body is fed both whole and one unit per
// call. The engine is called for a unit or a finish of ENGINE_FLOOR bytes or more; software handles
// the rest.
static void unitDigests(void) {
	static const struct {
		u32 n;
		const char *digest;
	} VECTORS[] = {
		{0, "u:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
		{1, "u:80903da4e6bbdf96e8ff6fc3966b0cfd355c7e860bdd1caa8e4722d9230e40ac"},
		{1048575, "u:3ba85163b3e9a11197592d073d9e7d57fc2646201366cb9f3a704194325d2488"},
		{1048576, "u:0650756fd33c813b4541443e458d2616696ed1b57bd2729fac99bf0f4d9c0292"},
		{1048577, "u:2895dda6697b1166b130b69de087eaad792838abe974fc9932b277e597c241a1"},
		{3670016, "u:1a123ad2daaf45074139d526e884c29c5c1f3e5c08fb7746ccd08ffbbcbdb43b"},
	};
	u8 *body = memalign(0x1000, 3670016), whole[32], units[32];
	for (u32 i = 0; i < 3670016; i++) {
		body[i] = (u8) "3drop\n"[i % 6];
	}
	fake.linearFull = true; // no unit list, so it is refused and makes no engine call
	unsigned calls = fake.fsCalls;
	assert(digestOf(body, 1, whole) == OUT_OF_MEMORY && fake.fsCalls == calls);
	fake.linearFull = false;
	for (size_t v = 0; v < sizeof(VECTORS) / sizeof(VECTORS[0]); v++) {
		u32 n = VECTORS[v].n;
		calls = fake.fsCalls;
		assert(digestOf(body, n, whole) == 0 && strcmp(digestHex(whole), VECTORS[v].digest) == 0);
		// The engine takes each unit except a last one under ENGINE_FLOOR. The finish (32 bytes per
		// unit) stays in software until there are 256 units.
		u32 count = (n + DIGEST_UNIT - 1) / DIGEST_UNIT, tail = n % DIGEST_UNIT;
		u32 engineUnits = count - (tail && tail < ENGINE_FLOOR);
		assert(fake.fsCalls - calls == engineUnits);
		digestStart();
		for (u32 at = 0; at < n; at += DIGEST_UNIT) {
			assert(digestUnits(body + at, n - at < DIGEST_UNIT ? n - at : DIGEST_UNIT) == 0);
		}
		assert(digestFinish(units) == 0 && digestEqual(units, whole));
		assert(strcmp(refDigestText(body, n), VECTORS[v].digest) == 0);
	}
	units[31] ^= 1;
	assert(!digestEqual(units, whole));
	// The floor: a body one byte below it is hashed in software, which no engine failure reaches; a
	// body of exactly ENGINE_FLOOR bytes goes to the engine, with the same digest.
	for (u32 n = ENGINE_FLOOR - 1; n <= ENGINE_FLOOR; n++) {
		calls = fake.fsCalls;
		fake.shaResult = (Result)0xE0E01BF5;
		assert((digestOf(body, n, whole) == 0) == (n < ENGINE_FLOOR));
		fake.shaResult = 0;
		assert(fake.fsCalls - calls == (n < ENGINE_FLOOR ? 0 : 1));
		assert(digestOf(body, n, whole) == 0 &&
			   strcmp(digestHex(whole), refDigestText(body, n)) == 0);
	}
	// An empty body is neither flushed nor sent, as libctru handles it.
	fake.flushResult = fake.shaResult = (Result)0xE0E01BF5;
	assert(digestOf(body, 0, whole) == 0 && strcmp(digestHex(whole), VECTORS[0].digest) == 0);
	// The first failure sticks: nothing after it is hashed, and the finish returns it.
	fake.flushResult = 0;
	calls = fake.fsCalls;
	digestStart();
	assert(digestUnits(body, 2 * DIGEST_UNIT) == fake.shaResult && fake.fsCalls == calls + 1);
	fake.shaResult = 0;
	assert(digestUnits(body, DIGEST_UNIT) == (Result)0xE0E01BF5);
	assert(digestFinish(whole) == (Result)0xE0E01BF5 && fake.fsCalls == calls + 1);
	// A unit after a short one is a caller's bug, never a digest.
	digestStart();
	assert(digestUnits(body, 1000) == 0 && digestUnits(body, 1) == DIGEST_PAST_END);
	assert(digestFinish(whole) == DIGEST_PAST_END);
	free(body);
}

int main(void) {
	assert(R_SUCCEEDED(authInit()));
	failClosed();
	wholeWrites();
	regenFailures();
	mintedKeys();
	uint8_t mac[MAC_SIZE];
	assert(loadKey("abcd1234wxyz\n"));
	char line[3][80];
	FILE *cache = fopen(KEY_CACHE_PATH, "r");
	assert(cache && fgets(line[0], 80, cache) && fgets(line[1], 80, cache) &&
		   fgets(line[2], 80, cache));
	assert(!strcmp(line[2], "e7003b08e5152d193641cd33e8f081cf5a375796d8801c1964cf365d9f144896\n"));

	// Each MAC domain is pinned by a vector computed separately from this code; testWire.sh pins
	// wire.sh to the same vectors.
	assert(loadKey("abcd-efgh-2345\n") && !strcmp(authDisplayKey(), "abcd-efgh-2345"));
	const char *request = "info 00112233445566778899aabbccddeeff";
	assert(macRequest(mac, request));
	assert(strcmp(hexOf(mac, MAC_SIZE),
				  "7771cf626ba7c87b1cfe00b408a2c38d26637c7bd6e95120b83938479cb16079"
				  "abedfbc88ea2bac536c1e43976d381ecf70461eac5cbd64d11c5cc2e55403b5a") == 0);
	// A cached line past its 64 hex digits never loads. If it were cut to fit, its zeros would key
	// every MAC.
	char pinned[MAC_HEX + 1];
	snprintf(pinned, sizeof(pinned), "%s", hexOf(mac, MAC_SIZE));
	FILE *longer = fopen(KEY_CACHE_PATH, "w");
	assert(longer && fprintf(longer, "abcd-efgh-2345\n10000\n%066d\n", 0) > 0 &&
		   fclose(longer) == 0);
	assert(loadKey("abcd-efgh-2345\n") && macRequest(mac, request) &&
		   strcmp(hexOf(mac, MAC_SIZE), pinned) == 0);
	macResponse(mac, request,
				"response 00112233445566778899aabbccddeeff info 52 000400000ff40a00 52 ready");
	assert(strcmp(hexOf(mac, MAC_SIZE),
				  "bc03ddf532f7ccde156cdab9e41a7b6142cdb6d3b1a99920381ff2094f8f9d3f"
				  "0ba6af5c8c86acaacf3c93a49f5dbbdd49b2d7fa5e25f7d94dbb5cf309c7a38b") == 0);
	macTrailer(mac, "get 0123456789abcdef0123456789abcdef /a",
			   "response 0123456789abcdef0123456789abcdef get ok 4",
			   "trailer 0123456789abcdef0123456789abcdef get ok "
			   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	assert(strcmp(hexOf(mac, MAC_SIZE),
				  "c8486003023a517914b0c7d1af6355a864f9d44a47415cbd26663b87b20d773c"
				  "64315048f579071bb33d13f188db44246d66537a9d8c26ced53ef251f97645be") == 0);
	// The console's name, pinned likewise: testDiscover.sh pins wire.sh's _wireName to it.
	char tag[NAME_TAG_HEX + 1];
	assert(authNameTag(tag) && strcmp(tag, "1938bccf") == 0);

	char nonce[NONCE_HEX + 1];
	snprintf(nonce, sizeof(nonce), "%s", nonceLive());
	assert(strlen(nonce) == NONCE_HEX && !strcmp(nonceLive(), nonce)); // re-announced while young
	assert(!nonceSpend("00") && nonceSpend(nonce) && !nonceSpend(nonce));
	assert(strcmp(nonceLive(), nonce) != 0); // spent, so the next reply carries a fresh one
	nonceLife();
	failingRng();
	unitDigests();
	TEST_PASS();
	return 0;
}

// cc: src/fs.c src/path.c src/codec.c src/hash.c src/transfer.c src/ring.c src/stats.c -lz
// Runs the device's FIRM check for testFirmRules.sh, which compares the fake's check against it.
// Reads one SD path per line from stdin and prints firmCheck's verdict for each file: `ok`, or its
// refusal word.
#include "fakes.h"

#include "sdXfer.c"

int main(void) {
	char path[SD_PATH_MAX];
	while (fgets(path, sizeof(path), stdin)) {
		path[strcspn(path, "\n")] = '\0';
		Handle h;
		u64 size;
		Result why;
		if (R_FAILED(fsOpenRead(fsArchive(), path, &h)) || R_FAILED(FSFILE_GetSize(h, &size))) {
			fprintf(stderr, "firmProbe: cannot read %s\n", path);
			return 1;
		}
		const char *refusal = firmCheck(h, (u32)size, &why);
		FSFILE_Close(h);
		printf("%s\n", refusal ? refusal : "ok");
	}
	return 0;
}

// cc: src/layeredFs.c src/path.c src/codec.c
// The exheader.bin sizes that Luma's loader takes, which src/layeredFs.c restricts writes to.
// testDispatch covers `run` and the writes made through their verbs.
#include "fakes.h"

#include "layeredFs.h"

#include <assert.h>
#include <string.h>

static bool refused(const char *path, u64 size) {
	const char *word = layeredFsWriteRefusal(path, size);
	return word && strcmp(word, "layeredfs") == 0;
}

// An exheader.bin is written only at a size Luma's loader takes, wherever FAT would find it.
static void exheaderSizes(void) {
	const char *at = "/luma/titles/000400000f800100/exheader.bin";
	assert(!layeredFsWriteRefusal(at, 0x400) && !layeredFsWriteRefusal(at, 0x800));
	assert(refused(at, 0x3FF) && refused(at, 0));
	assert(refused("/LUMA/Titles/000400000F800100/EXHEADER.BIN", 0x401));
	assert(!layeredFsWriteRefusal("/luma/titles/000400000f800100/romfs/exheader.bin", 1));
	assert(!layeredFsWriteRefusal("/luma/titles/000400000f800100/exheader.bin.txt", 1));
	assert(!layeredFsWriteRefusal("/luma/titles/000400000f80010/exheader.bin", 1));
	assert(!layeredFsWriteRefusal("/luma/exheader.bin", 1));
}

int main(void) {
	exheaderSizes();
	TEST_PASS();
	return 0;
}

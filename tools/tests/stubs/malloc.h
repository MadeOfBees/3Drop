// <malloc.h> for net.c's memalign; macOS has none.
#pragma once
#include <stdlib.h>
void *memalign(size_t alignment, size_t size);

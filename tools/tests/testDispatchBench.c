// cc: -DTHREEDROP_TEST_VERBS=1 src/bench/bench.c src/bench/capture.c src/bench/ledRead.c
// cc: src/bench/hw.c
// Builds testDispatch with the bench sources. It covers every release verb as the bench build
// answers it, and the bench verb.
#include "testDispatch.c"

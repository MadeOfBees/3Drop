// SPDX-License-Identifier: GPL-3.0-or-later
// quirc (src/quirc/UPSTREAM.txt), the QR decoder, is built as one unit so that its files stay byte
// for byte the same as upstream's. They do not build clean under this project's -Wall -Wextra
// -Werror, because they compare an unsigned count with an int (-Wsign-compare), leave a callback's
// parameter unused (-Wunused-parameter) and test an unsigned enum against 0 (-Wtype-limits). Those
// three warnings are off while the compiler reads its files, and stay on for every other file.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wtype-limits"
#include "quirc/decode.c"
#include "quirc/identify.c"
#include "quirc/quirc.c"
#include "quirc/version_db.c"
#pragma GCC diagnostic pop

// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef QR_H
#define QR_H

#include "protocol.h" // After

// Runs when Y is pressed on the idle screen. It scans a QR code, shows the links it holds and, on
// A, fetches and installs each CIA by the route and checks a computer's install has (install.c). It
// returns when that is over, with the outcome on the screen and in the log. Discovery is not served
// meanwhile, and the listener is served only through serveBusy. The caller refuses a stranded ring
// first.
//
// *after is AFTER_SELF_UPDATED when 3Drop itself was installed. In that case libctru's chainloader
// is set, and the caller leaves exactly as it does after a computer's `put ... self`.
void qrRun(After *after);
// Returns true while a download thread runs: a link's, from its start until its install has ended
// (the ring holds tier 1 and an import may be live), and the certificate list's. It returns false
// at the scanner and at every question. serveBusy serves nothing then, because a socket call or a
// request served on the main thread can hold it past a short key press.
bool qrDownloading(void);
// Implemented in main.c, which owns the listener. Every wait of the flow, and every question it
// asks, calls it once a frame, so a client that connects at a screen is answered (see there).
void serveBusy(void);
// Stops libcurl and sslc, which the first confirmed download started. Safe to call if none did.
void qrExit(void);

#endif // QR_H

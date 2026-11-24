// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CMD_H
#define CMD_H

// This is what a host can ask of the console. The verb table and the verbs about titles and the
// console are in cmd.c; the verbs about files are in cmdFs.c. cmdServe serves one request from the
// table.

#include "protocol.h"

// Returns false when the MAC did not verify.
bool cmdServe(int client, After *after);

// Serves a request with the same framing, MAC and replies as cmdServe, while the QR flow holds the
// main thread (main.c's serveBusy). Only the verbs that name that state (Verb.whileBusy) are
// answered; every other verb is refused `busy`. It takes no After and hands none back, so nothing
// served here can leave the loop.
void cmdServeBusy(int client, Busy busy);

#endif // CMD_H

// cc: -DTLS_ALLOW_PLAIN_HTTP=0
// testQr's harness with plain http turned off, the one switch certs.h keeps. It tests the flow's
// refusal of a plain link, which the shipped build never reaches (it allows plain http, as FBI and
// Universal Updater do).
#include "testQr.c"

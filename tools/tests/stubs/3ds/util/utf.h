// <3ds/util/utf.h>, for fs.c's directory walk.
#pragma once
#include <stdint.h>
#include <sys/types.h>
ssize_t utf16_to_utf8(uint8_t *out, const uint16_t *in, size_t len);
ssize_t utf8_to_utf16(uint16_t *out, const uint8_t *in, size_t len);

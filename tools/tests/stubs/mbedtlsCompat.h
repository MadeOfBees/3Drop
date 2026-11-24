// macOS has only mbedtls 3, which renamed the `_ret` SHA-256 functions of the device's
// mbedtls 2.28 to the plain names.
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#define mbedtls_sha256_starts_ret mbedtls_sha256_starts
#define mbedtls_sha256_update_ret mbedtls_sha256_update
#define mbedtls_sha256_finish_ret mbedtls_sha256_finish
#define mbedtls_sha256_ret mbedtls_sha256
#endif

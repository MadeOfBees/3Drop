// The subset of libcurl 8.4.0 that src/download.c uses, with the real signatures and values
// (curl.h, curl/easy.h). The host tests don't link libcurl. The test that runs a download defines
// these functions itself and scripts what a transfer does; no other test reaches them.
#ifndef TEST_STUBS_CURL_H
#define TEST_STUBS_CURL_H

#include <stddef.h>
#include <stdint.h>

typedef void CURL;
typedef int64_t curl_off_t;

typedef enum {
	CURLE_OK = 0,
	CURLE_UNSUPPORTED_PROTOCOL = 1,
	CURLE_FAILED_INIT = 2,
	CURLE_COULDNT_RESOLVE_PROXY = 5,
	CURLE_COULDNT_RESOLVE_HOST = 6,
	CURLE_COULDNT_CONNECT = 7,
	CURLE_HTTP_RETURNED_ERROR = 22,
	CURLE_WRITE_ERROR = 23,
	CURLE_OPERATION_TIMEDOUT = 28,
	CURLE_SSL_CONNECT_ERROR = 35,
	CURLE_ABORTED_BY_CALLBACK = 42,
	CURLE_TOO_MANY_REDIRECTS = 47,
	CURLE_GOT_NOTHING = 52,
	CURLE_SEND_ERROR = 55,
	CURLE_RECV_ERROR = 56,
	CURLE_OUT_OF_MEMORY = 27,
	CURLE_PEER_FAILED_VERIFICATION = 60,
	CURLE_SSL_CACERT_BADFILE = 77,
	CURLE_LAST = 100
} CURLcode;

// The option and info numbers are libcurl's own (CURLOPTTYPE_* + n), so a wrong one here is a wrong
// one there.
#define CURLOPTTYPE_LONG 0
#define CURLOPTTYPE_OBJECTPOINT 10000
#define CURLOPTTYPE_FUNCTIONPOINT 20000
#define CURLOPTTYPE_OFF_T 30000
#define CURLOPTTYPE_BLOB 40000
typedef enum {
	CURLOPT_WRITEDATA = CURLOPTTYPE_OBJECTPOINT + 1,
	CURLOPT_URL = CURLOPTTYPE_OBJECTPOINT + 2,
	CURLOPT_PROXY = CURLOPTTYPE_OBJECTPOINT + 4,
	CURLOPT_USERAGENT = CURLOPTTYPE_OBJECTPOINT + 18,
	CURLOPT_LOW_SPEED_LIMIT = CURLOPTTYPE_LONG + 19,
	CURLOPT_LOW_SPEED_TIME = CURLOPTTYPE_LONG + 20,
	CURLOPT_CAINFO = CURLOPTTYPE_OBJECTPOINT + 65,
	CURLOPT_FOLLOWLOCATION = CURLOPTTYPE_LONG + 52,
	CURLOPT_MAXREDIRS = CURLOPTTYPE_LONG + 68,
	CURLOPT_FAILONERROR = CURLOPTTYPE_LONG + 45,
	CURLOPT_WRITEFUNCTION = CURLOPTTYPE_FUNCTIONPOINT + 11,
	CURLOPT_NOPROGRESS = CURLOPTTYPE_LONG + 43,
	CURLOPT_NOSIGNAL = CURLOPTTYPE_LONG + 99,
	CURLOPT_SSL_VERIFYPEER = CURLOPTTYPE_LONG + 64,
	CURLOPT_SSL_VERIFYHOST = CURLOPTTYPE_LONG + 81,
	CURLOPT_CONNECTTIMEOUT = CURLOPTTYPE_LONG + 78,
	CURLOPT_BUFFERSIZE = CURLOPTTYPE_LONG + 98,
	CURLOPT_SOCKOPTFUNCTION = CURLOPTTYPE_FUNCTIONPOINT + 148,
	CURLOPT_XFERINFODATA = CURLOPTTYPE_OBJECTPOINT + 57,
	CURLOPT_XFERINFOFUNCTION = CURLOPTTYPE_FUNCTIONPOINT + 219,
	CURLOPT_PROTOCOLS_STR = CURLOPTTYPE_OBJECTPOINT + 318,
	CURLOPT_REDIR_PROTOCOLS_STR = CURLOPTTYPE_OBJECTPOINT + 319,
	CURLOPT_SSL_CTX_FUNCTION = CURLOPTTYPE_FUNCTIONPOINT + 108,
	CURLOPT_SSL_CTX_DATA = CURLOPTTYPE_OBJECTPOINT + 109,
	CURLOPT_CAINFO_BLOB = CURLOPTTYPE_BLOB + 309
} CURLoption;

// curl/easy.h's blob. With NOCOPY, libcurl keeps `data` and copies only this struct.
#define CURL_BLOB_COPY 1
#define CURL_BLOB_NOCOPY 0
struct curl_blob {
	void *data;
	size_t len;
	unsigned int flags;
};

#define CURLINFO_LONG 0x200000
#define CURLINFO_OFF_T 0x600000
#define CURLINFO_STRING 0x100000
typedef enum {
	CURLINFO_EFFECTIVE_URL = CURLINFO_STRING + 1,
	CURLINFO_RESPONSE_CODE = CURLINFO_LONG + 2,
	CURLINFO_SSL_VERIFYRESULT = CURLINFO_LONG + 13,
	CURLINFO_CONTENT_LENGTH_DOWNLOAD_T = CURLINFO_OFF_T + 15
} CURLINFO;

#define CURL_GLOBAL_DEFAULT 3L

typedef int curl_socket_t;
typedef enum { CURLSOCKTYPE_IPCXN, CURLSOCKTYPE_ACCEPT, CURLSOCKTYPE_LAST } curlsocktype;
#define CURL_SOCKOPT_OK 0
typedef int (*curl_sockopt_callback)(void *clientp, curl_socket_t curlfd, curlsocktype purpose);
typedef size_t (*curl_write_callback)(char *buffer, size_t size, size_t nitems, void *outstream);
// The setup hook for the TLS config. Under this backend, `ssl_ctx` is Mbed TLS's
// mbedtls_ssl_config.
typedef CURLcode (*curl_ssl_ctx_callback)(CURL *curl, void *ssl_ctx, void *userptr);
typedef int (*curl_xferinfo_callback)(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
									  curl_off_t ultotal, curl_off_t ulnow);

// The URL API (curl/urlapi.h), with its real values.
typedef struct Curl_URL CURLU;
typedef enum {
	CURLUE_OK = 0,
	CURLUE_MALFORMED_INPUT = 3,
	CURLUE_OUT_OF_MEMORY = 6,
	CURLUE_NO_PORT = 15,
	CURLUE_NO_QUERY = 16
} CURLUcode;
typedef enum {
	CURLUPART_URL = 0,
	CURLUPART_HOST = 5,
	CURLUPART_PORT = 6,
	CURLUPART_PATH = 7,
	CURLUPART_QUERY = 8
} CURLUPart;
#define CURLU_URLDECODE (1 << 6) // decodes percent codes on get (the path is shown as written)
CURLU *curl_url(void);
void curl_url_cleanup(CURLU *handle);
CURLUcode curl_url_set(CURLU *handle, CURLUPart what, const char *part, unsigned int flags);
CURLUcode curl_url_get(const CURLU *handle, CURLUPart what, char **part, unsigned int flags);
void curl_free(void *p);

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *handle);
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *handle);
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...);

#endif // TEST_STUBS_CURL_H

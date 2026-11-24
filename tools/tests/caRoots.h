// Two root certificates from Mozilla's bundle, as C strings. A bundle that the publish test keeps
// has to be one Mbed TLS can read, so this fixture is real certificate data, not just a pair of
// markers. They are certificates 45 (Amazon Root CA 3) and 79 (GlobalSign ECC Root CA - R4) of
// `https://curl.se/ca/cacert.pem` as fetched on 2026-10-20, copied verbatim and in the file's
// order, each BEGIN..END block as one C string. To recreate them, take the same two blocks from a
// fresh copy. They are the two smallest ECC roots in that file, which keeps a test's bundle small;
// both are valid past 2037.
//
// CA_JUNK is a file that a marker scan can't tell from a bundle but Mbed TLS can't read. A captive
// portal, a cut download or another writer of the shared list can leave such a file behind.
#ifndef TEST_CA_ROOTS_H
#define TEST_CA_ROOTS_H

// clang-format off
#define CA_ROOT_AMAZON3 \
	"-----BEGIN CERTIFICATE-----\n" \
	"MIIBtjCCAVugAwIBAgITBmyf1XSXNmY/Owua2eiedgPySjAKBggqhkjOPQQDAjA5MQswCQYDVQQG\n" \
	"EwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6b24gUm9vdCBDQSAzMB4XDTE1MDUy\n" \
	"NjAwMDAwMFoXDTQwMDUyNjAwMDAwMFowOTELMAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZ\n" \
	"MBcGA1UEAxMQQW1hem9uIFJvb3QgQ0EgMzBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABCmXp8ZB\n" \
	"f8ANm+gBG1bG8lKlui2yEujSLtf6ycXYqm0fc4E7O5hrOXwzpcVOho6AF2hiRVd9RFgdszflZwjr\n" \
	"Zt6jQjBAMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgGGMB0GA1UdDgQWBBSrttvXBp43\n" \
	"rDCGB5Fwx5zEGbF4wDAKBggqhkjOPQQDAgNJADBGAiEA4IWSoxe3jfkrBqWTrBqYaGFy+uGh0Psc\n" \
	"eGCmQ5nFuMQCIQCcAu/xlJyzlvnrxir4tiz+OpAUFteMYyRIHN8wfdVoOw==\n" \
	"-----END CERTIFICATE-----\n"

#define CA_ROOT_GLOBALSIGN_R4 \
	"-----BEGIN CERTIFICATE-----\n" \
	"MIIB3DCCAYOgAwIBAgINAgPlfvU/k/2lCSGypjAKBggqhkjOPQQDAjBQMSQwIgYDVQQLExtHbG9i\n" \
	"YWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkds\n" \
	"b2JhbFNpZ24wHhcNMTIxMTEzMDAwMDAwWhcNMzgwMTE5MDMxNDA3WjBQMSQwIgYDVQQLExtHbG9i\n" \
	"YWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkds\n" \
	"b2JhbFNpZ24wWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAS4xnnTj2wlDp8uORkcA6SumuU5BwkW\n" \
	"ymOxuYb4ilfBV85C+nOh92VC/x7BALJucw7/xyHlGKSq2XE/qNS5zowdo0IwQDAOBgNVHQ8BAf8E\n" \
	"BAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUVLB7rUW44kB/+wpu+74zyTyjhNUwCgYI\n" \
	"KoZIzj0EAwIDRwAwRAIgIk90crlgr/HmnKAWBVBfw147bmF0774BxL4YSFlhgjICICadVGNA3jdg\n" \
	"UM/I2O2dgq43mLyjj0xMqTQrbO/7lZsm\n" \
	"-----END CERTIFICATE-----\n"
// clang-format on

#define CA_ROOTS CA_ROOT_AMAZON3 CA_ROOT_GLOBALSIGN_R4

#define CA_JUNK "-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n"

// The two symbols src/caBundle.S gives the app (certs.c reads them), defined here with the two
// roots, so a test's "shipped bundle" is one that Mbed TLS can read, not the real 200 KB one
// (testCerts reads res/cacert.pem itself). The test that includes this header defines them.
const unsigned char CA_BUNDLE[] = CA_ROOTS;
const unsigned CA_BUNDLE_SIZE = sizeof(CA_BUNDLE) - 1;

#endif // TEST_CA_ROOTS_H

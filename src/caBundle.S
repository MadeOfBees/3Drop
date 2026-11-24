/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The certificate bundle, res/cacert.pem, as bytes of the app: certs.c hands them to libcurl when
 * there is no list on the card that reads. CACERT_FILE is the Makefile's, the bundle's whole path.
 * The bytes are the file's own, with no end byte added, and CA_BUNDLE_SIZE is how many.
 */
	.section .rodata.caBundle, "a"
	.balign 4
	.global CA_BUNDLE
	.global CA_BUNDLE_SIZE
CA_BUNDLE:
	.incbin CACERT_FILE
CA_BUNDLE_END:
	.balign 4
CA_BUNDLE_SIZE:
	.word CA_BUNDLE_END - CA_BUNDLE

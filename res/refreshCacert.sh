#!/usr/bin/env bash
# refreshCacert.sh: res/cacert.pem, the certificate bundle the app ships, fetched again from curl.se with
# its checksum beside it (res/cacert.upstream.txt says what it is). Run it by hand when a release wants a
# newer bundle: the build never fetches. Takes no arguments; needs curl and openssl.
set -euo pipefail
# shellcheck source=tools/lib.sh
. "$(dirname "$0")/../tools/lib.sh"
[ "$#" = 0 ] || usage
SOURCE=https://curl.se/ca
scratch
for name in cacert.pem cacert.pem.sha256; do
	curl --fail --silent --show-error --location --proto '=https' -o "$SCRATCH/$name" "$SOURCE/$name" ||
		die "could not fetch $SOURCE/$name"
done
want=$(awk '{ print $1; exit }' "$SCRATCH/cacert.pem.sha256")
got=$(openssl dgst -sha256 -r "$SCRATCH/cacert.pem" | awk '{ print $1; exit }')
if [ -z "$want" ] || [ "$got" != "$want" ]; then
	die "cacert.pem is not the file its checksum names: got $got, want ${want:-none}"
fi
grep -q -- '^-----BEGIN CERTIFICATE-----' "$SCRATCH/cacert.pem" || die "cacert.pem holds no certificate"
cp "$SCRATCH/cacert.pem" "$SCRATCH/cacert.pem.sha256" "$ROOT/res/"
echo "$TOOL: res/cacert.pem is now $got; $(grep -m 1 'Certificate data from Mozilla' "$ROOT/res/cacert.pem")"

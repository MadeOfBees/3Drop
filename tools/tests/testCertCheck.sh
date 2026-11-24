#!/usr/bin/env bash
# The certificate hook of src/download.c (onCertificate) under Mbed TLS's own verification, on the
# certificates openssl makes here, run by certCheckProbe.c. A link to an IPv4 address takes a
# certificate that lists the address among its IP-address names, which Mbed TLS 2.28 does not read
# (it compares DNS names only). No certificate's dates are checked, on any certificate of the chain,
# since the console's own clock is no judge of them; every other check stays. Each case is one real
# verification of a chain. The probe says the flags Mbed TLS ends with, which libcurl decides on,
# and the flags the failure screen is given, which must be the same.
# host-only: ccTest links mbedtls, which the Linux image lacks.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
command -v openssl >/dev/null || fail "no openssl to make the certificates with"
ccTest "$TMP/probe" tools/tests/certCheckProbe.c -lmbedx509

# The certificates, made the way the benchmark kit makes its own: self-signed, with the list the
# console trusts holding the certificate itself. issue <name> <key> <cn> <extensions> <from> <to>
# [<issuer>] makes <name>.pem, valid from <from> minutes from now until <to> minutes from now,
# self-signed by <key> or signed by <issuer>. The `site` extensions name what $SAN says; `plain`
# names nothing, so the CN is read.
mkdir "$TMP/ca"
: >"$TMP/ca/index.txt"
echo 01 >"$TMP/ca/serial"
cat >"$TMP/ca/ca.cnf" <<EOF
[req]
distinguished_name=d
[d]
[ca]
default_ca=k
[k]
database=$TMP/ca/index.txt
serial=$TMP/ca/serial
new_certs_dir=$TMP/ca
default_md=sha256
policy=p
unique_subject=no
[p]
commonName=supplied
[authority]
basicConstraints=critical,CA:TRUE
[site]
basicConstraints=CA:FALSE
subjectAltName=\${ENV::SAN}
[plain]
basicConstraints=CA:FALSE
EOF
now=$(date +%s)
DAY=1440 YEAR=$((365 * 1440)) # minutes
# stamp <minutes> prints the time that many minutes from now, in openssl ca's UTC time format.
stamp() {
	local at=$((now + $1 * 60))
	date -u -d "@$at" +%y%m%d%H%M%SZ 2>/dev/null || date -u -r "$at" +%y%m%d%H%M%SZ
}
issue() {
	local name=$1 key=$2 sign=(-selfsign -keyfile "$TMP/$2.key")
	[ -z "${7:-}" ] || sign=(-cert "$TMP/$7.pem" -keyfile "$TMP/$7.key")
	{
		openssl req -new -key "$TMP/$key.key" -subj "/CN=$3" -config "$TMP/ca/ca.cnf" -out "$TMP/$name.csr" &&
			openssl ca -batch -notext -config "$TMP/ca/ca.cnf" "${sign[@]}" -in "$TMP/$name.csr" \
				-extensions "$4" -startdate "$(stamp "$5")" -enddate "$(stamp "$6")" -out "$TMP/$name.pem"
	} >"$TMP/issue.log" 2>&1 || fail "openssl made no $name: $(cat "$TMP/issue.log")"
}
for key in server authority intermediate; do
	openssl genrsa -out "$TMP/$key.key" 2048 >/dev/null 2>&1 || fail "openssl made no $key key"
done
export SAN=''
IP=192.168.1.5 OTHER=192.168.1.6 NAME=files.example
IPLINK=https://$IP:8443/f.cia NAMELINK=https://$NAME:8443/f.cia
# Valid from three days back to thirty days ahead, except where a case moves a date; its name says which.
FROM=$((-3 * DAY)) TO=$((30 * DAY))
SAN=IP:$IP issue ip server $IP site $FROM $TO
SAN=IP:$OTHER issue other server $IP site $FROM $TO
SAN=DNS:$NAME issue dns server $NAME site $FROM $TO
issue cn server $IP plain $FROM $TO
# A leaf dated years out, either way, with a name, with an address, and with another address.
SAN=DNS:$NAME issue expired server $NAME site $((-5 * YEAR)) $((-3 * YEAR))
SAN=DNS:$NAME issue notYet server $NAME site $((3 * YEAR)) $((5 * YEAR))
SAN=IP:$IP issue ipExpired server $IP site $((-5 * YEAR)) $((-3 * YEAR))
SAN=IP:$IP issue ipNotYet server $IP site $((3 * YEAR)) $((5 * YEAR))
SAN=IP:$OTHER issue otherExpired server $IP site $((-5 * YEAR)) $((-3 * YEAR))
# An address of four parts, as a control, and an IPv6 address. Then names that aren't IPv4 addresses
# even though their bytes begin with one: a DNS name of four letters that spell an address
# (97.98.99.100), and an IPv6 address whose first four bytes are $IP's (c0a8:105::).
SAN=IP:1.2.3.4 issue four server $IP site $FROM $TO
SAN=IP:::1 issue six server $IP site $FROM $TO
SAN=DNS:abcd issue spells server $IP site $FROM $TO
SAN=IP:c0a8:105:: issue longer server $IP site $FROM $TO
# Authorities dated years out, either way, over one key and name: a root (the list's own) and an
# intermediate (signed by the valid root), with a leaf signed by each. The chain files are the leaf
# then the intermediate.
issue authority authority Authority authority $((-30 * DAY)) $TO
SAN=DNS:$NAME issue leaf server $NAME site $FROM $TO authority
issue rootExpired authority Authority authority $((-5 * YEAR)) $((-3 * YEAR))
issue rootNotYet authority Authority authority $((3 * YEAR)) $((5 * YEAR))
issue intermediate intermediate Intermediate authority $FROM $TO authority
issue interExpired intermediate Intermediate authority $((-5 * YEAR)) $((-3 * YEAR)) authority
issue interNotYet intermediate Intermediate authority $((3 * YEAR)) $((5 * YEAR)) authority
SAN=DNS:$NAME issue leafInter server $NAME site $FROM $TO intermediate
for name in intermediate interExpired interNotYet; do
	cat "$TMP/leafInter.pem" "$TMP/$name.pem" >"$TMP/chain-$name.pem"
done

# check <what> <flags> <trust> <chain> <first link> [<link>] verifies the chain against the list <trust>
# for the host of <link> (the first link's when none is given). The flags are what Mbed TLS ends with
# and what the screen gets. A wrong case is reported at the end, with the others.
wrong=''
check() {
	local got
	got=$("$TMP/probe" "$TMP/$3.pem" "$TMP/$4.pem" "$5" "${6:-$5}")
	[ "$got" = "flags=$2 screen=$2" ] || wrong+="$1: got '$got', want '$2'"$'\n'
}

# An address among the IP-address names is the host's.
check "an IP link, the same address among the IP names" ok ip ip "$IPLINK"
check "an IP link, another address among the IP names" CN_MISMATCH other other "$IPLINK"
check "an IP link, DNS names only" CN_MISMATCH dns dns "$IPLINK"
check "a DNS link, its address only among the IP names" CN_MISMATCH ip ip "$NAMELINK"
check "an IP link, no names and the CN the address" ok cn cn "$IPLINK"
check "a DNS link, its name among the DNS names" ok dns dns "$NAMELINK"
# Nothing else is forgiven along with it. The issuer's flag stays, whether or not the host's flag is gone.
check "an IP link, the address named, the issuer not trusted" NOT_TRUSTED dns ip "$IPLINK"
check "an IP link, DNS names only, the issuer not trusted" CN_MISMATCH+NOT_TRUSTED ip dns "$IPLINK"
# The host is the connection's own: a redirect's, not the first link's.
check "a DNS link sent on to an IP link" ok ip ip "$NAMELINK" "$IPLINK"
check "an IP link sent on to a DNS link" CN_MISMATCH ip ip "$IPLINK" "$NAMELINK"
check "an IP link sent on to another address" CN_MISMATCH ip ip "$IPLINK" "https://$OTHER/f.cia"
# Anything that isn't an IPv4 address, or is one the certificate doesn't name, stays as Mbed TLS
# flagged it. The hosts are canonical, as libcurl's parser hands them over; the strict parse's own
# inputs are below.
check "an address of four parts, as a control" ok four four "$IPLINK" https://1.2.3.4/f.cia
check "an IPv6 link" CN_MISMATCH six six "$IPLINK" 'https://[::1]:8443/f.cia'
check "a DNS name that spells the address" CN_MISMATCH spells spells "$IPLINK" https://97.98.99.100/f.cia
check "an IPv6 name that begins with the address" CN_MISMATCH longer longer "$IPLINK"
# No date is checked, so a leaf that expired years ago and one that starts years from now both pass.
check "a leaf that ended years ago" ok expired expired "$NAMELINK"
check "a leaf that starts in years" ok notYet notYet "$NAMELINK"
# The same holds on every certificate of the chain: the intermediate and the root, each way.
check "an intermediate that ended years ago" ok authority chain-interExpired "$NAMELINK"
check "an intermediate that starts in years" ok authority chain-interNotYet "$NAMELINK"
check "a root that ended years ago" ok rootExpired leaf "$NAMELINK"
check "a root that starts in years" ok rootNotYet leaf "$NAMELINK"
check "a chain with an intermediate, in date, as a control" ok authority chain-intermediate "$NAMELINK"
# Every other check stays when a date is out too, and the screen is given what is left.
check "an expired leaf, the issuer not trusted" NOT_TRUSTED ip expired "$NAMELINK"
check "a leaf not yet valid, the issuer not trusted" NOT_TRUSTED ip notYet "$NAMELINK"
check "an expired leaf, another site's name" CN_MISMATCH expired expired "$IPLINK"
check "an expired leaf, the address named" ok ipExpired ipExpired "$IPLINK"
check "a leaf not yet valid, the address named" ok ipNotYet ipNotYet "$IPLINK"
check "an expired leaf, another address named" CN_MISMATCH otherExpired otherExpired "$IPLINK"
check "an expired root, the leaf's name wrong" CN_MISMATCH rootExpired leaf "$IPLINK"
check "an expired intermediate, the root not trusted" NOT_TRUSTED ip chain-interExpired "$NAMELINK"
# The strict IPv4 parse is called on host texts directly, not through a URL (whose parser is
# libcurl's). It takes four parts of one to three digits, 0 to 255, and nothing else.
while read -r host want; do
	got=$("$TMP/probe" ipv4 "$host")
	[ "$got" = "$want" ] || wrong+="the IPv4 parse of '$host': got $got, want $want"$'\n'
done <<EOF
1.2.3.4 1.2.3.4
192.168.1.5 192.168.1.5
0.0.0.0 0.0.0.0
255.255.255.255 255.255.255.255
1.2.3 no
256.1.1.1 no
1.2.3.4.5 no
0001.2.3.4 no
+1.2.3.4 no
EOF
# A defensive white-box check, not a chain Mbed TLS can build (it sets the host's flag for the
# site's certificate alone). The hook leaves an issuer's host flag as it was, whatever the
# certificate names.
[ "$("$TMP/probe" issuer "$TMP/ip.pem" "$IPLINK")" = "flags=CN_MISMATCH screen=CN_MISMATCH" ] ||
	wrong+="the host's flag on an issuer: not left as it was"$'\n'
[ -z "$wrong" ] || fail "wrong verdicts:"$'\n'"$wrong"
echo "$TEST_NAME: PASS"

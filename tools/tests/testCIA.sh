#!/usr/bin/env bash
# The CIA parser (src/cia.c) against malformed CIAs, and cia.sh, the host's reader, against the
# parser on CIAs laid out otherwise than makerom lays out an application; with arguments, on real
# CIAs too, e.g. `testCIA.sh 3drop.cia`.
# host-only: ccTest links mbedtls, which the Linux image lacks.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
# shellcheck source=tools/lib.sh
. "$ROOT/tools/lib.sh"
# shellcheck source=tools/cia.sh
. "$ROOT/tools/cia.sh"
ccTest "$TMP/ciaProbe" tools/tests/ciaProbe.c src/cia.c src/codec.c
mkdir "$TMP/vectors"
"$TMP/ciaProbe" "$TMP/vectors"
# parsed <cia> prints the title and version the console reads, given the prefix size the host sends.
parsed() { "$TMP/ciaProbe" "$1" "$(ciaPrefixSize "$1")"; }

for cia in "$TMP"/vectors/*.cia "$@"; do
	expect "$(ciaTitleID "$cia") $(ciaRead "$cia" && echo "$ciaVer")" "$(parsed "$cia")" "$cia: what the host reads"
done
for cia in "$@"; do
	echo "testCIA: parsed $cia title=$(ciaTitleID "$cia") version=$(ciaRead "$cia" && echo "$ciaVer")"
done
for cia in "$TMP"/vectors/*.cia; do
	title=$(ciaTitleID "$cia")
	ciaSetVersion "$cia" 4660 || fail "$cia: ciaSetVersion failed"
	expect "$(parsed "$cia")" "$title 4660" "$cia: the version ciaSetVersion wrote"
done

# A title version is 16 bits, so ciaSetVersion refuses anything else and leaves the CIA as it was.
cia="$TMP/vectors/app.cia"
cp "$cia" "$TMP/app.orig"
for v in 65536 -1 x ''; do
	ciaSetVersion "$cia" "$v" 2>/dev/null && fail "ciaSetVersion took version '$v'"
done
cmp -s "$cia" "$TMP/app.orig" || fail "ciaSetVersion wrote a version it refused"
ciaSetVersion "$cia" 65535 || fail "ciaSetVersion refused version 65535"
expect "$(parsed "$cia")" "0004000001234500 65535" "the last title version"

# refused <what> <file> checks that every reader refuses the file and says so once, and that ciaSetVersion
# leaves it as it was.
refused() {
	local reader out said="$TOOL: $2 is not a CIA laid out as its header says"
	cp "$2" "$TMP/refused.orig"
	for reader in ciaTitleID ciaPrefixSize; do
		out=$("$reader" "$2" 2>"$TMP/err") && fail "$reader took $1"
		expect "$out$(cat "$TMP/err")" "$said" "$reader on $1"
	done
	ciaSetVersion "$2" 1 2>"$TMP/err" && fail "ciaSetVersion took $1"
	expect "$(cat "$TMP/err")" "$said" "ciaSetVersion on $1"
	cmp -s "$2" "$TMP/refused.orig" || fail "ciaSetVersion wrote into $1"
}
cp "$TMP/vectors/dlc.cia" "$TMP/long.cia"
printf x >>"$TMP/long.cia"
refused "a byte past its sections" "$TMP/long.cia"
head -c 20 "$TMP/vectors/dlc.cia" >"$TMP/short.cia"
refused "a header cut short" "$TMP/short.cia"
cp "$TMP/vectors/dlc.cia" "$TMP/top.cia"
filePatch "$TMP/top.cia" 31 80 # the size of the contents goes negative when read as signed 64 bits
refused "a contents size past 2^63" "$TMP/top.cia"
# app.cia with no content and its meta grown to keep the sum.
cp "$TMP/vectors/app.cia" "$TMP/empty.cia"
filePatch "$TMP/empty.cia" 20 003b00000000000000000000
refused "no content" "$TMP/empty.cia"
cp "$TMP/vectors/dlc.cia" "$TMP/far.cia"
filePatch "$TMP/far.cia" 12 00001000 # a ticket of 1048576 bytes
refused "a TMD past the file" "$TMP/far.cia"
# app.cia with its TMD cut to its signature type and its contents grown to keep the sum.
cp "$TMP/vectors/app.cia" "$TMP/thin.cia"
filePatch "$TMP/thin.cia" 16 04000000
filePatch "$TMP/thin.cia" 24 400b000000000000
refused "a TMD short of its fixed part" "$TMP/thin.cia"
echo "testCIA: PASS"

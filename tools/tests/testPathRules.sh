#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2034,SC2153 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# The fake's path rules held to the device's, src/path.c compiled here, over one corpus: the fake stays
# bash, since Git Bash has no compiler, and so must not drift from the console.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
if ! command -v cc >/dev/null; then
	skip "no C compiler here, so the fake's path rules are unchecked against src/path.c"
	echo "$TEST_NAME: PASS"
	exit 0
fi
# Compile path.c and codec.c alone, without ccTest's fakes and mbedtls, which the Linux image lacks.
# glibc hides strnlen under plain -std=c11.
cc -std=c11 -D_DEFAULT_SOURCE -DSELF_TITLE_ID="0x${FAKE_SELF}ULL" -Wall -Wextra -Werror -pedantic \
	-I"$STUBS" -I"$ROOT/src" \
	"$TESTS/pathRulesProbe.c" "$ROOT/src/path.c" "$ROOT/src/codec.c" -o "$TMP/pathRules"

# fakeRules <field> prints the fake's verdicts, in pathRulesProbe.c's order.
fakeRules() {
	local path root detail tid ls=ok read=ok target=ok sd=ok write=- tree=- removal=- sysWrite=- sysRemoval=-
	fakeNormalize "$1" || ls=path
	fakePath "$1" "" read || read=$detail
	fakePath "$1" sd || sd=$detail
	fakePath "$1" || target=$detail
	if [ "$target" = ok ] && [ "$root" = sd ]; then
		write=ok tree=ok removal=ok sysWrite=ok sysRemoval=ok
		fakeWritable "$path" || write=$detail
		fakeWritable "$path" tree || tree=$detail
		fakeRemovable || removal=$detail
		fakeWritable "$path" "" system || sysWrite=$detail
		fakeRemovable system || sysRemoval=$detail
	elif [ "$target" = ok ]; then
		write=ok
		fakeSchemeWritable || write=$detail
		sysWrite=$write
	fi
	echo "$ls $read $target $sd $write $tree $removal $sysWrite $sysRemoval"
}

printf -v a249 '%249s' ''
a249=${a249// /a}
{
	# Shape: the root, empty and dot segments, `sdmc:`, the characters FAT refuses.
	printf '%s\n' / /a /a/b sdmc:/a sdmc:a sdmc: /a/ //a /a//b /a. /a./b /. /.. '/a:b' '/a\b'
	# The keys, the console's data and what it boots from, 8.3 aliases included, in either case.
	printf '%s\n' /3ds /3DS /3ds/3drop /3Ds/3DROP /3ds/3drop/key.txt /3ds/3drop/KEY.cache \
		/3ds/3drop/keyring/x /3ds/3drop/x /3ds/3dropx /3ds/apps /3ds/apps/x.3dsx /ninten~1 /NINTEN~1/x \
		/ni~2 /nix /boot.firm /BOOT.FIRM /boot~1.fir /bo~2 /bootx /luma /LUMA /luma/config.ini \
		/luma/payloads /luma/payloads/x.firm /luma/sysmodules/x /luma/paylo~1 /lumax \
		/LUMA/SYSMODULES/X.CXI /luma/payloads/a/b /luma/paylo~1/x /luma/payloads/x~1.fir \
		/boot.firm/x /lumax/payloads/x /luma/payloadsx/x /luma/titles/x.firm
	# Luma's per-title directories: managed, variation 02, a patch, 3Drop's own, not an id.
	for dir in /luma/titles /luma/plugins /LUMA/Titles; do
		for sub in '' /0004000001234500 /0004000001234500/romfs/a /0004000001234502/code.bin \
			/0004000E01234500/x /0004000003d37200/x /0004000003D37200 /0004000003d37201/x \
			/0004003000008f02/x /nothex00nothex00/x /000400000123450/x x/0004000003d37200; do
			echo "$dir$sub"
		done
	done
	# Inside a save or extdata: the title is managed, is 3Drop's, or is neither, plus malformed ids.
	printf '%s\n' save:0004000001234500/a save:0004000001234500/ save:0004000001234500 \
		save:0004000001234500sdmc:/a save:0004000001234500hex:2f61 save:0004000001234502/a \
		save:0004000003d37200/a save:0004000003D37200/a save:0004000e01234500/a save:00040000012345/a \
		extdata:00012345/a extdata:00000300/a extdata:000002ff/a extdata:000fffff/a extdata:00100000/a \
		extdata:000014d1/a extdata:0000165c/a extdata:0003d372/a extdata:0003D372/a extdata:12345/a
	# hex: fields add the space and nothing else, are decoded once, and hold 255 bytes at most.
	printf '%s\n' hex:2f hex:2f61 hex:2F61 hex:2f6 hex: hex:zz hex:2f617f hex:2f61ff \
		"hex:$(wireHex '/a b')" "hex:$(wireHex '/Nintendo 3DS')" "hex:$(wireHex 'sdmc:/a b')" \
		"hex:$(wireHex hex:2f61)" "save:0004000001234500hex:$(wireHex '/a b')"
	# A newline and a NUL, which bash drops in a decode, and 257 bytes, measured before `sdmc:` comes
	# off.
	printf '%s\n' hex:2f610a hex:2f61000a "hex:$(wireHex "sdmc:/${a249}aa")"
	# The length at its edges, plain and in hex, with and without `sdmc:`.
	printf '%s\n' "/${a249}aaaaaa" "/${a249}aaaaa" "sdmc:/${a249}aaaaa" "hex:$(wireHex "/${a249}aaaaaa")" \
		"hex:$(wireHex "/${a249}aaaaa")" "hex:$(wireHex "sdmc:/${a249}a")" "hex:$(wireHex "sdmc:/$a249")"
} >"$TMP/fields"

# The rules pinned as `<verdicts>|<path>` (printf %b escapes), so the device and the fake cannot drift together.
pin() { # `<verdicts>|<path>` lines on stdin
	while IFS='|' read -r want p; do
		printf -v raw '%b' "$p"
		field="hex:$(wireHex "$raw")"
		echo "$field" >>"$TMP/fields"
		got=$("$TMP/pathRules" <<<"$field")
		[ "$got" = "$want" ] || fail "$p: the device answered '$got', not '$want'"
	done
}
pin <<'EOF'
ok ok ok ok system system system ok ok|/luma/payloads/x.firm
ok ok ok ok system system system ok ok|/LUMA/SYSMODULES/X.CXI
ok ok ok ok system system system ok ok|/luma/payloads/\xe3\x83\x9d.firm
ok ok ok ok system system system ok ok|/luma/payloads/x~1.fir
ok ok ok ok system system system system system|/luma/payloads
ok ok ok ok system system system system system|/luma/payloads/a/b
ok ok ok ok system system system system system|/luma/paylo~1/x
ok ok ok ok system system system ok system|/boot.firm
ok ok ok ok system system system ok system|/BOOT.FIRM
ok ok ok ok system system system system system|/boot~1.fir
ok ok ok ok system system system system system|/boot.firm/x
ok ok ok ok system system system system system|/boot.firm.3drop.bak
ok ok ok ok system system system system system|/BOOT.FIRM.3DROP.BAK
ok ok ok ok system system system system system|/luma/native.firm
ok ok ok ok system system system system system|/LUMA/TWL.FIRM
ok ok ok ok system system system system system|/luma/agb.firm
ok ok ok ok system system system system system|/luma/safe.firm
ok ok ok ok system system system system system|/luma/sysupdater.firm
ok ok ok ok system system system system system|/luma/native~1.fir
ok ok ok ok ok ok ok ok ok|/luma/native.fir
ok ok ok ok reserved reserved ok reserved ok|/boot.firm.3drop.tmp
ok ok ok ok reserved reserved ok reserved ok|/d/x.3drop.tmp
ok ok ok ok reserved reserved ok reserved ok|/d/X.3DROP.BAK
ok ok ok ok reserved reserved ok reserved ok|/d/.3drop.bak
ok ok ok ok system system system reserved ok|/luma/payloads/x.3drop.bak
ok ok ok ok ok ok ok ok ok|/d/x.3drop.tmpx
ok ok ok ok reserved reserved ok reserved ok|/d.3drop.tmp/x
ok ok ok ok ok ok ok ok ok|/d/x.3drop.tm\xc3\xa9
ok ok ok ok ok ok ok ok ok|/boot.firm.bak
ok ok ok ok ok refused system ok system|/luma
ok ok ok ok refused refused ok refused ok|/luma/titles/x.firm
ok ok ok ok refused refused refused refused refused|/Nintendo 3DS/x
ok ok protected protected - - - - -|/3d\xc5\xbf
ok ok protected protected - - - - -|/3d\xc5\xbf/3drop/key.txt
ok ok protected protected - - - - -|/\xef\xbc\x93ds
ok ok protected protected - - - - -|/\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0
ok ok protected protected - - - - -|/3ds/3drop/\xe2\x84\xaaey.txt
ok ok protected protected - - - - -|/3ds/3drop/\xe3\x83\xad\xe3\x82\xb0
ok ok protected protected - - - - -|/3ds/M\xc3\xbasica
ok ok protected protected - - - - -|/3ds/\xe3\x83\x9d.3dsx
ok protected protected protected - - - - -|/3ds/3drop/key.txt
ok ok ok ok system system system system system|/boot.f\xc4\xb1rm
ok ok ok ok refused refused refused refused refused|/N\xc4\xb1ntendo 3DS
ok ok ok ok refused refused refused refused refused|/N\xc4\xb1ntendo 3DS/x
ok ok ok ok refused refused refused refused refused|/\xef\xbc\xae\xef\xbc\xa9\xef\xbc\xae\xef\xbc\xb4\xef\xbc\xa5\xef\xbc\xae~1
ok ok ok ok system system system system system|/l\xc3\xbcma/payloads/x.firm
ok ok ok ok system system system system system|/\xef\xbc\xac\xef\xbc\xb5\xef\xbc\xad\xef\xbc\xa1/payloads/x
ok ok ok ok ok refused system ok system|/\xef\xbc\xac\xef\xbc\xb5\xef\xbc\xad\xef\xbc\xa1
ok ok ok ok system system system system system|/luma/\xc5\xbfysmodules/x
ok ok ok ok system system system system system|/luma/t\xc4\xb1tles/0004000001234500/x
ok ok ok ok refused refused ok refused ok|/luma/titles/\xef\xbc\x90004000001234500/x
ok ok ok ok ok ok ok ok ok|/\xe9\x9f\xb3\xe6\xa5\xbd
ok ok ok ok ok ok ok ok ok|/\xe9\x9f\xb3\xe6\xa5\xbd/x
ok ok ok ok ok ok ok ok ok|/\xe9\x9f\xb3\xe6\xa5\xbd/\xe6\x9b\xb2.mp3
ok ok ok ok ok ok ok ok ok|/\xe9\x9f\xb3\xe6\xa5\xbd/\xe3\x82\xa2/\xe3\x82\xa2/01.mp3
ok ok ok ok ok ok ok ok ok|/Pok\xc3\xa9mon.sav
ok ok ok ok ok ok ok ok ok|/M\xc3\xbasica/x
ok ok ok ok ok ok ok ok ok|/\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3/x
ok ok ok ok system system system system system|/\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3/\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96/x
ok ok ok ok ok ok ok ok ok|/3ds/Checkpoint/saves/0x0055D \xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3/x
ok ok ok ok ok ok ok ok ok|/JKSV/Saves/\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3 \xe3\x83\xa0\xe3\x83\xbc\xe3\x83\xb3/x
ok ok ok ok ok ok ok ok ok|/3ds/M\xc3\xbasica/x
ok ok ok ok ok ok ok ok ok|/themes/\xe3\x83\x86\xe3\x83\xbc\xe3\x83\x9e
ok ok ok ok ok ok ok ok ok|/x/a\xc2\x9b
ok ok ok ok ok ok ok ok ok|/x/a\xf0\x9f\x98\x80
ok ok ok ok ok ok ok ok ok|/x/a\xe2\x80\xa8
ok ok ok ok ok ok ok ok ok|/x/a.b\xc3\xa9
ok ok ok ok ok ok ok ok ok|/x/\xef\xbc\x8e\xef\xbc\x8e
path path path path - - - - -|/x/..\xc2\xa0
path path path path - - - - -|/3ds/.\xc2\xa0/3drop/key.txt
path path path path - - - - -|/x/. \xe3\x80\x80/boot.firm
path path path path - - - - -|/x/.\xe2\x80\x8b
path path path path - - - - -|/x/.\xef\xbc\x8e
path path path path - - - - -|/x/\xe3\x83\x9d.\xe3\x83\xa0
path path path path - - - - -|/3\xc1\xa4s
path path path path - - - - -|/\xc0\xaf3ds
path path path path - - - - -|/x/a\xed\xa0\x80
path path path path - - - - -|/x/a\xf4\x90\x80\x80
path path path path - - - - -|/x/a\xe3\x81
path path path path - - - - -|/x/a\x80
EOF
# FS opens 253 UTF-16 units; one more is not a path, and a 2-byte character counts as one.
a252=${a249}aaa
pin <<EOF
ok ok ok ok ok ok ok ok ok|/$a252
path path path path - - - - -|/${a252}a
ok ok ok ok ok ok ok ok ok|/${a249}aa\xc3\xa9
path path path path - - - - -|/${a249}aaa\xc3\xa9
EOF

"$TMP/pathRules" <"$TMP/fields" >"$TMP/byDevice"
while IFS= read -r field; do fakeRules "$field"; done <"$TMP/fields" >"$TMP/byFake"
[ "$(wc -l <"$TMP/byDevice")" = "$(wc -l <"$TMP/fields")" ] || fail "the device's rules answered $(wc -l <"$TMP/byDevice") of $(wc -l <"$TMP/fields") fields"
if ! cmp -s "$TMP/byDevice" "$TMP/byFake"; then
	paste -d'|' "$TMP/fields" "$TMP/byDevice" "$TMP/byFake" | awk -F'|' '$2 != $3 { printf "%s\n  device %s\n  fake   %s\n", $1, $2, $3 }' >&2
	fail "the fake's path rules are not the device's"
fi
echo "testPathRules: PASS ($(wc -l <"$TMP/fields" | tr -d ' ') fields)"

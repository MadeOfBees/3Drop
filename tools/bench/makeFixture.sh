#!/usr/bin/env bash
# makeFixture.sh <size> [unique-id-hex]: the Testing Tool and size x 1048576 bytes of random RomFS,
# as buildFixture/fixture<size>m.cia, or in THREEDROP_FIXTURE_DIR; size 0 is the Testing Tool alone.
# The unique id is an application's (0x300 to 0xFFFFF) other than 3Drop's, and defaults to ff430:
# title 000400000ff43000. Install the CIA with drop.sh, then run: probe.sh tool <ip> <title-id>.
# The RomFS is kept per size and rsf hash, so a new id repackages only the tool. Intermediates are
# private, and the CIA arrives by a rename, so a reader of the old one keeps its bytes.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -gt 0 ] || usage
size="$1"
uid="${2:-ff430}"
requireUnits "$size" || exit
size=$((10#$size)) # a leading 0 isn't octal, and `00` isn't size 0
# makerom accepts a malformed unique id silently and stamps something else.
uid="${uid#0[xX]}"
case "$uid" in "" | *[!0-9a-fA-F]*) dieWith 2 "unique id must be hex, e.g. ff711 -- got '$uid'" ;; esac
# At most 8 digits, because bash reads 16#ffffffffffffffff as -1 and 17 digits wrap into range. Ids
# below 0x300 are system titles, and ids above 0xFFFFF are New 3DS titles (src/titles.h).
if [ "${#uid}" -gt 8 ] || [ $((16#$uid)) -lt $((16#300)) ] || [ $((16#$uid)) -gt $((16#fffff)) ]; then
	dieWith 2 "unique id '$uid' is no application's, 0x300 to 0xFFFFF (pass the uid, not a title id)"
fi
title=$(uidTitleID "$uid")
self=$(selfTitleID) || exit # it says why
[ "$title" != "$self" ] || dieWith 2 "unique id '$uid' is 3Drop's own, the title the tool returns to"

resolveCIATools
out=$FIXTURES
cia="$out/fixture${size}m.cia"
mkdir -p "$out"
content1=$(romfsKept "$size")
TMPDIR=$out scratch # beside the fixture, so the kept content arrives by a rename and is never half written
work=$SCRATCH

# The uid reaches only makerom, so the tool is rebuilt only when something it is built from is
# newer, and then it is rebuilt whole (-B), since make cannot see the flags (3Drop's title id comes
# from res/buildCIA.rsf, which tools/cia.sh reads).
if [ ! -f "$ROOT/testingTool.elf" ] || [ -n "$(find "$ROOT/src" "$ROOT/tools/bench/testingTool.c" "$ROOT/Makefile" \
	"$ROOT/tools/lint.Dockerfile" "$ROOT/res/buildCIA.rsf" "$ROOT/tools/cia.sh" -newer "$ROOT/testingTool.elf")" ]; then
	"$ROOT/tools/build.sh" -B TARGET=testingTool all >/dev/null
fi
short="Fixture ${size}M"
long="3drop $short"
# These are the SMDH's short and long titles. The tool's long title names the id it is uninstalled
# by.
if [ "$size" = 0 ]; then
	short="3Drop Test"
	long="3Drop Testing Tool, title $title"
fi
ciaIconBanner "$work" "$short" "$long" 3drop "$ROOT/tools/bench/blankIcon.png" "$ROOT/tools/bench/blankBanner.png"

contents=(-content "$work/content0.ncch:0:0x00000000")
if [ "$size" != 0 ]; then
	if [ ! -f "$content1" ]; then
		[ -x "$ciaThreeDsTool" ] || die "3dstool not found or not executable: $ciaThreeDsTool"
		mkdir -p "$work/root"
		dd if=/dev/urandom of="$work/root/data.bin" bs=1048576 count="$size" 2>/dev/null
		"$ciaThreeDsTool" -c -t romfs -f "$work/part.romfs" --romfs-dir "$work/root"
		rm -rf "$work/root"
		"$ciaMakeRom" -f ncch -o "$work/content1.ncch" -romfs "$work/part.romfs" \
			-rsf "$ROOT/res/fixtureCFA.rsf" -target t
		mv "$work/content1.ncch" "$content1"
	fi
	contents+=(-content "$content1:1:0x00000001")
fi
sed "s/__UNIQUE_ID__/0x$uid/" "$ROOT/res/testingToolCIA.rsf" >"$work/cxi.rsf"
"$ciaMakeRom" -f ncch -o "$work/content0.ncch" -elf "$ROOT/testingTool.elf" -icon "$work/icon.icn" \
	-banner "$work/banner.bnr" -rsf "$work/cxi.rsf" -exefslogo -target t
"$ciaMakeRom" -f cia -o "$work/fixture.cia" "${contents[@]}" -rsf "$work/cxi.rsf" -target t

got=$(ciaTitleID "$work/fixture.cia")
[ "$got" = "$title" ] || die "built title $got, asked for $title; the fixture was left as it was"
# The new CIA is made beside the old one, then renamed over it, so a rep still reading the old one
# (another arm of its size) keeps its bytes.
mv -f "$work/fixture.cia" "$cia"
# Flush, or the first rep measures the host's page-cache writeback instead of the transfer.
sync
echo "fixture: $cia ($(fileSize "$cia") bytes, title $got)"

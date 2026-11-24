#!/usr/bin/env bash
# makeCIA.sh: packages 3drop.elf as 3drop.cia at title version BUILD_NUMBER, 0 to 65535 (0 when unset;
# needs bannertool and makerom)

# BUILD_NUMBER is patched into the TMD directly because makerom's -ver cannot express most of 0..65535.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
# shellcheck source=tools/cia.sh
. "$ROOT/tools/cia.sh"
resolveCIATools

[ -f "$ROOT/3drop.elf" ] || die "3drop.elf not found: run tools/build.sh first"
version="${BUILD_NUMBER:-0}"
ciaValidVersion "$version" || die "BUILD_NUMBER must be 0..65535, got $version"

# The title's SMDH strings live in the Makefile.
makeVar() {
	local value
	value=$(sed -n "s/^$1[[:space:]]*:=[[:space:]]*//p" "$ROOT/Makefile")
	[ -n "$value" ] || die "no $1 in the Makefile"
	echo "$value"
}

mkdir -p "$ROOT/build"
ciaIconBanner "$ROOT/build" "$(makeVar APP_TITLE)" "$(makeVar APP_DESCRIPTION)" "$(makeVar APP_AUTHOR)" \
	"$ROOT/res/icon.png" "$ROOT/res/banner.png"
"$ciaMakeRom" -f cia -o "$ROOT/3drop.cia" -elf "$ROOT/3drop.elf" -rsf "$ROOT/res/buildCIA.rsf" \
	-icon "$ROOT/build/icon.icn" -banner "$ROOT/build/banner.bnr" -exefslogo -target t

# 3Drop's own id is what makes an install of it `self`.
titleID=$(ciaTitleID "$ROOT/3drop.cia")
self=$(selfTitleID)
[ "$titleID" = "$self" ] || die "the CIA is title $titleID, not 3Drop's $self"
ciaSetVersion "$ROOT/3drop.cia" "$version"
echo "CIA title version: $version"
ls -la "$ROOT/3drop.cia"

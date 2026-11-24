# shellcheck shell=bash
# shellcheck disable=SC2153 # TOOL is lib.sh's
# Functions for a CIA file on the host; source this after lib.sh. The console parses every CIA
# again (src/cia.c) and decides; these reads only route and size what is sent.

CIA_TMD_TITLE_ID=$((0x4C)) # past the TMD's signature, as in src/cia.c
CIA_TMD_VERSION=$((0x9C))

# ciaLayout <cia>: sets ciaPrefix, the bytes through the TMD, and ciaTmdBody, where the TMD's
# body starts past its signature. It follows ciaParse's rule in src/cia.c: each section the
# header sizes starts 64-byte aligned, and the last ends the file.
ciaLayout() {
	local size hex header cert ticket tmd meta content=0 at=0 s i sig=0
	local -a w=() start=()
	size=$(fileSize "$1") && hex=$(fileHex "$1" 0 32) || return
	if [ "${#hex}" = 64 ]; then
		for ((i = 0; i < 64; i += 8)); do # the header's sizes are little-endian
			w+=("$((16#${hex:i+6:2}${hex:i+4:2}${hex:i+2:2}${hex:i:2}))")
		done
		header=${w[0]} cert=${w[2]} ticket=${w[3]} tmd=${w[4]} meta=${w[5]}
		content=$((w[6] | w[7] << 32))
		for s in "$header" "$cert" "$ticket" "$tmd" "$content" "$meta"; do
			at=$(((at + 63) & ~63))
			start+=("$at")
			at=$((at + s))
		done
	fi
	# The device's own checks: the sections end the file exactly, around some content, and the TMD
	# holds its signature and fixed part, so every read below stays inside it.
	if [ "$at" = "$size" ] && [ "$content" -gt 0 ]; then
		case $(fileHex "$1" "${start[3]}" 4) in # RSA-4096, RSA-2048, ECDSA; each SHA-1 or SHA-256
		0001000[03]) sig=$((0x240)) ;;
		0001000[14]) sig=$((0x140)) ;;
		0001000[25]) sig=$((0x80)) ;;
		esac
	fi
	if [ "$sig" = 0 ] || [ "$tmd" -lt $((sig + 0x9C4)) ]; then
		echo "$TOOL: $1 is not a CIA laid out as its header says" >&2
		return 1
	fi
	ciaPrefix=$((start[3] + tmd))
	ciaTmdBody=$((start[3] + sig))
}

ciaPrefixSize() { ciaLayout "$1" && echo "$ciaPrefix"; }

# ciaRead <cia>: runs ciaLayout, and sets ciaTitle and ciaVer to the title id and version with
# one read of the TMD.
ciaRead() {
	local hex
	ciaLayout "$1" && hex=$(fileHex "$1" $((ciaTmdBody + CIA_TMD_TITLE_ID)) $((CIA_TMD_VERSION + 2 - CIA_TMD_TITLE_ID))) || return
	# shellcheck disable=SC2034 # ciaVer is dropBatch.sh's
	ciaTitle=${hex:0:16} ciaVer=$((16#${hex:$(((CIA_TMD_VERSION - CIA_TMD_TITLE_ID) * 2)):4}))
}

ciaTitleID() { ciaRead "$1" && echo "$ciaTitle"; }

# selfTitleID: 3Drop's own title id, from the RSF makerom stamps it with.
selfTitleID() {
	local uid
	uid=$(sed -n 's/^[[:blank:]]*UniqueId[[:blank:]]*:[[:blank:]]*0x\([0-9A-Fa-f]\{1,5\}\)[[:blank:]]*$/\1/p' "$ROOT/res/buildCIA.rsf")
	if ! [[ "$uid" =~ ^[0-9A-Fa-f]{1,5}$ ]]; then
		echo "$TOOL: no UniqueId in $ROOT/res/buildCIA.rsf" >&2
		return 1
	fi
	uidTitleID "$uid"
}

# uidTitleID <unique-id, hex>: the application title id makerom stamps for it.
uidTitleID() { printf '%016x\n' $((0x0004000000000000 | 16#$1 << 8)); }

# ciaValidVersion <n>: a title version is 16 bits, so none follows 65535.
ciaValidVersion() { [[ "$1" =~ ^[0-9]{1,5}$ ]] && [ $((10#$1)) -le 65535 ]; }

ciaSetVersion() {
	ciaValidVersion "$2" || {
		echo "$TOOL: $2 is not a title version (0..65535)" >&2
		return 1
	}
	ciaLayout "$1" && filePatch "$1" $((ciaTmdBody + CIA_TMD_VERSION)) "$(printf %04x $((10#$2)))"
}

# 3dstool is needed only to build a RomFS image.
resolveCIATools() {
	local bin=$ROOT/tools/bin tool
	ciaBannerTool="${BANNERTOOL:-$(command -v bannertool || echo "$bin/bannertool")}"
	ciaMakeRom="${MAKEROM:-$(command -v makerom || echo "$bin/makerom")}"
	# shellcheck disable=SC2034 # makeFixture.sh's, for a RomFS
	ciaThreeDsTool="${THREEDSTOOL:-$(command -v 3dstool || echo "$bin/3dstool")}"
	for tool in "$ciaBannerTool" "$ciaMakeRom"; do
		[ -x "$tool" ] || {
			echo "$TOOL: not found or not executable: $tool (bannertool and makerom are not bundled: point \$BANNERTOOL and \$MAKEROM at them, or put them on your PATH or in tools/bin/; docs/building.md says more)" >&2
			return 1
		}
	done
}

# ciaIconBanner <dir> <short-title> <long-title> <publisher> <icon.png> <banner.png>: writes
# <dir>/icon.icn and <dir>/banner.bnr. Every CIA here plays 3Drop's own banner sound.
ciaIconBanner() {
	"$ciaBannerTool" makesmdh -s "$2" -l "$3" -p "$4" -i "$5" -o "$1/icon.icn" >/dev/null &&
		"$ciaBannerTool" makebanner -i "$6" -a "$ROOT/res/banner.wav" -o "$1/banner.bnr" >/dev/null
}

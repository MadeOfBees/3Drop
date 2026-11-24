# wire.sh: the tools' client for 3Drop's wire protocol, sourced by them, never run. Its twin on the
# console is src/protocol.h and the files it names: a change to one is a change to both. The sections
# build upward: primitives, transport, the exchange (its layers are mapped at its head), finding the
# console, streamed replies, mutations, trees, seeds.
# shellcheck shell=bash
if [ -z "${BASH_VERSION:-}" ]; then
	echo "wire.sh must be sourced from bash" >&2
	return 2
fi
set -o pipefail
# shellcheck source=tools/lib.sh
. "${BASH_SOURCE[0]%/*}/lib.sh"
# shellcheck source=tools/cia.sh
. "${BASH_SOURCE[0]%/*}/cia.sh"
# Empty when no private directory could be made; every exchange that needs one then fails. The first
# runDir call runs outside a $(...) so that the second, inside one, finds the folder already made
# (lib.sh: _privateDir).
runDir >/dev/null 2>&1 || :
WIRE_RUN_DIR=$(runDir 2>/dev/null) || WIRE_RUN_DIR=""

# Twinned with src/app.h DEFAULT_PORT. This is where 3Drop listens and answers a probe.
WIRE_DEFAULT_PORT=1337
: "${THREEDROP_PORT:=$WIRE_DEFAULT_PORT}"
# Twinned with the device (src/auth.c); a different value derives a key the console rejects.
PBKDF2_SALT=33447a6b3344ffee
PBKDF2_ITERS=10000
# Twinned with gzipWorthwhile() in src/sdXfer.c. Compressing files this small, or with these
# extensions, cannot pay off.
GZIP_MIN_BYTES=1024
GZIP_SKIP_EXTS='cia gz tgz zip 7z rar xz bz2 zst png jpg jpeg gif webp mp3 ogg opus m4a mp4 mkv 3dsx cxi cci 3ds app bcstm bcwav'
# Twinned with src/protocol.h REQUEST_LINE_MAX, which counts the NUL.
REQUEST_LINE_MAX=1151
# The trailer and its MAC are looked for within this many bytes of the end of a body (src/protocol.h
# TRAILER_MAX).
WIRE_TRAILER_WINDOW=512
# Twinned with src/protocol.h REPLY_MAX, which counts the NUL. This is the longest reply line.
WIRE_REPLY_MAX=1543
# Twinned with src/protocol.h PROTOCOL_EPOCH, which every signed reply names. A console on another
# epoch is sent only the requests that every epoch keeps (_wireCompatible).
WIRE_PROTO=1
# A listing's walk ends at 120 s (src/cmdFs.c LS_WALK_MS). Over the whole 120 s, this many bytes is
# 2.2 MB/s, which is more than this console's radio sends.
WIRE_LISTING_MAX=268435456
# How long a kept nonce is used. The device never hands out a nonce with less than half its life
# left (src/auth.c NONCE_TTL_MS / 2, 150 s), and what remains of those 150 s after the keep covers
# the request's trip to the console.
WIRE_NONCE_KEEP=120
# How far ahead of now a kept nonce's stamp may be while the nonce is still used as one of age 0.
# This covers a clock stepped back since the stamp was made (WSL's NTP and Hyper-V disagree, 1.4 s
# every 30 s). It comes out of that remaining time, so the keep, this value and a request's control
# timeout (WIRE_CONTROL) stay under 150 s (testWire.sh). A nonce stamped further ahead is read
# afresh.
WIRE_CLOCK_STEP=10
# Twinned with src/hash.h DIGEST_UNIT. A digest is the SHA-256 of the SHA-256s of its units.
WIRE_UNIT=1048576
# A file goes in pieces, each its own signed exchange, so a cut costs one piece and a rerun resumes.
# The bridge's smaller THREEDROP_PIECE keeps a piece within a GUI client's 15 s timeout.
WIRE_PIECE=33554432
if [ -n "${THREEDROP_PIECE:-}" ]; then
	# Checked before the arithmetic, which would evaluate a name.
	if ! isUint "$THREEDROP_PIECE" || [ "${#THREEDROP_PIECE}" -gt 8 ] || [ $((10#$THREEDROP_PIECE % WIRE_UNIT)) != 0 ] ||
		[ $((10#$THREEDROP_PIECE)) = 0 ] || [ $((10#$THREEDROP_PIECE)) -gt "$WIRE_PIECE" ]; then
		dieWith 2 "THREEDROP_PIECE=$THREEDROP_PIECE: a whole number of $WIRE_UNIT-byte units, $WIRE_PIECE bytes at most"
	fi
	WIRE_PIECE=$((10#$THREEDROP_PIECE))
fi
# Twinned with src/cmdFs.c FAT32_FILE_MAX. This is FAT32's largest file, which bounds a put.
WIRE_FILE_MAX=4294967295
# Twinned with src/path.h SD_PUT_TEMP. The console keeps a put's pieces at the path with this
# appended.
WIRE_PUT_TEMP=.3drop.tmp
# Twinned with src/path.h SD_PUT_BACKUP. The file a put replaces is kept at the path with this
# appended until the put publishes.
WIRE_PUT_BACKUP=.3drop.bak

# ---- Primitives ------------------------------------------------------------------------------

# requireHex <length> <value> <what>. The digits are spelled out because a range such as a-f also
# matches A-E in a UTF-8 locale.
requireHex() {
	if [ "${#2}" -ne "$1" ] || [[ "$2" == *[!0123456789abcdef]* ]]; then
		echo "$TOOL: invalid $3" >&2
		return 1
	fi
	echo "$2"
}

# lowerHex <text>: prints the text with A-F as a-f, the device's one spelling of hex. It starts no
# process; bash 3.2 has no ${x,,} to do it.
lowerHex() {
	local s=$1
	s=${s//A/a} s=${s//B/b} s=${s//C/c} s=${s//D/d} s=${s//E/e} s=${s//F/f}
	printf '%s\n' "$s"
}

# caseless <text> <glob>: matches the way the device's strcasecmp compares. Both options are on only
# here, because requireHex must stay exact.
caseless() {
	local LC_ALL=C st=1
	shopt -s nocasematch extglob
	# shellcheck disable=SC2053 # the glob is the pattern
	[[ $1 == $2 ]] && st=0
	shopt -u nocasematch extglob
	return "$st"
}

# _wirePattern <path>: prints the path's last step as a caseless glob when it holds `*` or `?`,
# which no FAT name does.
_wirePattern() {
	local LC_ALL=C step=${1##*/} tail=$'\x80-\xbf' glob="" i
	[[ "$1" == */* && "$step" == *[*?]* ]] || return 1
	_wireNFC "$step"
	step=$wireNFC
	for ((i = 0; i < ${#step}; i++)); do
		case "${step:i:1}" in
		\*) glob+='*' ;;
		\?) glob+="[!$tail]*([$tail])" ;; # matches a lead byte, then its continuation bytes
		*) glob+="\\${step:i:1}" ;;
		esac
	done
	echo "$glob"
}

# Dashes in the key are cosmetic, as on the device. So is a CR, which is where the device ends the
# line (src/auth.c trimLine). A key that a Windows editor ended with CRLF is the same key.
deriveDK() {
	local out key=${THREEDROP_KEY%%$'\r'*}
	out=$(printf '%s\n' "${key//-/}" |
		openssl enc -aes-256-cbc -md sha512 -pbkdf2 -iter "$PBKDF2_ITERS" -pass stdin \
			-S "$PBKDF2_SALT" -P 2>/dev/null | sed -n 's/^key=//p') || return
	requireHex 64 "$(lowerHex "$out")" "derived key"
}

# Exported, so the tools a process runs never redo the stretch. The device uses the 64 hex
# characters, as text, for its key. The pads stay printf escapes because the inner one can hold a
# NUL.
wireKey() {
	local dk i byte tag
	# A key that a parent already stretched returns before the cache lookup below, on purpose. That
	# process makes the folder where it needs it, and one that never reads the cache would pay a
	# mkdir and a chmod for nothing (tests/testDrop.sh counts both).
	[ "${WIRE_KEY_FOR:-}" != "$THREEDROP_KEY" ] || return 0
	dk=$(deriveDK) || return
	WIRE_IPAD="" WIRE_OPAD=""
	for ((i = 0; i < 128; i++)); do
		byte=0
		[ "$i" -ge "${#dk}" ] || printf -v byte '%d' "'${dk:i:1}"
		printf -v WIRE_IPAD '%s\\x%02x' "$WIRE_IPAD" $((byte ^ 0x36))
		printf -v WIRE_OPAD '%s\\x%02x' "$WIRE_OPAD" $((byte ^ 0x5c))
	done
	WIRE_KEY_FOR=$THREEDROP_KEY
	# The book is named by a MAC no exchange makes. It tells two keys' books apart and says nothing
	# about either key. The same tag (WIRE_TAG) binds what a key began to that key: a get's part, a
	# drop's install note. cacheDir is called outside a $(...) first, so every later $(cacheDir) has
	# nothing left to make.
	tag=$(macOf 3drop/remembered) && cacheDir >/dev/null 2>&1 && WIRE_BOOK=$(cacheDir)/addresses-${tag:0:16} || WIRE_BOOK=""
	WIRE_TAG=${tag:0:16}
	export WIRE_IPAD WIRE_OPAD WIRE_KEY_FOR WIRE_BOOK WIRE_TAG
}

# requireKey: takes THREEDROP_KEY from the environment, else from the gitignored $ROOT/.env; exits 2
# if neither has it.
requireKey() {
	if [ -z "${THREEDROP_KEY:-}" ] && [ -f "$ROOT/.env" ]; then
		# The file is run without its CRs, which a Windows editor's CRLF puts at the end of every
		# value (the port's too), and without the UTF-8 byte-order mark an editor may put first.
		# Bash reads it whole, so this starts no more processes than sourcing the file would. The
		# read stops at a NUL, which no text holds except UTF-16 (Windows PowerShell 5.1's > writes
		# it), so none of that file runs.
		local text line n=0
		if IFS= read -r -d '' text <"$ROOT/.env"; then
			dieWith 2 "$ROOT/.env is UTF-16, as Windows PowerShell 5.1's > writes it: save it as UTF-8"
		fi
		text=${text//$'\r'/}
		text=${text#$'\xef\xbb\xbf'}
		# Each line is checked whole before any line runs, because the shell's "command not found"
		# would print a key that ran as a command. A line is blank, a comment, or NAME=value
		# followed by at most a comment, set off by a blank (KEY=<key>#;x is one word, then a
		# command). The value is one word (no blank, quote, backslash, backtick, parenthesis, brace,
		# bracket or ;&|<>) or one quoted string (in "", no backslash, backtick, parenthesis, brace
		# or bracket either; in '', any character), so the line is an assignment, never a command.
		# $NAME expands; ${..} and $[..] do not, because their errors print the value they name.
		local word='[^[:space:]'\''"\`;&|<>[(){]*' sq="'[^']*'" dq='"[^"\`[(){]*"'
		while IFS= read -r line; do
			n=$((n + 1))
			[[ $line =~ ^[[:blank:]]*((export[[:blank:]]+)?[A-Za-z_][A-Za-z0-9_]*=($word|$sq|$dq)([[:blank:]]+#.*)?|#.*)?[[:blank:]]*$ ]] ||
				dieWith 2 "$ROOT/.env line $n is not NAME=value: the key goes in as THREEDROP_KEY=<key>"
		done <<<"$text"
		set -a
		eval "$text"
		set +a
	fi
	[ -n "${THREEDROP_KEY:-}" ] || {
		echo "$TOOL: set THREEDROP_KEY to the 3Drop key (shown on the 3DS screen and in sdmc:/3ds/3drop/key.txt), or put the line THREEDROP_KEY=<key> in $ROOT/.env" >&2
		exit 2
	}
	wireKey || die "could not derive the key: openssl needs enc -pbkdf2 -iter (1.1.1 or later, or LibreSSL)"
}

# wireConsole <address> [port]: loads the key, then sets the console's address in wireIP and
# wirePort. wireAddress runs after .env, which may set THREEDROP_PORT. A bad address ends in usage,
# naming what it was given.
wireConsole() {
	requireKey
	wireAddress "$@" || usage 2 "not an address: $1${2:+ $2}"
}
# wireConsoleAt [address]: loads the key, then sets wireIP and wirePort to the address as typed (a
# bad one ends in usage). With no address, it uses the first address where this key's console
# answers (wireFind); if none does, it says why, then points to the console's screen.
wireConsoleAt() {
	requireKey
	if [ -n "${1:-}" ]; then
		wireAddress "$1" || usage 2 "not an address: $1"
	else
		wireFind || dieWith "$?" "$WIRE_SCREEN"
	fi
}

# This is two plain SHA-512 passes on stdin, so the key never reaches an argv; `openssl dgst -hmac
# <key>` would show it to every local user in `ps`.
macOf() {
	local out
	wireKey || return # a no-op once requireKey ran; otherwise this call pays for the stretch
	out=$({
		printf '%b' "$WIRE_OPAD"
		{
			printf '%b' "$WIRE_IPAD"
			printf '%s' "$1"
		} | openssl dgst -sha512 -binary
	} | openssl dgst -sha512) || return
	requireHex 128 "${out##* }" HMAC
}

# The domains are separate, so one MAC can never stand in for another. A response's MAC covers its
# request.
macRequest() { _macDomain request "$1"; }
macResponse() { _macDomain response "$1" "$2"; }
macTrailer() { _macDomain trailer "$1" "$2" "$3"; }
# _macDomain <domain> <fields...>: MACs the text `3drop/<domain>` followed by the fields, a line
# each. The text is built with printf -v because a command substitution would cost a process.
_macDomain() {
	local domain=$1 text
	shift # so "$@" can replace "${@:2}", which bash 3.2 joins into one word under any other IFS
	printf -v text '%s\n' "3drop/$domain" "$@"
	# Drop the trailing newlines, as a command substitution would.
	while [[ "$text" == *$'\n' ]]; do text=${text%$'\n'}; done
	macOf "$text"
}

newRequestID() { requireHex 32 "$(openssl rand -hex 16)" "request ID"; }

# _wireNow: sets wireNow to the clock's epoch seconds without starting a process. bash 3.2 has no
# EPOCHSECONDS, so there it counts SECONDS from the epoch second this shell began (WIRE_EPOCH,
# _wireEpoch).
_wireNow() { wireNow=${EPOCHSECONDS:-$((WIRE_EPOCH + SECONDS))}; }
# _wireEpoch: runs date once per process, not once per exchange. The value is taken at a point where
# SECONDS held still across the date call, so it is exact.
_wireEpoch() {
	local at
	for _ in 1 2 3; do
		at=$SECONDS WIRE_EPOCH=$(($(date +%s) - at))
		[ "$SECONDS" != "$at" ] || return 0
	done
}
[ -n "${EPOCHSECONDS:-}" ] || _wireEpoch

# The digest is openssl dgst's last word in LibreSSL and OpenSSL alike.
sha256File() {
	local out
	out=$(openssl dgst -sha256 <"$1") || return
	requireHex 64 "${out##* }" SHA-256
}

# _wireWords <command> <file...>: sets wireWords to one word per file, taken from the command run
# over as many files at a time as fit on a command line. A process per file is expensive on Git
# Bash, and Windows caps a command line at 32,767 characters (Git Bash may lengthen a path). Returns
# 0 when each file has its word.
_wireWords() {
	local LC_ALL=C command=$1 n=$(($# - 1)) room=0 out
	local -a batch=()
	shift
	wireWords=()
	while [ "$#" != 0 ]; do
		batch+=("$1") room=$((room + ${#1} + 64))
		shift
		[ "$#" = 0 ] || [ $((room + ${#1})) -ge 30000 ] || continue
		out=$("$command" "${batch[@]}") || return
		# shellcheck disable=SC2206 # a word a file, of hex digits or decimal
		wireWords+=($out)
		batch=() room=0
	done
	[ "${#wireWords[@]}" = "$n" ]
}
_wireHashWords() {
	openssl dgst -sha256 -binary "$@" 2>/dev/null | od -An -v -tx1 |
		awk '{ for (i = 1; i <= NF; i++) if (length(h = h $i) == 64) { print h; h = "" } }'
}
# wc ends with a total line when given two or more files, and no file has a word for it.
_wireSizeWords() { wc -c "$@" 2>/dev/null | awk -v n=$# 'NR <= n { print $1 }'; }
# _wireHashes <file...>: sets wireHashes to the files' SHA-256s; _wireSizes <file...>: sets
# wireSizes to their sizes in bytes.
_wireHashes() { _wireWords _wireHashWords "$@" && wireHashes=("${wireWords[@]}"); }
_wireSizes() { _wireWords _wireSizeWords "$@" && wireSizes=("${wireWords[@]}"); }

# _wireWork <function> [args]: runs the function in a subshell, so its traps remove the function's
# work directory however it ends, including a deadline's TERM.
_wireWork() (
	work=$(mktemp -d "${SCRATCH:-${TMPDIR:-/tmp}}/3drop-work.XXXXXX") || exit
	trap 'rm -rf "$work"' EXIT
	# bash 3.2 runs a subshell's EXIT trap on a Ctrl-C only through an INT trap. TERM needs none,
	# and one would wait for the running command to finish.
	trap 'exit 130' INT
	"$1" "$work" "${@:2}"
)

# unitDigest <file> [bytes]: prints the wire's digest of the file's first `bytes` bytes (all of them
# by default). The digest is `u:` followed by the SHA-256 of the SHA-256s of its units (WIRE_UNIT).
unitDigest() { _unitDigestOne "$@" || { [ "$?" = 2 ] && _wireWork _unitDigest "$@"; }; }
_unitDigest() { _unitDigestOne "${@:2}" || { [ "$?" = 2 ] && _unitHashes "$@" && _unitsDigest "$1/hashes"; }; }
# _unitDigestOne <file> [bytes]: digests a file of one unit or none (nearly every listing and tree
# file, a CIA's prefix) with no work directory or split. Returns 2, saying nothing, for any other.
_unitDigestOne() {
	local size out
	# The file is opened twice, by wc and by openssl, and a pipe or a /dev name for a descriptor may
	# not give its bytes again.
	[ -f "$1" ] && [[ "$1" != /dev/* ]] || return 2
	size=$(fileSize "$1" 2>/dev/null) || return 2
	if [ -n "${2:-}" ]; then
		isUint "$2" && [ "$2" -le "$size" ] || return 2 # the full path says why
		size=$2
	fi
	[ "$size" -le "$WIRE_UNIT" ] || return 2
	if [ "$size" = 0 ]; then
		out=$(openssl dgst -sha256 </dev/null)
	elif [ -n "${2:-}" ]; then
		out=$(head -c "$size" -- "$1" | openssl dgst -sha256 -binary | openssl dgst -sha256)
	else
		out=$(openssl dgst -sha256 -binary <"$1" | openssl dgst -sha256)
	fi || return 1
	_unitTag "$out"
}
# _unitDigestSha <sha256> <bytes>: the wire's digest of a file of one unit or none, worked out from
# its SHA-256 so the file is read once. It is the SHA-256 of that digest's bytes. An empty file has
# no unit, and its digest is the SHA-256 itself.
_unitDigestSha() {
	local out=$1
	[ "$2" = 0 ] || out=$(hexDecode "$1" | openssl dgst -sha256) || return
	_unitTag "$out"
}

# freeKiB <dir>: prints the KiB free there, read from the column beside the capacity column because
# Git Bash names a filesystem with its spaces ("C:/Program Files/Git").
freeKiB() {
	local kib
	kib=$(df -Pk "$1" 2>/dev/null | awk 'NR == 2 { for (i = 2; i <= NF; i++) if ($i ~ /%$/) print $(i - 1) }') &&
		isUint "$kib" && echo "$kib"
}

# _unitHashes <work> <file> [bytes]: writes <work>/hashes with one split and one openssl for all the
# units, because a process pair per unit is expensive on Git Bash.
_unitHashes() {
	local LC_ALL=C work=$1 size bytes units avail
	local -a parts
	size=$(fileSize "$2") || return
	bytes=${3:-$size}
	if ! isUint "$bytes" || [ "$bytes" -gt "$size" ]; then
		echo "$TOOL: $2 has no $bytes bytes to digest" >&2
		return 1
	fi
	units=$(((bytes + WIRE_UNIT - 1) / WIRE_UNIT))
	if ! avail=$(freeKiB "$work") || [ $((avail * 1024)) -le "$bytes" ]; then
		echo "$TOOL: no room under ${work%/*} to split $2 into units" >&2
		return 1
	fi
	if [ "$units" -gt 0 ]; then
		if [ "$bytes" = "$size" ]; then
			split -a 4 -b "$WIRE_UNIT" -- "$2" "$work/u"
		else
			head -c "$bytes" -- "$2" | split -a 4 -b "$WIRE_UNIT" - "$work/u"
		fi || return
		parts=("$work"/u*)
		if [ ! -e "${parts[0]}" ] || [ "${#parts[@]}" != "$units" ]; then
			echo "$TOOL: split cut $2 into ${#parts[@]} units, not $units" >&2
			return 1
		fi
	fi
	# Names, not paths, because Windows caps a command line at 32,767 characters.
	{ [ "$units" = 0 ] || (cd "$work" && openssl dgst -sha256 -binary u*); } >"$work/hashes" || return
	rm -f "$work"/u*
}

# _unitsDigest <hashes> [units]: prints u: and the SHA-256 of the first `units` unit digests (all of
# them by default). That is the wire's digest of the file's first `units` units.
_unitsDigest() {
	local out
	out=$({ if [ -n "${2:-}" ]; then dd if="$1" bs=32 count="$2" 2>/dev/null; else cat "$1"; fi; } |
		openssl dgst -sha256) || return
	_unitTag "$out"
}

# _unitTag <openssl dgst output>: prints u: and its digest, the wire's one spelling of a unit
# digest.
_unitTag() { requireHex 64 "${1##* }" "unit digest" >/dev/null && echo "u:${1##* }"; }
# requireDigest <value> <what>: the value must be u: followed by 64 lowercase hex digits.
requireDigest() {
	if [[ "$1" != u:* ]] || ! requireHex 64 "${1#u:}" "$2" >/dev/null 2>&1; then
		echo "$TOOL: invalid $2" >&2
		return 1
	fi
	echo "$1"
}

# Opt-in, because the 3DS pays for gzip on the core that receives.
compressionWorthwhile() {
	[ "${THREEDROP_COMPRESSION:-none}" = auto ] || return 1
	! caseless "$1" "*.@(${GZIP_SKIP_EXTS// /|})" || return 1
	[ "$(fileSize "$1")" -ge "$GZIP_MIN_BYTES" ] && ! _wireNoGzip
}

# _wireNoGzip: returns 0 when no gzip is installed, and says so once per run. Each exchange is a
# subshell, so the scratch directory holds the record that it was said.
_wireNoGzip() {
	! command -v gzip >/dev/null || return 1
	if [ -z "${SCRATCH:-}" ] || _wireMake "$SCRATCH/no-gzip" 2>/dev/null; then
		echo "$TOOL: no gzip here, so THREEDROP_COMPRESSION=auto compresses nothing this run" >&2
	fi
}

wireHex() {
	local LC_ALL=C hex="" i byte
	for ((i = 0; i < ${#1}; i++)); do
		printf -v byte '%d' "'${1:i:1}"
		printf -v hex '%s%02x' "$hex" $((byte & 255))
	done
	echo "$hex"
}

# _wireNFC <text>: sets wireNFC to the text in composed form (NFC) on macOS, which hands out
# decomposed (NFD) names that FAT would keep as a second spelling. It uses macOS's own iconv, and
# keeps the text as given where iconv refuses.
_wireNFC() {
	local LC_ALL=C composed
	wireNFC=$1
	if [[ "$OSTYPE" == darwin* && "$1" == *[![:print:]]* ]] &&
		composed=$(printf '%s' "$1" | /usr/bin/iconv -f UTF-8-MAC -t UTF-8 2>/dev/null && printf x); then
		wireNFC=${composed%x}
	fi
}

# wirePath <sd-path|save:<id>/<path>|extdata:<id>/<path>|title:[id]>: request fields are printable
# ASCII without spaces, so any other path travels as hex, and an id is sent in the device's one
# spelling.
wirePath() {
	local LC_ALL=C scheme="" path=$1
	if [[ "$path" =~ ^title:[0-9A-Fa-f]*$ ]]; then
		lowerHex "$path"
		return
	fi
	if [[ "$path" =~ ^(save:[0-9A-Fa-f]{16}|extdata:[0-9A-Fa-f]{8})(/.*)$ ]]; then
		scheme=$(lowerHex "${BASH_REMATCH[1]}") path=${BASH_REMATCH[2]}
	fi
	_wireNFC "$path"
	path=$wireNFC
	if [[ "$path" == hex:* || "$path" == *[^!-~]* ]]; then
		echo "${scheme}hex:$(wireHex "$path")"
	else
		echo "$scheme$path"
	fi
}

# A title ID is the caller's argument, so a bad one is a usage error (status 2).
wireTitleID() {
	requireHex 16 "$(lowerHex "$1")" "title ID (16 hex digits)" || return 2
}

# wireApplication <title-id>: prints the game a patch or DLC belongs to, which a launch starts and
# NS starts with its patch. Any other title is its own.
wireApplication() {
	case "$1" in
	0004000e*) echo "00040000${1:8}" ;;
	0004008c*) echo "00040000${1:8:6}00" ;; # a DLC's low byte is its index, not a variation
	*) echo "$1" ;;
	esac
}

# verifyReply <request> <"response\nmac">
verifyReply() {
	local response mac
	if [[ "$2" != *$'\n'* ]]; then
		echo "$TOOL: reply was not two lines${wireTryOf:-}" >&2
		return 1
	fi
	response=${2%%$'\n'*}
	mac=${2#*$'\n'}
	if [[ "$mac" == *$'\n'* ]] || ! requireHex 128 "$mac" "response MAC" >/dev/null 2>&1; then
		echo "$TOOL: response MAC was not canonical${wireTryOf:-}" >&2
		return 1
	fi
	if [ "$mac" != "$(macResponse "$1" "$response")" ]; then
		echo "$TOOL: response authentication failed${wireTryOf:-}" >&2
		return 1
	fi
	echo "$response"
}

# wireNonceless <reply>: prints the reply as the tools print it, without its nonce and protocol
# epoch.
wireNonceless() {
	local _r _id nonce proto
	read -r _r _id nonce proto _ <<<"$1"
	[[ "$proto" == proto=* ]] || proto=""
	echo "${1/ $nonce ${proto:+$proto }/ }"
}

# The one reply parser, for a reply as the tools print it or a trailer.
wireFields() {
	# shellcheck disable=SC2034 # the fields are the caller's
	read -r _ wireId wireVerb wireOutcome wireDetail wireResult wireRest <<<"$1"
}

# kv <text> <key>: prints a key=value field of a reply, or `-` when it is missing or empty. Fields
# are found by name because a reply only ever gains keys.
kv() {
	local v=" $1 "
	case "$v" in
	*" $2="*)
		v=${v#*" $2="}
		v=${v%% *}
		echo "${v:--}"
		;;
	*) echo - ;;
	esac
}

# _wireWhy <verb> <outcome> <detail> [result] [fields]: prints the sentence that explains a refusal,
# or nothing if there is none.
_wireWhy() {
	if [ "$3:${4:-}" = system:d963f818 ]; then # src/result.h ON_BATTERY
		echo "the console is off its charger: a /boot.firm goes only on AC, since a power cut between its two renames leaves none"
		return
	fi
	case "$2:$3" in
	*:unmanaged) echo "3Drop never touches this title: a system title, DSiWare, a demo, a Download Play child, variation 02, or a patch or DLC of one" ;;
	*:self) echo "that title is 3Drop's own" ;;
	*:not-application) echo "a patch or DLC: this takes an application only" ;;
	*:route) echo "only 3Drop's own title goes as --self, and it goes no other way" ;;
	*:stranded) echo "a transfer is still stuck in the system, and nothing that needs it starts until it returns (stats self: state=stranded)" ;;
	*:busy) echo "the console is busy with a QR scan or install: finish or cancel it at the console, then send this again" ;;
	rejected:secure-value) echo "the save's secure value would not go, so nothing was committed" ;;
	*:hbldr-title) echo "Luma's 3dsx loader title is not an installed application 3Drop may launch" ;;
	*:hbldr) echo "Luma's 3dsx loader is off, missing, or would not take the launch, so nothing ran: turn it on in Luma's configuration" ;;
	*:stdio) echo "the console could not tell this PC's address for the 3dsx's stdio, so nothing ran: run it without stdio" ;;
	*:exists-pending) echo "an unfinished import of it is held: uninstalling the title clears it" ;;
	*:exists)
		case "$1" in
		put) echo "it is there already: the title is installed (drop.sh with no intent flag sends an update), or a folder has that path" ;;
		seed) echo "the console holds a seed for this title already, and never replaces one" ;;
		*) echo "something there has that name already" ;;
		esac
		;;
	*:not-newer)
		if [[ " ${5:-} " == *" self "* ]]; then
			echo "3Drop runs this build or a newer one already, and takes only a newer one: tools/deploy.sh numbers its build past the installed one, and release vN installs over every build deployed on the release before it; FBI installs an older release"
		else
			echo "the console holds this title at the same version or newer, and takes an update only above it: raise the CIA's version (tools/deploy.sh does; drop.sh with no intent flag bumps an application's)"
		fi
		;;
	*:metadata) echo "that is not a CIA 3Drop can read: its header, ticket or TMD did not parse" ;;
	*:am-info) echo "AM would not say what is installed of this title, so nothing was done" ;;
	*:layeredfs) # a Result code with it means the card would not answer
		case "$1:${4:-00000000}" in
		run:00000000) echo "not launched: the title has a romfs folder, which crashes the console in Luma's loader unless the title is a game built with Nintendo's SDK; for such a game, tools/device.sh run <ip> --game <title-id>; for your own app, delete the folder: tools/device.sh delete <ip> /luma/titles/<title-id>/romfs r" ;;
		run:*) echo "not launched: the card could not be checked for a romfs folder; check the card and try again" ;;
		*:00000000) echo "not written: an exheader.bin that is not 1024 or 2048 bytes crashes the console in Luma's loader" ;;
		*) echo "not written: the card could not be checked for an exheader.bin; check the card and try again" ;;
		esac
		;;
	rejected:notfound) [ "$1" != put ] || echo "that title, or its save data, is not on the console: an update, or a save or extdata put, needs it installed (3Drop itself: install its CIA with FBI first)" ;;
	*:path) echo "the console cannot use that path: malformed, too long (a put needs room for .3drop.tmp after it), the card's root, or not one this takes (an SD path; for a run, a .3dsx)" ;;
	*:protected) echo "the key files are never read, replaced or moved" ;;
	*:isdir) echo "that is a folder, not a file: a folder comes with r (get <path> <local-dir> r)" ;;
	*:reserved) echo "a name ending .3drop.tmp or .3drop.bak is a put's own temp or backup: nothing is put, made or moved there" ;;
	*:gzip)
		if [ "${4:-}" = d963f811 ]; then # src/result.h OUT_OF_MEMORY
			echo "the console had no memory for gzip: send it again, or with THREEDROP_COMPRESSION unset"
		else
			echo "the gzip body was damaged, or did not inflate to exactly the size it declared, so nothing was written"
		fi
		;;
	rejected:digest) echo "the bytes that came are not the ones this PC hashed: the file changed as it went, or was damaged on the way; send it again" ;;
	rejected:short) echo "the body ended before its size: this PC stopped sending it (a tool stopped, a file that shrank); send it again" ;;
	rejected:receive) echo "the body stopped arriving: the link failed partway; send it again" ;;
	rejected:timeout) echo "the body went quiet past the console's limit: a stalled or sleeping PC, or a slow link; send it again" ;;
	rejected:send) echo "the console could not send its reply: the link failed partway; ask again" ;;
	rejected:space) echo "the card has no room for the file" ;;
	rejected:open) echo "the console could not make the file there: a card locked or failing, or a folder that cannot hold it" ;;
	rejected:write)
		if [ "${4:-}" = c8804471 ]; then # FS: a file under a folder that is not there
			echo "a folder on the file's path is missing, and the console could not make it"
		else
			echo "the card refused a write: full, locked or failing"
		fi
		;;
	rejected:publish) echo "the file came whole, but the card would not give it its name: send it again" ;;
	rejected:ring) echo "the console had no memory for the transfer: send it again" ;;
	rejected:writer) echo "the console had no memory to start the install: send it again" ;;
	rejected:read) echo "the card failed a read partway: it may be failing, or was taken out" ;;
	rejected:deadline) echo "the console's walk of the folder ran out of time: list a smaller part of it" ;;
	rejected:depth) echo "a folder too many levels down was named but not entered: list it by its own path" ;;
	rejected:name) echo "a name the console cannot send, or a path past its length limit, was left out" ;;
	rejected:worker-stall) echo "the console's card or AM call answered nothing for 600 s: look at the console before sending it again" ;;
	rejected:size) [ "$1" != get ] || echo "the file ends before the byte the get starts from" ;;
	*:system) echo "that is what the console boots from: --system takes only a put or a delete of one file directly in /luma/payloads or /luma/sysmodules, and a put of a /boot.firm that passes the console's FIRM checks (some of boot9strap's, not all: docs/safety.md)" ;;
	*:refused) echo "3Drop never takes that path in this form" ;;
	*:challenge) echo "the console's nonce was spent or expired before this request came, twice: nothing was done, so send it again" ;;
	unknown:ticket)
		case "$1" in
		put) echo "nothing was installed, but its ticket may remain, and HOME will not show it: the console's screen names the title ID and FBI's way to delete it" ;;
		*) echo "the title is gone, but AM could not say its ticket is: it may still be held" ;;
		esac
		;;
	unknown:*) echo "whether it took effect is unknown: look before sending it again" ;;
	rejected:grammar) echo "the console's 3Drop does not take a word of this request: 3Drop and these tools may come from different releases; its top screen names its release: check out that tag, or update 3Drop" ;;
	rejected:unsupported) echo "the console's 3Drop does not know this request: a typo, or 3Drop and these tools may come from different releases; update 3Drop (in 3Drop, press Y and scan the update QR), or check out the tag its top screen names" ;;
	esac
}

# wireReport <reply>: prints the verdict on a verified reply. An ok reply goes to stdout and returns
# 0. Any other reply goes to stderr with its sentence (_wireWhy) and returns 4 if the transport cut
# the body (the same request may be sent again, as a piece's is), else 1.
wireReport() {
	case "$1" in "response "* | "trailer "*) wireFields "$1" ;; *) wireFields "- - $1" ;; esac
	case "$wireOutcome $wireDetail" in "committed ok" | "ok ok")
		echo "$1"
		return 0
		;;
	esac
	echo "$TOOL: $1" >&2
	_wireWhy "$wireVerb" "$wireOutcome" "$wireDetail" "$wireResult" "$wireRest" | sed "s/^/$TOOL: /" >&2
	case "$wireOutcome:$wireDetail" in rejected:short | rejected:receive | rejected:timeout | rejected:send) return 4 ;; esac
	return 1
}

# wireExplainError <reply> <ip:port>: for the console's unsigned refusal, prints its sentence and
# returns 1.
wireExplainError() {
	local at=$2
	case "${1:-}" in
	"ERR malformed")
		echo "$TOOL: the console rejected the request as malformed (not an authentication failure)" >&2
		return 1
		;;
	"ERR auth") # if the address was typed, the key may be right and the address another console's
		echo "$TOOL: the 3Drop at $at holds another key: THREEDROP_KEY is not the key on its screen, or $at is not your console" >&2
		return 1
		;;
	esac
}

# ---- Transport -------------------------------------------------------------------------------

# The control timeout, in seconds (twinned with src/net.h HOST_REPLY_WAIT_MS). It outlasts the
# console's pre-auth deadline, so a close there reads as no reply and never as our own timeout.
WIRE_CONTROL=15
wireTimeout() { echo "${THREEDROP_TIMEOUT:-${1:-$WIRE_CONTROL}}"; }

# An exchange may go quiet for as long as the console waits before it answers, plus WIRE_MARGIN. The
# waits are src/net.h TRANSFER_WIRE_STALL_MS for a body's quiet peer and src/ring.c WORKER_STALL_MS
# for a worker.
WIRE_MARGIN=60
WIRE_SILENCE=$((180 + WIRE_MARGIN))
WIRE_WORKER_SILENCE=$((600 + WIRE_MARGIN))
# A hash answers once it has read and digested the whole file. This value is twice this console's
# time, to allow for a slower card.
WIRE_HASH_MS_PER_MB=230
# The seconds before a failed exchange is sent again; the tests use a short pause of their own
# (tests/lib.sh).
WIRE_RETRY_PAUSE=${_THREEDROP_RETRY_PAUSE:-1}

# _wireQuiet <request>: sets wireQuiet to how long the request's exchange may go quiet. A get, or a
# put anywhere but into a save or extdata, waits on a worker, and a ranged put waits on the read
# back of what the temp file keeps.
_wireQuiet() {
	local -a f
	wireQuiet=$WIRE_SILENCE
	case "$1" in
	"put "*" "*" save:"* | "put "*" "*" extdata:"*) ;;
	"put "*" range "*)
		read -r -a f <<<"$1"
		isUint "${f[7]:-}" || f[7]=$WIRE_FILE_MAX
		wireQuiet=$((WIRE_WORKER_SILENCE + f[7] * WIRE_HASH_MS_PER_MB / 1000000000))
		;;
	"get "* | "put "*) wireQuiet=$WIRE_WORKER_SILENCE ;;
	esac
}

# _wireMono: sets wireMono to monotonic whole seconds, and returns 1 when there is no monotonic
# clock. Linux and Git Bash expose uptime without a process; elsewhere Perl supplies the monotonic
# clock. Never mix this origin with wall time.
_wireMono() {
	local up
	if read -r up _ 2>/dev/null </proc/uptime; then
		wireMono=${up%%.*}
	else
		wireMono=$(
			perl -MTime::HiRes \
				-e 'print int(Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC()))'
		) || return 1
	fi
}

WIRE_SEGMENT=8388608
# _wireEmit <request> <mac> [body|-] [sent-file]: returns 42 for a ghost (AM stalled; the close
# makes the console cancel), detected by two slow segments in a row because a pause in this tool
# slows only the segment it falls in. Without a monotonic clock the check is dropped, never the
# send.
_wireEmit() {
	local floor="${THREEDROP_GHOST_FLOOR:-0}" grace="${THREEDROP_GHOST_GRACE:-45}"
	local max="${THREEDROP_GHOST_MAX_ELAPSED:-150}" size off=0 t0 el seg slow=0 wireMono
	printf '%s\n%s\n' "$1" "$2"
	[ -n "${3:-}" ] || return 0
	if [ "$3" = - ] || { [ "$floor" = 0 ] && [ -z "${4:-}" ]; }; then
		_wireWriter cat -- "$3"
		return
	fi
	size=$(fileSize "$3") || return 1
	if [ "$floor" != 0 ] && _wireMono; then
		t0=$wireMono seg=$wireMono
	else
		floor=0
	fi
	while [ "$off" -lt "$size" ]; do
		_wireWriter dd if="$3" bs="$WIRE_UNIT" skip=$((off / WIRE_UNIT)) count=$((WIRE_SEGMENT / WIRE_UNIT)) 2>/dev/null || return 1
		off=$((off + WIRE_SEGMENT))
		[ "$off" -le "$size" ] || off=$size
		[ -z "${4:-}" ] || echo "$off" >"$4"
		[ "$floor" != 0 ] || continue
		# Never after the last segment, because the whole body went and the console will answer it.
		[ "$off" -lt "$size" ] || continue
		_wireMono || {
			floor=0
			continue
		}
		el=$((wireMono - t0))
		if [ $((WIRE_SEGMENT / (wireMono > seg ? wireMono - seg : 1))) -lt "$floor" ]; then
			slow=$((slow + 1))
		else
			slow=0
		fi
		seg=$wireMono
		if [ "$el" -ge "$grace" ] && [ "$el" -le "$max" ] && [ "$slow" -ge 2 ] &&
			[ $((off / (el > 0 ? el : 1))) -lt "$floor" ]; then
			return 42
		fi
	done
}

# _wireWriter <command...>: runs the command in the background so a timeout's trap can stop it. Its
# pid is in WIRE_WRITER while it runs, and WIRE_WRITER is cleared once the process is reaped, when
# the pid may belong to another process.
_wireWriter() {
	local st=0
	"$@" <&0 &
	WIRE_WRITER=$!
	wait "$WIRE_WRITER" || st=$?
	WIRE_WRITER=""
	return "$st"
}

# Returns 3 while another run's bench rep holds the console (tools/bench/lib.sh repLock), because an
# exchange served between its put and its reads could replace what they report. An ownerless lock
# gets a minute (its holder may be between mkdir and writing its owner file). A stale lock is moved
# aside first, so two callers cannot both remove it. A lock made before this boot is stale whatever
# process has its pid now, because on macOS the run directory outlives a restart.
_wireRepFree() {
	local lock=$WIRE_RUN_DIR/rep-$1 token="" pid="" made="" boot
	[ -n "$WIRE_RUN_DIR" ] && [ -d "$lock" ] || return 0
	read -r token pid made 2>/dev/null <"$lock/owner" || :
	[ -z "$token" ] || [ "$token" != "${THREEDROP_REP_OWNER:-}" ] || return 0
	if { [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; } ||
		{ isUint "$made" && boot=$(_wireBoot) && [ "$made" -lt "$boot" ]; } ||
		{ [ -z "$pid" ] && [ -n "$(find "$lock" -maxdepth 0 -mmin +1 2>/dev/null)" ]; }; then
		mv "$lock" "$lock.stale.$$" 2>/dev/null && rm -rf "$lock.stale.$$"
		return 0
	fi
	echo "$TOOL: a benchmark run on this PC holds the console at $1 (bench.sh pid ${pid:-?} holds $lock); not sending" >&2
	return 3
}
# _wireBoot: prints the epoch second this host booted, from Darwin's kern.boottime or Linux's
# /proc/stat btime. Where neither exists it fails quietly.
_wireBoot() {
	local b
	b=$(sysctl -n kern.boottime 2>/dev/null) && b=${b#*sec = } && b=${b%%,*} && isUint "$b" ||
		b=$(awk '$1 == "btime" { print $2 }' /proc/stat 2>/dev/null) && isUint "$b" || return 1
	echo "$b"
}

# _wireCap <request>: sets wireCap to the most bytes its reader takes: the reply's line and MAC, the
# body its verb allows and the trailer's window. The reader stops there, so a console that keeps
# sending, or a host posing as one, cannot fill the disk.
_wireCap() {
	local -a f
	read -r -a f <<<"$1"
	case "${f[0]}:${f[3]:-}:${f[2]:-}" in
	get:range:*) wireCap=${f[5]} ;;
	# The most deflate can add past its input.
	get:gzip:*) wireCap=$((WIRE_PIECE + WIRE_PIECE / 1000 + 1024)) ;;
	ls:* | bench:*:mb | bench:*:hw) wireCap=$WIRE_LISTING_MAX ;;
	screenshot:*) wireCap=1048576 ;; # a BMP of the widest screen: 800 x 240 x 3 + 54 bytes
	bench:*:tx) wireCap=$((f[3] * WIRE_UNIT)) ;;
	*) wireCap=0 ;;
	esac
	isUint "$wireCap" || wireCap=0
	wireCap=$((WIRE_REPLY_MAX + 2 + 128 + wireCap + WIRE_TRAILER_WINDOW))
}

# _wireTcp <ip> <port> <timeout> <request> <mac> <body> <reply-file>: reads while it sends, so an
# early refusal is kept. It never reads with `read`, which drops NUL bytes. A watchdog stops a
# console that has gone silent (124). The silence is timed in seconds from the start, and only bytes
# count as movement, not a connect.
_wireTcp() {
	local sent=$7.sent why=$7.why said job watchdog st=0 reply=$7 bound=$3 last="0 0" seen=$SECONDS quiet=0 limit
	# The sent file exists only after a KILL, and running rm costs a process.
	if [ -e "$sent" ] || [ -L "$sent" ]; then rm -f "$sent"; fi
	# Empty, to match the watchdog's initial `last`, so no earlier exchange's reply is left to read.
	: >"$reply"
	(
		LC_ALL=C # so the connect's error comes in the words wireReach reads, whatever the user's language
		exec 2>"$why" 9<>"/dev/tcp/$1/$2" || exit 1
		exec 2>/dev/null
		echo 0 >"$sent"
		# dd needs fullblock here because otherwise each short read counts as a block and cuts a
		# reply short.
		if [ -n "$wireCountBytes" ]; then
			dd bs=4096 iflag=count_bytes,fullblock count="$wireCap" status=none <&9 >"$7" &
		else
			head -c "$wireCap" <&9 >"$7" &
		fi
		reader=$!
		# Use SIGALRM, so an outside SIGTERM is not read as a timeout.
		trap 'kill "$reader" ${WIRE_WRITER:+"$WIRE_WRITER"} 2>/dev/null; exit 124' ALRM
		_wireEmit "$4" "$5" "$6" "$sent" >&9 || st=$?
		if [ "$st" = 42 ]; then
			kill "$reader" # the socket closes, and the console cancels
			exit 42
		fi
		wait "$reader"
		exit "$st"
	) <&0 2>/dev/null &
	job=$!
	limit=$(wireTimeout)
	watchOwner "$job" _wireTcpTick kill -ALRM "$job"
	watchdog=$!
	wait "$job" 2>/dev/null || st=$? # an alarm before the job's trap is set is reported below, not by bash
	# Never send a TERM, which in the instant after the watchdog's fork runs the caller's EXIT trap
	# in the watchdog (bash 5).
	kill -KILL "$watchdog" 2>/dev/null
	wait "$watchdog" 2>/dev/null || :
	if [ "$st" = 124 ]; then
		echo "$TOOL: timed out: $1:$2 went quiet for $3 s${wireTryOf:-}" >&2
	elif [ "$st" = 142 ]; then # the watchdog's alarm arrived before the trap was set
		echo "$TOOL: timed out: no connection to $1:$2 in $(wireTimeout) s${wireTryOf:-}" >&2
		st=124
	fi
	wireReach=none
	if [ -e "$sent" ]; then
		wireReach=connected
	elif read -r said 2>/dev/null <"$why" && [[ "$said" == *"Connection refused"* ]]; then
		wireReach=refused
	fi
	rm -f "$sent" "$why"
	return "$st"
}
# _wireTcpTick: the watchdog's check, run about once a second. It takes no arguments (watchOwner
# calls it bare), so it works on _wireTcp's locals. It reads `reply` (the bytes coming in) and
# `sent` (the bytes gone out; the file is there once connected). It keeps `last`, those two numbers
# as of when they last moved; `quiet`, the seconds since; and `seen`, the SECONDS of the tick
# before. It returns false once `quiet` passes `limit` (the control timeout until the connect, then
# `bound`). Its whole seconds, counted from a window opened mid-second, can pass in a little over
# N-1 s, so a limit of N s ends after N to N+1 s, never before. SECONDS follows the wall clock,
# which an NTP or VM time sync steps, so a tick adds the SECONDS it saw pass, but never less than 0
# or more than 2 (a tick, a sleep 1 and a wc take under 2 s even on Git Bash). A step costs one
# interval's credit, 2 s at most, plus the polling, which can land the expiry two polls late. A tick
# truly delayed past 2 s is under-credited the same way.
_wireTcpTick() {
	local now got=0 passed=$((SECONDS - seen))
	seen=$((seen + passed))
	now=$(exec wc -c <"$reply" 2>/dev/null) || now=0
	read -r got 2>/dev/null <"$sent" && limit=$bound
	now="${now//[[:space:]]/} $got"
	[ "$now" = "$last" ] || last=$now quiet=0 passed=0 # bytes moved, so the quiet time starts again
	quiet=$((quiet + (passed < 0 ? 0 : passed > 2 ? 2 : passed)))
	[ "$quiet" -le "$limit" ]
}

# _wireNcKind: sets wireNcKind for an nc whose -w does not bound both the connect and the silence
# after it. `apple`: only -G bounds its connect, so nc is given -G. `ncat`: -w bounds only the
# connect, so /dev/tcp is used instead.
_wireNcKind() {
	local nc
	wireNcKind=""
	case $OSTYPE in msys* | cygwin*) return 0 ;; esac # Git Bash has no nc, and a process there is expensive
	nc=$(command -v nc 2>/dev/null) || return 0
	if [[ $OSTYPE == darwin* && $nc == /usr/bin/nc ]]; then
		wireNcKind=apple
	elif [ "$nc" -ef "${nc%/*}/ncat" ]; then
		wireNcKind=ncat
	fi
}
_wireNcKind

# _wireCountBytes: sets wireCountBytes where dd can count bytes (Git Bash), so the reply is read by
# dd there. Git Bash's head holds 65536 bytes back from the watchdog's file. It is asked once per
# process because a process is expensive there.
_wireCountBytes() {
	wireCountBytes=""
	case $OSTYPE in msys* | cygwin*) dd iflag=count_bytes,fullblock count=0 </dev/null >/dev/null 2>&1 && wireCountBytes=1 ;; esac
	return 0 # sourced under set -e, and a dd that cannot count bytes is an answer, not a failure
}
_wireCountBytes

# _wireReplyFile: makes a file for a reply in the tool's scratch, which the tool's trap removes, on
# an interrupt or a deadline too.
_wireReplyFile() { mktemp "${SCRATCH:-${TMPDIR:-/tmp}}/3drop-reply.XXXXXX"; }

# _wireTransport <ip> <port> <timeout> <request> <mac> [body] [reply-file]: returns 0 if a reply
# came, 1 if nothing was sent (the request was too long, or there is no scratch), 3 if nothing was
# sent (a rep holds the console), 4 if there was no reply, 42 for a ghost and 124 for quiet. A
# stream comes over /dev/tcp because macOS nc's receive loop capped every get.
_wireTransport() {
	local ip=$1 port=$2 body=${6:-} out=${7:-} st=0 fifo pid reader guard t0
	local -a ncWait=(-w "$3")
	_wireRepFree "$ip" || return
	if [ "${#4}" -gt "$REQUEST_LINE_MAX" ]; then
		echo "$TOOL: the request is longer than the device reads ($REQUEST_LINE_MAX bytes)" >&2
		return 1
	fi
	[ -n "$out" ] || out=$(_wireReplyFile) || return 1
	_wireCap "$4"
	t0=$SECONDS
	# A connect gets the control timeout, as _wireTcp's watchdog gives it.
	[ "$wireNcKind" != apple ] || ncWait=(-G "${THREEDROP_TIMEOUT:-$WIRE_CONTROL}" -w "$3")
	if [ -n "${7:-}" ] || [ "$wireNcKind" = ncat ] || ! command -v nc >/dev/null 2>&1; then
		_wireTcp "$ip" "$port" "$3" "$4" "$5" "$body" "$out" || st=$?
	else
		if [ -n "$body" ] && [ "${THREEDROP_GHOST_FLOOR:-0}" != 0 ]; then
			# Run nc in the background on a fifo, so a ghost can kill it and close the socket.
			fifo=$(mktemp -u "${SCRATCH:-${WIRE_RUN_DIR:?}}/fifo.XXXXXX") && mkfifo "$fifo" "$fifo.reply" || return 1
			nc "${ncWait[@]}" "$ip" "$port" <"$fifo" >"$fifo.reply" 2>/dev/null &
			pid=$!
			head -c "$wireCap" <"$fifo.reply" >"$out" &
			reader=$!
			watchOwner "$pid" : kill -KILL "$pid" "$reader"
			guard=$!
			# Run in a subshell, so a SIGPIPE from an early signed refusal ends the subshell and not
			# the caller.
			(_wireEmit "$4" "$5" "$body") >"$fifo" 2>/dev/null || st=$?
			[ "$st" != 42 ] || kill "$pid" 2>/dev/null
			wait "$pid" "$reader" 2>/dev/null || :
			kill -KILL "$guard" 2>/dev/null # use KILL, never TERM; see _wireTcp's watchdog
			wait "$guard" 2>/dev/null || :
			rm -f "$fifo" "$fifo.reply"
		else
			(_wireEmit "$4" "$5" "$body") 2>/dev/null | nc "${ncWait[@]}" "$ip" "$port" 2>/dev/null | head -c "$wireCap" >"$out"
			st=${PIPESTATUS[0]}
		fi
		# If nc's -w ran out, report it the way _wireTcp's watchdog does.
		if [ "$st" != 42 ] && [ ! -s "$out" ] && [ $((SECONDS - t0)) -ge "$3" ]; then
			echo "$TOOL: timed out: $ip:$port went quiet for $3 s${wireTryOf:-}" >&2
			st=124
		fi
	fi
	case "$st" in
	42 | 124) ;;
	*)
		if [ -s "$out" ]; then
			st=0 # a send cut short by an early refusal is not a failure, since its reply came
		elif [ -n "$body" ] && [ "$st" != 0 ]; then
			echo "$TOOL: the send to $ip:$port failed: no reply, and the body did not all go" >&2
			st=4
		else
			echo "$TOOL: no reply from $ip:$port${wireTryOf:-}" >&2
			st=4
		fi
		;;
	esac
	[ -n "${7:-}" ] || {
		cat "$out"
		rm -f "$out"
	}
	return "$st"
}

# ---- The exchange ----------------------------------------------------------------------------
# One signed request goes down these layers, each calling the next:
#   wireMutation           a change (delete, mkdir, rename, run, seed; a put and a screenshot enter
#                          at _wireWithNonce, each with its own try). It is never sent twice,
#                          because it may have run.
#   _wireWithNonce         puts the kept nonce in the request. The console refuses a spent nonce
#                          before running anything, so only that refusal is sent again, once.
#   _wireMutateRecovering  one try. A lost reply is read back from the console's receipt
#                          (_wireReceipt).
#   wireRead               a read (stats, hash, bench), alongside the three above. It uses no nonce
#                          and is asked again, three tries in all, while no reply comes.
#   wireExchange           one exchange: sends the request, verifies the signed reply (_wireVerify)
#                          and keeps the nonce it brings.
#   _wireTransport         one connection, by nc or bash's /dev/tcp (_wireTcp), with its silence
#                          bounded.
# A streamed reply (ls, get, screenshot) goes through _wireStreamOpen instead of wireExchange; the
# rest is the same.

# _wireNonceFile <ip> <port>: prints the kept nonce's file, which is per user and per device, and
# sets wireNonceFile to it. An exchange that may have spent the nonce, or ended unverified, drops
# it.
_wireNonceFile() {
	wireNonceFile=$WIRE_RUN_DIR/nonce-$1-$2
	if [ -z "$WIRE_RUN_DIR" ]; then
		echo "$TOOL: no private directory for the kept nonce (XDG_RUNTIME_DIR, XDG_CACHE_HOME, HOME)" >&2
		return 1
	fi
	printf '%s' "$wireNonceFile"
}
# _wireNonceDrop <ip> <port>: removes the kept nonce without a subshell, and runs rm only where
# there is a nonce file.
_wireNonceDrop() {
	_wireNonceFile "$1" "$2" >/dev/null 2>&1 || return 0
	if [ -e "$wireNonceFile" ] || [ -L "$wireNonceFile" ]; then rm -f "$wireNonceFile"; fi
}

# _wireVerify <ip> <port> <request> <"response\nmac"> [sent]: prints the verified reply as the tools
# print it (wireNonceless). It returns 1 for an unsigned refusal, and for a console on another
# protocol epoch when the request is not one every epoch keeps (_wireCompatible). It returns 4 for a
# reply that does not verify. It keeps the reply's nonce, stamped as of `sent` (a stream's header is
# sent long before its body ends), together with the reply's epoch, which is how the next run's
# mutation is checked.
_wireVerify() {
	local response _r _id nonce proto file st=0 at verb intent
	_wireNonceFile "$1" "$2" >/dev/null || return
	file=$wireNonceFile
	wireExplainError "$4" "$1:$2" || st=1
	[ "$st" != 0 ] || response=$(verifyReply "$3" "$4") || st=4
	if [ "$st" != 0 ]; then
		_wireNonceDrop "$1" "$2"
		return "$st"
	fi
	_wireNow
	at=${5:-$wireNow}
	_wireRemember "$1" "$2" "$at" || : # a book it cannot write never fails the exchange
	read -r _r _id nonce proto _ <<<"$response"
	# `-` means a 3Drop from before the epoch, which no host may take for this one.
	if [[ "$proto" =~ ^proto=([1-9][0-9]{0,3})$ ]]; then proto=${BASH_REMATCH[1]}; else proto=-; fi
	if requireHex 64 "$nonce" nonce >/dev/null 2>&1; then
		printf '%s %s %s\n' "$at" "$nonce" "$proto" >"$file"
	else
		_wireNonceDrop "$1" "$2" # the nonce is "-" when the console could not mint one
	fi
	read -r verb _ _ intent _ <<<"$3"
	_wireCompatible "$1" "$2" "$proto" "$verb" "$intent" || return 1
	wireNonceless "$response"
}

# _wireKept <epoch> <verb> [intent]: whether a console of that protocol epoch is sent the request. A
# console on these tools' epoch is sent any request; every epoch keeps `stats` and a self put, which
# updates 3Drop either way (a developer deploys an older build on purpose).
_wireKept() { [ "$1" = "$WIRE_PROTO" ] || [ "$2" = stats ] || [ "$2 ${3:-}" = "put self" ]; }

# _wireCompatible <ip> <port> <epoch> <verb> [intent]: returns 0 when the request is kept
# (_wireKept), else 1 and a message saying which side to update. The release the console names
# (`rel=`) is shown, never run, and is named in a checkout command only when it is a release tag.
_wireCompatible() {
	local rel said side fix
	! _wireKept "$3" "$4" "${5:-}" || return 0
	rel=$(kv "$(wireRead "$1" "$2" stats op 2>/dev/null)" rel) || :
	[[ "$rel" =~ ^[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}$ ]] || rel=""
	said="protocol $3"
	if [ "$3" = - ] || [ "$3" -lt "$WIRE_PROTO" ]; then
		[ "$3" != - ] || said="a protocol from before epochs"
		side=older fix="update 3Drop (in 3Drop, press Y and scan the update QR, or send a newer release's 3drop.cia with tools/drop.sh --self)"
	else
		side=newer fix="update these tools"
		if [[ "$rel" =~ ^v([1-9][0-9]?)$ ]] && [ "${BASH_REMATCH[1]}" -le 63 ]; then
			fix+=": git fetch --tags && git checkout $rel"
		else
			fix+=" to the release on the console's top screen"
		fi
	fi
	echo "$TOOL: 3Drop${rel:+ $rel} speaks $said, $side than these tools' $WIRE_PROTO, so only stats and a self-update work with it: $fix" >&2
	return 1
}

# wireExchange <ip> <port> <request> [body] [timeout]: prints the verified reply. A lost reply drops
# the kept nonce, which the request may have spent.
wireExchange() {
	local reply st=0
	reply=$(_wireTransport "$1" "$2" "${5:-$(wireTimeout)}" "$3" "$(macRequest "$3")" "${4:-}") || st=$?
	case "$st" in
	0) _wireVerify "$1" "$2" "$3" "$reply" ;;
	3) return 3 ;;
	*)
		_wireNonceDrop "$1" "$2"
		return "$st"
		;;
	esac
}

# wireRead <ip> <port> <verb> [fields] [timeout]: reports the reply (wireReport). If no reply comes,
# or none that verifies, the request is asked again, up to twice; an explained refusal or a held
# console is never asked again. The transport and verifyReply end each try's failure line with
# wireTryOf, ` (try 2 of 3)`; one line after the third try says what to check.
wireRead() {
	local id response attempt st wireTryOf
	for attempt in 1 2 3; do
		wireTryOf=" (try $attempt of 3)"
		id=$(newRequestID) || return
		st=0
		response=$(wireExchange "$1" "$2" "$3 $id${4:+ $4}" "" "${5:-}") || st=$?
		case "$st" in
		0)
			wireReport "$response"
			return
			;;
		1 | 3) return "$st" ;;
		esac
		[ "$attempt" = 3 ] || sleep "$WIRE_RETRY_PAUSE"
	done
	echo "$TOOL: no good answer from $1:$2 in 3 tries: check the address on 3Drop's top screen, that 3Drop is open, and that this PC is on the console's network (tools/device.sh discover looks for it)" >&2
	return "$st"
}

# _wireNonce <ip> <port> [verb intent]: prints the kept nonce while it is young, else a fresh one
# from a `stats op` read. Returns 1 before anything is sent when the console that minted it speaks
# another protocol epoch and the verb is not one every epoch keeps (_wireCompatible). A kept nonce
# whose epoch refuses the verb is asked for afresh first, because 3Drop may have relaunched as
# another build since. The nonce file is checked before any arithmetic, which would evaluate a name
# (`x[$(cmd)]` runs cmd). A stamp up to WIRE_CLOCK_STEP s ahead (a clock stepped back since) counts
# as new, and one further ahead counts as old. A nonce with no epoch (an older tool's) is read
# afresh.
_wireNonce() {
	local file stamp nonce proto age
	_wireNonceFile "$1" "$2" >/dev/null || return
	file=$wireNonceFile
	if ! { [ -O "$file" ] && read -r stamp nonce proto <"$file" && [[ $stamp =~ ^[1-9][0-9]*$ ]] &&
		requireHex 64 "$nonce" nonce && [[ $proto =~ ^(-|[1-9][0-9]*)$ ]] && _wireNow &&
		age=$((wireNow - stamp)) && [ "$age" -ge "-$WIRE_CLOCK_STEP" ] && [ "$age" -lt "$WIRE_NONCE_KEEP" ]; } >/dev/null 2>&1 ||
		! _wireKept "$proto" "${3:-}" "${4:-}"; then
		wireRead "$1" "$2" stats op >/dev/null || return
		if ! { read -r stamp nonce proto <"$file" && requireHex 64 "$nonce" nonce; } >/dev/null 2>&1; then
			echo "$TOOL: the console offered no nonce" >&2
			return 4
		fi
	fi
	_wireCompatible "$1" "$2" "$proto" "${3:-}" "${4:-}" || return
	[ "$proto" = "$WIRE_PROTO" ] ||
		echo "$TOOL: 3Drop speaks protocol $proto, these tools $WIRE_PROTO: a self-update goes either way, so check that this build is the one you meant" >&2
	echo "$nonce"
}

# _wireReceipt <ip> <port> <request-id> [timeout]: prints the request's reply, read back from its
# receipt (_wireReceiptOf). Returns 2 for another request's receipt.
_wireReceipt() {
	local receipt
	receipt=$(wireRead "$1" "$2" stats op "${4:-}") || return
	_wireReceiptOf "$receipt" "$3"
}
# _wireReceiptOf <stats op reply> <request-id>: prints the request's reply, rebuilt from the receipt
# the console keeps of its last change (`op <id> <verb> <outcome> ...`). Returns 2 for another
# request's receipt; the console serves one request at a time, so it is done with this one.
_wireReceiptOf() {
	[[ "$1" == *" op $2 "* ]] || return 2
	echo "response $2 ${1#*" op $2 "}"
}

# _wireWithNonce <ip> <port> <verb> <fields> <exchange...>: runs the exchange with the kept nonce in
# its request. The console refuses a spent nonce before it runs anything, so only that refusal is
# sent again, once, with the live nonce the refusal brings.
_wireWithNonce() {
	local ip=$1 port=$2 verb=$3 fields=$4 id nonce response attempt st
	shift 4
	for attempt in 1 2; do
		id=$(newRequestID) && nonce=$(_wireNonce "$ip" "$port" "$verb" "${fields%% *}") || return
		st=0
		response=$("$@" "$ip" "$port" "$verb $id $nonce${fields:+ $fields}") || st=$?
		wireFields "$response"
		[[ "$attempt" = 1 && "$wireVerb $wireOutcome $wireDetail" = "$verb rejected challenge" ]] || break
	done
	[ -z "$response" ] || echo "$response"
	return "$st"
}

# wireMutation <ip> <port> <timeout> <verb> [fields]: never resent. A lost reply is read back from
# the request's receipt.
wireMutation() {
	local response
	response=$(_wireWithNonce "$1" "$2" "$4" "${*:5}" _wireMutateRecovering "$3") || return
	wireReport "$response"
}
# _wireMutateRecovering <timeout> <ip> <port> <request>: one try. When its reply is lost, the
# console's receipt says what became of it. A request queued behind one that held 3Drop past a
# host's wait is refused without running (src/protocol.c).
_wireMutateRecovering() {
	local id st=0 rst=0 wait
	read -r _ id _ <<<"$4"
	wireExchange "$2" "$3" "$4" "" "$1" || st=$?
	case "$st" in 4 | 124) ;; *) return "$st" ;; esac
	_wireReceipt "$2" "$3" "$id" "$1" || rst=$?
	[ "$rst" != 0 ] || return 0
	wait=$(THREEDROP_TIMEOUT='' wireTimeout) # uses the device's rule, whatever this host waits
	if [ "$rst" = 2 ]; then
		echo "$TOOL: no reply came, and 3Drop's receipt is another request's: this one did not run (lost on its way, or queued behind a request that held 3Drop past $wait s, which refuses it unrun), unless it ran and another change has run since" >&2
	else
		echo "$TOOL: no reply came, and no read of its receipt was answered: 3Drop is busy or gone; one queued behind a request that holds 3Drop past $wait s is refused unrun, and if 3Drop went away, tools/device.sh stats $2:$3 op says what became of it once it is back" >&2
	fi
	return "$st"
}

# ---- Finding the console ---------------------------------------------------------------------

# Every failure to find the console ends with this, because the address on its top screen works from
# every host and network.
WIRE_SCREEN="use the address on the console's top screen (ip:$WIRE_DEFAULT_PORT) once, as in tools/device.sh stats 192.168.1.20:$WIRE_DEFAULT_PORT, and the tools remember it"
# Two consoles can share a key, and a console moves between networks.
WIRE_BOOK_MAX=8
# How long a proof waits to connect and for its reply, since an idle console answers `stats` at
# once.
WIRE_PROVE_WAIT=3
# Twinned with src/discover.h PROBE_MAGIC and PROBE_LEN. This is the probe that 3Drop answers on UDP
# 1337, padded with NULs.
WIRE_PROBE=3drop-probe/1
WIRE_PROBE_LEN=40
# How long a search probes unless told otherwise. It tries once every 150 ms because the console
# misses most broadcasts.
WIRE_PROBE_SECONDS=2
# The most probe answers that are proved. A LAN host forging many answers would hold up a search for
# 3 s each.
WIRE_PROBE_ANSWERS=8

# _wireHost <text>: succeeds for a host name or IPv4 address; it never accepts an option nc would
# take (`-x`) or a space that would split a remembered line.
_wireHost() {
	local LC_ALL=C
	[[ $1 =~ ^[0-9A-Za-z][0-9A-Za-z._-]*$ ]]
}

# wireAddress <address> [port]: sets wireIP and wirePort from an address typed or pasted as the
# screen shows it (ip:port). Returns 2 for an address no exchange could use, or for two ports that
# differ.
wireAddress() {
	local LC_ALL=C
	wireIP=$1 wirePort=${2:-$THREEDROP_PORT}
	if [[ "$1" == *:* ]]; then
		wireIP=${1%:*}
		[ -z "${2:-}" ] || [ "$2" = "${1##*:}" ] || return 2
		wirePort=${1##*:}
	fi
	isPort "$wirePort" && _wireHost "$wireIP" || return 2
}

# _wireBookRead: fills wireBook, most recent first. Each line is checked before its numbers meet
# arithmetic, which would evaluate a name.
_wireBookRead() {
	local LC_ALL=C ip port at rest
	wireBook=()
	[ -n "${WIRE_BOOK:-}" ] && [ -f "$WIRE_BOOK" ] && [ -r "$WIRE_BOOK" ] || return 0
	while read -r ip port at rest; do
		if [ -z "$rest" ] && _wireHost "$ip" && isPort "$port" && [[ $at =~ ^[1-9][0-9]{0,11}$ ]]; then
			wireBook+=("$ip $port $at")
		fi
	done <"$WIRE_BOOK"
}

# _wireRemember <ip> <port> <epoch>: puts the address first in this key's book, after a reply that
# only this key's console could sign. The book is left as it is while the address is first there and
# under a minute old.
_wireRemember() {
	[ -n "${WIRE_BOOK:-}" ] && _wireHost "$1" || return 0
	_wireBookRead
	[[ "${wireBook[0]:-}" != "$1 $2 "* ]] || [ $(($3 - ${wireBook[0]##* })) -ge 60 ] || return 0
	_wireBookWrite "$1" "$2" "$1 $2 $3"
}
# _wireForget <ip> <port>: removes the address from this key's book, because something else answers
# there now.
_wireForget() { [ -n "${WIRE_BOOK:-}" ] && _wireBookRead && _wireBookWrite "$1" "$2"; }
# _wireBookWrite <ip> <port> [line]: replaces the whole book through its own .new file, so a cut
# leaves the old one and two tools running at once never publish each other's half.
_wireBookWrite() {
	local line new lines=("${@:3}")
	for line in ${wireBook[@]+"${wireBook[@]}"}; do
		[[ "$line" == "$1 $2 "* ]] || lines+=("$line")
	done
	if [ "${#lines[@]}" = 0 ]; then
		rm -f "$WIRE_BOOK" 2>/dev/null
		return
	fi
	new=$(mktemp "$WIRE_BOOK.new.XXXXXX" 2>/dev/null) || return
	{ printf '%s\n' "${lines[@]:0:WIRE_BOOK_MAX}" >"$new" && mv -f "$new" "$WIRE_BOOK"; } 2>/dev/null || {
		rm -f "$new"
		return 1
	}
}

# _wireRead2 <file> <var> <var>: reads the file's first two lines, whole, into the named variables.
# It uses read, not head and wc, which cost Git Bash a process each.
_wireRead2() {
	IFS= read -r "$2"
	IFS= read -r "$3"
} <"$1"

# wireProve <ip> <port>: sends one signed `stats op`, which never waits on AM or the card, over
# /dev/tcp, which shows how far it got. What answered goes in wireProof (each word is explained by
# _wireProofSays) and a verified answer goes in wireProofReply.
wireProve() {
	local wait=${THREEDROP_TIMEOUT:-$WIRE_PROVE_WAIT} request reply st=0 header="" mac=""
	wireProof="" wireReach="" wireProofReply=""
	request="stats $(newRequestID) op" && reply=$(_wireReplyFile) ||
		return 0 # the console was not asked, as the empty wireProof says
	THREEDROP_TIMEOUT=$wait _wireTransport "$1" "$2" "$wait" "$request" "$(macRequest "$request")" "" "$reply" 2>/dev/null || st=$?
	_wireRead2 "$reply" header mac || :
	rm -f "$reply"
	case "$st:$wireReach:$header" in
	"0:connected:ERR auth") wireProof=other-key ;;
	"0:connected:ERR "*) wireProof=unreadable ;;
	"0:connected:response "*)
		wireProof=unsigned
		! _wireVerify "$1" "$2" "$request" "$header"$'\n'"$mac" >/dev/null 2>&1 || wireProof=ok wireProofReply=$header
		;;
	0:*) wireProof=other ;;
	3:*) wireProof=held ;;
	124:connected:*) wireProof=busy ;;
	*:connected:*) wireProof=closed ;;
	*:refused:*) wireProof=refused ;;
	*) wireProof=quiet ;;
	esac
}

# _wireProofSays <ip:port> <proof> [remembered]: says why this key's console did not answer there,
# and never blames the key for another console's answer. A remembered address may be one the console
# left.
_wireProofSays() {
	local moved=${3:+, or your console moved}
	case "$2" in
	quiet) echo "nothing answers at $1: the console is off or asleep, on another network, or on a Wi-Fi that keeps its devices apart (a guest network)$moved" ;;
	busy) echo "$1 took the connection but did not answer in ${THREEDROP_TIMEOUT:-$WIRE_PROVE_WAIT} s: the console is busy with another PC or a transfer" ;;
	closed) echo "$1 took the connection and closed it without answering: 3Drop may be relaunching, so try again" ;;
	refused) echo "$1 refuses connections: 3Drop is not running there$moved" ;;
	other-key) echo "$1 is a 3Drop holding another key: not your console, or its key was regenerated (put the new one in THREEDROP_KEY)${3:+; this address is forgotten}" ;;
	unreadable) echo "$1 is a 3Drop that cannot read this request: update it, or these tools" ;;
	unsigned) echo "$1 answered as 3Drop, but its reply does not verify: a 3Drop from another release, or something posing as one" ;;
	other) echo "$1 answers as something that is not 3Drop${3:+ (your console moved, so this address is forgotten)}" ;;
	held) echo "a benchmark run on this PC holds $1" ;;
	*) echo "$1 was not asked: this PC could not make the request" ;;
	esac
}

# _wireName: sets wireName to 3drop-<tag>.local, where the tag comes from a MAC no exchange makes
# (src/auth.c authNameTag), so the name identifies this key's console and says nothing about the
# key.
_wireName() {
	local tag
	if [ -n "${THREEDROP_NAME+set}" ]; then
		wireName=$THREEDROP_NAME
		return
	fi
	tag=$(macOf 3drop/name) && wireName=3drop-${tag:0:8}.local
}

# _wireResolve <name>: prints each IPv4 address the OS's own resolver gives the name, a line each.
# That is mDNS wherever the OS looks .local names up. It is asked once per search because Windows
# takes ~2.9 s a lookup.
_wireResolve() {
	perl -MSocket -e 'my @host = gethostbyname($ARGV[0]) or exit 1;
		print inet_ntoa($_), "\n" for @host[4 .. $#host]' "$1"
}

# _wireProbe <port> <seconds>: prints `pid <pid>`, then each answer as it comes, then `end`. The
# <ip> is where the answer came from, never what it says. An empty line follows each try; once no
# reader is left, that write ends the probe.
_wireProbe() {
	perl -MIO::Socket::INET -MSocket -e '
		my ($tool, $to, $port, $seconds, $magic, $length, $most) = @ARGV;
		$| = 1;
		print "pid $$\n";
		my $s = IO::Socket::INET->new(Proto => "udp", Broadcast => 1) or die "$tool: could not probe: $!\n";
		my $at = sockaddr_in($port, inet_aton($to) // die "$tool: THREEDROP_PROBE=$to is no address\n");
		my ($end, %seen) = (time + $seconds + 1); # a second more: the clock counts whole seconds
		for (my $try = 0; $try < $seconds / 0.15 && time < $end && keys %seen < $most; $try++) {
			send($s, pack("a$length", $magic), 0, $at);
			my $in = "";
			vec($in, fileno($s), 1) = 1;
			while (select(my $ready = $in, undef, undef, 0.15) > 0 && time < $end) {
				my $from = $s->recv(my $answer, 512);
				if (!defined $from) { # an earlier try refused (ICMP): this one still waits its turn
					select(undef, undef, undef, 0.15);
					last;
				}
				my ($fromPort, $ip) = sockaddr_in($from);
				next unless $fromPort == $port && keys %seen < $most &&
					$answer =~ /\A3drop (?:\d{1,3}\.){3}\d{1,3} ([1-9]\d{0,4}) ([!-~]{1,32})\n?\z/ && $1 <= 65535;
				$ip = inet_ntoa($ip);
				print "$ip $1 $2\n" unless $seen{"$ip $1"}++;
			}
			print "\n";
		}
		print "end\n";' "$TOOL" "${THREEDROP_PROBE-255.255.255.255}" "$1" "$2" "$WIRE_PROBE" "$WIRE_PROBE_LEN" "$WIRE_PROBE_ANSWERS"
}

# _wireTry <first|all> <ip> <port> <model> [remembered]: proves the address, once per search. A
# remembered address that something else now answers at is forgotten.
_wireTry() {
	[[ "$wireTried" != *" $2:$3 "* ]] || return 1
	wireTried+="$2:$3 "
	wireProve "$2" "$3"
	[ "$wireProof" != held ] || wireHeld=1
	if [ "$wireProof" = ok ]; then
		wireIP=$2 wirePort=$3 wireFound=1
		[ "$1" != all ] || echo "$2 $3 $4"
		return 0
	fi
	wireNotes+=("$(_wireProofSays "$2:$3" "$wireProof" "${5:-}")")
	if [ -n "${5:-}" ]; then
		case "$wireProof" in other-key | other) _wireForget "$2" "$3" ;; esac
	fi
	return 1
}

# _wireSeek <first|all> <port> <seconds>: tries the cheapest source first (remembered, name, probe),
# and proves each address with a signed `stats` before taking it. `first` stops at the first address
# found, the probe with it.
_wireSeek() {
	local mode=$1 port=$2 seconds=$3 line ip atPort model answers heard="" ended="" probe=""
	wireNotes=() wireTried=" " wireHeld="" wireFound=""
	_wireBookRead
	[ "${#wireBook[@]}" != 0 ] || wireNotes=("no address has answered this key on this PC yet")
	for line in ${wireBook[@]+"${wireBook[@]}"}; do
		read -r ip atPort _ <<<"$line"
		_wireTry "$mode" "$ip" "$atPort" - remembered || continue
		[ "$mode" = all ] || return 0
	done
	if ! command -v perl >/dev/null 2>&1; then
		wireNotes+=("no perl, so neither the console's name nor a probe was tried")
		[ -n "$wireFound" ]
		return
	fi
	_wireName || wireName="" # if openssl failed, every proof will say so
	if [ -n "$wireName" ] && ! answers=$(_wireResolve "$wireName"); then
		wireNotes+=("$wireName, the console's name, does not resolve: the console is off, asleep or busy, or on another network, or this PC does not look .local names up")
	fi
	while read -r ip; do
		if [ -z "$ip" ] || ! _wireTry "$mode" "$ip" "$port" -; then continue; fi
		[ "$mode" = all ] || return 0
	done <<<"${answers:-}"
	if [ "$seconds" -gt 0 ] && [ -n "${THREEDROP_PROBE-255.255.255.255}" ]; then
		while read -r -u 3 ip atPort model; do
			case "$ip" in "") continue ;; pid) probe=$atPort && continue ;; end) ended=1 && continue ;; esac
			heard=1
			_wireTry "$mode" "$ip" "$atPort" "$model" 3<&- || continue
			[ "$mode" != all ] || continue
			# Send PIPE, since the probe's next write would end it anyway. Bash 3.2 announces a TERM
			# by printing the whole perl script.
			! isUint "$probe" || kill -PIPE "$probe" 2>/dev/null || :
			return 0
		done 3< <(_wireProbe "$port" "$seconds")
		if [ -n "$ended" ] && [ -z "$heard" ]; then # without an end line, perl has already said why
			wireNotes+=("no console answered a probe in $seconds s: it is off, asleep or busy, or on another network, or a firewall here drops the answer (Linux ufw or firewalld); with a VPN up, THREEDROP_PROBE=<this network's broadcast address> sends it on this network; under WSL 2's default network the probe stays in the VM, so give the console's address once")
		fi
	fi
	[ -n "$wireFound" ]
}

# _wireNotFound [line...]: reports a search that found nothing. It prints wireNotes, then the lines,
# on stderr as the tool's own messages, and returns 3 when a bench rep on this PC held an address,
# else 4.
_wireNotFound() {
	local note
	for note in ${wireNotes[@]+"${wireNotes[@]}"} "$@"; do echo "$TOOL: $note" >&2; done
	if [ -n "$wireHeld" ]; then return 3; fi
	return 4
}

# wireFind: sets wireIP and wirePort to the first address this key's console answers at (_wireSeek);
# if there is none, says why each address did not answer (_wireNotFound).
wireFind() { _wireSeek first "$THREEDROP_PORT" "$WIRE_PROBE_SECONDS" || _wireNotFound; }

# wireDiscover <port> [seconds]: prints each address this key's console answers at; if there is
# none, says why, then points to the console's screen.
wireDiscover() { _wireSeek all "$1" "${2:-$WIRE_PROBE_SECONDS}" || _wireNotFound "$WIRE_SCREEN"; }

# ---- Streamed replies (ls, get, screenshot) --------------------------------------------------

# _wireStreamOpen <ip> <port> <request> <work>: prints the verified header, whatever it says; the
# header is kept as sent, for the trailer, and what follows it goes to <work>/rest.
_wireStreamOpen() {
	local response sent header="" mac="" st=0
	_wireNow
	sent=$wireNow
	_wireQuiet "$3"
	_wireTransport "$1" "$2" "$wireQuiet" "$3" "$(macRequest "$3")" "" "$4/reply" || st=$?
	if [ "$st" != 0 ]; then
		# If the reply is lost, drop the nonce as wireExchange does, because a screenshot's request
		# may have spent it.
		_wireNonceDrop "$1" "$2"
		return "$st"
	fi
	_wireRead2 "$4/reply" header mac
	printf '%s\n' "$header" >"$4/header"
	response=$(_wireVerify "$1" "$2" "$3" "$header${mac:+$'\n'$mac}" "$sent") || return
	local LC_ALL=C # from here on, so the lengths below count the bytes head and wc counted
	fileFrom "$4/reply" $((${#header} + ${#mac} + 2)) >"$4/rest"
	echo "$response"
}

# _wireStream <ip> <port> <request> <work>: prints the verified header of a reply that a stream
# follows. Any other header is reported (wireReport).
_wireStream() {
	local response
	response=$(_wireStreamOpen "$@") || return
	wireFields "$response"
	if [ "$wireOutcome" != ok ]; then
		wireReport "$response"
		return
	fi
	echo "$response"
}

# The trailer is found by its request's fresh id, so no body byte can stand in for it; a body cut
# short has no trailer and fails closed.
_wireStreamClose() {
	local id rest end at t1 t2 header
	id=${2#* }
	id=${id%% *}
	rest=$(fileSize "$1/rest") || return
	end=$((rest < WIRE_TRAILER_WINDOW ? rest : WIRE_TRAILER_WINDOW))
	tail -c "$end" "$1/rest" >"$1/end"
	at=$(LC_ALL=C grep -abo "trailer $id " "$1/end" | tail -n 1)
	at=${at%%:*}
	if [ -z "$at" ]; then
		echo "$TOOL: the reply ended without its trailer" >&2
		return 4
	fi
	# BSD head refuses a count of 0, which an empty body would give.
	if [ $((rest - end + at)) = 0 ]; then
		: >"$1/body"
	else
		head -c $((rest - end + at)) "$1/rest" >"$1/body"
	fi
	fileFrom "$1/end" "$at" >"$1/trailer"
	t1="" t2="" header=""
	_wireRead2 "$1/trailer" t1 t2
	IFS= read -r header <"$1/header"
	if [ $((end - at)) != $((${#t1} + ${#t2} + 2)) ] ||
		! requireHex 128 "$t2" "trailer MAC" >/dev/null 2>&1 ||
		[ "$t2" != "$(macTrailer "$2" "$header" "$t1")" ]; then
		echo "$TOOL: the trailer did not authenticate" >&2
		return 4
	fi
	echo "$t1"
}

# _wireListing <work> <ip> <port> <request>: leaves the trailer's fields in wireOutcome and the variables
# after it. A partial listing is checked by its digest too; `-` is a digest the console could not take.
_wireListing() {
	local trailer count digest got
	_wireStream "$2" "$3" "$4" "$1" >/dev/null || return
	trailer=$(_wireStreamClose "$1" "$4") || return
	wireFields "$trailer"
	read -r count digest _ <<<"$wireRest"
	if [ "$digest" = - ]; then
		echo "$TOOL: the listing has no digest to check it by: $wireDetail $wireResult" >&2
		return 1
	fi
	requireDigest "$digest" "listing's digest" >/dev/null || return
	got=$(_unitDigest "$1" "$1/body") || return
	if [ "$count" != $(($(wc -l <"$1/body"))) ] || [ "$got" != "$digest" ]; then
		echo "$TOOL: the listing did not match its trailer" >&2
		return 4
	fi
}

# _wireLs says this about a listing that ended early, and wirePutTree finds it in what wireLs said. It is
# kept in one place so a rewording cannot make the two disagree.
WIRE_ENDED_EARLY="the listing ended early"
# wireLs <ip> <port> <sd-path|title:|title:<id>> [d|t|dt|r|rt]: prints lines `f <size> <mtime|-> <name>`,
# `d - <mtime|-> <name>` or `t <version> <size> <title-id>`. Read the fields by position.
wireLs() { _wireWork _wireLs "$@"; }
_wireLs() {
	local why
	_wireLsLines "$@" || return
	if [ "$wireOutcome" != ok ]; then
		why=$(_wireWhy ls "$wireOutcome" "$wireDetail" "$wireResult")
		echo "$TOOL: $WIRE_ENDED_EARLY ($wireDetail $wireResult); what came is above${why:+$'\n'$TOOL: $why}" >&2
		return 1
	fi
}
# wireLsMatch <ip> <port> <path> [flags]: if the last step of the path holds `*` or `?`, lists what
# matches it in its folder.
wireLsMatch() {
	local glob line
	if ! glob=$(_wirePattern "$3"); then
		wireLs "$@"
		return
	fi
	if [ -n "${4:-}" ] && [ "$4" != t ]; then
		echo "$TOOL: a pattern lists the names in one folder: no flag but t" >&2
		return 2
	fi
	wireLs "$1" "$2" "$(_wireTreeTop "${3%/*}")" ${4:+"$4"} | while IFS= read -r line; do
		! caseless "${line#* * * }" "$glob" || printf '%s\n' "$line"
	done
}
# _wireLsLines <work> <ip> <port> <path> [flags]: prints the listing's lines however its walk ended; how it
# ended is left in wireOutcome and the variables after it.
_wireLsLines() {
	local work=$1 ip=$2 port=$3 request
	request="ls $(newRequestID) $(wirePath "$4")${5:+ $5}" || return
	_wireListing "$work" "$ip" "$port" "$request" || return
	# One awk pass rather than a process per entry. Any control character (C0, DEL, or C1 as UTF-8 c2 80-9f)
	# becomes '?', so a crafted filename cannot drive the terminal.
	LC_ALL=C awk 'BEGIN { for (i = 0; i < 256; i++) byte[sprintf("%02x", i)] = i < 32 || i == 127 ? "?" : sprintf("%c", i) }
		$1 == "t" { print; next }
		{
			name = ""
			for (i = 1; i < length($4); i += 2) {
				if (substr($4, i, 2) == "c2" && substr($4, i + 2, 1) ~ /[89]/) {
					name = name "?"
					i += 2
				} else {
					name = name byte[substr($4, i, 2)]
				}
			}
			print $1, $2, $3, name
		}' "$work/body"
}

# wireTitleVersion <ip> <port> <title-id>: prints the installed version, `-` when AM gives none, or nothing
# when the title isn't installed. Any other answer goes to stderr with its status, never treated as "absent".
wireTitleVersion() {
	local listing st=0
	listing=$(wireLs "$1" "$2" "title:$3" 2>&1) || st=$?
	case "$st:$listing" in
	0:*) awk '{ print $2 }' <<<"$listing" ;;
	1:*" ls rejected notfound "*) ;;
	1:*" ls rejected am-info "*) echo - ;;
	*)
		echo "$listing" >&2
		return "$st"
		;;
	esac
}

# _wireCardSize <ip> <port> <sd-path>: prints the size of the card's file there (only a listing tells it
# before a transfer). It fails quietly if there is no file there, or no listing.
_wireCardSize() {
	local size
	size=$(wireLs "$1" "$2" "$3" d 2>/dev/null) && read -r _ size _ <<<"$size" && isUint "$size" && echo "$size"
}

# _wireLand <work> <request> <header> <file>: the file appears only if its length, trailer MAC and digest
# check out. A trailer saying the send timed out or failed is a cut (4); any other is a refusal.
_wireLand() {
	local work=$1 verb raw kind extra trailer digest wire got land=$1/body size
	read -r verb _ <<<"$2"
	wireFields "$3"
	read -r raw kind extra <<<"$wireRest"
	if [ -n "$extra" ] || ! isUint "$raw" || [[ "$kind" != raw && "$kind" != gzip ]]; then
		echo "$TOOL: $verb failed: $3" >&2
		return 1
	fi
	# At most one piece was asked for. A larger size would let a body inflate that far in the work folder.
	if [ "$kind" = gzip ] && { [ "${#raw}" -gt 10 ] || [ "$raw" -gt "$WIRE_PIECE" ]; }; then
		echo "$TOOL: $verb answered $raw bytes of gzip, past the one piece ($WIRE_PIECE) it was asked for, so none was inflated: if the file grew since its listing, get it again" >&2
		return 1
	fi
	trailer=$(_wireStreamClose "$work" "$2") || return
	wireFields "$trailer"
	read -r digest wire extra <<<"$wireRest"
	if [ "$wireDetail" != ok ]; then
		echo "$TOOL: $verb stopped after $wire of the body's bytes: $wireDetail $wireResult" >&2
		case "$wireDetail" in timeout | send) return 4 ;; *) return 1 ;; esac
	fi
	if [[ "$wireVerb $wireDetail $wireResult" != "$verb ok 00000000" || -n "$extra" ||
		"$wire" != "$(fileSize "$work/body")" ]]; then
		echo "$TOOL: $verb trailer was not canonical" >&2
		return 1
	fi
	requireDigest "$digest" "$verb trailer's digest" >/dev/null || return
	if [ "$kind" = gzip ]; then
		land=$work/raw
		# Inflate at most one byte past the declared size, so no inflate fills the disk before the check.
		# The check then judges the result, a damaged inflate included.
		gzip -dc <"$work/body" 2>"$work/gunzip" | head -c $((raw + 1)) >"$land" || :
	fi
	got=$(_unitDigest "$work" "$land") || return
	size=$wire # a raw body's size, already checked against the trailer's above, so no second wc
	[ "$kind" = raw ] || size=$(fileSize "$land") || :
	if [ "$size" != "$raw" ] || [ "$got" != "$digest" ]; then
		if [ "$kind" = gzip ]; then
			got=$(grep . "$work/gunzip" | tail -n 1) || : # GNU gzip opens with an empty line
			echo "$TOOL: $verb body did not inflate to the authenticated size and trailer digest${got:+ (gzip: ${got##*: })}" >&2
		else
			echo "$TOOL: $verb body did not match the authenticated trailer digest" >&2
		fi
		return 4
	fi
	wireLanded=$digest
	wirePlace "$land" "$4"
}

# wirePlace <file> <out>: moves the file to out, unless a folder or link is there now, since mv would put the
# file inside the folder or through the link. Another local user may have made one since the exchange began.
wirePlace() {
	if [ -d "$2" ] || [ -L "$2" ]; then
		echo "$TOOL: '$2' is now a folder or a link, which a get never writes into" >&2
		return 1
	fi
	mv -f -- "$1" "$2"
}

# wireMine <file> <who>: prints a message and returns 1 for a link or another user's file there. Either
# could be planted in a shared folder to take what lands there or to feed what is read from it. A mount that
# maps owners shows the user's own files as another user's too.
wireMine() {
	if [ -L "$1" ]; then
		echo "$TOOL: '$1' is a link, which $2 never writes through" >&2
		return 1
	elif [ -e "$1" ] && [ ! -O "$1" ]; then
		echo "$TOOL: '$1' is another user's, which $2 never writes or reads (a mount that maps owners, as sshfs or NFS can, shows your own files so: use a folder on this PC's own disk)" >&2
		return 1
	fi
}

# _wireMake <file>: creates the file only where nothing exists, so a name another user planted is refused,
# never followed. It uses set -C's O_EXCL after a check, since set -C opens a FIFO as it is. It does not run
# in a subshell, which costs Git Bash a process.
_wireMake() {
	local made=0 was=$-
	if [ -e "$1" ] || [ -L "$1" ]; then
		echo "$TOOL: $1: cannot overwrite existing file" >&2
		return 1
	fi
	set -C
	: >"$1" || made=1
	[[ "$was" == *C* ]] || set +C
	[ "$made" = 0 ] && [ -f "$1" ] && [ ! -L "$1" ]
}

# _wirePieces says this when a piece went quiet and the stats asked after it went unanswered too, and
# _wireTreeFailed finds it and stops without asking again. It is defined in one place so a rewording
# cannot make the two disagree.
WIRE_NO_STATS="the console answered no stats after it"
# _wirePieces <step> <work> <ip> <port> [args]: gives each piece three tries. A quiet piece is tried again
# only if the console answers a stats, since silence would cost each try the whole quiet bound.
_wirePieces() {
	local try st
	while :; do
		for try in 1 2 3; do
			st=0
			"$@" || st=$?
			case "$st" in 4 | 124 | 42) ;; *) break ;; esac
			[ "$try" != 3 ] || break
			if [ "$st" = 124 ] && ! wireAnswers "$3" "$4"; then
				echo "$TOOL: the piece at byte $wireDone went quiet, and $WIRE_NO_STATS: it is off, asleep or gone; run this again once it answers" >&2
				return 124
			fi
			sleep "$WIRE_RETRY_PAUSE"
		done
		case "$st" in
		0) [ "$wireSize" = - ] || [ "$wireDone" -lt "$wireSize" ] || return 0 ;;
		4 | 124 | 42)
			echo "$TOOL: the piece at byte $wireDone failed $try times" >&2
			return "$st"
			;;
		*) return "$st" ;;
		esac
	done
}

# wireAnswers <ip> <port>: succeeds if one signed `stats op` (never waiting on AM or the card) is answered
# within the control timeout. A refusal, or a bench rep holding the console, is an answer too.
wireAnswers() {
	local st=0
	wireExchange "$1" "$2" "stats $(newRequestID) op" >/dev/null 2>&1 || st=$?
	[ "$st" != 4 ] && [ "$st" != 124 ]
}

# wireGet <ip> <port> <sd-path> <local-file> [card-size]: resumes through its .3drop-part and .3drop-meta
# files. With compression set to auto, a file that fits in one piece goes as one gzip get.
wireGet() { _wireWork _wireGet "$@"; }
_wireGet() {
	local work=$1 ip=$2 port=$3 out=$5 part=$5.3drop-part meta=$5.3drop-meta path was bound size st=0
	# mv would land the file inside the directory, under another name.
	if [ -d "$out" ]; then
		echo "$TOOL: '$out' is a directory" >&2
		return 1
	fi
	for was in "$out" "$part" "$meta"; do wireMine "$was" "a get" || return; done
	# The part file is made first, so a name this host cannot hold with the suffix (NAME_MAX) is reported
	# before any exchange.
	if [ ! -e "$part" ] && ! _wireMake "$part" 2>"$work/made"; then
		read -r was <"$work/made" || :
		echo "$TOOL: could not make '$part' here: ${was##*: }" >&2
		return 1
	fi
	path=$(wirePath "$4") || return
	wireKey || return # needed for the tag below; a no-op once requireKey has run
	wireDone=0 wireBack=0 wireSize=- wireMtime=- wireAgain=1 wireLast="" was="" bound=""
	# The meta's numbers are checked before any arithmetic, because arithmetic would evaluate a name.
	# A part belongs to one console. One begun under another key or port, or by a get that wrote neither,
	# starts over.
	if { read -r was wireSize wireMtime bound <"$meta"; } 2>/dev/null && [ "$was" = "$path" ] &&
		isUint "$wireSize" && [ -f "$part" ] && [ ! -L "$part" ] && wireDone=$(fileSize "$part") &&
		[ "$wireDone" -le "$wireSize" ] && [ "$bound" = "${WIRE_TAG:--} $port" ]; then
		wireBack=$((wireDone < WIRE_UNIT ? wireDone : WIRE_UNIT))
	else
		[ "$was" != "$path" ] || [ "$bound" = "${WIRE_TAG:--} $port" ] || [ ! -s "$part" ] ||
			echo "$TOOL: $part was not begun under this key and port: it starts over" >&2
		# This also removes the probe made above. The first piece makes the part again, exclusively.
		rm -f -- "$part" "$meta"
		wireDone=0 wireSize=- wireMtime=-
	fi
	[ "$wireSize" = - ] || { _wirePartCheck "$work" "$part" "$meta" && _wireRoom "$part" $((wireSize - wireDone)) "$out"; } || return
	if [ "${THREEDROP_COMPRESSION:-none}" = auto ] && [ "$wireSize" = - ] &&
		{ isUint "${6:-}" && size=$6 || size=$(_wireCardSize "$ip" "$port" "$4"); } &&
		[ "$size" -le "$WIRE_PIECE" ] && ! _wireNoGzip; then
		_wirePieces _wireGetWhole "$work" "$ip" "$port" "$path" "$out" "$size" || return
		echo "$wireLast"
		return
	fi
	_wirePieces _wireGetPiece "$work" "$ip" "$port" "$path" "$part" "$meta" || st=$?
	if [ "$st" != 0 ]; then
		if [ -s "$part" ]; then
			echo "$TOOL: $wireDone bytes of it are kept in $part: get it again to resume" >&2
		else
			rm -f -- "$part" "$meta"
		fi
		return "$st"
	fi
	if [ "$(fileSize "$part")" != "$wireSize" ]; then
		echo "$TOOL: $part changed under this get: get it again" >&2
		return 1
	fi
	wirePlace "$part" "$out" && rm -f -- "$meta" || return
	echo "$wireLast"
}
# _wireGetWhole <work> <ip> <port> <wire-path> <out> <size>: lands the file beside out, then renames it
# into place, since a move from a scratch folder on another filesystem is a copy, and a copy that gets cut
# would leave out half done.
_wireGetWhole() {
	local request response
	request="get $(newRequestID) $4 gzip"
	response=$(_wireStream "$2" "$3" "$request" "$1") && _wireLand "$1" "$request" "$response" "$5.3drop-part" &&
		wirePlace "$5.3drop-part" "$5" || return
	wireDone=$6 wireSize=$6 wireLast=$response
}
# wireGetPipe <ip> <port> <sd-path> [from]: writes each piece to stdout only once it checks out, so a reader
# never sees a byte that failed the check.
wireGetPipe() { _wireWork _wireGetPipe "$@"; }
_wireGetPipe() {
	local path
	path=$(wirePath "$4") || return
	wireDone=${5:-0} wireBack=0 wireSize=- wireMtime=- wireLast=""
	_wirePieces _wireGetPiece "$1" "$2" "$3" "$path" - -
}

# _wireGetPiece <work> <ip> <port> <wire-path> <part|-> <meta>: the first piece of a resume starts one unit
# back. That unit must match the end of the part, so a rewrite that FAT's 2 s mtime misses is caught there.
_wireGetPiece() {
	local work=$1 at=$((wireDone - wireBack)) request response length size mtime extra
	request="get $(newRequestID) $4 range $at $WIRE_PIECE"
	response=$(_wireStreamOpen "$2" "$3" "$request" "$work") || return
	wireFields "$response"
	if [ "$wireVerb $wireOutcome $wireDetail" = "get rejected size" ] && [ "$wireSize" != - ]; then
		_wireGetAgain "$5" "$6" # the file shrank below what came
		return
	fi
	if [ "$wireOutcome" != ok ]; then
		wireReport "$response" # returns 4 for a cut, and a cut is tried again
		return
	fi
	read -r length _ size mtime extra <<<"$wireRest"
	if [ -n "$extra" ] || ! isUint "$size" || { [ "$mtime" != - ] && ! isUint "$mtime"; }; then
		echo "$TOOL: get failed: $response" >&2
		return 1
	fi
	# _wireLand reads `<length> raw`, so the size and mtime are cut off first.
	_wireLand "$work" "$request" "${response% * *}" "$work/piece" || return
	if [ "$wireSize" = - ]; then # the first piece names the file
		[ "$5" = - ] || _wireRoom "$5" $((size - at)) "${5%.3drop-part}" || return
		wireSize=$size wireMtime=$mtime
		if [ "$5" != - ] && { ! _wireMake "$6" 2>/dev/null || ! _wireMake "$5" 2>/dev/null; }; then
			echo "$TOOL: another process made '$5' or its meta during this get: get it again" >&2
			return 1
		fi
		[ "$5" = - ] || printf '%s %s %s %s %s\n' "$4" "$size" "$mtime" "${WIRE_TAG:--}" "$3" >"$6" || return
	elif [ "$size $mtime" != "$wireSize $wireMtime" ]; then
		_wireGetAgain "$5" "$6"
		return
	fi
	if [ "$length" != $((size - at < WIRE_PIECE ? size - at : WIRE_PIECE)) ]; then
		echo "$TOOL: get failed: $response" >&2
		return 1
	fi
	if [ "$5" = - ]; then
		cat "$work/piece" || return
		wireDone=$((at + length)) wireLast=$response
		return
	fi
	if [ "$(fileSize "$5")" != "$wireDone" ]; then # changed by another run
		echo "$TOOL: $5 changed under this get: get it again" >&2
		return 1
	fi
	if [ "$wireBack" != 0 ]; then
		tail -c "$wireBack" "$5" >"$work/had" && head -c "$wireBack" "$work/piece" >"$work/again" || return
		if ! cmp -s "$work/had" "$work/again"; then
			_wireGetAgain "$5" "$6"
			return
		fi
	fi
	# The piece's line goes first. If the append is then cut, the part falls short of that line, and a resume
	# cuts the line back.
	printf 'piece %s %s %s\n' "$at" "$length" "$wireLanded" >>"$6" && fileFrom "$work/piece" "$wireBack" >>"$5" || return
	wireDone=$((at + length)) wireBack=0 wireLast=$response
}
# _wirePartCheck <work> <part> <meta>: checks a resumed part against each piece's digest, because a restart
# can leave a zeroed or lost extent under a length and last unit that look right.
_wirePartCheck() {
	local work=$1 part=$2 meta=$3 head tag at length digest got kept=""
	{
		IFS= read -r head
		while read -r tag at length digest; do
			if [ "$tag" != piece ] || ! isUint "$at" || ! isUint "$length" || [ $((at % WIRE_UNIT)) != 0 ]; then break; fi
			if [ $((at + length)) -le "$wireDone" ] &&
				dd if="$part" of="$work/check" bs="$WIRE_UNIT" skip=$((at / WIRE_UNIT)) \
					count=$(((length + WIRE_UNIT - 1) / WIRE_UNIT)) 2>/dev/null &&
				got=$(_unitDigest "$work" "$work/check") && [ "$got" = "$digest" ]; then
				kept+="piece $at $length $digest"$'\n'
				continue
			fi
			echo "$TOOL: $part no longer holds what came from byte $at (a restart can do that): it is asked again from there" >&2
			dd if=/dev/null of="$part" bs=1 seek="$at" 2>/dev/null && printf '%s\n%s' "$head" "$kept" >"$meta" || return
			wireDone=$at
			wireBack=$((wireDone < WIRE_UNIT ? wireDone : WIRE_UNIT))
			break
		done
	} <"$meta"
	rm -f "$work/check"
}
# _wireRoom <file> <bytes> <name>: prints a message and returns 1 when the file's folder has no room for that
# many more bytes, which a get would otherwise learn as a reply cut short, three times. A df that cannot
# say passes.
_wireRoom() {
	local dir=${1%/*} room
	[ "$dir" != "$1" ] || dir=.
	room=$(freeKiB "${dir:-/}") || return 0
	[ $((room * 1024)) -lt "$2" ] || return 0
	echo "$TOOL: no room for $3: $2 bytes to come, $((room * 1024)) free in ${dir:-/}" >&2
	return 1
}
# _wireGetAgain <part|-> <meta>: the file changed under the part. The part starts over, but only once per
# run; what already went to stdout cannot start over.
_wireGetAgain() {
	if [ "$1" = - ]; then
		echo "$TOOL: the file changed on the console as it came, with its bytes to $wireDone out: get it again" >&2
		return 1
	elif [ "$wireAgain" = 0 ]; then
		echo "$TOOL: the file changed again as it came: get it again" >&2
		return 1
	fi
	echo "$TOOL: the file changed since $1 began: it starts over" >&2
	rm -f -- "$1" "$2"
	wireAgain=0 wireDone=0 wireBack=0 wireSize=- wireMtime=-
}

# A screenshot goes as a mutation, so a captured request cannot be replayed to watch the screen. Each
# screen spends the kept nonce, as wireMutation does.
wireScreenshot() {
	local screen response st
	for screen in top bottom; do
		st=0
		response=$(_wireWithNonce "$1" "$2" screenshot "$screen" _wireWork _wireShot "$3-$screen.bmp") || st=$?
		[ "$st" != 0 ] || continue
		wireFields "$response"
		[ -z "$response" ] || [ "$wireDetail" = ok ] || wireReport "$response"
		return "$st"
	done
	printf '%s\n%s\n' "$3-top.bmp" "$3-bottom.bmp"
}
# _wireShot <work> <file> <ip> <port> <request>: prints the verified header, and the screen follows if
# the header is ok.
_wireShot() {
	local response
	response=$(_wireStreamOpen "$3" "$4" "$5" "$1") || return
	echo "$response"
	wireFields "$response"
	[ "$wireDetail $wireResult" = "ok 00000000" ] && _wireLand "$1" "$5" "$response" "$2"
}

# wireWaitUp <ip> <port> [version] [seconds=60]: waits for the relaunched 3Drop and prints its version.
# The old process no longer takes connections, so whatever answers is the new one. The version comes
# from `stats op`; `stats self`, which waits on AM, is asked only of a 3Drop too old to put it there.
WIRE_KNOCK=0.5
wireWaitUp() {
	local end=$((SECONDS + ${4:-60})) ver seen="" reply st
	while :; do
		st=0 reply=""
		if [ "${4:-60}" != 0 ]; then
			wireProve "$1" "$2"
			case "$wireProof" in ok) [ -n "${3:-}" ] || return 0 ;; held) st=3 ;; *) st=4 ;; esac
			reply=$wireProofReply
		else
			reply=$(wireRead "$1" "$2" stats op 2>/dev/null) || st=$?
		fi
		case "$st:$reply" in 0:*" ver="*) ;; 0:*) reply=$(wireRead "$1" "$2" stats self 2>/dev/null) || st=$? ;; esac
		if [ "$st" = 3 ]; then
			echo "$TOOL: a benchmark run on this PC holds the console at $1" >&2
			return 3
		fi
		ver=$(kv "$reply" ver)
		isUint "$ver" || ver="" # deploy.sh adds 1 to it, and arithmetic would evaluate a name
		[ -z "$ver" ] || seen=$ver
		if [ -n "$ver" ] && { [ -z "${3:-}" ] || [ "$ver" = "$3" ]; }; then
			echo "$ver"
			return 0
		fi
		[ "$SECONDS" -lt "$end" ] || break
		sleep "$WIRE_KNOCK"
	done
	if [ -n "$seen" ]; then
		echo "$TOOL: 3Drop at $1 answered as build $seen, not $3" >&2
		return 1
	fi
	echo "$TOOL: no answer from 3Drop at $1" >&2
	return 4
}

# wireHash <ip> <port> <sd-path> [local-file]: prints `u:<hex> <size>`, read on the card. With a local file,
# returns 1 when it differs, as cmp does.
wireHash() {
	local mine="" reply digest size extra held
	if [ -n "${4:-}" ]; then
		_wireRegular "$4" || return
		digest=$(unitDigest "$4") && size=$(fileSize "$4") || return
		mine="$digest $size"
	fi
	held=$(_wireCardSize "$1" "$2" "$3") || held=$WIRE_FILE_MAX # the card's file sets the wait
	reply=$(wireRead "$1" "$2" hash "$(wirePath "$3")" $((WIRE_MARGIN + (held / 1000000 + 1) * WIRE_HASH_MS_PER_MB / 1000))) || return
	wireFields "$reply"
	read -r size digest extra <<<"$wireRest"
	if [ -n "$extra" ] || ! isUint "$size" || ! requireDigest "$digest" "hash's digest" >/dev/null; then
		echo "$TOOL: hash failed: $reply" >&2
		return 1
	fi
	echo "$digest $size"
	if [ -n "$mine" ] && [ "$mine" != "$digest $size" ]; then
		echo "$TOOL: $3 differs from $4" >&2
		return 1
	fi
}

# ---- Mutations -------------------------------------------------------------------------------

# wireDelete <ip> <port> <sd-path|title:<id>> [r|ticket|system]: deleting a tree or a title can outlast the
# control timeout, so it gets 120 s.
wireDelete() {
	local target wait=""
	target=$(wirePath "$3") || return
	case "$target:${4:-}" in title:* | *:r) wait=120 ;; esac
	wireMutation "$1" "$2" "$(wireTimeout "$wait")" delete "$target" ${4:+"$4"}
}
wireMkdir() { wireMutation "$1" "$2" "$(wireTimeout)" mkdir "$(wirePath "$3")"; }
wireRename() { wireMutation "$1" "$2" "$(wireTimeout)" rename "$(wirePath "$3")" "$(wirePath "$4")"; }
# wireUninstall <ip> <port> <title-id> [ticket]: an application's save data goes with it. Use `ticket`
# only for a ticket the caller created, which the console cannot tell from an eShop license.
wireUninstall() {
	local titleID
	titleID=$(wireTitleID "$3") || return
	wireDelete "$1" "$2" "title:$titleID" ${4:+"$4"}
}
# wireRun <ip> <port> <self|[title:]title-id|sd-path.3dsx> [stdio|game]: with stdio, start
# `nc -l 17491` first. `game` says a title is a game, which the console then launches with a romfs
# folder too. A title goes on the wire as `title:<id>`, its one spelling.
wireRun() {
	local target
	if caseless "$3" self; then
		target=self
	elif caseless "$3" '*.3dsx'; then
		target=$(wirePath "$3") # the device matches the extension in any case
	else
		target=title:$(wireTitleID "${3#title:}") || return
	fi
	wireMutation "$1" "$2" "$(wireTimeout)" run "$target" ${4:+"$4"} || return
	# 3Drop has left, and its nonce with it. The next exchange is a read, so a console that is still away is
	# found at once, never by a put's reconcile.
	_wireNonceDrop "$1" "$2"
}

# _wirePutBody <ip> <port> <body> <fields> [cia intent]: prints the reply, or what the reconcile found.
# Returns 42 for a ghost the console canceled.
_wirePutBody() { _wireWithNonce "$1" "$2" put "$4" _wirePutSend "$3" "${5:-}" "${6:-}"; }
# _wirePutSend <body> <cia> <intent> <ip> <port> <request>: a body whose reply is lost is never sent
# again, because that put may have run. Its receipt says what became of it.
_wirePutSend() {
	local ip=$4 port=$5 request=$6 id reply st=0
	read -r _ id _ <<<"$request"
	_wireQuiet "$request"
	reply=$(_wireTransport "$ip" "$port" "$wireQuiet" "$request" "$(macRequest "$request")" "$1") || st=$?
	# Nothing was sent: either the request could not go (1) or a bench rep held the console (3).
	case "$st" in 1 | 3) return "$st" ;; esac
	if [ "$st" = 0 ]; then
		_wireVerify "$ip" "$port" "$request" "$reply" || st=$?
		case "$st" in
		0) return 0 ;;
		1) return 1 ;; # the console's unsigned refusal comes before anything runs, and says why
		esac
	fi
	_wireNonceDrop "$ip" "$port" # the put spent it
	[ "$st" != 42 ] ||
		echo "$TOOL: ghost: the send rate fell below THREEDROP_GHOST_FLOOR, so the socket was closed and the console cancels the put" >&2
	reply=$(_wireReconcile "$ip" "$port" "$id" "$2" "$3") || {
		[ "$st" = 124 ] || st=4 # the outcome is unknown; a put that went quiet timed out
		return "$st"
	}
	echo "$reply"
	wireFields "$reply"
	if [ "$st" = 42 ] && [ "$wireOutcome" != committed ]; then
		return 42 # the console canceled it, so it may be sent again
	fi
}

# A console that loses a put's reply may still be finishing it, or relaunching.
WIRE_RECONCILE_WAIT=60
# _wireReconcile <ip> <port> <request-id> [cia intent]: looks for a lost put's receipt, knocking as
# wireWaitUp does; then, for a CIA, looks for its title at the sent version, since a committed self put
# relaunches 3Drop, which takes the receipt with it.
_wireReconcile() {
	local end=$((SECONDS + WIRE_RECONCILE_WAIT)) titleID="" version="" st=4
	[ -z "${4:-}" ] || { ciaRead "$4" && titleID=$ciaTitle version=$ciaVer; } || return
	while [ "$st" = 4 ] && [ "$SECONDS" -lt "$end" ]; do
		wireProve "$1" "$2"
		case "$wireProof" in ok) st=0 ;; other-key | unreadable) st=1 ;; held) st=3 ;; *) st=4 ;; esac
		if [ "$st" = 0 ]; then
			_wireReceiptOf "$wireProofReply" "$3" && return 0
			st=2
		fi
		[ "$st" != 4 ] || sleep "$WIRE_KNOCK"
	done
	if [ "$st" = 2 ] && [ -n "$titleID" ] && _wireInstalledAt "$1" "$2" "$titleID" "$version" "$5"; then
		echo "response $3 put committed ok 00000000 $titleID $version $5"
		return 0
	fi
	case "$st" in
	2) echo "$TOOL: the put's outcome is unknown: no receipt${titleID:+, and $titleID is not installed at v$version}" >&2 ;;
	4) echo "$TOOL: the put's outcome is unknown: the console said nothing for $WIRE_RECONCILE_WAIT s" >&2 ;;
	*) echo "$TOOL: the put's outcome is unknown: its receipt could not be read" >&2 ;;
	esac
	return 4
}
# _wireInstalledAt <ip> <port> <title-id> <version> <intent>
_wireInstalledAt() {
	if [ "$5" = self ]; then
		wireWaitUp "$1" "$2" "$4" 0 >/dev/null 2>&1
	else
		[ "$(wireTitleVersion "$1" "$2" "$3" 2>/dev/null)" = "$4" ]
	fi
}

# wirePut <ip> <port> <new|update|self> <cia> [prefix [index total installed]]: <prefix> is the CIA's
# ciaPrefix, for a caller that has already read it (ciaRead); if none is given it is read here. Only the
# prefix is hashed, since AM checks every content against the TMD as it installs.
wirePut() {
	local size prefix=${5:-} digest reply st=0
	[ -n "$prefix" ] || prefix=$(ciaPrefixSize "$4") || return 1
	size=$(fileSize "$4") && digest=$(unitDigest "$4" "$prefix") || return 1
	reply=$(_wirePutBody "$1" "$2" "$4" "$3 $size $prefix $digest${8+ $6 $7 $8}" "$4" "$3") || st=$?
	[ -n "$reply" ] || return "$st"
	wireFields "$reply"
	# Appends the put's own request id for a caller that must tie the receipt and stats to it. This happens
	# only when asked (THREEDROP_PUT_ID_FILE), one line per put sent, so after a ghost retry the retry's id
	# is the last of two lines. It prints nothing itself.
	if [ -n "${THREEDROP_PUT_ID_FILE:-}" ]; then
		printf '%s\n' "$wireId" >>"$THREEDROP_PUT_ID_FILE"
	fi
	# A committed self put relaunches 3Drop, as a run does.
	[ "$3:$wireOutcome" != self:committed ] || _wireNonceDrop "$1" "$2"
	echo "${reply#response * }"
	return "$st"
}

# _wireWhole <wire-path> [system]: succeeds if the console takes the file only whole and raw: a save's or
# extdata's, or a file put with the word `system`.
_wireWhole() { [ -n "${2:-}" ] || [[ "$1" == save:* || "$1" == extdata:* ]]; }

# _wirePutCommitted <response> <bytes> <digest> <file|piece>: a committed put's reply must name these
# bytes, or it belongs to another put (1). A put that a bench nohash build took no digest of is unverified.
_wirePutCommitted() {
	case "$1" in
	*" put committed ok 00000000 $2 $3") ;;
	*" put committed ok 00000000 $2 digest=skipped")
		echo "$TOOL: the console took no digest of this $4 (a bench build's nohash): it is unverified" >&2
		;;
	*)
		# bridge.pl maps this message: "other bytes" makes it delete the console's temp.
		echo "$TOOL: the reply names other bytes than these: $1" >&2
		return 1
		;;
	esac
}

# _wireRegular <local-file>: prints a message and returns 1 for a file that is not a regular file. Opening a
# FIFO waits forever for a writer, and a pipe's /dev/fd name gives its bytes only once, to the first
# measure.
_wireRegular() {
	[ -f "$1" ] && [[ "$1" != /dev/* ]] && return
	echo "$TOOL: '$1' is not a regular file: send a pipe's bytes as put - <sd-path>, from stdin" >&2
	return 1
}

# wireFilePut <ip> <port> <local-file> <sd-path> [system] [sha256]: never gzips into a save or extdata,
# which the device refuses. A caller's SHA-256 saves a second read of a file of one unit or less.
wireFilePut() { _wireWork _wireFilePut "$@"; }
_wireFilePut() {
	local work=$1 ip=$2 port=$3 file=$4 path raw digest body=$4 option=${6:+ $6} fields response gz
	_wireRegular "$file" || return
	path=$(wirePath "$5") && raw=$(fileSize "$file") || return
	if [ "$raw" -gt "$WIRE_FILE_MAX" ]; then
		echo "$TOOL: $file is over $WIRE_FILE_MAX bytes, the most a FAT32 file holds" >&2
		return 1
	fi
	if ! _wireWhole "$path" "$option" && [ "$raw" -gt "$WIRE_PIECE" ]; then
		_wirePutPieces "$work" "$ip" "$port" "$file" "$path" "$5" "$raw"
		return
	fi
	if [ -n "${7:-}" ] && [ "$raw" -le "$WIRE_UNIT" ]; then
		digest=$(_unitDigestSha "$7" "$raw") || return
	else
		digest=$(unitDigest "$file") || return
	fi
	if ! _wireWhole "$path" "$option" && compressionWorthwhile "$file" &&
		gzip -n -c -- "$file" >"$work/body.gz" && gz=$(fileSize "$work/body.gz") && [ "$gz" -lt "$raw" ]; then
		body=$work/body.gz option=" gzip $gz"
	fi
	# The file is measured again as it goes, because one shorter than declared would hold the console
	# until its stall rule.
	if [ "$(fileSize "$file")" != "$raw" ]; then
		echo "$TOOL: $file changed as it went: put it again" >&2 # bridge.pl maps "put it again" to 450
		return 1
	fi
	fields="$path $raw $digest$option"
	response=$(_wirePutBody "$ip" "$port" "$body" "$fields") || return
	case "$response" in
	"response "*" put committed ok "*) _wirePutCommitted "$response" "$raw" "$digest" file || return ;;
	*)
		wireReport "$response"
		return
		;;
	esac
	echo "$response"
}

# wirePipePut <ip> <port> <sd-path> [system]: puts stdin as the file at the path. A piece or less, an empty
# stdin too, goes as a put of that file (wireFilePut); more goes in pieces (_wirePutPieces).
wirePipePut() { _wireWork _wirePipePut "$@"; }
_wirePipePut() {
	local work=$1 path one
	path=$(wirePath "$4") || return
	wireDone=0 wirePipeAt=- wireSize=-
	: >"$work/next"
	: >"$work/hashes"
	_wirePipeNext "$work" || return
	if [ "$wireSize" != - ]; then
		one=$work/piece
		if [[ "${4##*/}" =~ \.([0-9A-Za-z]+)$ ]]; then # its extension, since gzip is never tried on some
			one=$work/piece.${BASH_REMATCH[1]}
			mv -f "$work/piece" "$one" || return
		fi
		_wireFilePut "$work" "$2" "$3" "$one" "$4" ${5:+"$5"}
		return
	fi
	if _wireWhole "$path" "${5:-}"; then
		echo "$TOOL: '$4' goes whole, so stdin gives it $WIRE_PIECE bytes at most" >&2
		return 1
	fi
	_wirePutPieces "$work" "$2" "$3" - "$path" "$4" -
}
# _wirePipeNext <work>: reads stdin's piece whole (fullblock, because a pipe hands a read only what it
# holds), and only once however often the piece goes. The byte after the piece is kept, and shows whether
# it is the last.
_wirePipeNext() {
	local work=$1 had length
	[ "$wirePipeAt" != "$wireDone" ] || return 0
	had=$(fileSize "$work/next") &&
		{ cat "$work/next" && dd bs=$((WIRE_PIECE - had)) count=1 iflag=fullblock status=none; } >"$work/piece" &&
		dd bs=1 count=1 status=none of="$work/next" && length=$(fileSize "$work/piece") && mkdir -p "$work/units" &&
		_unitHashes "$work/units" "$work/piece" && cat "$work/units/hashes" >>"$work/hashes" || return
	# A bridge marks its client's end first, because a session KILLed mid-upload closes stdin without a mark.
	if [ ! -s "$work/next" ] && [ -n "${THREEDROP_BRIDGE_END:-}" ] && [ ! -e "$THREEDROP_BRIDGE_END" ]; then
		echo "$TOOL: stdin ended without the bridge's mark of the file's end (its session was killed): nothing is published" >&2
		return 1
	fi
	wirePipeAt=$wireDone
	[ -s "$work/next" ] || wireSize=$((wireDone + length))
}

# _wirePutPieces <work> <ip> <port> <file|-> <wire-path> <sd-path> <size|->: starts after the whole pieces
# the console's .3drop.tmp already keeps. The last piece always goes, since it publishes.
_wirePutPieces() {
	local work=$1 ip=$2 port=$3 kept st=0
	wireSize=$7 wireDone=0 wireKept=0 wireAgain=1 wireLast="" wireTempSaid=""
	[ "$4" = - ] || _unitHashes "$work" "$4" || return
	# Only a listing that finds no temp shows that nothing is kept, because a piece sent from 0 would cut off
	# what is kept.
	kept=$(wireLs "$ip" "$port" "$6$WIRE_PUT_TEMP" d 2>"$work/listed") || st=$?
	case "$st:$(cat "$work/listed")" in
	0:*)
		read -r _ kept _ <<<"$kept"
		if isUint "$kept"; then
			[ "$wireSize" = - ] || [ "$kept" -lt "$wireSize" ] || kept=$((wireSize - 1))
			wireKept=$((kept / WIRE_PIECE * WIRE_PIECE))
			[ "$4" = - ] || wireDone=$wireKept
		fi
		;;
	1:*" ls rejected notfound "*) st=0 ;;
	*)
		cat "$work/listed" >&2
		return "$st"
		;;
	esac
	_wirePieces _wirePutPiece "$work" "$ip" "$port" "$4" "$5" "$6" || st=$?
	if [ "$st" != 0 ]; then
		# bridge.pl maps this message: "put it again" is 450. Once it deletes the temp, it drops "the console
		# keeps N bytes of it in" from the message.
		[ "$wireDone" = 0 ] || [ -n "$wireTempSaid" ] ||
			echo "$TOOL: the console keeps $wireDone bytes of it in $6$WIRE_PUT_TEMP: put it again to resume, or delete that file" >&2
		return "$st"
	fi
	echo "$wireLast"
}
# _wirePutPiece <work> <ip> <port> <file|-> <wire-path> <sd-path>: a cut or damaged body is a failed
# exchange, but a file's piece that no longer hashes as it did at the start changed as it went (1).
_wirePutPiece() {
	local work=$1 at=$wireDone length size=- through kept response why
	if [ "$4" = - ]; then
		_wirePipeNext "$work" && length=$(fileSize "$work/piece") || return
		if [ $((at + length)) -gt "$WIRE_FILE_MAX" ]; then
			# bridge.pl maps this message: "went past" is 552 (no room).
			echo "$TOOL: stdin went past $WIRE_FILE_MAX bytes, the most a FAT32 file holds: delete $6$WIRE_PUT_TEMP" >&2
			wireTempSaid=1
			return 1
		fi
		if [ "$wireSize" = - ] && [ $((at + length)) -le "$wireKept" ]; then
			wireDone=$((at + length)) # the console keeps it
			return
		fi
	else
		length=$((wireSize - at < WIRE_PIECE ? wireSize - at : WIRE_PIECE))
		if ! dd if="$4" of="$work/piece" bs="$WIRE_UNIT" skip=$((at / WIRE_UNIT)) count=$(((length + WIRE_UNIT - 1) / WIRE_UNIT)) 2>"$work/dd"; then
			read -r why <"$work/dd" || why="dd failed"
			echo "$TOOL: could not copy $4 from byte $at for its piece: $why" >&2
			return 1
		fi
		if [ "$(fileSize "$work/piece")" != "$length" ]; then
			echo "$TOOL: $4 changed as it went" >&2
			return 1
		fi
	fi
	[ $((at + length)) != "$wireSize" ] || size=$wireSize
	through=$(_unitsDigest "$work/hashes" $(((at + length + WIRE_UNIT - 1) / WIRE_UNIT))) &&
		kept=$(_unitsDigest "$work/hashes" $((at / WIRE_UNIT))) || return
	response=$(_wirePutBody "$2" "$3" "$work/piece" "$5 $size $through range $at $length $kept") || return
	case "$response" in
	"response "*" put committed ok "*) _wirePutCommitted "$response" $((at + length)) "$through" piece || return ;;
	"response "*" put rejected digest d963f80a" | "response "*" put rejected short "* | \
		"response "*" put rejected receive "* | "response "*" put rejected timeout "*)
		if [ "$4" != - ] && [[ "$response" == *" digest d963f80a" ]] && mkdir -p "$work/now" &&
			_unitHashes "$work/now" "$work/piece" && ! dd if="$work/hashes" bs=32 skip=$((at / WIRE_UNIT)) \
			count=$(((length + WIRE_UNIT - 1) / WIRE_UNIT)) 2>/dev/null | cmp -s - "$work/now/hashes"; then
			echo "$TOOL: $4 changed as it went" >&2
			return 1
		fi
		echo "$TOOL: the piece at byte $at did not arrive whole: $response" >&2
		return 4
		;;
	"response "*" put rejected size "* | "response "*" put rejected digest 00000000")
		_wirePutAgain "$4" "$6" "$response"
		return
		;;
	*)
		wireReport "$response"
		return
		;;
	esac
	wireDone=$((at + length)) wireLast=$response
}
# _wirePutAgain <file|-> <sd-path> <response>: the console's temp does not keep the bytes this piece
# continues from. A file then starts over, but only once per run; stdin cannot, because its start is gone.
# bridge.pl maps these sentences: "put again" is 450, and "other bytes" deletes the temp.
_wirePutAgain() {
	if [ "$1" = - ]; then
		wireTempSaid=1
		case "$3" in
		*" size "*) echo "$TOOL: the console's $2$WIRE_PUT_TEMP holds less than its listing said: put it again" >&2 ;;
		*) echo "$TOOL: the console's $2$WIRE_PUT_TEMP keeps other bytes than this stdin's: delete that file, then put again" >&2 ;;
		esac
		return 1
	elif [ "$wireAgain" = 0 ]; then
		echo "$TOOL: $3" >&2
		return 1
	fi
	echo "$TOOL: the console's $2$WIRE_PUT_TEMP keeps other bytes: the file goes again from its start" >&2
	wireAgain=0 wireDone=0
}

# ---- Trees (get and put <dir> r) -------------------------------------------------------------
# Adding and updating only. A manifest of what landed lets a copy skip what the last copy left as it is,
# so a copy that stopped goes on from there.

# _wireTreeDir <dir>: prints the directory without its trailing slashes (the root becomes empty). A name
# that starts with a dash gets a ./ prefix, so find does not read it as an option.
_wireTreeDir() {
	local dir=$1
	while [[ "$dir" == */ ]]; do dir=${dir%/}; done
	[[ "$dir" != -* ]] || dir=./$dir
	echo "$dir"
}

# _wireTreeTop <sd-dir>: prints the directory as the device lists it. A root, the card's or an archive's
# (`save:<id>/`), keeps its slash.
_wireTreeTop() { if [[ "$1" == */* ]]; then echo "$1"; else echo "$1/"; fi; }

# _wireTreeExcluded <relative-path>: succeeds for Finder's ._* and .DS_Store, a get's own part and meta
# files, and a put's temp and backup on the console, none of which a copy carries either way.
_wireTreeExcluded() {
	case "/$1/" in */._*/* | */.DS_Store/* | *.3drop-part/* | *.3drop-meta/* | *"$WIRE_PUT_TEMP"/* | *"$WIRE_PUT_BACKUP"/*) ;; *) return 1 ;; esac
}

# _wireTreeManifest <push|pull> <port> <key...>: prints the manifest's name, which comes from the key's tag
# and not from an address, because an address moves with the network.
_wireTreeManifest() {
	local cache tag key
	cache=$(cacheDir) && tag=$(requireHex 16 "${WIRE_BOOK##*addresses-}" "key tag") &&
		key=$(printf '%s\n' "${@:3}" | openssl dgst -sha256) && key=$(requireHex 64 "${key##* }" "manifest key") || return
	echo "$cache/$1-$tag-$2-$key.manifest"
}

# _wireTreeKeep <manifest>: keeps each name's last line (the name follows three words). The manifest is
# replaced whole through a temp of its own, so a cut leaves the old one and two copies running at once never
# share a temp.
_wireTreeKeep() {
	local new
	[ -f "$1" ] || : >"$1"
	new=$(mktemp "$1.new.XXXXXX") || return
	if ! LC_ALL=C awk '{ name = substr($0, length($1 $2 $3) + 4); if (!(name in last)) order[++n] = name; last[name] = $0 }
		END { for (i = 1; i <= n; i++) print last[order[i]] }' "$1" >"$new" || ! mv -f "$new" "$1"; then
		rm -f "$new"
		return 1
	fi
}

# _wireTreeSkip <relative-path> <why>: skips a name, reporting it and counting it in wireSkipped.
_wireTreeSkip() {
	printf "%s: skipped '%s': %s\n" "$TOOL" "$1" "$2" >&2
	wireSkipped=$((wireSkipped + 1))
}

# _wireTreeFailed <status> <relative-path> <put|fetched> <what it said> <bytes> [local-dir]: a refused file
# is skipped, and a silent one is left while the console still answers. A failure that every later file
# would meet stops the copy.
_wireTreeFailed() {
	if [ "$1" = 1 ] && ! _wireTreeAll "$4" "$5" "${6:-}"; then
		_wireTreeSkip "$2" "it was not $3"
		return 0
	fi
	if { [ "$1" = 4 ] || [ "$1" = 124 ]; } && [[ "$4" != *"$WIRE_NO_STATS"* ]] && wireAnswers "$ip" "$port"; then
		printf "%s: '%s' was not %s, and the console answers after it: going on without it\n" "$TOOL" "$2" "$3" >&2
		wireFailed+=("$2")
		return 0
	fi
	printf "%s: '%s' was not %s; stopping: the same command again goes on from there\n" "$TOOL" "$2" "$3" >&2
	return "$1"
}

# _wireTreeCopy <why> <overwrite> <relative-path> <bytes> <put|fetched> <local-dir|""> <command...>:
# copies one file, unless why says that the file it would replace is not as the last copy left it.
_wireTreeCopy() {
	local why=$1 overwrite=$2 rel=$3 size=$4 what=$5 dir=$6 said st=0
	shift 6
	wireCopied=""
	if [ -n "$why" ] && [ -z "$overwrite" ]; then
		_wireTreeSkip "$rel" "$why; --overwrite replaces it"
		return 0
	fi
	printf '%s: %s (%s bytes)\n' "$TOOL" "$rel" "$size" >&2
	# What the command says is shown as it comes and kept for the rules after it, so a file in a long wait
	# says why while it waits. It goes through a file because Linux reopens /dev/stderr, which would truncate
	# a log it names.
	[ -n "${wireSaid:-}" ] || wireSaid=$(_wireReplyFile) || return
	"$@" 2>&1 </dev/null >/dev/null | tee "$wireSaid" >&2 || st=${PIPESTATUS[0]}
	IFS= read -r -d '' said <"$wireSaid" || :
	if [ "$st" != 0 ]; then
		_wireTreeFailed "$st" "$rel" "$what" "$said" "$size" "$dir"
		return
	fi
	wireCopied=1
}

# _wireTreeAll <what it said> <bytes> [local-dir]: succeeds if every later file would meet this failure.
_wireTreeAll() {
	local dir room need re=$'(^|\n)'"$TOOL"': response [0-9a-f]{32} [a-z]+ rejected (stranded|space) '
	[[ ! "$1" =~ $re ]] || return 0
	isUint "$2" || return 1
	need=$(($2 < WIRE_PIECE ? $2 : WIRE_PIECE))
	for dir in "${SCRATCH:-${TMPDIR:-/tmp}}" ${3:+"$3"}; do
		room=$(freeKiB "$dir") || continue
		[ $((room * 1024)) -ge "$need" ] || return 0
	done
	return 1
}

# _wireTreeDone <moved> <total> <what was done, and where>: prints the copy's last message. Returns 4 if a
# file failed while the console was still there (wireFailed), otherwise 1 if it skipped a name.
_wireTreeDone() {
	local name
	if [ "$1" = 0 ] && [ "$wireSkipped" = 0 ] && [ "${#wireFailed[@]}" = 0 ]; then
		echo "$TOOL: nothing changed -- $2 file(s) already current" >&2
	else
		echo "$TOOL: $1 of $2 file(s) $3" >&2
	fi
	[ "$wireSkipped" = 0 ] || echo "$TOOL: $wireSkipped name(s) skipped" >&2
	if [ "${#wireFailed[@]}" != 0 ]; then
		echo "$TOOL: ${#wireFailed[@]} file(s) failed with no answer, the console answering after each: the same command again tries them" >&2
		for name in "${wireFailed[@]}"; do printf "%s: failed: '%s'\n" "$TOOL" "$name" >&2; done
		return 4
	fi
	[ "$wireSkipped" = 0 ]
}

# _wireTreeLinked <dir> <relative-path>: succeeds if a step of the path below the directory is a link here,
# which a get never writes through.
_wireTreeLinked() {
	local at=$1 rest=$2/
	while [ -n "$rest" ]; do
		at=$at/${rest%%/*} rest=${rest#*/}
		[ ! -L "$at" ] || return 0
	done
	return 1
}

# _wireHostName <relative-path>: fails for a name that FAT holds but this host cannot make.
_wireHostName() {
	local LC_ALL=C rest=$1/ part dev
	[[ "$1" != *\?* ]] || return 1
	while [ -n "$rest" ]; do
		part=${rest%%/*} rest=${rest#*/}
		case "$part" in "" | . | ..) return 1 ;; esac
		case "$OSTYPE" in msys* | cygwin*) ;; *) continue ;; esac
		[[ "$part" != *[\\\*\"\<\>\|:]* && "$part" != *[.\ ] ]] || return 1
		for dev in CON PRN AUX NUL 'COM[1-9]' COM¹ COM² COM³ 'LPT[1-9]' LPT¹ LPT² LPT³; do
			! caseless "${part%%.*}" "$dev" || return 1
		done
	done
}

# _wireTreeLeft <manifest> <card listing> <names composed> <name here...>: in one pass, fills five arrays
# with one word per file, `-` where nothing says: what the last put committed (wireLastDigest, wireLastSize,
# wireLastMtime), and what the card holds now (wireCardSize, wireCardMtime).
_wireTreeLeft() {
	local manifest=$1 card=$2 names=$3 left array
	shift 3
	left=$(LC_ALL=C awk 'FILENAME == ARGV[1] { last[substr($0, length($1 $2 $3) + 4)] = $1 " " $2 " " $3; next }
		FILENAME == ARGV[2] { if ($1 == "f") held[tolower(substr($0, length($1 $2 $3) + 4))] = $2 " " $3; next }
		FILENAME == ARGV[3] { rel[FNR] = $0; next }
		{
			split((rel[FNR] in last ? last[rel[FNR]] : "- - -") " " (tolower($0) in held ? held[tolower($0)] : "- -"), w)
			for (i = 1; i <= 5; i++) out[i] = out[i] " " w[i]
		}
		END { for (i = 1; i <= 5; i++) print out[i] }' \
		"$manifest" <(printf '%s\n' "$card") <(printf '%s\n' "$@") <(printf '%s\n' "$names")) || return
	for array in wireLastDigest wireLastSize wireLastMtime wireCardSize wireCardMtime; do
		# shellcheck disable=SC2229 # into the array $array names: bash 3.2 has no nameref
		read -r -a "$array"
	done <<<"$left"
}

# _wireWide <text>: succeeds if the text holds a byte past printable ASCII, whatever the locale.
_wireWide() {
	local LC_ALL=C
	[[ "$1" == *[![:print:]]* ]]
}

# _wireCardDigest <work> <ip> <port> <sd-path> <size>: prints the SHA-256 of the card's file, which is
# fetched for this at the size its listing gave.
_wireCardDigest() { wireGet "$2" "$3" "$4" "$1/card" "$5" </dev/null >/dev/null && sha256File "$1/card"; }

# wirePutTree <ip> <port> <local-dir> <sd-dir> [overwrite|all]: a card file that is not as the last put left
# it is replaced only under `overwrite`; `all` puts every file.
wirePutTree() {
	local ip=$1 port=$2 overwrite=${5:-} from to top manifest kept card names line rel digest size why
	local said had hs hm cs cm i twin early="" st=0
	local -a files=() dirs=() putAt=() putSizes=() putDigests=() hashAt=() hashOf=() hashed=() twinOf=()
	local -a freshAt=() freshOf=() fresh=() sized=()
	local -a wireLastDigest wireLastSize wireLastMtime wireCardSize wireCardMtime # _wireTreeLeft fills them
	local -a excluded=(-name '._*' -o -name .DS_Store -o -name '*.3drop-part' -o -name '*.3drop-meta'
		-o -name "*$WIRE_PUT_TEMP" -o -name "*$WIRE_PUT_BACKUP")
	from=$(_wireTreeDir "$3") to=$(_wireTreeDir "$4") top=$(_wireTreeTop "$to")
	wireSent=0 wireSkipped=0 wireFailed=()
	if [ ! -d "${from:-/}" ]; then
		echo "$TOOL: '$3' is not a directory" >&2
		return 1
	fi
	# A name with a line break, which FAT never holds, is reported once with its tree, because the walk below
	# reads one line per name and a line break would split it.
	while IFS= read -r -d '' line; do
		rel=${line#"$from/"}
		_wireTreeSkip "${rel//$'\n'/?}" "a line break in its name, which FAT never holds"
	done < <(LC_ALL=C find -H "${from:-/}" -mindepth 1 \( "${excluded[@]}" \) -prune -o -name "*"$'\n'"*" -prune -print0 2>/dev/null)
	while IFS= read -r line; do
		rel=${line#"$from/"}
		if [ "$rel" = "$line" ]; then # not a path: find's message about a folder it could not read
			why=${line##*: } rel=${line#find: } rel=${rel%: *}
			[[ "$rel" != \'*\' ]] || rel=${rel:1:${#rel}-2} # strip GNU's quotes
			_wireTreeSkip "${rel#"$from/"}" "this host cannot read it: $why"
		elif _wireTreeExcluded "${rel##*/}"; then
			continue
		elif [ -L "$from/$rel" ]; then
			printf "%s: left out '%s': a link, which a put never follows\n" "$TOOL" "$rel" >&2
		elif [ -d "$from/$rel" ]; then
			dirs+=("$rel")
		elif [ ! -f "$from/$rel" ]; then
			printf "%s: left out '%s': no file or folder (a FIFO, socket or device), which a put never reads\n" "$TOOL" "$rel" >&2
		else
			files+=("$rel")
		fi
	done < <(LC_ALL=C find -H "${from:-/}" -mindepth 1 \( "${excluded[@]}" \) -prune -o ! -path "*"$'\n'"*" -print 2>&1 | LC_ALL=C sort)
	if [ "${#files[@]}" = 0 ]; then
		if [ "${#dirs[@]}" = 0 ]; then
			echo "$TOOL: no files under '$3'" >&2
			return 1
		fi
		_wireTreeEmpties "$ip" "$port" "$to" "$top" || return
		_wireTreeDone "$wireSent" 0 "put in $top"
		return
	fi
	# A name that FAT takes for an earlier one (ASCII case: A.bin and a.bin, both here on a case-sensitive
	# host) would be put over it, just as a get skips what this host takes for another.
	while read -r i twin; do twinOf[i]=$twin; done < <(printf '%s\n' "${files[@]}" |
		LC_ALL=C awk '{ k = tolower($0) } k in first { print NR - 1, first[k]; next } { first[k] = NR - 1 }')
	manifest=$(_wireTreeManifest push "$port" "$(wirePath "$top")" "$(CDPATH='' cd -- "${from:-/}" && pwd -P)") &&
		_wireTreeKeep "$manifest" || return
	kept=$manifest
	[ "$overwrite" != all ] || kept=/dev/null
	# The card's copy is listed because a reformat, another card or a delete leaves the manifest stale.
	# A listing that ended early (wireLs says so, after what came) still serves: each file it did not name is
	# asked for alone.
	card=$(wireLs "$ip" "$port" "$top" rt 2>&1) || {
		st=$?
		case "$card" in
		*" ls rejected notfound "*) card="" ;;
		*"$TOOL: $WIRE_ENDED_EARLY ("*)
			echo "$TOOL: the listing of '$top' ended early: each file it did not name is asked for alone" >&2
			early=1
			;;
		*)
			echo "$TOOL: could not list '$top' on the console: $card" >&2
			return "$st"
			;;
		esac
	}
	_wireNFC "$(printf '%s\n' "${files[@]}")"
	names=$wireNFC
	_wireTreeLeft "$kept" "$card" "$names" "${files[@]}" || return
	# Each file the card holds as the last put left it is hashed by one openssl before any is sent, so an
	# unchanged file then goes by with no process of its own. The other files except a twin are hashed and
	# sized together too (an openssl and a wc), not a process each.
	for i in "${!files[@]}"; do
		[ -z "${twinOf[i]:-}" ] || continue
		had=${wireLastDigest[i]:--} hs=${wireLastSize[i]:--} hm=${wireLastMtime[i]:--} cs=${wireCardSize[i]:--} cm=${wireCardMtime[i]:--}
		if [ "$had" != - ] && [ "$cs" != - ] && [ "$cs" = "$hs" ] && { [ "$hm" = - ] || [ "$cm" = "$hm" ]; }; then
			hashAt+=("$i") hashOf+=("$from/${files[i]}")
		else
			freshAt+=("$i") freshOf+=("$from/${files[i]}")
		fi
	done
	if [ "${#hashAt[@]}" != 0 ] && _wireHashes "${hashOf[@]}"; then
		for i in "${!hashAt[@]}"; do hashed[hashAt[i]]=${wireHashes[i]}; done
	fi
	if [ "${#freshAt[@]}" != 0 ] && _wireHashes "${freshOf[@]}" && _wireSizes "${freshOf[@]}"; then
		for i in "${!freshAt[@]}"; do fresh[freshAt[i]]=${wireHashes[i]} sized[freshAt[i]]=${wireSizes[i]}; done
	fi
	for i in "${!files[@]}"; do
		rel=${files[i]}
		if [ -n "${twinOf[i]:-}" ]; then
			_wireTreeSkip "$rel" "the console takes it for '${files[twinOf[i]]}'"
			continue
		fi
		had=${wireLastDigest[i]:--} hs=${wireLastSize[i]:--} hm=${wireLastMtime[i]:--} cs=${wireCardSize[i]:--} cm=${wireCardMtime[i]:--}
		[ "${hashed[i]:-}" != "$had" ] || continue
		# A digest the batches gave is never asked of the file again. A file they could not read, or a changed
		# file that the first batch hashed but did not size, is asked for on its own, and reported if it
		# cannot be read.
		if ! digest=${hashed[i]:-${fresh[i]:-$(sha256File "$from/$rel" 2>/dev/null)}} ||
			! size=${sized[i]:-$(fileSize "$from/$rel" 2>/dev/null)}; then
			# Run again for the reason, and only now, because a warning on success must never join a digest.
			why=$({ sha256File "$from/$rel" && fileSize "$from/$rel"; } 2>&1 >/dev/null || :)
			why=${why%%$'\n'*}
			_wireTreeSkip "$rel" "this host cannot read it: ${why##*: }"
			continue
		fi
		# A file the listing did not name may still be on the card (the listing ended early, or the name is
		# non-ASCII in another case), so it is asked for alone.
		if [ -z "$overwrite" ] && [ "$cs" = - ] && { [ -n "$early" ] || _wireWide "$rel"; }; then
			st=0
			line=$(wireLs "$ip" "$port" "$to/$rel" dt 2>&1) || st=$?
			case "$st:$line" in
			0:"f "*) line=${line#f } cs=${line%% *} line=${line#* } cm=${line%% *} ;;
			0:* | 1:*" ls rejected notfound "*) ;;
			*)
				printf '%s\n' "$line" >&2
				_wireTreeFailed "$st" "$rel" put "$line" "$size" || return
				continue
				;;
			esac
		fi
		# Only a card file that is as the last put left it is replaced, unless the word says otherwise.
		# That is judged by size and mtime or, where an mtime is missing, by its bytes, which are fetched.
		why=""
		if [ "$cs" = - ]; then
			:
		elif [ "$had" = - ]; then
			why="the console has one this host never put"
		elif [ "$cs" != "$hs" ] || { [ "$hm" != - ] && [ "$cm" != "$hm" ]; }; then
			why="it changed on the console since the last put"
		elif [ "$digest" = "$had" ]; then
			continue
		elif [ -z "$overwrite" ] && { [ "$hm" = - ] || [ "$cm" = - ]; }; then
			st=0
			said=$(_wireWork _wireCardDigest "$ip" "$port" "$to/$rel" "$cs" 2>&1) || st=$?
			case "$st:$said" in
			0:*) [ "${said##*$'\n'}" = "$had" ] || why="it changed on the console since the last put" ;;
			1:*" get rejected notfound "*) ;; # gone since the listing, so the file is put
			1:*)
				printf '%s\n' "$said" >&2
				why="the console's could not be read to tell whether it changed since the last put"
				;;
			*)
				printf '%s\n' "$said" >&2
				_wireTreeFailed "$st" "$rel" put "$said" "$size" || return
				continue
				;;
			esac
		fi
		_wireTreeCopy "$why" "$overwrite" "$rel" "$size" put "" wireFilePut "$ip" "$port" "$from/$rel" "$to/$rel" "" "$digest" || return
		[ -n "$wireCopied" ] || continue
		printf '%s\n' "$digest $size - $rel" >>"$manifest" || return
		putAt+=("$i") putSizes[i]=$size putDigests[i]=$digest
		wireSent=$((wireSent + 1))
	done
	# Record the card's mtime of each file put, which only a listing tells, so the next put sees a change made
	# after it. A file this listing does not name keeps `-`, and its bytes decide then.
	if [ "${#putAt[@]}" != 0 ]; then
		card=$(wireLs "$ip" "$port" "$top" rt 2>/dev/null) || :
		if _wireTreeLeft /dev/null "$card" "$names" "${files[@]}"; then
			for i in "${putAt[@]}"; do
				cs=${wireCardSize[i]:--} cm=${wireCardMtime[i]:--}
				if [ "$cs" = "${putSizes[i]}" ] && [ "$cm" != - ]; then
					printf '%s\n' "${putDigests[i]} $cs $cm ${files[i]}" >>"$manifest" || return
				fi
			done
		fi
	fi
	_wireTreeEmpties "$ip" "$port" "$to" "$top" "$card" || return
	_wireTreeDone "$wireSent" "${#files[@]}" "put in $top"
}
# _wireTreeEmpties <ip> <port> <sd-dir> <top> [card listing]: makes each folder that has nothing to put on
# the card, unless it holds it. Only the deepest are made, since the console's mkdir makes parents.
_wireTreeEmpties() {
	local rel card said st
	[ "${#dirs[@]}" != 0 ] || return 0
	if [ "$#" -ge 5 ]; then card=$5; else card=$(wireLs "$1" "$2" "$4" rt 2>/dev/null) || card=""; fi
	while IFS= read -r rel; do
		if said=$(wireMkdir "$1" "$2" "$3/$rel" 2>&1 >/dev/null); then
			printf "%s: made '%s/': a folder with no file to put\n" "$TOOL" "$rel" >&2
		else
			st=$?
			printf '%s\n' "$said" >&2
			_wireTreeFailed "$st" "$rel/" made "$said" 0 || return
		fi
	done < <(LC_ALL=C awk 'FILENAME == ARGV[1] { if ($1 == "d") card[tolower(substr($0, length($1 $2 $3) + 4))]; next }
		{ kind[++n] = substr($0, 1, 1); p = path[n] = substr($0, 3); while (sub(/\/[^\/]*$/, "", p)) under[p] }
		END { for (i = 1; i <= n; i++) if (kind[i] == "d" && !(path[i] in under) && !(tolower(path[i]) in card)) print path[i] }' \
		<(printf '%s\n' "$card") <(
			printf 'd %s\n' "${dirs[@]}"
			[ "${#files[@]}" = 0 ] || printf 'f %s\n' "${files[@]}"
		))
}

# _wireTreePlan <manifest> <folded listing> <listing>: prints each listing line preceded by
# `<landed 1|0> <sha|-> <twin line|0>`, where the twin is an earlier name this host may take for it.
# It makes one pass, not a search per file.
_wireTreePlan() {
	awk 'FILENAME == ARGV[1] { name = substr($0, length($1 $2 $3) + 4); had[name] = $1 " " $2; dig[name] = $3; next }
		FILENAME == ARGV[2] { key[FNR] = tolower(substr($0, length($1 $2 $3) + 4)); next }
		{
			name = substr($0, length($1 $2 $3) + 4)
			landed = $3 != "-" && had[name] == $2 " " $3
			twin = key[FNR] in first ? first[key[FNR]] : 0
			if (!twin) first[key[FNR]] = FNR
			print landed, name in dig ? dig[name] : "-", twin, $0
		}' "$@"
}

# wireGetTree <ip> <port> <sd-dir> <local-dir> [overwrite]: no host mtime is read or set. A name this host
# cannot make, or one reached through a link, is skipped and never renamed.
wireGetTree() { _wireWork _wireGetTree "$@"; }
_wireGetTree() {
	local work=$1 ip=$2 port=$3 overwrite=${6:-} from to top manifest line landed digest twin kind size
	local mtime rel why glob step flags=rt early="" n=0 total=0 got=0 st=0 i
	local -a rels=() hashAt=() hashOf=() hashed=()
	from=$(_wireTreeDir "$4") to=$(_wireTreeDir "$5")
	if glob=$(_wirePattern "$from"); then
		# Only the folder's own names are listed here. A folder the pattern matches is walked afterwards.
		step=${from##*/} from=${from%/*} flags=t
	fi
	top=$(_wireTreeTop "$from")
	wireSkipped=0 wireFailed=()
	_wireLsLines "$work" "$ip" "$port" "$top" "$flags" >"$work/listing" || return
	[ "$wireOutcome" = ok ] || early="$wireDetail $wireResult"
	# A file lists as itself, as one line named like the path's last step. `ls d` tells it from a folder that
	# holds only its namesake.
	_wireNFC "${from##*/}"
	if { IFS= read -r line && ! IFS= read -r _; } <"$work/listing" && [[ "$line" == "f "*" $wireNFC" ]] &&
		[[ "$(wireLs "$ip" "$port" "$top" d 2>/dev/null)" == "f "* ]]; then
		echo "$TOOL: '$4' is a file, not a directory: get it without r" >&2
		return 1
	fi
	if [ -n "$glob" ]; then
		while IFS= read -r line; do
			rel=${line#* * * }
			caseless "$rel" "$glob" || continue
			_wireTreeExcluded "$rel" && continue
			printf '%s\n' "$line"
			# The loop skips a name this host cannot make.
			if [ "${line%% *}" != d ] || ! _wireHostName "$rel"; then continue; fi
			st=0
			_wireLsLines "$work" "$ip" "$port" "$from/$rel" rt >"$work/walked" || st=$?
			# A folder the console refuses on its own is reported, as a file is;
			# busy, gone or stranded stops the copy.
			if [ "$st" != 0 ] && { [ "$st" != 1 ] || [ "$wireDetail" = stranded ]; }; then return "$st"; fi
			[ "$wireOutcome" = ok ] || early="$wireDetail $wireResult"
			dir="$rel/" LC_ALL=C awk '{ head = $1 " " $2 " " $3 " "; print head ENVIRON["dir"] substr($0, length(head) + 1) }' "$work/walked"
		done <"$work/listing" >"$work/named"
		if [ ! -s "$work/named" ]; then
			echo "$TOOL: nothing in '$top' matches '$step'" >&2
			return 1
		fi
		mv -f "$work/named" "$work/listing"
	fi
	if ! why=$(mkdir -p -- "${to:-/}" 2>&1); then
		echo "$TOOL: could not make '$5' here: ${why##*: }" >&2
		return 1
	fi
	manifest=$(_wireTreeManifest pull "$port" "$(wirePath "$top")" "$(CDPATH='' cd -- "${to:-/}" && pwd -P)") &&
		_wireTreeKeep "$manifest" || return
	_wireNFC "$(<"$work/listing")"
	printf '%s\n' "$wireNFC" >"$work/folded"
	# Run in this locale, which folds as the host may. macOS awk aborts on a name that isn't UTF-8 there.
	_wireTreePlan "$manifest" "$work/folded" "$work/listing" >"$work/plan" 2>/dev/null ||
		LC_ALL=C _wireTreePlan "$manifest" "$work/folded" "$work/listing" >"$work/plan" || return
	# Each file here as the last get landed it is hashed in one batch, as a put tree's files are. None is
	# read through a link or from outside the folder.
	i=0
	while IFS= read -r line; do
		i=$((i + 1)) landed=${line%% *} line=${line#* } digest=${line%% *} line=${line#* * } kind=${line%% *}
		rel=${line#* * * }
		if [ "$landed:$kind" = 1:f ] && [ "$digest" != - ] && ! _wireTreeExcluded "$rel" && _wireHostName "$rel" && ! _wireTreeLinked "$to" "$rel" &&
			[ -f "$to/$rel" ]; then
			hashAt+=("$i") hashOf+=("$to/$rel")
		fi
	done <"$work/plan"
	if [ "${#hashAt[@]}" != 0 ] && _wireHashes "${hashOf[@]}"; then
		for i in "${!hashAt[@]}"; do hashed[hashAt[i]]=${wireHashes[i]}; done
	fi
	while IFS= read -r line; do
		landed=${line%% *} line=${line#* } digest=${line%% *} line=${line#* } twin=${line%% *} line=${line#* }
		kind=${line%% *} line=${line#* } size=${line%% *} line=${line#* } mtime=${line%% *} rel=${line#* }
		n=$((n + 1)) rels[n]=$rel
		if _wireTreeExcluded "$rel"; then
			continue
		fi
		[ "$kind" = d ] || total=$((total + 1))
		if ! _wireHostName "$rel"; then
			_wireTreeSkip "$rel" "no file on this host can have that name"
			continue
		elif _wireTreeLinked "$to" "$rel"; then
			_wireTreeSkip "$rel" "a link here, which a get never writes through"
			continue
		elif [ "$kind" = d ]; then
			if [ ! -d "$to/$rel" ] && ! why=$(mkdir -p -- "$to/$rel" 2>&1); then
				_wireTreeSkip "$rel" "no folder could be made for it here: ${why##*: }"
			fi
			continue
		elif [ "$twin" != 0 ] && [ "$to/$rel" -ef "$to/${rels[twin]}" ]; then
			_wireTreeSkip "$rel" "this host takes it for '${rels[twin]}'"
			continue
		fi
		# Only a file that is here as the last get landed it is replaced, unless the word says otherwise.
		why=""
		if [ ! -e "$to/$rel" ]; then
			:
		elif [ "$digest" = - ]; then
			why="this folder has one no get here landed"
		elif [ "${hashed[n]:-}" = "$digest" ]; then
			continue
		elif [ "$(sha256File "$to/$rel" 2>/dev/null)" != "$digest" ]; then
			why="it changed here since the last get"
		elif [ "$landed" = 1 ]; then
			continue
		fi
		_wireTreeCopy "$why" "$overwrite" "$rel" "$size" fetched "$to" wireGet "$ip" "$port" "$from/$rel" "$to/$rel" "$size" || return
		[ -n "$wireCopied" ] || continue
		# No message if the digest cannot be taken; the file then counts as one no get here landed.
		digest=$(sha256File "$to/$rel" 2>/dev/null) && { printf '%s\n' "$size $mtime $digest $rel" >>"$manifest" || return; }
		got=$((got + 1))
	done <"$work/plan"
	st=0
	_wireTreeDone "$got" "$total" "fetched into ${to:-/}" || st=$?
	if [ -n "$early" ]; then
		why=$(_wireWhy ls rejected "${early% *}" "${early##* }")
		[ -z "$why" ] || echo "$TOOL: $why" >&2
		echo "$TOOL: $WIRE_ENDED_EARLY ($early): only what it named was fetched" >&2
		[ "$st" = 4 ] || st=1
	fi
	return "$st"
}

# ---- Seeds -----------------------------------------------------------------------------------

# TITLE stands for the title id. country=US is used because every country gets the same seed.
SEED_URL='https://kagiya-ctr.cdn.nintendo.net/title/0xTITLE/ext_key?country=US'
# The server's issuer, Nintendo's own CA, is pinned. The console never replaces a seed it holds, so a wrong
# seed given by an on-path host would stay. It is Nintendo Class 2 CA - G3 (Nintendo of America),
# self-signed, valid to 2049, SHA-256
# 58054CAA85DA74B2B9F3407B17BB8428040E46A9159A1AFB9F58CADAC27642C0; the seed server's
# *.cdn.nintendo.net certificate chains to it. To recheck it, run
# `openssl s_client -connect kagiya-ctr.cdn.nintendo.net:443 -CAfile tools/nintendo-class2-ca-g3.pem`;
# the output ends with "Verify return code: 0 (ok)".
SEED_CA=$ROOT/tools/nintendo-class2-ca-g3.pem
# A server or route that stalls must not hold an install's exit.
SEED_TIMEOUT=15

# seedFetch <title-id>: prints 32 hex digits, or nothing for a title with no seed. Returns 1 when no fetch
# the pinned CA vouches for could be made, and 4 for no answer.
seedFetch() { _wireWork _seedFetch "$@"; }
_seedFetch() {
	local LC_ALL=C work=$1 titleID status said hex st=0
	titleID=$(wireTitleID "$2") || return
	: >"$work/seed"
	# -q comes first, so no .curlrc can turn a check off. The pin alone is used, because a Linux curl adds its
	# CA directory to any --cacert, and macOS's curl falls back on the keychain. Schannel fails a CA that
	# publishes no revocation list.
	status=$(OPENSSL_X509_TEA_DISABLE=1 curl -q -sS --max-time "$SEED_TIMEOUT" --cacert "$SEED_CA" \
		--capath /dev/null --ssl-no-revoke -o "$work/seed" -w '%{http_code}' "${SEED_URL/TITLE/$titleID}" \
		2>"$work/said") || st=$?
	read -r said <"$work/said" || :
	said=${said%$'\r'} # a native curl on Windows ends its line with CR
	if [ "$st" != 0 ]; then
		[ "$st" != 127 ] || said="no curl here to fetch it with"
		echo "$TOOL: the seed was not fetched ($said), so nothing went to the console: if this title is seed-encrypted, it will not boot until it has its seed (device.sh seed <ip> $titleID <seed-file> takes it from a 16-byte file)" >&2
		# A handshake cut short (35) counts as no answer.
		# A certificate the pin refuses (60), or no curl, returns 1.
		case "$st" in 6 | 7 | 18 | 28 | 35 | 52 | 55 | 56) return 4 ;; *) return 1 ;; esac
	fi
	hex=$(fileHex "$work/seed")
	case "$status:${#hex}" in
	200:32) echo "$hex" ;;
	404:*) ;;
	*)
		echo "$TOOL: the seed server answered $status with $((${#hex} / 2)) bytes, not a seed" >&2
		return 1
		;;
	esac
}

# wireSeed <ip> <port> <title-id> [seed-file]: sends the seed from the file (16 bytes, as in FBI's
# sdmc:/fbi/seed/<title-id>.dat) or else from the server. Nothing is sent for a title with no seed.
wireSeed() {
	local titleID seed
	titleID=$(wireTitleID "$3") || return
	if [ -n "${4:-}" ]; then
		if [ "$(fileSize "$4")" != 16 ]; then
			echo "$TOOL: $4 is not a seed file, which holds 16 bytes" >&2
			return 1
		fi
		seed=$(fileHex "$4")
	else
		seed=$(seedFetch "$titleID") || return
		[ -n "$seed" ] || return 0
	fi
	wireSeedHex "$1" "$2" "$titleID" "$seed"
}
# wireSeedHex <ip> <port> <title-id> <seed>: sends a seed already in hand, given as 32 hex digits (16 bytes).
wireSeedHex() {
	local titleID seed
	titleID=$(wireTitleID "$3") && seed=$(requireHex 32 "$4" seed) || return
	wireMutation "$1" "$2" "$(wireTimeout)" seed "title:$titleID" "$seed"
}

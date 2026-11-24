#!/usr/bin/env bash
# The host tools' kit: cDefine, each host copy of a device fact, every script's usage, a tool's
# scratch and signals, ports, ids and pieces, the build image, the UniStore stamp, and test timeouts.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
export TMPDIR="$TMP" XDG_CACHE_HOME="$TMP/cache" XDG_RUNTIME_DIR="$TMP/run"
# shellcheck source=tools/wire.sh
. "$ROOT/tools/wire.sh"

# A define is read whole: a hex value is not read as its leading 0, and a name, a string or two definitions
# give nothing.
mkdir "$TMP/src"
cat >"$TMP/src/pins.h" <<'EOF'
#define DECIMAL 49152
#define COMMENTED 512 // the request line and its NUL
#define TABBED 12		 // the widest request
#define BLOCK 7 /* seven */
#define HEX 0x305B14ULL
#define SUFFIXED 600000u
#define PRODUCT (24u * 1024 * 1024)
#define QUOTIENT (300000 / 2)
#define SHIFTED (1u << 20)
#define NAMED (OTHER_MS * 6)
#define PARTIAL 12 + x
#define STRING "/3ds/3drop"
#define TWICE 1
#define TWICE 2
#define SUFFIXES LL
#define OPERATORS ()
EOF
for pin in DECIMAL:49152 COMMENTED:512 TABBED:12 BLOCK:7 HEX:3169044 SUFFIXED:600000 PRODUCT:25165824 QUOTIENT:150000 SHIFTED:1048576; do
	expect "$(ROOT=$TMP cDefine pins.h "${pin%%:*}")" "${pin#*:}" "cDefine ${pin%%:*}"
done
for name in NAMED PARTIAL STRING TWICE SUFFIXES OPERATORS MISSING DECIMA; do
	if out=$(ROOT=$TMP cDefine pins.h "$name" 2>/dev/null) || [ -n "$out" ]; then fail "cDefine read $name as '$out'"; fi
done

# wire.sh keeps its own copies so that it runs without the device's source; each copy is checked against that
# source.
expect "$REQUEST_LINE_MAX" $(($(cDefine protocol.h REQUEST_LINE_MAX) - 1)) "the longest request line (the device counts its NUL)"
expect "$GZIP_MIN_BYTES" "$(cDefine sdXfer.c GZIP_MIN_BYTES)" "the smallest file worth gzip"
expect "$WIRE_REPLY_MAX" $(($(cDefine protocol.h REPLY_MAX) - 1)) "the longest reply line (the device counts its NUL)"
expect "$WIRE_UNIT" "$(cDefine hash.h DIGEST_UNIT)" "the unit a digest is taken over"
expect "$WIRE_PROTO" "$(cDefine protocol.h PROTOCOL_EPOCH)" "the protocol epoch every signed reply names"
expect "$WIRE_DEFAULT_PORT" "$(cDefine app.h DEFAULT_PORT)" "the port 3Drop listens on"
expect "$WIRE_FILE_MAX" "$(cDefine cmdFs.c FAT32_FILE_MAX)" "the largest file a put takes"
expect "$WIRE_PUT_TEMP" "$(sed -n 's/^#define SD_PUT_TEMP "\(.*\)"$/\1/p' "$ROOT/src/path.h")" "the temp beside a put's path"
expect "$WIRE_PUT_BACKUP" "$(sed -n 's/^#define SD_PUT_BACKUP "\(.*\)"$/\1/p' "$ROOT/src/path.h")" "the backup beside a put's path"
# What must fit: the widest trailer (its NUL as a newline), its MAC and a newline.
[ $(($(cDefine protocol.h TRAILER_MAX) + 2 * $(cDefine auth.h MAC_SIZE) + 1)) -le "$WIRE_TRAILER_WINDOW" ] ||
	fail "the widest trailer and its MAC do not fit the $WIRE_TRAILER_WINDOW bytes the host searches"
expect "$(wireTimeout)" $(($(cDefine net.h HOST_REPLY_WAIT_MS) / 1000)) "a host's wait for a reply"
# A kept nonce's last use leaves its request at least a connect's control timeout of the half life.
[ $((WIRE_NONCE_KEEP + $(wireTimeout))) -lt $(($(cDefine auth.c NONCE_TTL_MS) / 2000)) ] ||
	fail "a kept nonce is used for $WIRE_NONCE_KEEP s, leaving its request under $(wireTimeout) s of the half life the device hands one out with"
# shellcheck disable=SC2086 # one extension per word
hostExts=$(printf '%s\n' $GZIP_SKIP_EXTS | sort)
deviceExts=$(awk -F'"' '/GZIP_SKIP_EXTS\[\] = \{/ { on = 1; next }
	on && /^};/ { exit }
	on { for (i = 2; i < NF; i += 2) print $i }' "$ROOT/src/sdXfer.c" | sort)
expect "$hostExts" "$deviceExts" "the extensions gzip is never tried on"
# Each refusal word that src/urls.c explains on the console's screen has a sentence on the host too
# (_wireWhy), except the words of the console's own downloads, which no reply to the host carries.
fetchOnly=" connect tls no-tls no-curl redirect no-length length net canceled thread bundle too-big plain-http "
while read -r word; do
	[[ "$fetchOnly" == *" $word "* ]] || [ -n "$(_wireWhy put rejected "$word")" ] ||
		fail "src/urls.c has words for '$word', and wire.sh's _wireWhy has none"
done < <(sed -n 's/^[[:blank:]]*{"\([a-z-]*\)", .*/\1/p' "$ROOT/src/urls.c")

# 3Drop's own id is the one its RSF gives makerom (the builds take it from there too).
expect "$(selfTitleID)" 0004000003d37200 "3Drop's title id"
mkdir "$TMP/res"
sed 's/^\( *UniqueId *: *\).*/\10xABCDE/' "$ROOT/res/buildCIA.rsf" >"$TMP/res/buildCIA.rsf"
expect "$(ROOT=$TMP selfTitleID)" 000400000abcde00 "the id another RSF gives"

# Every script called with no arguments prints its usage, which is its header, on stderr and exits 2; the
# scripts that take no arguments (they build, gate, download or serve) are not run. A tool's refusal names the
# tool, as its own messages do.
for script in "$ROOT"/tools/*.sh "$ROOT"/tools/bench/*.sh; do
	name=${script##*/}
	case "$name" in build.sh | check.sh | lint.sh | makeCIA.sh | bootstrapCIPackagingTools.sh | bridge.sh) continue ;; esac
	[ "$(head -c 2 "$script")" = '#!' ] || continue # a library, sourced
	st=0
	bash "$script" </dev/null >"$TMP/usage.out" 2>"$TMP/usage.err" || st=$?
	if [ "$st" != 2 ] || [ -s "$TMP/usage.out" ] || [[ "$(head -n 1 "$TMP/usage.err")" != "$name "* ]]; then
		fail "$name with no arguments exited $st, saying: $(head -n 2 "$TMP/usage.err")"
	fi
done
# --help is the usage asked for, not a call the tool cannot take. It goes to stdout and exits 0. A flag a
# tool does not know gets the usage on stderr and exit status 2, and for drop.sh and dropBatch.sh that usage
# is the user's lines alone, none of the notes below them.
for name in drop.sh dropBatch.sh device.sh deploy.sh sync.sh bridge.sh; do
	st=0
	bash "$ROOT/tools/$name" --help </dev/null >"$TMP/help.out" 2>"$TMP/help.err" || st=$?
	if [ "$st" != 0 ] || [ -s "$TMP/help.err" ] || [[ "$(head -n 1 "$TMP/help.out")" != "$name "* ]]; then
		fail "$name --help exited $st, saying: $(head -n 2 "$TMP/help.out" "$TMP/help.err")"
	fi
done
for pair in drop.sh:nonce dropBatch.sh:"benchmark run"; do
	run 2 bash "$ROOT/tools/${pair%%:*}" --nope
	[[ "$(head -n 1 "$TMP/err")" == "${pair%%:*} "* && "$(cat "$TMP/err")" != *"${pair#*:}"* && ! -s "$TMP/out" ]] ||
		fail "${pair%%:*} with a flag it does not know said: $(cat "$TMP/err")"
done
# A first word that is not a command, such as the <ip> put first, is named on its own, with where the command
# goes and where the list of commands is, and nothing is asked. An address that no exchange could take is
# named above the usage.
run 2 bash "$ROOT/tools/device.sh" 192.168.1.20 stats self
expect "$(cat "$TMP/out")$(cat "$TMP/err")" "device: '192.168.1.20' is not a command: the command comes first, as in device.sh stats <ip> self; device.sh --help lists them" \
	"device.sh with the address first"
run 2 bash "$ROOT/tools/device.sh" stats 192.168.1.20:99999
[[ "$(head -n 1 "$TMP/err")" == "device: not an address: 192.168.1.20:99999" && "$(sed -n 2p "$TMP/err")" == "device.sh "* ]] ||
	fail "device.sh with a port past 65535 said: $(head -n 2 "$TMP/err")"
# An empty argument, such as an unset variable's, is a usage error before any request, because the console
# would answer only `grammar`, which reads as an old console.
emptyArg() {
	local st=0
	bash "$ROOT/tools/device.sh" "$@" </dev/null >/dev/null 2>&1 || st=$?
	expect "$st" 2 "device.sh $1 with an empty argument"
}
for verb in delete mkdir hash ls; do emptyArg "$verb" 127.0.0.1 ""; done
emptyArg rename 127.0.0.1 /a ""
# --ticket is the user's word, which the console cannot check: its usage says so.
[[ "$(bash "$ROOT/tools/device.sh" 2>&1)" == *"--ticket"*"its ticket too, only one you created: the console cannot tell"* ]] ||
	fail "device.sh's usage does not say --ticket rests on the user's word"
# A 3dsx's stdio listener has to be where the console dials, which under WSL's default network is
# not the VM: its usage says so.
[[ "$(bash "$ROOT/tools/device.sh" 2>&1)" == *"under WSL 2's default network is Windows, not the VM"* ]] ||
	fail "device.sh's usage does not say where a 3dsx's stdio listener must be"
# Its exit list has the two statuses beyond the common ones, as docs/troubleshooting.md's Exit status table does.
[[ "$(bash "$ROOT/tools/device.sh" 2>&1)" == *", 42 a send"*"141 a \`get ... -\` whose reader stopped early"* ]] ||
	fail "device.sh's usage does not list exit 42 and 141"
# A key its openssl cannot derive is the dependency's failure, not the key's: an openssl that lacks
# enc -pbkdf2 fails as one older than 1.1.1 does, and the tool names what it needs.
mkdir "$TMP/noPbkdf2"
printf '#!/bin/sh\nexit 1\n' >"$TMP/noPbkdf2/openssl"
chmod +x "$TMP/noPbkdf2/openssl"
st=0
failed=$(PATH="$TMP/noPbkdf2:$PATH" THREEDROP_KEY=wxyz-2345-abcd bash "$ROOT/tools/device.sh" stats 127.0.0.1 2>&1) || st=$?
expect "$st" 1 "a tool whose openssl cannot derive the key"
[[ "$failed" == *"could not derive the key: openssl needs enc -pbkdf2 -iter"* ]] ||
	fail "a derivation failure does not name openssl: $failed"

run 2 env THREEDROP_DEPLOY_WAIT=60s "$ROOT/tools/deploy.sh" 127.0.0.1
saidLine "deploy: THREEDROP_DEPLOY_WAIT must be a whole number of seconds" "a malformed deploy wait"
# A wait set in .env is checked as well, before anything is built or asked, because wireConsole reads the
# .env.
mkdir -p "$TMP/envWait/tools"
cp "$ROOT"/tools/{lib,wire,cia,deploy}.sh "$TMP/envWait/tools/"
printf 'THREEDROP_KEY=abcd-efgh-2345\nTHREEDROP_DEPLOY_WAIT=60s\n' >"$TMP/envWait/.env"
run 2 env -u THREEDROP_KEY "$TMP/envWait/tools/deploy.sh" 127.0.0.1
saidLine "deploy: THREEDROP_DEPLOY_WAIT must be a whole number of seconds" "a malformed deploy wait in .env"
# A packaging tool that is not there is named with where it comes from, not by its path alone.
run 1 env BANNERTOOL="$TMP/none/bannertool" MAKEROM="$TMP/none/makerom" "$ROOT/tools/makeCIA.sh"
[[ "$(cat "$TMP/err")" == "makeCIA: not found or not executable: $TMP/none/bannertool ("*"point \$BANNERTOOL and \$MAKEROM at them, or put them on your PATH or in tools/bin/"* ]] ||
	fail "makeCIA.sh without bannertool said: $(cat "$TMP/err")"

# A tool's scratch is its own: a SCRATCH in the environment belongs to some other program and is never filled.
mkdir "$TMP/theirs"
# shellcheck disable=SC2016 # the inner shell's own $1 and $SCRATCH
[ "$(SCRATCH=$TMP/theirs bash -c '. "$1/tools/lib.sh"; scratch; echo "$SCRATCH"' _ "$ROOT")" != "$TMP/theirs" ] ||
	fail "scratch took a SCRATCH from the environment"
# A tool's scratch goes with it however it ends, and its failures stay failures, because bash 3.2 exits 0 from
# an unset variable in a process with an EXIT trap, so the tool's work runs in a child. It goes on with the
# arguments it was given, before it shifted any.
# shellcheck disable=SC2016 # expanded when the tool runs
printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' ". \"$ROOT/tools/lib.sh\"" 'run=$1' 'shift' scratchTool \
	'echo "$SCRATCH" >"$TMPDIR/scratchWas"' 'if [ "$run" = unset ]; then echo "$notSet"; fi' \
	'if [ "$run" = hold ]; then sleep 30; fi' 'exit "$run"' >"$TMP/tool.sh"
for run in 0 3 unset; do
	st=0
	bash "$TMP/tool.sh" "$run" 2>/dev/null || st=$?
	want=$run
	[ "$run" != unset ] || want=1
	expect "$st" "$want" "a tool's status, run with '$run'"
	read -r was <"$TMP/scratchWas"
	[[ "$was" == "$TMP"/3drop.* && ! -e "$was" ]] || fail "a tool's scratch '$was' outlived it, run with '$run'"
done
# A TERM to the tool's own pid alone (launchd, an IDE's stop button) runs its trap at once and ends its work
# in the child too, never leaving that running with its scratch gone.
rm -f "$TMP/scratchWas"
perl -e 'setpgrp(0, 0); exec(@ARGV) or die' bash "$TMP/tool.sh" hold 2>"$TMP/held.err" &
leader=$!
waitFor "$TMP/scratchWas" 10
kill -TERM "$leader"
wait "$leader" || :
tryFor 60 gone "-$leader" || {
	kill -KILL -- "-$leader" 2>/dev/null || :
	fail "a TERM to a tool alone left its child's work running"
}
read -r was <"$TMP/scratchWas"
[ ! -e "$was" ] || fail "a TERM to a tool alone left its scratch '$was'"
expect "$(cat "$TMP/held.err")" "" "what a tool said as a TERM to it alone ended its child"
# A tool stopped the way bridge.pl stops device.sh (TERM to its group, the leader reaped, KILL to the rest)
# leaves no scratch, even though a process the TERM missed writes into it. macOS skips a child forked as a
# group signal goes (2 of 100 stops of a forking loop), and a process ignoring TERM stands in for it here.
# shellcheck disable=SC2016 # expanded when the tool runs
printf '%s\n' '#!/usr/bin/env bash' ". \"$ROOT/tools/lib.sh\"" scratchTool \
	'perl -e '\''$SIG{TERM} = "IGNORE"; alarm 20; open(my $m, ">", $ARGV[1]); print $m 1; close $m; while (1) { open(my $f, ">", "$ARGV[0]/w") && unlink "$ARGV[0]/w" }'\'' "$SCRATCH" "$TMPDIR/writing" &' \
	'sleep 30' >"$TMP/missed.sh"
stops=10
case $OSTYPE in msys* | cygwin*) stops=0 && skip "Windows cannot remove a folder a stray process keeps writing in, so a stop that missed one leaves its scratch to the next run's sweep" ;; esac
for _ in $(seq "$stops"); do
	rm -f "$TMP/writing"
	perl -e 'setpgrp(0, 0); exec(@ARGV) or die' bash "$TMP/missed.sh" &
	leader=$!
	waitFor "$TMP/writing" 10
	kill -TERM -- "-$leader"
	wait "$leader" || :
	kill -KILL -- "-$leader" 2>/dev/null || :
	left=$(find "$TMP" -maxdepth 1 -name "3drop.$leader.*")
	[ -z "$left" ] || fail "a tool stopped as the bridge stops one left its scratch: $left"
done
# A port has one spelling, so two spellings of one never pass for two consoles' addresses.
for port in 1 80 1337 65535; do isPort "$port" || fail "isPort refused $port"; done
for port in "" 0 00 080 65536 99999 123456 -1 +80 1a ' 80' '80 ' 8$'\n'0; do
	! isPort "$port" || fail "isPort took '$port'"
done
# lowerHex gives an id in the device's one spelling and changes nothing else.
expect "$(lowerHex 0004000E0ABCDEF0)" 0004000e0abcdef0 "lowerHex of a title id"
expect "$(lowerHex 'title:ABCDEFG xyz-')" 'title:abcdefG xyz-' "lowerHex past A-F"
# fileFrom gives a piece's bytes from any offset: none are lost to a buffer, and the header is not kept.
head -c 3000000 /dev/urandom >"$TMP/from"
for at in 0 1 117 1048576 2999999 3000000; do
	cmp -s <(fileFrom "$TMP/from" "$at") <(tail -c $((3000000 - at)) "$TMP/from") || fail "fileFrom at $at"
done
# The key and the port as .env gives them: written with CRLF, as a Windows editor does, they come out the same
# as with LF, and the key comes out as the console reads its own file (src/auth.c trimLine), whichever way it
# was given.
mkdir "$TMP/envLf" "$TMP/envCrlf"
printf 'THREEDROP_KEY=wxyz-2345-abcd\nTHREEDROP_PORT=4242\n' >"$TMP/envLf/.env"
printf 'THREEDROP_KEY="wxyz-2345-abcd"\r\nTHREEDROP_PORT=4242\r\n' >"$TMP/envCrlf/.env"
# envKey <folder>: prints the key requireKey derives from the .env there, and the port it left.
envKey() (
	unset THREEDROP_KEY
	ROOT=$1 requireKey
	echo "$(deriveDK) $THREEDROP_PORT"
)
want="$(THREEDROP_KEY=wxyz-2345-abcd deriveDK) 4242"
expect "$(envKey "$TMP/envLf")" "$want" "the key and port of an LF .env"
expect "$(envKey "$TMP/envCrlf")" "$want" "the key and port of a CRLF .env"
expect "$(THREEDROP_KEY=$'wxyz-2345-abcd\r' deriveDK)" "${want% 4242}" "an exported key that ends in CR"
# A plain build runs in the image that the lint image extends, which is the gate's toolchain, so one pin
# serves both.
mkdir -p "$TMP/pin/tools" "$TMP/pin/bin"
cp "$ROOT/tools/lib.sh" "$ROOT/tools/build.sh" "$TMP/pin/tools/"
sed 's/@sha256:[0-9a-f]*/@sha256:0123abcd/' "$ROOT/tools/lint.Dockerfile" >"$TMP/pin/tools/lint.Dockerfile"
# shellcheck disable=SC2016 # the fake's own $1 and $@
printf '#!/bin/sh\n[ "$1" = info ] || echo "$@"\n' >"$TMP/pin/bin/docker"
chmod +x "$TMP/pin/bin/docker"
ran=$(PATH="$TMP/pin/bin:$PATH" bash "$TMP/pin/tools/build.sh") || fail "build.sh under a fake docker: $ran"
[[ " $ran " == *" devkitpro/devkitarm@sha256:0123abcd make "* ]] || fail "build.sh's image is not its lint image's base: $ran"
# A docker run under Git Bash (OSTYPE msys) gets its arguments untouched by MSYS's path conversion
# (MSYS2_ARG_CONV_EXCL, for that call alone, because the container's /work is not a path here) and the bind
# source as Windows names it; elsewhere the same call carries the plain path. The fake docker records each
# call's arguments and that variable; the fake cygpath stands in for Git Bash's.
cp "$ROOT/tools/lint.sh" "$TMP/pin/tools/"
printf '#!/bin/sh\nexit 128\n' >"$TMP/pin/bin/git" # not a repository: no version, no files
# shellcheck disable=SC2016 # the fake's own $1 and $2
printf '#!/bin/sh\n[ "$1" = -m ] && printf "C:/win%%s\\n" "$2"\n' >"$TMP/pin/bin/cygpath"
cat >"$TMP/pin/bin/docker" <<'EOF'
#!/bin/sh
[ "$1" = info ] || printf '%s\n' -- "env=$MSYS2_ARG_CONV_EXCL" "$@" >>"$DOCKER_CALLS"
EOF
chmod +x "$TMP/pin/bin/git" "$TMP/pin/bin/cygpath" "$TMP/pin/bin/docker"
pinned=$(cd "$TMP/pin" && pwd)
# dockerRun <ostype> <tool>: records the last docker call of the tool, one argument a line, with its variable
# first.
dockerRun() {
	rm -f "$TMP/dockerCalls"
	DOCKER_CALLS=$TMP/dockerCalls OSTYPE=$1 PATH="$TMP/pin/bin:$PATH" bash "$TMP/pin/tools/$2" >/dev/null || fail "$2 under a fake docker, as $1"
	awk '/^--$/ { call = ""; next } { call = call $0 "\n" } END { printf "%s", call }' "$TMP/dockerCalls" >"$TMP/dockerRun"
}
# after <flag> <value>: checks that the last call has the flag followed by the value.
after() { grep -xF -A1 -e "$1" "$TMP/dockerRun" | grep -qxF -e "$2" || fail "$tool as $os: no $1 $2 in: $(tr '\n' ' ' <"$TMP/dockerRun")"; }
for tool in build.sh lint.sh; do
	flag=-v
	[ "$tool" = build.sh ] || flag=--volume
	for os in linux-gnu msys; do
		dockerRun "$os" "$tool"
		src=$pinned
		[ "$os" = linux-gnu ] || src=C:/win$pinned
		grep -qxF 'env=*' "$TMP/dockerRun" || fail "$tool as $os: docker ran with path conversion on: $(tr '\n' ' ' <"$TMP/dockerRun")"
		after "$flag" "$src:/work"
		after "$([ "$tool" = build.sh ] && echo -w || echo --workdir)" /work
		after --env HOME=/tmp
	done
done
# The UniStore manifest as Universal Updater reads it. A release stamps its version, the store's revision, and
# a last_updated of now that UU shows as a date ("%d-%d-%d at %d:%d") and that sorts after an earlier
# release's, even one from the same day; nothing else changes. Each icon sheet it names has a URL (no release
# publishes one), and an icon index needs a sheet.
if command -v jq >/dev/null 2>&1; then
	cp "$ROOT/3drop.unistore" "$TMP/store"
	before=$(date -u '+%Y-%m-%d at %H:%M (UTC)')
	bash "$ROOT/tools/stampUnistore.sh" 1.2.3 77 "$TMP/store" || fail "stampUnistore.sh"
	after=$(date -u '+%Y-%m-%d at %H:%M (UTC)')
	expect "$(jq -r '.storeContent[0].info.version, .storeInfo.revision' "$TMP/store" | tr '\n' ' ')" "1.2.3 77 " "the stamped version and revision"
	stamp=$(jq -r '.storeContent[0].info.last_updated' "$TMP/store")
	[[ "$stamp" == "$before" || "$stamp" == "$after" ]] || fail "last_updated '$stamp' is not now, written as UU reads a date"
	stamped='del(.storeContent[0].info.version, .storeContent[0].info.last_updated, .storeInfo.revision)'
	expect "$(jq -S "$stamped" "$TMP/store")" "$(jq -S "$stamped" "$ROOT/3drop.unistore")" "the manifest past its stamp"
	jq -e '([.storeInfo.sheet // empty] | flatten) as $s | ([.storeInfo.sheetURL // empty] | flatten) as $u |
		($s | length) == ($u | length) and all($u[]; . != "") and
		(($s | length) > 0 or all(.storeContent[].info; has("icon_index") | not))' "$TMP/store" >/dev/null ||
		fail "the manifest names an icon sheet with no URL, or an icon with no sheet"
else
	skip "no jq, so the UniStore stamp is unchecked"
fi
# Release numbers, as release.yml runs tools/release.sh: only v1 to v63, release N at title version N x 1024
# and store revision N; deploy.sh numbers the 1023 builds between (testWire).
release() { bash "$ROOT/tools/release.sh" "$@"; }
expect "$(release number v1):$(release number v2):$(release number v63)" "1 1024:2 2048:63 64512" "release numbers"
for tag in v0 v01 v64 v100 v2-rc1 v1.0 V1 v x1 'v1 ' ''; do
	run 1 release number "$tag"
	saidLine "release: release tags are v1 to v63, with no leading 0 or suffix: $tag is not one, so nothing is built" "the tag '$tag'"
done
# Publication reads the releases published now and every tag (files, one tag a line). The cases are: a failed
# high tag, a rejected high tag, out-of-order completion and an old retry.
pub=$TMP/published tags=$TMP/tags
latestIs() { # <want> <published tags...>: checks that Latest of those published is <want>
	printf '%s\n' "${@:2}" >"$pub"
	expect "$(release latest "$pub")" "$1" "Latest of the published ${*:2}"
}
# shellcheck disable=SC2086 # word lists, a tag a word
mayPublish() { # <status> <tag> <published, space-separated> <tags, space-separated>
	printf '%s\n' $3 >"$pub"
	printf '%s\n' $4 >"$tags"
	run "$1" release check "$2" "$pub" "$tags"
}
# The first release is v1, and each next one is the number after the highest published.
mayPublish 0 v1 "" "v1"
mayPublish 0 v2 "v1" "v1 v2"
mayPublish 1 v2 "" "v2"
saidLine "release: v2 skips v1, which no tag holds: the next release is v1, and a higher one would spend the title versions of every release below it"
# A failed high tag: v3's build failed, so v2 still publishes, then v4 publishes past the tag v3 holds. Latest
# is the highest published, never the failed tag.
mayPublish 0 v2 "v1" "v1 v2 v3"
mayPublish 0 v4 "v1 v2" "v1 v2 v3 v4"
latestIs v2 v1 v2
# A rejected high tag: a mistaken v63 is refused before anything publishes, and v2 still goes after it.
mayPublish 1 v63 "v1" "v1 v63"
saidLine "release: v63 skips v2, which no tag holds: the next release is v2, and a higher one would spend the title versions of every release below it"
mayPublish 0 v2 "v1" "v1 v2 v63"
latestIs v2 v1 v2 v2-rc1 v63x
# Out-of-order completion: v3 published before v2, then v2 publishes; Latest stays v3.
mayPublish 0 v3 "v1" "v1 v2 v3"
mayPublish 0 v2 "v1 v3" "v1 v2 v3"
latestIs v3 v3 v1 v2
# An old retry: a published release is never published again, and its rerun leaves Latest the highest.
mayPublish 1 v1 "v1 v2" "v1 v2"
saidLine "release: v1 is published already, and a release never changes: a fix goes out as the next number"
latestIs v10 v9 v10 v2
run 1 release latest "$TMP/none"
run 1 release check v2-rc1 "$pub" "$tags"
# What a release page carries: one list of assets (not counting SHA256SUMS, since it lists the others) that
# the sums, the create and the page's paragraph all follow.
run 0 release assets
assets=$(cat "$TMP/out")
expect "${assets//$'\n'/ }" "3drop.cia 3drop.elf 3drop-qr.png 3drop.unistore 3drop-library-sources.tar.gz 3drop-library-sources.txt" "the release's assets"
mkdir "$TMP/rel"
for asset in $assets; do printf 'the bytes of %s\n' "$asset" >"$TMP/rel/$asset"; done
run 0 release sums "$TMP/rel"
for asset in $assets; do
	grep -qxF "$(sha256File "$TMP/rel/$asset")  $asset" "$TMP/rel/SHA256SUMS" || fail "SHA256SUMS has no line for $asset"
done
expect "$(wc -l <"$TMP/rel/SHA256SUMS" | tr -d ' ')" "$(printf '%s\n' "$assets" | wc -l | tr -d ' ')" "the lines of SHA256SUMS, one an asset"
# A release is never published with a file missing: nothing is written, and the missing file is named.
rm "$TMP/rel/SHA256SUMS" "$TMP/rel/3drop.elf"
run 1 release sums "$TMP/rel"
saidLine "release: 3drop.elf is not in $TMP/rel, and a release is never published without it"
[ ! -e "$TMP/rel/SHA256SUMS" ] || fail "release sums wrote SHA256SUMS short of an asset"
# The paragraph is the same on every release, names the asset it explains, and tells how to install,
# how to update, and that the console app and the tools come from one release, and why.
run 0 release notes owner/repo
paragraph=$(cat "$TMP/out")
for want in "open FBI's Remote Install and scan the code in the README's [Install 3Drop on the 3DS](https://github.com/owner/repo#1-install-3drop-on-the-3ds)" "https://github.com/owner/repo/releases/latest/download/3drop.unistore" \
	"press Y and scan that same code" "tools/drop.sh --self 3drop.cia <ip>" "come from the same release" \
	"git clone --branch <tag> https://github.com/owner/repo.git" "git fetch --tags && git checkout <tag>" \
	"only \`stats\` and a self-update still work between them"; do
	[[ "$paragraph" == *"$want"* ]] || fail "the release paragraph never says '$want'"
done
for asset in $assets SHA256SUMS; do
	[[ "$paragraph" == *"\`$asset\`"* ]] || fail "the release paragraph never names $asset"
done
# The paragraph sends the reader to the README's fixed code by its heading's anchor, so the heading must be
# there, and no release page is told to scan a code it only holds as a download.
grep -qxF '### 1. Install 3Drop on the 3DS' "$ROOT/README.md" || fail "the release paragraph links README.md's step 1, which is no heading there"
[[ "$paragraph" != *"scan \`3drop-qr.png\`"* ]] || fail "the release paragraph tells a release page's reader to scan 3drop-qr.png, which is an asset to download"
run 0 release notes other/repo
expect "$(sed 's|other/repo|owner/repo|g' "$TMP/out")" "$paragraph" "the paragraph of another repository"
# release.yml gathers the sources before the gate, and creates the release from that list, the sums
# and the paragraph, still without Latest (the probe in fix/REL/scripts runs the step itself).
yml=$ROOT/.github/workflows/release.yml
[ "$(grep -n 'run: bash tools/librarySources.sh' "$yml" | head -n 1 | cut -d: -f1)" -lt "$(grep -n 'run: tools/check.sh' "$yml" | head -n 1 | cut -d: -f1)" ] ||
	fail "release.yml gathers the libraries' sources after the gate, not before"
# shellcheck disable=SC2016 # release.yml's own text, matched as written
grep -qF 'gh release create "${GITHUB_REF_NAME}" $(bash tools/release.sh assets) SHA256SUMS' "$yml" || fail "release.yml's create does not take the assets and SHA256SUMS"
# shellcheck disable=SC2016 # release.yml's own text, matched as written
grep -qF -- '--notes "$(bash tools/release.sh notes "$GITHUB_REPOSITORY")"' "$yml" || fail "release.yml's create does not take the paragraph"
grep -qF -- '--generate-notes' "$yml" || fail "release.yml's create dropped the generated notes"

# Tests the libraries' sources (tools/librarySources.sh) against a list of its own, with file:// for the
# downloads and a docker that answers dkp-pacman -Q from a file.
src=$TMP/libsrc out=$TMP/libout bin=$TMP/libbin list=$TMP/libs.tsv packages=$TMP/libpackages
mkdir "$src" "$bin"
image=$(sed -n 's/^FROM //p' "$ROOT/tools/lint.Dockerfile")
cat >"$bin/docker" <<'EOF'
#!/usr/bin/env bash
[ "$*" = "run --rm $WANT_IMAGE dkp-pacman -Q" ] || exit 99
cat "$PACKAGES"
EOF
chmod +x "$bin/docker"
printf 'alpha 1.0-1\nbeta 2.0-1\nunrelated 9-9\n' >"$packages"
printf 'alpha source\n' >"$src/alpha-1.tar"
printf 'alpha recipe\n' >"$src/PKGBUILD"
printf 'beta source\n' >"$src/beta-2.tar"
fileUrl() { echo "file:///$(hostPath "$1" | sed 's|^/||')"; }
sumA=$(sha256File "$src/alpha-1.tar") sumR=$(sha256File "$src/PKGBUILD") sumB=$(sha256File "$src/beta-2.tar")
row() { (IFS=$'\t' && printf '%s\n' "$*"); }
# libList <alpha's source SHA-256> <beta's URLs, tab-separated>: writes the list with the two libraries.
libList() {
	{
		echo "# a comment"
		row library alpha 1.0-1 MIT https://example.org/alpha https://example.org/recipes/alpha
		row file alpha source alpha-1.tar "$1" "$(fileUrl "$src/alpha-1.tar")"
		row file alpha recipe PKGBUILD "$sumR" "$(fileUrl "$src/PKGBUILD")"
		row library beta 2.0-1 "Apache-2.0 OR GPL-2.0-or-later" https://example.org/beta https://example.org/recipes/beta
		echo "file	beta	source	beta-2.tar	$sumB	$2"
	} >"$list"
}
# buildSources <status>: runs librarySources.sh on $list, into an empty $out, against the stub docker.
buildSources() {
	rm -rf "$out"
	mkdir "$out"
	PATH="$bin:$PATH" run "$1" env WANT_IMAGE="$image" PACKAGES="$packages" THREEDROP_LIBRARY_SOURCES="$list" \
		bash "$ROOT/tools/librarySources.sh" "$out"
}
# nothingMade: checks that a stop leaves neither the archive nor the manifest.
# shellcheck disable=SC2012 # the test's own folder, whose names it chose
nothingMade() { [ -z "$(ls "$out")" ] || fail "$1: left $(ls "$out" | tr '\n' ' ')"; }
libList "$sumA" "$(fileUrl "$src/missing.tar")	$(fileUrl "$src/beta-2.tar")"
buildSources 0
mkdir "$TMP/unpacked"
tar -xzf "$out/3drop-library-sources.tar.gz" -C "$TMP/unpacked"
unpacked=$TMP/unpacked/3drop-library-sources
cmp -s "$src/alpha-1.tar" "$unpacked/sources/alpha/alpha-1.tar" || fail "the archive holds another alpha source"
cmp -s "$src/PKGBUILD" "$unpacked/recipes/alpha/PKGBUILD" || fail "the archive holds another alpha recipe"
cmp -s "$src/beta-2.tar" "$unpacked/sources/beta/beta-2.tar" || fail "a dead first URL did not fall back to the next"
cmp -s "$unpacked/MANIFEST.txt" "$out/3drop-library-sources.txt" || fail "the manifest by itself is not the archive's"
for want in "  $image" "alpha 1.0-1" "  license:           MIT" "  upstream:          https://example.org/alpha" \
	"  devkitPro recipe:  https://example.org/recipes/alpha" "  sources/alpha/alpha-1.tar" "    SHA-256 $sumA, $(($(wc -c <"$src/alpha-1.tar"))) bytes" \
	"    from $(fileUrl "$src/alpha-1.tar")" "beta 2.0-1" "  license:           Apache-2.0 OR GPL-2.0-or-later" "  recipes/alpha/PKGBUILD" \
	"    SHA-256 $sumB, $(($(wc -c <"$src/beta-2.tar"))) bytes" "    from $(fileUrl "$src/beta-2.tar")" "    or $(fileUrl "$src/missing.tar")" \
	"URL for the file: tried only if needed, and accepted only at the same SHA-256. The files are"; do
	grep -qxF -- "$want" "$out/3drop-library-sources.txt" || fail "the manifest never says '$want': $(cat "$out/3drop-library-sources.txt")"
done
# A download whose SHA-256 is not the pinned one is never kept, and the message says what to update.
libList "$sumB" "$(fileUrl "$src/beta-2.tar")"
buildSources 1
said "gave SHA-256 $sumA, not the $sumB that tools/librarySources.tsv pins"
said "no source for alpha-1.tar matches its SHA-256 $sumB"
said "put the new SHA-256, or a URL that works, on its line in tools/librarySources.tsv"
nothingMade "a download that is not the pinned one"
# One that is wrong at the first URL but right at the fallback is the pinned bytes, so it is kept, and the
# message says so.
libList "$sumA" "$(fileUrl "$src/alpha-1.tar")	$(fileUrl "$src/beta-2.tar")"
buildSources 0
said "gave SHA-256 $sumA, not the $sumB that tools/librarySources.tsv pins"
cmp -s "$src/beta-2.tar" <(tar -xOzf "$out/3drop-library-sources.tar.gz" 3drop-library-sources/sources/beta/beta-2.tar) || fail "the fallback's bytes were not kept"
# A SHA-256 that is not a SHA-256, and a source that cannot be got at all, stop it too.
libList "$sumA" "$(fileUrl "$src/missing.tar")"
buildSources 1
said "no source for beta-2.tar matches its SHA-256 $sumB"
nothingMade "a source that cannot be downloaded"
libList "SHA-256-of-alpha" "$(fileUrl "$src/beta-2.tar")"
buildSources 1
said "the SHA-256 of alpha-1.tar is not 64 lowercase hex digits"
# The image's versions are checked before anything downloads, and a library it lacks counts. The message says
# which file to change, and the sources may even be dead.
libList "$sumA" "$(fileUrl "$src/missing.tar")"
printf 'alpha 1.1-1\nbeta 2.0-1\n' >"$packages"
buildSources 1
said "the build image has alpha 1.1-1, but tools/librarySources.tsv says 1.0-1: put the image's version in that file, with its source's URL and SHA-256 and its recipe's commit"
nothingMade "an image of another version"
printf 'alpha 1.0-1\n' >"$packages"
buildSources 1
said "the build image has beta not at all, but tools/librarySources.tsv says 2.0-1"
rm "$packages"
buildSources 1
said "could not read the packages of $image"
printf 'alpha 1.0-1\nbeta 2.0-1\n' >"$packages"
# A library with no source file, or none at all, is a release without its sources.
{
	row library alpha 1.0-1 MIT https://example.org/alpha https://example.org/recipes/alpha
	row file alpha recipe PKGBUILD "$sumR" "$(fileUrl "$src/PKGBUILD")"
} >"$list"
buildSources 1
said "tools/librarySources.tsv has no source file for alpha"
echo "# no library" >"$list"
buildSources 1
said "tools/librarySources.tsv names no library"
# The list as shipped is well-formed, with one recipe commit per library (every recipe file's URL holds the
# library row's), a source for each, and a package for every library the Makefile links except the system's.
shipped=$ROOT/tools/librarySources.tsv
while IFS=$'\t' read -r kind package version licence home recipe; do
	[ "$kind" = library ] || continue
	[[ "$version" =~ ^[0-9][0-9A-Za-z.+~_-]*-[0-9]+$ && -n "$licence" && "$home" == https://* && "$recipe" =~ ^https://github.com/devkitPro/pacman-packages/tree/[0-9a-f]{40}/ ]] ||
		fail "librarySources.tsv: the row of $package is malformed"
	commit=${recipe#*/tree/}
	commit=${commit%%/*}
	sources=0
	while IFS=$'\t' read -r kind owner type file sha url _; do
		[[ $kind == file && $owner == "$package" ]] || continue
		[[ ($type == source || $type == recipe) && $sha =~ ^[0-9a-f]{64}$ && $url == https://* ]] || fail "librarySources.tsv: $package's $file is malformed"
		[ "$type" = source ] || [[ "$url" == https://raw.githubusercontent.com/devkitPro/pacman-packages/$commit/* ]] ||
			fail "librarySources.tsv: $package's recipe $file is not at the commit its row names, $commit"
		[ "$type" != source ] || sources=$((sources + 1))
	done <"$shipped"
	[ "$sources" != 0 ] || fail "librarySources.tsv: $package has no source"
done <"$shipped"
dups=$(awk -F'\t' '$1 == "file" { print $2, $4 }' "$shipped" | sort | uniq -d)
expect "$dups" "" "a file named twice in librarySources.tsv"
# Each library the Makefile links is a package in the list, or the system's (-lm); a new one fails here.
# shellcheck disable=SC2013 # LIBS is one line of words
for lib in $(sed -n 's/^LIBS[[:blank:]]*:=//p' "$ROOT/Makefile"); do
	case $lib in
	-lctru) package=libctru ;;
	-lcitro2d) package=citro2d ;;
	-lcitro3d) package=citro3d ;;
	-lcurl) package=3ds-curl ;;
	-lmbedtls | -lmbedx509 | -lmbedcrypto) package=3ds-mbedtls ;;
	-lz) package=3ds-zlib ;;
	-lm) continue ;;
	*) fail "the Makefile links $lib: say which package's sources it is, here, and in librarySources.tsv" ;;
	esac
	grep -q "^library	$package	" "$shipped" || fail "the Makefile links $lib, and librarySources.tsv has no $package"
done
# A test past TEST_TIMEOUT ends with status 124, along with every process it started, so a hang fails the
# gate. The shell names its two processes first. A shell that a busy host starts too late to name them within
# 1 s is run again, with 4 s, then 16 s.
for limit in 1 4 16; do
	t0=$SECONDS st=0
	: >"$TMP/hung"
	# shellcheck disable=SC2016 # the inner shell's own $$, $! and $1
	withTimeout "$limit" bash -c 'echo $$ >"$1"; sleep 30 & echo $! >>"$1"; wait' _ "$TMP/hung" 2>/dev/null || st=$?
	(($(wc -l <"$TMP/hung") < 2)) || break
done
expect "$st:$(($(wc -l <"$TMP/hung")))" 124:2 "a command past its timeout, its two processes named"
for _ in $(seq 30); do
	alive=0
	while read -r pid; do if kill -0 "$pid" 2>/dev/null; then alive=$pid; fi; done <"$TMP/hung"
	[ "$alive" != 0 ] || break
	sleep 0.1
done
[ "$alive" = 0 ] || fail "process $alive outlived its command's timeout"
[ $((SECONDS - t0)) -lt $((limit + 7)) ] || fail "a command past its $limit s timeout held $((SECONDS - t0)) s"
# killChildren in an EXIT trap ends what the shell ran, including its background work, which a Ctrl-C never
# reaches, both on a Ctrl-C to its group and on a TERM to it alone (whose trap runs at once). The work sleeps
# far longer than any killChildren that is merely slow (its ps -ef reads every process, taking seconds in a
# busy gate); one that misses the work leaves the group alive through the 20 s waited here.
for sig in INT TERM; do
	rm -f "$TMP/began"
	# shellcheck disable=SC2016 # the inner shell's own
	perl -e '$SIG{INT} = "DEFAULT"; setpgrp(0, 0); exec(@ARGV) or die' bash -c '. "$1/tools/lib.sh"; trap killChildren EXIT
		{ trap "" INT; echo set >"$2"; sleep 60; } & sleep 30' _ "$ROOT" "$TMP/began" &
	leader=$!
	waitFor "$TMP/began" 10
	if [ "$sig" = INT ]; then kill -INT -- "-$leader"; else kill -TERM "$leader"; fi
	wait "$leader" || :
	tryFor 400 gone "-$leader" || {
		kill -KILL -- "-$leader" 2>/dev/null || :
		fail "an EXIT trap's killChildren left the shell's work running after its $sig"
	}
done
# A gate's Ctrl-C, Ctrl-\, TERM or hangup ends the tests too. Each test runs in a group of its own, which the
# terminal's signal never reaches, under a wrapper that run.sh starts in the background (Ctrl-C ignored
# there).
export -f withTimeout
for sig in INT QUIT TERM HUP; do
	rm -f "$TMP/inner"
	# shellcheck disable=SC2016 # the inner shells' own
	perl -e '$SIG{INT} = $SIG{QUIT} = "IGNORE"; setpgrp(0, 0); exec(@ARGV) or die' \
		bash -c 'withTimeout 60 sh -c "echo \$\$ >\"\$1\"; exec sleep 60" _ "$1"' _ "$TMP/inner" 2>/dev/null &
	wrapper=$!
	waitFor "$TMP/inner" 10
	kill -s "$sig" -- "-$wrapper"
	tryFor 40 gone "$(cat "$TMP/inner")" || {
		kill -KILL "$(cat "$TMP/inner")" 2>/dev/null || :
		fail "a $sig to the runner's group left its test running"
	}
	wait "$wrapper" 2>/dev/null || :
done
# The gate lets a leg list only the skips its host cannot help. A Mac may have no jq (a check a host cannot
# make is listed, not failed), and it has the nc that the Linux image lacks.
eval "$(sed -n '/^onlySkips() {/,/^}$/p' "$ROOT/tools/check.sh")"
mayList() { # <host, as uname says it> <skip>: a leg on that host may list the skip
	printf 'SKIPPED: %s\n' "$2" >"$TMP/skips.out"
	(SCRATCH=$TMP && onlySkips skips.out "$1") >/dev/null 2>&1
}
for host in Darwin Linux; do mayList "$host" "testTools: no jq" || fail "a $host leg may skip the jq check"; done
mayList Darwin "testWire: no dd here that counts bytes" || fail "a Darwin leg may skip the byte-counting dd check"
mayList Linux "testAnswers: no nc" || fail "a Linux leg may skip the nc check"
! mayList Darwin "testAnswers: no nc" || fail "a Darwin leg skipped the nc check, which a Mac can make"
! mayList Linux "testWire: no dd here that counts bytes" || fail "a Linux leg skipped the dd check, which it can make"
! mayList Linux "testSync: no sync" || fail "a leg skipped a check it should make"
# The release build's scan for bench code names discoverFormat, the bench build's reader of discovery's
# counters, but not the capture calls libctru and the QR scan make.
eval "$(sed -n '/^benchNames() {/,/^}$/p' "$ROOT/tools/check.sh")"
printf 'symbol build %s\n' main discoverFormat aptConvertScreenForCapture CAMU_StartCapture benchFsBegin >"$TMP/elves"
expect "$(SCRATCH=$TMP benchNames build | tr '\n' ' ')" "discoverFormat benchFsBegin " "the names the release scan flags"
# deploy.sh records the build it put on a console in the arms ledger, and bench.sh asks the ledger what ran
# there last (armsLast). This test runs the lines deploy.sh runs, on a copy without .git, which must still get
# past its tree line, and then reads the ledger back.
mkdir "$TMP/nogit"
printf 'a build\n' >"$TMP/nogit/3drop.elf"
export THREEDROP_ARMS=$TMP/arms.tsv
st=0
line=$(grep '^tree=' "$ROOT/tools/deploy.sh")
record=$(grep '^armsRecord ' "$ROOT/tools/deploy.sh")
out=$(
	set -e
	# shellcheck disable=SC2030 # the nogit tree, for this $(...) alone: the cases below want the repo's
	# shellcheck disable=SC2034 # next is deploy.sh's version, read by its armsRecord line under eval
	ROOT=$TMP/nogit ip=10.0.0.5 next=7
	export GIT_CEILING_DIRECTORIES=$TMP
	eval "$line"
	eval "$record"
	echo "${tree-}"
) || st=$?
[ "$st$out" = 0nogit ] || fail "deploy.sh's tree line on a tree without .git gave '$out' (status $st)"
expect "$(armsLast 10.0.0.5)" 7 "the build deploy.sh recorded for its console"
expect "$(cut -f1,2,4,5 "$THREEDROP_ARMS" | tr '	' ' '):$(cut -f3 "$THREEDROP_ARMS" | awk '{ print length($0) }'):$(awk -F '	' '{ print NF }' "$THREEDROP_ARMS")" "10.0.0.5 7 nogit release:64:6" "deploy.sh's ledger line (cells, SHA-256, tree, flavor)"
armsRecord 10.0.0.6 3 aa nogit bench
armsRecord 10.0.0.5 9 bb nogit bench
expect "$(armsLast 10.0.0.5):$(armsLast 10.0.0.6):$(armsLast 10.0.0.7)" "9:3:" "each console's last build, and none for a console with no line"
st=0
out=$(THREEDROP_ARMS=$TMP/no-ledger.tsv armsLast 10.0.0.5) || st=$?
expect "$st:$out" "0:" "a console asked of a ledger that is not there yet"
# A private folder is made by its whole path, once per process: a | is legal in a path, so a run root
# <p>/r|<p>/cache is not the cache root <p>/cache, and a path with one more slash is the same path. Each case
# is a fresh shell that asks runDir, then cacheDir, outside any $(...) as wire.sh's head and wireKey do, with
# mkdir and chmod shimmed to log what they were asked.
mkdir "$TMP/shims" "$TMP/noChmod"
for exe in mkdir chmod; do
	# shellcheck disable=SC2016 # the shim's own $* and $@
	printf '#!/bin/sh\necho "%s $*" >>%q\nexec %q "$@"\n' "$exe" "$TMP/made.log" "$(type -P "$exe")" >"$TMP/shims/$exe"
	chmod +x "$TMP/shims/$exe"
done
printf '#!/bin/sh\nexit 1\n' >"$TMP/noChmod/chmod"
chmod +x "$TMP/noChmod/chmod"
privately() { # <run root> <cache root>: prints the calls that make their folders, one per line
	: >"$TMP/made.log"
	# shellcheck disable=SC2031 # the repo's root: the nogit case's stayed in its $(...)
	PATH="$TMP/shims:$PATH" XDG_RUNTIME_DIR=$1 XDG_CACHE_HOME=$2 bash -c '. "$1/tools/lib.sh"; runDir >/dev/null; cacheDir >/dev/null' _ "$ROOT"
	cat "$TMP/made.log"
}
# making <folder>: prints the two calls that make it
making() { printf '%s\n' "mkdir -p $1/3drop" "chmod 700 $1/3drop"; }
mkdir "$TMP/pipes"
p=$TMP/pipes/missing
expect "$(privately "$p/r|$p/cache" "$p/cache")" "$(making "$p/r|$p/cache" && making "$p/cache")" "the folders made for a run root whose path holds the cache root's"
[ -d "$p/cache/3drop" ] || fail "a cache folder was not made after a run folder whose path holds its own"
p=$TMP/pipes/open
mkdir -p "$p/cache/3drop"
chmod 777 "$p/cache/3drop"
case "$(ls -ld "$p/cache/3drop")" in drwxrwxrwx*) modes=kept ;; *) modes=none ;; esac # Git Bash's mounts keep none
expect "$(privately "$p/r|$p/cache" "$p/cache")" "$(making "$p/r|$p/cache" && making "$p/cache")" "the folders made for a run root whose path holds the cache root's, the cache folder open"
[ "$modes" = none ] || case "$(ls -ld "$p/cache/3drop")" in drwx------*) ;; *) fail "an open cache folder stayed open after a run folder whose path holds its own" ;; esac
p=$TMP/slash
for run in "$p/cache" "$p/cache/" "$p/cache//"; do
	expect "$(privately "$run" "$p/cache")" "$(making "$p/cache")" "the folders made for the run root '$run' and a cache root that is the same folder"
done
# A string, not a pattern: the folder [c]ache is not the folder cache.
p=$TMP/glob
expect "$(privately "$p/cache" "$p/[c]ache")" "$(making "$p/cache" && making "$p/[c]ache")" "the folders made for a cache root that a pattern in it matches in the run root"
# A folder that could not be closed is not taken for made, so the second ask goes the same way as the first.
st=0
# shellcheck disable=SC2031 # the repo's root: the nogit case's stayed in its $(...)
PATH="$TMP/noChmod:$PATH" XDG_CACHE_HOME=$TMP/unclosed bash -c '. "$1/tools/lib.sh"; cacheDir >/dev/null 2>&1; cacheDir >/dev/null 2>&1' _ "$ROOT" || st=$?
expect "$st" 1 "a cache folder that could not be closed, asked for twice"
echo "testTools: PASS"

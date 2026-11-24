# A fake 3Drop device, sourced after lib.sh. nc works on any port, and /dev/tcp gets a real port.
# $FAKE/log holds every request. Its verbs, knobs, limits and id are read from the device's source. Its
# path and FIRM rules are held to the device's own by testPathRules.sh and testFirmRules.sh. Everything
# else here (the title rules, each verb's grammar and outcomes, the result codes) is a hand copy of the
# console.
# shellcheck shell=bash
: "${FAKE_MODE:=valid}"
export FAKE_MODE ROOT FAKE_LIB="${BASH_SOURCE[0]}"
# How many requests slow-relaunch leaves unanswered. It is more than the ten polls a wait once made, so
# a relaunch is waited for by time. A request costs a quiet Git Bash 0.6 s, so these take 7 s of the
# wait's 60 s there.
FAKE_RELAUNCH_QUIET=12
# The tools' private directories live inside the test's own, set before wire.sh makes its run directory.
[ -n "${FAKE:-}" ] || export TMPDIR="$TMP" XDG_CACHE_HOME="$TMP/cache" XDG_RUNTIME_DIR="$TMP/run"
# shellcheck source=tools/wire.sh
. "$ROOT/tools/wire.sh"
# fakeNumber <literal|NAME>: resolves a KNOBS cell. A name is read from net.h, where the knobs' macros live.
fakeNumber() { if [[ "$1" == *[!0-9]* ]]; then cDefine net.h "$1"; else echo "$1"; fi; }
# fakeAppFile <file> <name>: the SD path of the device's `#define <name> APP_DIR "/..."` in that file.
fakeAppFile() {
	local app rest
	app=$(sed -n 's|^#define APP_PATH "\(/[^"]*\)"$|\1|p' "$ROOT/src/path.h")
	rest=$(sed -n "s|^#define $2 APP_DIR \"\\(/[^\"]*\\)\"\$|\\1|p" "$ROOT/$1")
	{ [ -n "$app" ] && [ -n "$rest" ]; } || fail "no #define $2 APP_DIR \"/...\" in $1, or no APP_PATH in src/path.h"
	echo "$app$rest"
}
if [ -z "${FAKE:-}" ]; then
	export FAKE="$TMP/device"
	mkdir -p "$FAKE/sd" "$FAKE/bin"
	: >"$FAKE/installed" # `<title-id> <version>` lines, version `-` where AM gives no info
	# shellcheck disable=SC2016 # expanded when the generated nc runs
	printf '#!/usr/bin/env bash\n. "$FAKE_LIB"\nfakeDevice\n' >"$FAKE/bin/nc"
	chmod +x "$FAKE/bin/nc"
	export PATH="$FAKE/bin:$PATH"
	wireKey
	# The release rows, plus the bench build's row (the fake plays a bench build).
	grep -ho '{"[a-z0-9]*", [0-9]*, [0-9]*, [a-z]*' "$ROOT/src/cmd.c" |
		tr -d '{",' >"$FAKE/verbs"
	# Each row is `<knob> <default> <ceiling>`, in the order the bare `bench` names them.
	while read -r knob _ def max; do
		def=$(fakeNumber "$def")
		max=$(fakeNumber "$max")
		echo "$knob $def $max"
	done <<<"$(grep -o '{"[a-z]*", &gKnobs\.[A-Za-z]*, [A-Z0-9_]*, [A-Z0-9_]*' "$ROOT/src/bench/bench.c" | tr -d '{",')" >"$FAKE/knobs"
	# Checked here, because a row these greps stop matching would fail its verb's tests far from the cause.
	[ "$(grep -c . "$FAKE/verbs")" -ge 12 ] || fail "the fake read $(grep -c . "$FAKE/verbs") verb rows of src/cmd.c"
	[ "$(grep -c . "$FAKE/knobs")" -ge 5 ] || fail "the fake read $(grep -c . "$FAKE/knobs") knob rows of src/bench/bench.c"
	FAKE_MAX_FIELDS=$(cDefine protocol.h MAX_FIELDS)
	FAKE_PATH_MAX=$(cDefine path.h SD_PATH_MAX)
	FAKE_PATH_UNITS=$(cDefine path.h SD_PATH_UNITS)
	FAKE_PREFIX_MAX=$(cDefine cia.h CIA_MAX_PREFIX_SIZE)
	FAKE_TIER1=$(cDefine ring.h RING_TIER1_BYTES)
	FAKE_TIER2=$(($(cDefine ring.c DRAIN_RING2_UNITS) * $(cDefine ring.h RING_UNIT)))
	FAKE_MB_ROWS=$(cDefine bench/bench.c MB_ROWS)
	# The file steps `fscalls` names, read from bench.c's own list, each as its key in the reply.
	FAKE_FS_STEPS=$(grep -o '\[FS_[A-Z]*\] = "[a-z]*"' "$ROOT/src/bench/bench.c" | sed 's/.*"\(.*\)"/ \1_us=-/' | tr -d '\n') || :
	[ -n "$FAKE_FS_STEPS" ] || fail "the fake read no file steps of src/bench/bench.c"
	FAKE_SELF=$(selfTitleID)
	FAKE_PROTO=$(cDefine protocol.h PROTOCOL_EPOCH)
	FAKE_TOOL_MARKER=$(fakeAppFile tools/bench/testingTool.c MARKER)
	FAKE_TOOL_EVIDENCE=$(fakeAppFile tools/bench/testingTool.c EVIDENCE)
	export FAKE_MAX_FIELDS FAKE_PATH_MAX FAKE_PATH_UNITS FAKE_PREFIX_MAX FAKE_TIER1 FAKE_TIER2 FAKE_MB_ROWS FAKE_SELF \
		FAKE_PROTO FAKE_TOOL_MARKER FAKE_TOOL_EVIDENCE FAKE_FS_STEPS
	fakeOwn=1
fi
nc() { fakeDevice; }
# fails <command...>: checks that the command fails, under whatever fault mode is set.
fails() { if "$@" >/dev/null 2>&1; then fail "accepted under $(cat "$FAKE/mode" 2>/dev/null || echo valid): $*"; fi; }
# count <verb>: prints how many requests of the verb are in $FAKE/log.
count() { grep -c "^$1 " "$FAKE/log" || true; }

# faulted <mode> <command...>: runs the command against a device in that fault mode, on either transport.
faulted() {
	local st=0
	echo "$1" >"$FAKE/mode"
	"${@:2}" || st=$?
	rm -f "$FAKE/mode"
	return "$st"
}

# faultedOn <verb> <ordinals> <mode> <command...>: like faulted, but the mode applies only to those
# requests of the verb, counted from 1 ("2 3").
faultedOn() {
	local st=0
	echo "$1 $2" >"$FAKE/fault-on"
	rm -f "$FAKE/fault-seen"
	faulted "$3" "${@:4}" || st=$?
	rm -f "$FAKE/fault-on" "$FAKE/fault-seen"
	return "$st"
}

# Stands in for Nintendo's seed server as curl reaches it. $FAKE/ext_key/<title-id> holds its answer
# (see fakeSeed), and no file means a 404. $FAKE/seed-log keeps each curl's arguments, each line starting
# with the macOS keychain switch it was given. It is exported so that no tool a test runs can reach the
# real server; testSeeds' TLS checks run curl itself.
curl() {
	local out="" tid status body=""
	printf '%s %s\n' "${OPENSSL_X509_TEA_DISABLE:-}" "$*" >>"$FAKE/seed-log"
	while [ "$#" -gt 1 ]; do
		[ "$1" != -o ] || out=$2
		shift
	done
	tid=${1#*/title/0x} tid=${tid%%/*}
	read -r status body 2>/dev/null <"$FAKE/ext_key/$tid" || status=404
	if [ "$status" = - ]; then
		echo "curl: (7) Failed to connect to kagiya-ctr.cdn.nintendo.net port 443" >&2
		return 7
	fi
	hexDecode "$body" >"$out"
	printf %s "$status"
}
export -f curl

# fakeSeed <title-id> <status|-> [body-hex]: sets the seed server's answer for the title; `-` means no
# answer, as when the server cannot be reached.
fakeSeed() {
	mkdir -p "$FAKE/ext_key"
	echo "$2 ${3:-}" >"$FAKE/ext_key/$1"
}

# fakeCia <out> <title-id> <version> [bytes, a multiple of 64]: writes a CIA of random contents laid out
# the way src/cia.c parses it. It is built here and not by cia.sh, whose reads it checks. Its ticket is
# larger than makerom's, as a DLC's can be.
fakeCia() {
	local size=${4:-16384} ticket=$((0x6D0)) tmdSize=$((0x140 + 0x9C4 + 0x30)) tmd content
	tmd=$(((0x2040 + 0xA00 + ticket + 63) & ~63))
	content=$((size - ((tmd + tmdSize + 63) & ~63)))
	head -c "$size" /dev/urandom >"$1"
	filePatch "$1" 0 "$(fakeLE 0x2020 4 0 4 0xA00 4 "$ticket" 4 "$tmdSize" 4 0 4 "$content" 8)"
	filePatch "$1" "$tmd" 00010004 # RSA-2048: the TMD's body starts 0x140 in
	filePatch "$1" $((tmd + 0x140 + 0x4C)) "$2"
	filePatch "$1" $((tmd + 0x140 + 0x9C)) "$(printf %04x "$3")"
}

# fakeDamaged <file> <offset>: prints the byte there in hex with each digit raised by one, so it is never
# the byte it was. A fixed byte written over random data would not damage it once in 256 times.
fakeDamaged() { fileHex "$1" "$2" 1 | tr 0-9a-f 1-9a-f0; }

# fakeLE <value> <bytes>...: each value as that many little-endian bytes, in hex.
fakeLE() {
	local i
	while [ "$#" -gt 1 ]; do
		for ((i = 0; i < $2; i++)); do printf %02x $(($1 >> 8 * i & 255)); done
		shift 2
	done
}

# fakeStop is the EXIT trap of a test with a device (installed below, where the first fakeListen is). It
# stops the devices before $TMP goes, so none is still writing there. Nothing in it may fail, because
# set -e would end the trap early. It uses killTree because Git Bash has no pkill.
# shellcheck disable=SC2317 # called by the EXIT trap, and by testBridge.sh's
fakeStop() {
	killTree "${fakeListenPid:-}" 2>/dev/null || :
	rm -rf "$TMP" || :
}

# fakeListen: serve fakeDevice on a 127.0.0.1 TCP port (sets fakePort), for the /dev/tcp path. Each
# connection closes the way the device's does: it sends its reply and a FIN, then reads the host's bytes
# until the host closes or a second passes (the device gives it 5). How long that took is appended to
# $FAKE/drainMs, measured with CLOCK_MONOTONIC so that a step of the wall clock cannot stretch or cut it
# (a perl without CLOCK_MONOTONIC reads the wall clock, as msNow does).
fakeListen() {
	rm -f "$FAKE/port"
	perl -MIO::Socket::INET -MTime::HiRes=time -e '
		sub now() { eval { Time::HiRes::clock_gettime(Time::HiRes::CLOCK_MONOTONIC()) } // time }
		my $s = IO::Socket::INET->new(Listen => 5, LocalAddr => "127.0.0.1", LocalPort => 0,
			ReuseAddr => 1) or die "listen: $!";
		open(my $f, ">", $ARGV[0]) or die; print $f $s->sockport; close $f;
		$SIG{CHLD} = "IGNORE";
		while (my $c = $s->accept) {
			my $kid = fork;
			if (!defined $kid) { close $c; next } # drops this request alone: the child branch would end the listener
			if (!$kid) {
				$SIG{CHLD} = "DEFAULT";
				my $pid = fork // die "fork: $!";
				if (!$pid) { open(STDIN, "<&", $c); open(STDOUT, ">&", $c); exec("bash", "-c", ". \"\$FAKE_LIB\"; fakeDevice"); }
				$SIG{TERM} = sub { kill("KILL", $pid); exit 0 };
				waitpid($pid, 0);
				shutdown($c, 1);
				my $t0 = now;
				$SIG{ALRM} = sub { };
				alarm(1);
				1 while sysread($c, my $b, 65536);
				open(my $d, ">>", $ARGV[1]) or die; printf $d "%d\n", (now - $t0) * 1000; close $d;
				exit 0;
			}
			close $c;
		}' "$FAKE/port" "$FAKE/drainMs" &
	fakeListenPid=$!
	disown
	waitFor "$FAKE/port"
	# shellcheck disable=SC2034 # read by the test that sourced this
	fakePort=$(cat "$FAKE/port")
}

# without <tool> <command...>: run a wire.sh command with the tool hidden from its `command -v`.
without() (
	hidden=$1
	shift
	# shellcheck disable=SC2317 # called by wire.sh's `command -v`
	command() {
		if [ "$1 ${2:-}" = "-v $hidden" ]; then return 1; fi
		builtin command "$@"
	}
	"$@"
)
# devtcp <command...>: runs the command with nc hidden, so it takes the /dev/tcp transport.
devtcp() { without nc "$@"; }

# fakeFlip <hex>: prints the value with its first digit changed, giving a MAC that does not verify.
fakeFlip() { if [ "${1:0:1}" = 0 ]; then echo "1${1#?}"; else echo "0${1#?}"; fi; }

# The nonce goes through a substitution because a native openssl on Windows ends its line with CR, which
# `read` keeps.
fakeNonce() {
	[ -s "$FAKE/nonce" ] || printf '%s\n' "$(openssl rand -hex 32)" >"$FAKE/nonce"
	cat "$FAKE/nonce"
}

# fakeReply <text>: sends the reply, which for a mutation is also the `stats op` receipt. Its protocol
# epoch is the source's, or the one in $FAKE/proto: a later console's, or `none` for a console from
# before epochs.
fakeReply() {
	local mac proto=$FAKE_PROTO
	[ ! -s "$FAKE/proto" ] || read -r proto <"$FAKE/proto"
	if [ "$proto" = none ]; then proto=""; else proto="proto=$proto "; fi
	header="response $id $(fakeNonce) $proto$1"
	[ "$spent" != true ] || printf 'op %s %s\n' "$([ "$FAKE_MODE" = foreign-both ] && echo ffffffffffffffffffffffffffffffff || echo "$id")" "$1" >"$FAKE/receipt"
	[ "$FAKE_MODE:$spent" != lost-reply:true ] || return 0 # done, but the reply is lost
	mac=$(macResponse "$req" "$header")
	case "$FAKE_MODE" in
	bad-mac) mac=$(fakeFlip "$mac") ;;
	uppercase) mac=$(printf '%s' "$mac" | tr a-f A-F) ;;
	wrong-binding) mac=$(macResponse "stats ffeeddccbbaa99887766554433221100" "$header") ;;
	extra-line) mac="$mac"$'\nextra' ;;
	esac
	printf '%s\n%s\n' "$header" "$mac"
}

fakeTrailer() { # <fields after `trailer <id> <verb>`>
	local trailer="trailer $id $verb $1" mac
	mac=$(macTrailer "$req" "$header" "$trailer")
	[ "$FAKE_MODE" != bad-trailer ] || mac=$(fakeFlip "$mac")
	printf '%s\n%s\n' "$trailer" "$mac"
}

# The device's UTF-8 check, run on a path's bytes as hex pairs. A path passes if it is well-formed
# (Unicode's table 3-7) and has no C0 control or DEL.
c='[89ab][0-9a-f]' # a continuation byte
FAKE_UTF8="^([2-6][0-9a-f]|7[0-9a-e]|c[2-9a-f]$c|d[0-9a-f]$c|e0[ab][0-9a-f]$c|e[1-9a-cef]$c$c|ed[89][0-9a-f]$c|f0[9ab][0-9a-f]$c$c|f[1-3]$c$c$c|f48[0-9a-f]$c$c)+\$"
unset c

# fakeUnits <path>: the device's pathUnits, left in `units`: one for each character's first byte, and one
# more for a 4-byte character.
fakeUnits() {
	local LC_ALL=C lead=${1//[$'\x80'-$'\xbf']/} four=${1//[^$'\xf0'-$'\xf4']/}
	units=$((${#lead} + ${#four}))
}

# fakeNormalize <field>: the device's targetParse; the path's units are left in `units`.
fakeNormalize() {
	local LC_ALL=C hex rest segment ascii
	path=$1 detail=path root=sd
	if [[ "$path" =~ ^(save:[0-9a-f]{16}|extdata:[0-9a-f]{8})(/.*|hex:.*)$ ]]; then
		root=${BASH_REMATCH[1]} path=${BASH_REMATCH[2]}
	elif [[ "$path" == save:* || "$path" == extdata:* ]]; then
		return 1
	fi
	# The path is checked before it is decoded (bash drops a NUL and a trailing newline), and measured
	# before `sdmc:` comes off, as the device does.
	if [[ "$path" == hex:* ]]; then
		hex=${path#hex:}
		[ $((${#hex} / 2)) -lt "$FAKE_PATH_MAX" ] && [[ "$hex" =~ $FAKE_UTF8 ]] || return 1
		path=$(hexDecode "$hex")
	elif [[ "$path" == *[![:print:]]* ]]; then # printable ASCII needs neither a check nor a process
		[[ "$(wireHex "$path")" =~ $FAKE_UTF8 ]] || return 1
	fi
	path=${path#sdmc:}
	fakeUnits "$path"
	[ "${#path}" -lt "$FAKE_PATH_MAX" ] && [ "$units" -le "$FAKE_PATH_UNITS" ] || return 1
	[[ "$path" == / ]] && return
	[[ "$path" == /?* && "$path" != *//* && "$path" != */ &&
		"$path" != *[.\ ] && "$path" != *[.\ ]/* && "$path" != *[\\:]* ]] || return 1
	# A segment with dots beside non-ASCII characters and no other ASCII but spaces is refused: it is a dot
	# segment if FS trims.
	rest=${path#/}
	while [[ "$rest" == *[![:print:]]* ]]; do
		segment=${rest%%/*}
		ascii=${segment//[![:print:]]/}
		[[ "$segment" == "$ascii" || "$ascii" != *.* || "$ascii" == *[!.\ ]* ]] || return 1
		[[ "$rest" == */* ]] || break
		rest=${rest#*/}
	done
}

# fakeParts <path>: sets the caller's c0, c1 and c2 to its first three components, empty past its last.
# It uses expansions rather than `read <<<` because a here-string is a temp file on Git Bash, made at
# every request.
fakeParts() {
	local rest=${1#/}
	c0=${rest%%/*}
	[[ "$rest" == */* ]] && rest=${rest#*/} || rest=
	c1=${rest%%/*}
	[[ "$rest" == */* ]] && rest=${rest#*/} || rest=
	c2=${rest%%/*}
}

# fakeIs <component> <glob>: the device's componentIs. It matches in either case, and a non-ASCII byte
# matches any name.
fakeIs() {
	local LC_ALL=C
	[[ "$1" == *[![:print:]]* ]] || caseless "$1" "$2"
}

# fakeRootIs <component> <name> [prefix]: the device's rootIs. The component must have as many
# characters as the name (at least as many, as a prefix), and a non-ASCII character stands for any.
fakeRootIs() {
	local LC_ALL=C one=${1//[$'\x80'-$'\xbf']/} i # only each character's lead byte is kept
	[[ "${#one}" = "${#2}" || (-n "${3:-}" && "${#one}" -gt "${#2}") ]] || return 1
	for ((i = 0; i < ${#2}; i++)); do
		[[ "${one:i:1}" == [$'\xc0'-$'\xff'] ]] || caseless "${one:i:1}" "${2:i:1}" || return 1
	done
}

# fakeProtected <path> [read]: the device's pathProtected; key* matches below a key name too, and a
# read compares ASCII names only.
fakeProtected() {
	local c0 c1 c2 is=fakeIs root=fakeRootIs
	[ -z "${2:-}" ] || is=caseless root=caseless
	fakeParts "$1"
	$root "$c0" 3ds && { [ -z "$c1" ] || { $is "$c1" 3drop && { [ -z "$c2" ] || $is "$c2" 'key*'; }; }; }
}

# fakeSystemFile <path> <put|delete>: the device's systemFile: a file directly in /luma/payloads or
# /luma/sysmodules, and for a put /boot.firm, every name spelled exactly.
fakeSystemFile() {
	local c0 c1 c2
	if [ "$2" = put ] && caseless "$1" /boot.firm; then return 0; fi
	fakeParts "$1"
	[[ -n "$c2" && "$1" != /*/*/*/* ]] && caseless "$c0" luma && { caseless "$c1" payloads || caseless "$c1" sysmodules; }
}

# fakeSafe <path> [put|delete]: the device's consoleData and bootInput, including 8.3 aliases and
# /boot.firm's backup. The verb that carried the word `system` opens a system file.
fakeSafe() {
	local c0 c1 c2 n
	fakeParts "$1"
	detail=refused
	! fakeRootIs "$c0" "nintendo 3ds" && ! { fakeRootIs "$c0" ni prefix && [[ "$c0" == *~* ]]; } || return 1
	[ -z "${2:-}" ] || ! fakeSystemFile "$1" "$2" || return 0
	detail=system
	! fakeRootIs "$c0" boot.firm && ! fakeRootIs "$c0" boot.firm.3drop.bak &&
		! { fakeRootIs "$c0" bo prefix && [[ "$c0" == *~* ]]; } || return 1
	[ -n "$c1" ] && fakeRootIs "$c0" luma || return 0
	for n in payloads sysmodules native.firm twl.firm agb.firm safe.firm sysupdater.firm; do
		! fakeIs "$c1" "$n" || return 1
	done
	[[ "$c1" != *~* ]]
}

# fakeRemovable [word]: the device's pathRemoveRefusal; removing /luma itself would take its boot files
# with it.
fakeRemovable() {
	local verb=""
	[ "${1:-}" != system ] || verb=delete
	fakeSafe "$path" "$verb" && ! { [[ "$path" != /*/* ]] && fakeRootIs "${path#/}" luma; }
}

# fakePath <field> [sd] [read]: the device's path gate: argTarget, or argPath with `sd`. With `read` it
# is the gate of a get or a 3dsx run.
fakePath() {
	fakeNormalize "$1" && [ "$path" != / ] && [[ -z "${2:-}" || "$root" == sd ]] || return 1
	[ "$root" = sd ] || return 0
	detail=protected
	! fakeProtected "$path" "${3:-}"
}

fakeSd() { printf '%s/%s%s' "$FAKE" "${root:-sd}" "$1"; }

# fakeManaged <title-id> and fakeAppUid <unique-id>: the device's titleManaged and
# titleUidIsApplication (src/titles.h), the one copy of each here.
fakeManaged() { [[ "$1" == 00040000* && "$1" != *02 ]]; }
fakeAppUid() { (($1 >= 0x300 && $1 <= 0xfffff)); }

# fakeWritable <path> [tree] [word]: the device's pathWriteRefusal; a tree moved onto /luma,
# /luma/titles or /luma/plugins could carry any title's patch.
fakeWritable() {
	local LC_ALL=C c0 c1 c2 tid verb=""
	[ "${3:-}" != system ] || verb=put
	fakeSafe "$1" "$verb" || return 1
	detail=reserved # a put's temp or backup name on the path (putSibling)
	! caseless "$1" '*.3drop.@(tmp|bak)?(/*)' || return 1
	fakeParts "$1"
	detail=refused
	fakeRootIs "$c0" luma || return 0
	[ -n "$c1" ] || {
		[ -z "${2:-}" ]
		return
	}
	fakeIs "$c1" titles || fakeIs "$c1" plugins || return 0
	[ -n "$c2" ] || {
		[ -z "${2:-}" ]
		return
	}
	tid=$(printf '%s' "$c2" | tr '[:upper:]' '[:lower:]')
	[[ "$tid" =~ ^[0-9a-f]{16}$ ]] || return 1
	fakeTitleRefusal "$tid" app
}

# fakeSchemeWritable: the device's pathWriteRefusal inside a save or extdata, whose title it names.
fakeSchemeWritable() {
	local id=${root#*:}
	tid=$id
	if [[ "$root" == extdata:* ]]; then
		tid=0000000000000000 # held by no SD application
		if fakeAppUid $((16#$id)) && ((16#$id != 0x14d1 && 16#$id != 0x165c)); then
			tid=$(printf '%016x' $((0x0004000000000000 | 16#$id << 8)))
		fi
	fi
	fakeTitleRefusal "$tid" app
}

# fakeArchive: the device's argStat on an archive. It checks that the archive's title is installed, then
# that the archive is there.
fakeArchive() {
	fakeInstalled "$tid" || return 1
	detail=notfound result=00000000
	[ -d "$FAKE/$root" ]
}

# fakeInstallable <title-id>: the device's titleInstallable (src/titles.h), the one copy of it here.
fakeInstallable() {
	local uid=$((16#${1:8:6}))
	case "$1" in
	00040000*) fakeManaged "$1" ;;
	0004000e*) fakeAppUid "$uid" && fakeManaged "00040000${1:8}" ;;
	0004008c*) fakeAppUid "$uid" ;;
	*) false ;;
	esac
}

# fakeTitleRefusal <title-id> [app]: the device's titleRefusal (src/titles.h), the one copy of it here.
# It is false, with `detail` set, for a title 3Drop never touches, for 3Drop's own, and with `app` for a
# patch or DLC.
fakeTitleRefusal() {
	detail=unmanaged
	fakeInstallable "$1" || return 1
	detail=self
	[ "${1:8:6}" != "${FAKE_SELF:8:6}" ] || return 1
	detail=not-application
	[[ -z "${2:-}" || "$1" == 00040000* ]]
}

# fakeTitle <title-id> [app]: false with `detail` unless it is a title id fakeTitleRefusal takes.
fakeTitle() {
	detail=grammar
	requireHex 16 "$1" id >/dev/null 2>&1 || return 1
	fakeTitleRefusal "$@"
}

# fakeInstalled <title-id>: AM's facts, as titleCheck reads them. False, with `detail` and `result` set,
# unless AM gives info for the title and it is installed.
fakeInstalled() {
	detail=am-info
	! grep -q "^$1 -$" "$FAKE/installed" || return 1
	grep -q "^$1 " "$FAKE/installed" && return
	detail=notfound result=d8a083fa
	return 1
}

fakeDevice() {
	local LC_ALL=C req mac id verb f=() n canonical=true rest token row min max mutation=false header
	local spent=false result=00000000
	local path detail file tid to nonce="" root=sd down=0 units
	# The host kills nc on a ghost, but the device still takes what came and answers into its receipt.
	fakeTermed=""
	trap 'fakeTermed=1' TERM
	# A relaunching console answers nothing yet; what it was asked goes to $FAKE/unanswered.
	[ ! -s "$FAKE/down" ] || read -r down <"$FAKE/down"
	if [ "$down" -gt 0 ]; then
		echo $((down - 1)) >"$FAKE/down"
		IFS= read -r req && printf '%s\n' "$req" >>"$FAKE/unanswered"
		return 0
	fi
	IFS= read -r req || return 0
	IFS= read -r mac || return 0
	[ ! -s "$FAKE/mode" ] || FAKE_MODE=$(cat "$FAKE/mode")
	if [ "${#req}" -gt "$REQUEST_LINE_MAX" ] || ! requireHex 128 "$mac" MAC >/dev/null 2>&1; then
		printf 'ERR malformed\n'
		return 0
	fi
	[ "$mac" = "$(macRequest "$req")" ] || {
		printf 'ERR auth\n'
		return 0
	}
	printf '%s\n' "$req" >>"$FAKE/log"
	[ ! -d "$WIRE_RUN_DIR/rep-127.0.0.1" ] || printf '%s\n' "$req" >>"$FAKE/locked" # while a rep's lock stood
	# The device's split: fields are separated by single spaces, are printable, and number at most
	# MAX_FIELDS. The fields before a fault still count, so a request whose id survived gets a signed
	# `grammar`.
	rest=$req
	while :; do
		token=${rest%% *}
		if [ -z "$token" ] || [ "${#f[@]}" -ge "$FAKE_MAX_FIELDS" ] || [[ "$token" == *[^!-~]* ]]; then
			canonical=false
			break
		fi
		f+=("$token")
		[[ "$rest" == *" "* ]] || break
		rest=${rest#* }
	done
	n=${#f[@]} verb=${f[0]:-} id=${f[1]:-}
	if [ "$n" -lt 2 ] || ! requireHex 32 "$id" id >/dev/null 2>&1; then
		printf 'ERR malformed\n'
		return 0
	fi
	if [ -s "$FAKE/fault-on" ]; then
		local on ordinals seen=0
		read -r on ordinals <"$FAKE/fault-on"
		[ ! -s "$FAKE/fault-seen" ] || read -r seen <"$FAKE/fault-seen"
		[ "$verb" != "$on" ] || echo $((seen += 1)) >"$FAKE/fault-seen"
		[[ "$verb" = "$on" && " $ordinals " == *" $seen "* ]] || FAKE_MODE=valid
	fi
	case "$FAKE_MODE:$verb" in
	mute:* | mute-put:put) fakeHold ;;
	deaf:*) fakeSilent ;; # it reads nothing more, so a body stops going
	hangup:*) return 0 ;;
	esac
	while read -r row min max mutation; do # with no row for the verb, read empties them at the end
		[ "$row" != "$verb" ] || break
	done <"$FAKE/verbs"
	[ "$FAKE_MODE:$verb" != release:bench ] || min= # a release build has no bench row
	if ! $canonical || [ -z "$min" ] || [ "$n" -lt "$min" ] || [ "$n" -gt "$max" ]; then
		mutation=false
		fakeReply "error rejected $($canonical && [ "$n" = 2 ] && echo unsupported || echo grammar) 00000000"
		return 0
	fi
	if [ "$FAKE_MODE:$verb" = refuse-early:put ]; then # refused, then closed without taking the body
		fakeReply "put rejected space 00000000"
		head -c 65536 >/dev/null
		return 0
	fi
	# The stranded mode is argIdle's. rejected-<word> refuses all but `stats` and `ls` with the word, before
	# anything runs.
	if [[ "$FAKE_MODE:$verb" =~ ^stranded:(get|put|hash|ls)$ || ("$FAKE_MODE" == rejected-* && "$verb" != stats && "$verb" != ls) ]]; then
		fakeReply "$verb rejected ${FAKE_MODE#rejected-} 00000000"
		[ "$verb" != put ] || head -c 65536 >/dev/null
		return 0
	fi
	# A mutation is checked (a refusal keeps the nonce), then the nonce is spent, then it runs. Every verb
	# refuses the same way: it sets `detail` and fails.
	if [ "$mutation" = true ]; then
		if declare -F "fakeCheck_$verb" >/dev/null && ! "fakeCheck_$verb"; then
			fakeRefuse
			return 0
		fi
		detail=challenge
		[ ! -s "$FAKE/nonce" ] || read -r nonce <"$FAKE/nonce"
		if [ "$nonce" != "${f[2]}" ]; then
			fakeRefuse
			return 0
		fi
		rm -f "$FAKE/nonce"
		spent=true
	fi
	"fake_$verb" || fakeReply "$verb rejected $detail $result"
	# The endless mode sends on past its reply, up to 64 units of 1048576 bytes, until the host stops
	# reading. $FAKE/endless counts the units that went.
	[ "$FAKE_MODE" != endless ] || perl -e '
		$SIG{PIPE} = "IGNORE";
		my ($b, $n) = ("\0" x 1048576, 0);
		$n++ while $n < 64 && syswrite(STDOUT, $b) == length($b);
		open(my $f, ">", $ARGV[0]) or die; print $f "$n\n";' "$FAKE/endless"
}

# fakeHold: takes what comes and holds the connection, but not past its nc's TERM, because no console is
# in the host's process group, which a stop signals whole.
fakeHold() {
	cat >/dev/null
	fakeSilent
}

# fakeExec <command...>: this shell becomes the command, unless a TERM came first. The trap is dropped
# before the check, so a TERM after the check kills this shell or the command it becomes. The trap never
# takes a TERM just before the exec, which a stop's whole group would then outlive.
fakeExec() {
	trap - TERM
	[ -z "$fakeTermed" ] || exit 0
	exec "$@"
}
# fakeSilent: stays silent for 30 s, or for the whole seconds $FAKE/hold names (a check that must
# outlast a slow stop sets it). A TERM ends it whenever it comes. It runs as this shell itself, so the
# listener's trap reaches it.
fakeSilent() {
	local hold=30
	[ ! -s "$FAKE/hold" ] || read -r hold <"$FAKE/hold"
	fakeExec sleep "$hold"
}
# fakeStoppable <command...>: fakeExec in a subshell, for a command after which the fake carries on.
fakeStoppable() (fakeExec "$@")

# A mutation refused before its body, which the sender is still sending, has the body drained.
fakeRefuse() {
	local bytes=""
	fakeReply "$verb rejected $detail $result"
	case "$verb:${f[3]:-}:${f[6]:-}" in
	put:new:* | put:update:* | put:self:*) bytes=${f[4]} ;;
	put:*:range) bytes=${f[8]} ;;
	put:*) bytes=${f[7]:-${f[4]}} ;;
	esac
	[[ -z "$bytes" || "$bytes" == *[!0-9]* ]] || head -c "$bytes" >/dev/null
}

# fakeFirm <file>: the device's FIRM check (src/sdXfer.c firmCheck). The file must start with "FIRM" and
# have an ARM9 entry point. Each non-empty section must be whole sectors inside the file, match its
# header's SHA-256, and have a whole-word load address in memory boot9strap allows. Each entry point must
# lie in a section, and the file must be as long as the header and sections.
fakeFirm() {
	perl -MDigest::SHA=sha256 -e '
		open(my $f, "<:raw", $ARGV[0]) or exit 1;
		my $b = do { local $/; <$f> };
		exit 1 if length($b) < 0x200 || substr($b, 0, 4) ne "FIRM";
		my ($arm11, $arm9) = unpack("x8 V V", $b);
		my ($in9, $in11, $want) = (0, !$arm11, 0x200);
		my @region = ([0x08000000, 0x08100000], [0x18000000, 0x18600000], [0x1ff00000, 0x1ffffc00], [0x20000000, 0x28000000]);
		exit 1 if !$arm9;
		for my $i (0 .. 3) {
			my ($at, $to, $n, undef, $hash) = unpack("V4 a32", substr($b, 0x40 + 0x30 * $i, 0x30));
			next if $n == 0;
			exit 1 if ($at | $n) % 0x200 || $at + $n > length($b) || $to % 4 || !grep({ $to >= $_->[0] && $to + $n <= $_->[1] } @region) || sha256(substr($b, $at, $n)) ne $hash;
			$want += $n;
			$in9 ||= $arm9 >= $to && $arm9 < $to + $n;
			$in11 ||= $arm11 >= $to && $arm11 < $to + $n;
		}
		exit 1 unless $in9 && $in11 && length($b) >= $want;' "$1"
}

# fakeFirmBuild <out> [plant]: builds a FIRM that boot9strap boots, with two sections holding the entry
# points and an empty one past the end, or the FIRM for one plant (testFirmRules.sh pins each plant's
# verdict). Six plants are the section layouts of real releases, taken from their headers (offset, load
# address, size), over bytes of this builder's own: Luma3DS v13.5, GodMode9 v2.2.3, SafeB9SInstaller
# v0.0.7, ntrboot_flasher v0.5.0, 3ds_hw_test v0.1.0 and bmbt3DS v1.2.
fakeFirmBuild() {
	perl -MDigest::SHA=sha256 -e '
		my ($out, $plant) = @ARGV;
		my $f = "\0" x 0x200 . join("", map { chr($_ * 7 & 255) } 0 .. 0x25ff);
		$f .= "\5" x 0x200 if $plant =~ /^(wrap|region|regionend|regionlow|regionpast)$/; # room for a third section
		substr($f, 0, 16) = "FIRM" . pack("V3", 0, 0x1ff80004, 0x08006000);
		my @s = ([0x200, 0x08006000, 0x200], [0x400, 0x1ff80000, 0x2400], [0xfffffe00, 0, 0]);
		my %real = (
			luma => [0, 0x1ff80000, 0x08006000, [0x200, 0x18180000, 0x2a600], [0x2a800, 0x1ff80000, 0x800], [0x2b000, 0x08006000, 0x18a00], [0x43a00, 0x18000000, 0x7800]],
			gm9 => [0, 0x1ff81dc0, 0x08000070, [0x200, 0x08000040, 0x73c00], [0x73e00, 0x080a0000, 0x39800], [0xad600, 0x1ff80000, 0x4000]],
			safeb9s => [1, 0, 0x23f00000, [0x200, 0x23f00000, 0x16e00]],
			ntrboot => [1, 0, 0x20600000, [0x200, 0x20600000, 0x37000]],
			hwtest => [2, 0x18100000, 0x08000100, [0x200, 0x08000100, 0x3200], [0x3400, 0x18100000, 0x5000]],
			bmbt => [1, 0, 0x08006000, [0x200, 0x08006000, 0x3e00]],
		);
		if (my $r = $real{$plant}) {
			my ($flags, $arm11, $arm9, @sec) = @$r;
			$f = "\0" x 0x200 . join("", map { chr($_ * 7 & 255) } 0 .. $sec[-1][0] + $sec[-1][2] - 0x201);
			substr($f, 0, 17) = "FIRM" . pack("V3", 0, $arm11, $arm9) . chr($flags);
			@s = @sec;
		}
		$s[1] = [0x408, 0x1ff80000, 0x2200] if $plant eq "offset";
		$s[1] = [0x400, 0x1ff80000, 0x23f8] if $plant eq "length";
		$s[0] = [0, 0x08006000, 0x200] if $plant eq "self"; # its hash is part of what it hashes
		$s[0][1] = 0x08005ffe if $plant eq "unaligned"; # still holds the ARM9 entry point
		$s[2] = [0x2800, 0xfffffe00, 0x200] if $plant eq "wrap"; # ends at 2^32: a u32 end reads 0
		$s[2] = [0x2800, 0x30000000, 0x200] if $plant eq "region"; # past FCRAM, the last memory it loads into
		$s[2] = [0x2800, 0x1ffffa00, 0x200] if $plant eq "regionend"; # ends where DSP and AXI WRAM does
		$s[2] = [0x2800, 0x1ffffc00, 0x200] if $plant eq "regionpast"; # starts there: 0x200 bytes over
		$s[2] = [0x2800, 0x07fffe00, 0x200] if $plant eq "regionlow"; # ends where ARM9 RAM starts
		@s = ([0x200, 0x08006000, 0x200], [0x200, 0x1ff80000, 0x200]) if $plant eq "short"; # one range twice
		@s = ([0x200, 1, 0x200]) if $plant eq "verifier"; # a section at address 1, which holds the ARM9 entry point (1)
		$s[2] = [0x123, 0, 0] if $plant eq "emptyodd"; # an empty one is skipped wherever it starts
		@s = ([0x200, 0x08006000, 0], [0x400, 0x1ff80000, 0]) if $plant eq "none";
		$s[0][1] = 0 if $plant eq "arm9zero"; # loaded at 0: only the zero check refuses entry 0
		for my $i (0 .. $#s) {
			my ($at, $to, $n) = @{ $s[$i] };
			substr($f, 0x40 + 0x30 * $i, 0x30) = pack("V4", $at, $to, $n, 0) . ($n ? sha256(substr($f, $at, $n)) : "\0" x 32);
		}
		substr($f, 0, 1) = "G" if $plant eq "magic";
		substr($f, 0x40, 4) = pack("V", 0xfffffe00) if $plant eq "bounds"; # a u32 sum wraps
		substr($f, 0x500, 1) ^= "\1" if $plant eq "byte";
		substr($f, 0x0c, 4) = pack("V", 0x08000000) if $plant eq "arm9";
		substr($f, 0x0c, 4) = pack("V", 0x08006200) if $plant eq "arm9end"; # just past its section
		substr($f, 0x08, 4) = pack("V", 0x08000000) if $plant eq "arm11";
		substr($f, 0x0c, 4) = pack("V", 0) if $plant eq "arm9zero";
		substr($f, 0x08, 4) = pack("V", 0) if $plant eq "arm11zero";
		substr($f, 0x08, 8) = pack("V2", 0, 1) if $plant eq "verifier";
		substr($f, 0x10, 1) = "x" if $plant eq "other"; # a byte no check reads
		$f = substr($f, 0, -1) if $plant eq "cut";
		$f = substr($f, 0, 0x1ff) if $plant eq "header";
		$f = substr($f, 0, 0x400) if $plant =~ /^(short|verifier)$/;
		open(my $o, ">:raw", $out) or die "$out: $!";
		print $o $f;' "$1" "${2:-}"
}

# fakeDigest <field>: the device's argDigest.
fakeDigest() {
	detail=grammar
	requireDigest "$1" digest >/dev/null 2>&1
}

# fakeXfer <bytes> <rx|tx> <cia|bench>: this request is the last transfer.
fakeXfer() { printf '%s %s %s %s\n' "$id" "$2" "$1" "$3" >"$FAKE/xfer"; }

# foreign-req names another request in the transfer records alone. foreign-both names it in the receipt
# too, as a foreign put that replaced both "latest" records does. foreign-mb names it in the mb listing
# alone, as a foreign put that came after the stats were read does.
fakeXferStats() {
	local of=- dir bytes=0 kind="" ring=- writes=0 mb
	if [ ! -s "$FAKE/xfer" ]; then
		echo "bytes=0 total_ms=- stall_n=0 req=-"
		return
	fi
	read -r of dir bytes kind <"$FAKE/xfer"
	case "$FAKE_MODE" in foreign-req | foreign-both) of=ffffffffffffffffffffffffffffffff ;; esac
	if [ "$kind" = cia ]; then
		ring=$FAKE_TIER1
		[ "$bytes" -le "$ring" ] || ring=$((ring + FAKE_TIER2))
		[ "$FAKE_MODE" != small-ring ] || ring=16777216
		writes=$(((bytes + 4194303) / 4194304))
	fi
	mb=$((bytes / 1000000))
	echo "bytes=$bytes total_ms=1000 first_write_ms=40 ring_cap=$ring ring_high=- join=- am_write_max_ms=- mb=$mb slow_mb=0 worst_mb_ms=10 stall_n=0 stall_ms=0 stall_max_ms=- am_write_us=$((writes * 1000)) am_write_n=$writes req=$of"
}

# fakeSelfVersion: prints the running 3Drop's title version, as main.c caches it at boot (0 is a possible
# version: makeCIA.sh's plain build). The parentheses in the print are for the Mac's awk, which takes no
# bare comparison there.
fakeSelfVersion() { awk -v self="$FAKE_SELF" '$1 == self { v = $2 } END { print (v != "" ? v : 52) }' "$FAKE/installed"; }
# fakeAc: prints the charger as `stats self` reports it: the first line of $FAKE/ac, or 1 if there is
# none. The next line is used by the next `stats self`, so an arm can lose its charger between two.
fakeAc() {
	local ac rest=""
	{ IFS= read -r ac && rest=$(cat); } 2>/dev/null <"$FAKE/ac" || ac=1
	[ -z "$rest" ] || printf '%s\n' "$rest" >"$FAKE/ac"
	echo "$ac"
}

# Am_tickets: the installed titles' tickets, plus any a mode kept even though its title went (a leak the
# last rep's uninstall can then leave). Only a title's removal gives up its ticket.
fakeTickets() {
	local kept=0
	[ ! -s "$FAKE/tickets-kept" ] || read -r kept <"$FAKE/tickets-kept"
	echo $(($(wc -l <"$FAKE/installed") + 40 + kept))
}

fake_stats() {
	detail=grammar
	case "${f[2]:-xfer}" in
	xfer) fakeReply "stats ok ok 00000000 $(fakeXferStats)" ;;
	# self-nover: once 3Drop has relaunched, `stats self` names no build
	self) fakeReply "stats ok ok 00000000 $([ "$FAKE_MODE" = self-nover ] && [ -e "$FAKE/relaunched" ] || echo "ver=$(fakeSelfVersion) ")tid=$FAKE_SELF state=ready am_pending=0 am_cleanup=0 am_rc=00000000 free=1000000 cluster=32768 clusters=64 am_titles=$(($(wc -l <"$FAKE/installed"))) am_tickets=$(fakeTickets) wifi=3 bat=5 charging=0 ac=$(fakeAc) lin_free=0 model=n3dsxl new3ds=1 sysver=11.17.0-50U luma=13.1.2 region=USA lid=open" ;;
	# old-op: a 3Drop from before `stats op` reported its version. Its release is the one in $FAKE/rel,
	# else dev.
	op) fakeReply "stats ok ok 00000000 $([ "$FAKE_MODE" = old-op ] || echo "ver=$(fakeSelfVersion) rel=$(cat "$FAKE/rel" 2>/dev/null || echo dev) ")$(cat "$FAKE/receipt" 2>/dev/null || echo op none)" ;;
	*) return 1 ;;
	esac
}

fakeEntry() { # <file> <name> [t]: with t, the entry includes its mtime, as an SD walk names it
	local mtime=-
	[ -z "${3:-}" ] || mtime=$(perl -e 'print +(stat shift)[9]' "$1")
	if [ -d "$1" ]; then
		echo "d - $mtime $(wireHex "$2")"
	else
		echo "f $(fileSize "$1") $mtime $(wireHex "$2")"
	fi
}

# fakeStream <file> <trailer fields> [header fields] [outcome]. Faults: short-body (no trailer),
# stall-body (30 s silent), slow-body (five pieces 1.5 s apart), stopped, timed-out, send-failed,
# ended-early and unhashed (a `rejected` trailer), corrupt-body, miscount and misdigest; no-trailer,
# half-trailer (its first 60 bytes) and stall-trailer (30 s silent before it), after the whole body.
fakeStream() {
	fakeReply "$verb ${4:-ok} ok 00000000${3:-}"
	case "$FAKE_MODE" in
	short-body | stopped | timed-out | send-failed) head -c $(($(fileSize "$1") / 2)) "$1" ;;
	stall-body)
		head -c $(($(fileSize "$1") / 2)) "$1"
		fakeSilent
		;;
	slow-body)
		for piece in 0 1 2 3 4; do
			[ "$piece" = 0 ] || sleep 1.5
			dd if="$1" bs=$((($(fileSize "$1") + 4) / 5)) skip="$piece" count=1 2>/dev/null
		done
		;;
	corrupt-body) { hexDecode "$(fakeDamaged "$1" 0)" && tail -c +2 "$1"; } ;;
	*) cat "$1" ;;
	esac
	case "$FAKE_MODE" in
	short-body | no-trailer) ;;
	half-trailer) fakeTrailer "${4:-ok} ok 00000000 $2" | head -c 60 ;;
	stall-trailer) fakeSilent ;;
	stopped) fakeTrailer "rejected read c8804464 - $(($(fileSize "$1") / 2))" ;;
	timed-out) fakeTrailer "rejected timeout d963f805 - $(($(fileSize "$1") / 2))" ;;
	send-failed) fakeTrailer "rejected send d963f816 - $(($(fileSize "$1") / 2))" ;;
	ended-early) fakeTrailer "rejected read c8804464 $2" ;;
	unhashed) fakeTrailer "rejected hash c8804478 ${2%% *} -" ;;
	# A listing's trailer is `<count> <digest>`, a get's `<digest> <length>`. miscount makes the number one
	# more; misdigest swaps the digest for another.
	miscount) fakeTrailer "ok ok 00000000 $(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+$/) { $i++; break } } 1' <<<"$2")" ;;
	misdigest) fakeTrailer "ok ok 00000000 $(awk -v z="u:$(printf '%064d' 0)" '{ for (i = 1; i <= NF; i++) if ($i ~ /^u:/) $i = z } 1' <<<"$2")" ;;
	*) fakeTrailer "${4:-ok} ok 00000000 $2" ;;
	esac
}

# ls: replies with `f <size> <mtime|-> <name-hex>`, `d - <mtime|-> <name-hex>` or `t <version> <size>
# <id>` lines. A tree is listed in byte order, so a test reads one order on every host (a directory still
# comes before what it holds). $FAKE/ls-wait holds the seconds a console busy with a transfer takes to
# answer a listing.
fake_ls() {
	local target=${f[2]} flags=${f[3]:-} depth=(-maxdepth 1) t v size times=""
	[ ! -s "$FAKE/ls-wait" ] || fakeStoppable sleep "$(cat "$FAKE/ls-wait")"
	detail=grammar
	case "$flags" in "" | d | t | dt | r | rt) ;; *) return 1 ;; esac
	if [[ "$target" == title:* ]]; then
		[ -z "$flags" ] || return 1
		tid=${target#title:}
		[ -z "$tid" ] || { fakeTitle "$tid" && fakeInstalled "$tid"; } || return 1
		while read -r t v; do # 3Drop's and anything not installable are not listed
			# AM's size: the contents a put installed ($FAKE/sizes), else none recorded
			size=0
			[ ! -s "$FAKE/sizes" ] || size=$(awk -v t="$t" '$1 == t { s = $2 } END { print s + 0 }' "$FAKE/sizes")
			[ "$v" != - ] || size=-
			if [[ -z "$tid" || "$t" == "$tid" ]] && fakeTitle "$t"; then echo "t $v $size $t"; fi
		done <"$FAKE/installed" >"$FAKE/list"
	else
		fakeNormalize "$target" || return 1
		file=$(fakeSd "${path%/}") detail=notfound
		[ -e "$file" ] || return 1
		[[ "$flags" != *r* ]] || depth=()
		[[ "$flags" != *t* || "$root" != sd ]] || times=t
		if [ -d "$file" ] && [[ "$flags" != *d* ]]; then
			# fakeEntry's lines, made by one perl for all of them
			find "$file" -mindepth 1 ${depth[@]+"${depth[@]}"} | LC_ALL=C sort | perl -ne '
				BEGIN { binmode STDIN; ($top, $t) = splice(@ARGV, 0, 2) } # bytes, whatever PERL_UNICODE says
				chomp; my @s = stat; my $m = $t ? $s[9] : "-"; my $h = unpack("H*", substr($_, length($top) + 1));
				print -d _ ? "d - $m $h\n" : "f $s[7] $m $h\n"' "$file" "$times"
		elif [ "$path" = / ]; then
			fakeEntry "$file" / "$times"
		else
			fakeEntry "$file" "${path##*/}" "$times"
		fi >"$FAKE/list"
	fi
	fakeStream "$FAKE/list" "$(wc -l <"$FAKE/list" | tr -d ' ') $(unitDigest "$FAKE/list")"
}

# fakeRange <offset> <length>: the device's parseRange; past 18 digits bash would wrap, where the
# device answers a size the host never sends.
fakeRange() { [[ "$1$2" != *[!0-9]* && ${#1} -le 18 && ${#2} -le 18 ]] && [ "$2" -gt 0 ]; }

# fakeSlice <file> <offset> <length>: those bytes of it.
fakeSlice() { [ "$3" = 0 ] || tail -c +$(($2 + 1)) "$1" | head -c "$3"; }

# fakeCut <file> <bytes>: the device's FSFILE_SetSize, cutting the file down to that many bytes.
fakeCut() { perl -e 'truncate($ARGV[0], $ARGV[1]) or die "truncate: $!"' "$1" "$2"; }

fake_get() {
	local raw kind=raw at length mtime
	detail=grammar
	case "${f[3]:-}:$n" in
	:3 | gzip:4) ;;
	range:6) fakeRange "${f[4]}" "${f[5]}" || return 1 ;;
	*) return 1 ;;
	esac
	fakePath "${f[2]}" "" read || return 1
	file=$(fakeSd "$path") detail=notfound
	[ ! -d "$file" ] || detail=isdir # src/sdXfer.c openRefusal
	[ -f "$file" ] || return 1
	raw=$(fileSize "$file")
	if [ "${f[3]:-}" = range ]; then # a range gives as many bytes as there are; mtime is `-` in an archive
		at=${f[4]} length=${f[5]} mtime=- detail=size
		[ "$at" -le "$raw" ] || return 1
		[ "$length" -le $((raw - at)) ] || length=$((raw - at))
		[ "$FAKE_MODE" != range-short ] || length=$((length - 1)) # a piece one byte short of the one asked, all else true to it
		[ "$root" != sd ] || mtime=$(perl -e 'print +(stat shift)[9]' "$file")
		fakeSlice "$file" "$at" "$length" >"$FAKE/send"
		fakeStream "$FAKE/send" "$(unitDigest "$FAKE/send") $length" " $length raw $raw $mtime"
		return
	fi
	cp "$file" "$FAKE/send"
	if [ "${f[3]:-}" = gzip ] && THREEDROP_COMPRESSION=auto compressionWorthwhile "$file"; then
		gzip -c "$file" >"$FAKE/send"
		# gzip-bomb: a body that inflates to 16777216 bytes, far past the size the header declares;
		# gzip-huge: that body under a declared size past any piece.
		case "$FAKE_MODE" in gzip-bomb | gzip-huge) head -c 16777216 /dev/zero | gzip -c >"$FAKE/send" ;; esac
		[ "$FAKE_MODE" != gzip-huge ] || raw=1099511627776
		[ "$FAKE_MODE" != gz-truncated ] || fakeCut "$FAKE/send" $(($(fileSize "$FAKE/send") / 2)) # its trailer true to what goes
		kind=gzip
	fi
	fakeStream "$FAKE/send" "$(unitDigest "$file") $(fileSize "$FAKE/send")" " $raw $kind"
}

# hash <path>: its size and unit digest, read as a get reads it.
fake_hash() {
	fakePath "${f[2]}" "" read || return 1
	file=$(fakeSd "$path") detail=notfound
	[ ! -d "$file" ] || detail=isdir
	[ -f "$file" ] || return 1
	fakeReply "hash ok ok 00000000 $(fileSize "$file") $(unitDigest "$file")"
}

fakeCheck_put() {
	detail=grammar
	case "${f[3]}" in
	new | update | self)
		# 0 < prefix < size, prefix <= CIA_MAX_PREFIX_SIZE; a batch is 1 <= index <= total <= 9999
		# with installed < index.
		[[ ("$n" = 7 || "$n" = 10) && "${f[4]}${f[5]}${f[7]:-}${f[8]:-}${f[9]:-}" != *[!0-9]* ]] &&
			[ "${f[5]}" -gt 0 ] && [ "${f[5]}" -lt "${f[4]}" ] && [ "${f[5]}" -le "$FAKE_PREFIX_MAX" ] || return 1
		[ "$n" = 7 ] || { [ "${f[7]}" -ge 1 ] && [ "${f[7]}" -le "${f[8]}" ] &&
			[ "${f[8]}" -le 9999 ] && [ "${f[9]}" -lt "${f[7]}" ]; } || return 1
		fakeDigest "${f[6]}"
		;;
	*)
		# A range before the file's last may give its size as `-`. The size is then held as FAT32's largest
		# file, which a range of whole units never ends.
		local size=${f[4]} unsized=""
		[[ "$n:${f[6]:-}" =~ ^(6:|8:gzip|10:range|7:system)$ ]] && fakePath "${f[3]}" || return 1
		if [ "$root" = sd ]; then fakeWritable "$path" "" "${f[6]:-}"; else fakeSchemeWritable; fi || return 1
		detail=grammar
		if [ "$n:$size" = 10:- ]; then size=$WIRE_FILE_MAX unsized=1; fi
		[[ "$size${f[7]:-}" != *[!0-9]* ]] && [ "${#size}" -le 10 ] &&
			[ "$size" -le "$WIRE_FILE_MAX" ] && { [ "$n" -le 7 ] || [ "$root" = sd ]; } || return 1
		if [ "$n" = 8 ]; then
			[ "${f[7]}" -lt "$size" ] || return 1
		# A digest over WIRE_UNIT-byte units: a range starts on a unit boundary, and the last one can end
		# off one.
		elif [ "$n" = 10 ]; then
			fakeRange "${f[7]}" "${f[8]}" && [ $((f[7] % WIRE_UNIT)) = 0 ] &&
				{ [ $((f[8] % WIRE_UNIT)) = 0 ] || { [ -z "$unsized" ] && [ $((f[7] + f[8])) -ge "$size" ]; }; } || return 1
			detail=size
			[ $((f[7] + f[8])) -le "$size" ] || return 1
		fi
		fakeDigest "${f[5]}" && { [ "$n" != 10 ] || fakeDigest "${f[9]}"; } || return 1
		file=$(fakeSd "$path")
		if [ "$root" = sd ]; then
			detail=path # the temp and backup siblings add ".3drop.tmp" and must fit too
			[ $((${#path} + 10)) -lt "$FAKE_PATH_MAX" ] && [ $((units + 10)) -le "$FAKE_PATH_UNITS" ] || return 1
			detail=system result=d963f818 # ON_BATTERY: no /boot.firm goes off the charger
			! caseless "$path" /boot.firm || [ "$(cat "$FAKE/ac" 2>/dev/null || echo 1)" = 1 ] || return 1
			detail=size result=00000000 # a /boot.firm is read back whole into tier 1 to be checked
			! caseless "$path" /boot.firm || [ "$size" -le "$FAKE_TIER1" ] || return 1
		else # a save or extdata: the body whole in tier 1, an extdata file only at its size
			fakeArchive || return 1
			detail=size
			[ "${f[4]}" -le "$FAKE_TIER1" ] &&
				[[ "$root" != extdata:* || ! -f "$file" || "$(fileSize "$file")" = "${f[4]}" ]] || return 1
		fi
		detail=exists
		[ ! -d "$file" ] || return 1
		# What the temp keeps, read back: the device reads it back unless it holds its digest.
		[[ "$n" = 10 && "${f[7]}" != 0 ]] || return 0
		detail=size result=c8804478
		[ -f "$file$WIRE_PUT_TEMP" ] || return 1
		result=00000000
		[ "$(fileSize "$file$WIRE_PUT_TEMP")" -ge "${f[7]}" ] || return 1
		detail=digest
		[ "$(unitDigest "$file$WIRE_PUT_TEMP" "${f[7]}")" = "${f[9]}" ]
		;;
	esac
}

# fakeCommitted <digest>: the digest a commit names. other-bytes makes it name another file's.
fakeCommitted() { if [ "$FAKE_MODE" = other-bytes ]; then printf 'u:%064d\n' 0; else echo "$1"; fi; }

fake_put() {
	local ver have outcome text got skipped=""
	if [ "${f[6]:-}" = range ]; then
		fakePutRange
		return
	fi
	if [[ ! "${f[3]}" =~ ^(new|update|self)$ ]]; then
		file=$(fakeSd "$path") detail=digest
		rm -f "$file$WIRE_PUT_TEMP" # a whole put takes what a range kept
		head -c "${f[7]:-${f[4]}}" >"$FAKE/body"
		cp "$FAKE/body" "$FAKE/wire" # the bytes as they came
		if [ "${f[6]:-}" = gzip ]; then echo gzip; else echo none; fi >"$FAKE/compression"
		# As src/sdXfer.c does, refuse with `gzip` unless the body inflates whole to the size.
		if [ "${f[6]:-}" = gzip ]; then
			detail=gzip
			gzip -dc <"$FAKE/body" >"$FAKE/raw" 2>/dev/null && [ "$(fileSize "$FAKE/raw")" = "${f[4]}" ] || return 1
			mv "$FAKE/raw" "$FAKE/body"
			detail=digest
		fi
		[[ "$root" != sd || "$(fakeKnob nohash 0)" != 1 ]] || skipped=1 # bench nohash: no digest taken
		[ -n "$skipped" ] || [ "$(unitDigest "$FAKE/body")" = "${f[5]}" ] || {
			result=d963f80a # src/result.h STAGE_DIGEST, as src/sdXfer.c refuses
			return 1
		}
		if [ "$root" = sd ] && caseless "$path" /boot.firm && ! fakeFirm "$FAKE/body"; then
			fakeReply "put rejected system d963f809" # STAGE_METADATA: the temp goes, the file stays
			return
		fi
		if [[ "$FAKE_MODE" == secure-value && "$root" == save:* ]]; then # the value would not go
			fakeReply "put rejected secure-value c8a04554"
			return
		fi
		# As src/sdXfer.c does, refuse a temp whose folder cannot be made (`open`) and a rename that fails.
		detail=open
		mkdir -p "${file%/*}" 2>/dev/null || return 1
		detail=publish
		mv "$FAKE/body" "$file" 2>/dev/null || return 1
		if [ -n "$skipped" ]; then
			fakeReply "put committed ok 00000000 ${f[4]} digest=skipped"
		else
			fakeReply "put committed ok 00000000 ${f[4]} $(fakeCommitted "${f[5]}")"
		fi
		return
	fi
	# A paused tool or a Mac asleep: the pause is 12 s after the body's first 64 KB, in its first 8 MB
	# segment, which the host cannot have buffered whole, so its own time shows the pause. ghost-then-fast
	# pauses again at the second segment's first byte, so the first put is a ghost by the real rule (two slow
	# segments in a row) and the retry, with no pause, commits. A read stops at each pause's byte because a
	# short socket read would walk past it and the pause would never come.
	if [ "$FAKE_MODE" = pause-once ] ||
		{ [ "$FAKE_MODE" = ghost-then-fast ] && [ ! -e "$FAKE/paused" ]; }; then
		# shellcheck disable=SC2016 # perl's variables, not the shell's
		fakeStoppable perl -e 'binmode STDIN; binmode STDOUT; ($x, $y, $n) = @ARGV; while ($m < $n) { $w = $m < $x ? $x - $m : $m < $y ? $y - $m : $n - $m; $r = sysread(STDIN, $b, $w > 65536 ? 65536 : $w) or last; print $b; $m += $r; if ($m == $x || $m == $y) { sleep 12 } }' 65536 \
			"$([ "$FAKE_MODE" = ghost-then-fast ] && echo 8388608 || echo 0)" "${f[4]}" >"$FAKE/body"
		[ "$FAKE_MODE" != ghost-then-fast ] || : >"$FAKE/paused" # the pauses, then a fast put
	else
		head -c "${f[4]}" >"$FAKE/body"
	fi
	got=$(fileSize "$FAKE/body")
	echo "$got" >>"$FAKE/bytes"
	fakeXfer "$got" rx cia
	# The device parses the prefix against the declared size, so a body cut short still names its title.
	[ "$got" = "${f[4]}" ] || head -c $((f[4] - got)) /dev/zero >>"$FAKE/body" # BSD head refuses a count of 0
	tid=$(ciaTitleID "$FAKE/body") ver=$(ciaRead "$FAKE/body" && echo "$ciaVer")
	# The sender closed, so AM cancels and the receipt says so. lost-receipt loses that receipt, slow-cancel
	# answers nothing for six requests first, and ghost-commits commits the body cut short, which the device
	# cannot do. It exercises the host's rule that a committed receipt ends a run.
	if [ "$got" != "${f[4]}" ] && [ "$FAKE_MODE" != ghost-commits ]; then
		[ "$FAKE_MODE" != slow-cancel ] || echo 6 >"$FAKE/down"
		[ "$FAKE_MODE" = lost-receipt ] || fakeReply "put rejected short d963f802 $tid $ver ${f[3]}"
		return
	fi
	if [ "$FAKE_MODE" != ghost-commits ] && [ "$(unitDigest "$FAKE/body" "${f[5]}")" != "${f[6]}" ]; then # src/install.c, before AM
		fakeReply "put rejected digest d963f80a - - ${f[3]}"
		return
	fi
	have=$(awk -v t="$tid" '$1 == t { print $2 }' "$FAKE/installed")
	outcome="committed ok"
	if [ "$FAKE_MODE" = put-exists ]; then # another client installed it after the bench's absence check
		outcome="rejected exists"
	elif ! fakeInstallable "$tid"; then
		outcome="rejected unmanaged"
	elif [ "$([ "$tid" = "$FAKE_SELF" ] && echo self)" != "$([ "${f[3]}" = self ] && echo self)" ]; then
		outcome="rejected route" # the self kind goes with 3Drop's own title, and only with it
	elif [ "${f[3]}" != self ] && ! fakeTitleRefusal "$tid"; then
		outcome="rejected $detail"
	elif [ "$have" = - ]; then
		outcome="rejected am-info" # AM gives no info for it: never installed over
	elif [ "${f[3]}" = new ] && [ -n "$have" ]; then
		outcome="rejected exists"
	elif [ "${f[3]}" != new ] && [ -z "$have" ]; then
		outcome="rejected notfound" result=d8a083fa
	elif [ "${f[3]}" != new ] && [ "$ver" -le "$have" ]; then
		outcome="rejected not-newer"
	elif [ "$FAKE_MODE" != ambiguous ]; then
		grep -v "^$tid " "$FAKE/installed" >"$FAKE/installed.new" || true
		printf '%s %s\n' "$tid" "$ver" >>"$FAKE/installed.new"
		mv "$FAKE/installed.new" "$FAKE/installed"
		echo "$tid $((got - $(ciaPrefixSize "$FAKE/body")))" >>"$FAKE/sizes" # AM's size: what follows the TMD
	fi
	text="put $outcome $result $tid $ver ${f[3]}"
	case "$FAKE_MODE" in
	lost-put) printf 'op %s %s\n' "$id" "$text" >"$FAKE/receipt" ;; # the reply is lost, not the receipt
	lost-receipt | ambiguous) rm -f "$FAKE/receipt" ;;
	gone-after) # it commits, then the link goes: no reply, no receipt, nothing answered until a test says
		rm -f "$FAKE/receipt"
		echo 1000000 >"$FAKE/down"
		;;
	slow-relaunch) # the relaunch takes the reply and the receipt, and answers nothing for a while
		rm -f "$FAKE/receipt"
		echo "$FAKE_RELAUNCH_QUIET" >"$FAKE/down"
		;;
	*) fakeReply "$text" ;;
	esac
}

# fakePutRange: the device's range put. It takes the body after what the temp keeps and checks the
# temp through the body's end. A failure cuts the temp back to what it kept, or removes a temp that
# kept nothing; the last range publishes. corrupt-put damages the body's first byte; cut-put takes
# half the body, as a dropped link leaves it, and loses the reply to the receipt's `short`.
fakePutRange() {
	local at=${f[7]} length=${f[8]} temp
	file=$(fakeSd "$path")
	temp=$file$WIRE_PUT_TEMP
	mkdir -p "${file%/*}"
	[ "$at" != 0 ] || : >"$temp"
	fakeCut "$temp" "$at"
	[ "$FAKE_MODE" != cut-put ] || FAKE_MODE=lost-reply length=$((length / 2))
	head -c "$length" >"$FAKE/body"
	[ "$FAKE_MODE" != corrupt-put ] || filePatch "$FAKE/body" 0 "$(fakeDamaged "$FAKE/body" 0)"
	cat "$FAKE/body" >>"$temp"
	detail=short result=d963f802
	[ "$(fileSize "$FAKE/body")" = "${f[8]}" ] && detail=digest result=d963f80a &&
		[ "$(unitDigest "$temp")" = "${f[5]}" ] && detail=ok
	if [ "$detail" != ok ]; then
		if [ "$at" = 0 ]; then rm -f "$temp"; else fakeCut "$temp" "$at"; fi
		fakeReply "put rejected $detail $result"
		return
	fi
	[ $((at + length)) != "${f[4]}" ] || mv "$temp" "$file"
	fakeReply "put committed ok 00000000 $((at + length)) $(fakeCommitted "${f[5]}")"
}

fakeCheck_delete() {
	local target=${f[3]} r=${f[4]:-}
	detail=grammar
	if [[ "$target" == title:* && "$r" != "" && "$r" != ticket ]] ||
		[[ "$target" != title:* && "$r" != "" && "$r" != r && "$r" != system ]]; then return 1; fi
	if [[ "$target" == title:* ]]; then # a patch's or DLC's ticket is its purchase
		fakeTitle "${target#title:}" "${r:+app}" && fakeInstalled "${target#title:}"
		return
	fi
	fakePath "$target" || return 1
	detail=refused # no subtree of a game's data in one request
	[[ "$root" == sd || "$r" != r ]] || return 1
	if [ "$root" = sd ]; then fakeRemovable "$r"; else fakeSchemeWritable && fakeArchive; fi || return 1
	detail=notfound
	[ -e "$(fakeSd "$path")" ] || return 1
	detail=system # the word takes single files
	[[ "$r" != system || ! -d "$(fakeSd "$path")" ]]
}

fake_delete() {
	if [[ "${f[3]}" == title:* && "$FAKE_MODE" == refuse-uninstall ]]; then
		fakeReply "delete rejected am-delete d8a08006 ${f[3]#title:}"
		return
	fi
	if [[ "${f[3]}" == title:* ]]; then
		grep -v "^${f[3]#title:} " "$FAKE/installed" >"$FAKE/installed.new" || true
		mv "$FAKE/installed.new" "$FAKE/installed"
		if [ "$FAKE_MODE" = keep-ticket ]; then # the title went, its ticket stayed: a leak to see
			local kept=0
			[ ! -s "$FAKE/tickets-kept" ] || read -r kept <"$FAKE/tickets-kept"
			echo $((kept + 1)) >"$FAKE/tickets-kept"
		fi
		if [ "$FAKE_MODE:${f[4]:-}" = unknown-ticket:ticket ]; then # gone, its ticket unproven
			fakeReply "delete unknown ticket d8a08006 ${f[3]#title:}"
		else
			fakeReply "delete committed ok 00000000 ${f[3]#title:}"
		fi
		return
	fi
	file=$(fakeSd "$path") detail=delete
	if [[ "$FAKE_MODE" == secure-value && "$root" == save:* ]]; then # the value would not go
		fakeReply "delete rejected secure-value c8a04554"
		return
	fi
	if [ ! -d "$file" ]; then
		rm -f "$file" 2>/dev/null || return 1
	elif [ "${f[4]:-}" = r ]; then
		rm -rf "$file" 2>/dev/null || return 1
	else
		rmdir "$file" 2>/dev/null || return 1
	fi
	fakeReply "delete committed ok 00000000"
}

fakeCheck_mkdir() { fakePath "${f[3]}" sd && fakeWritable "$path"; }
fake_mkdir() {
	file=$(fakeSd "$path") detail=mkdir
	if [ -f "$file" ]; then
		fakeReply "mkdir rejected exists c82044be" # FS's Result: a file holds the name
	elif mkdir -p "$file" 2>/dev/null; then
		fakeReply "mkdir committed ok 00000000"
	else
		return 1
	fi
}

fakeCheck_rename() {
	fakePath "${f[3]}" sd || return 1
	local from=$path
	fakePath "${f[4]}" sd || return 1
	to=$path path=$from detail=notfound
	[ -e "$(fakeSd "$path")" ] || return 1
	fakeRemovable || return 1
	fakeWritable "$to" "$([ ! -d "$(fakeSd "$path")" ] || echo tree)" || return 1
	detail=exists
	[ ! -e "$(fakeSd "$to")" ]
}
fake_rename() {
	detail=rename
	mv "$(fakeSd "$path")" "$(fakeSd "$to")" 2>/dev/null || return 1
	fakeReply "rename committed ok 00000000"
}

fakeCheck_run() {
	local tid=${f[3]#title:} game=""
	detail=grammar
	case "${f[3]}:$n:${f[4]:-}" in
	self:4: | "title:$FAKE_SELF":4: | "title:$FAKE_SELF":5:game) return 0 ;;
	title:*:5:game) game=game ;;
	self:* | title:*:5:*) return 1 ;;
	esac
	if [[ "${f[3]}" != title:* ]]; then # a 3dsx: Luma's loader is the device's
		[[ "$n" = 4 || "${f[4]}" = stdio ]] && fakePath "${f[3]}" sd read || return 1
		detail=path
		[[ "$path" == *.[3][dD][sS][xX] ]] || return 1
		detail=notfound
		[ -f "$(fakeSd "$path")" ]
		return
	fi
	fakeTitle "$tid" app && fakeInstalled "$tid" || return 1
	# A romfs folder: the device refuses it unless the user called the title a game (src/layeredFs.c).
	detail=layeredfs
	[ -n "$game" ] || [ ! -d "$(fakeSd "/luma/titles/$tid/romfs")" ]
}
# A run leaves 3Drop: the relaunched one mints its own nonce, and slow-relaunch answers nothing for
# FAKE_RELAUNCH_QUIET requests, a relaunch that outlasts ten polls.
fake_run() {
	: >"$FAKE/relaunched"
	while read -r knob _; do rm -f "$FAKE/knob-$knob"; done <"$FAKE/knobs" # a relaunch resets the knobs
	tid=${f[3]#title:}
	# The Testing Tool takes its marker and leaves its evidence where testingTool.c has them
	# (tool-stuck: the launch is accepted and the tool never comes up, so the marker stays).
	if [ "$FAKE_MODE" != tool-stuck ] && rm "$(fakeSd "$FAKE_TOOL_MARKER")" 2>/dev/null; then
		printf 'build 3Drop Testing Tool\ntitle %s\n0 ff00ff\ndone\n' "$tid" >"$(fakeSd "$FAKE_TOOL_EVIDENCE")"
	fi
	[ "$tid" != "$FAKE_SELF" ] || tid=self
	[ "${path:-}" = "" ] || tid="3dsx 000400000d921e00"
	fakeReply "run committed ok 00000000 $tid"
	rm -f "$FAKE/nonce"
	[ "$FAKE_MODE" != slow-relaunch ] || echo "$FAKE_RELAUNCH_QUIET" >"$FAKE/down"
}

# seed title:<id> <seed>: $FAKE/seeds holds the console's seed database, `<title-id> <seed>` lines.
fakeCheck_seed() {
	local tid=${f[3]#title:} held
	detail=grammar
	[[ "${f[3]}" == title:* ]] && requireHex 32 "${f[4]}" seed >/dev/null 2>&1 || return 1
	fakeTitle "$tid" app && fakeInstalled "$tid" || return 1
	held=$(awk -v t="$tid" '$1 == t { print $2 }' "$FAKE/seeds" 2>/dev/null)
	detail=exists
	[[ -z "$held" || "$held" == "${f[4]}" ]]
}
fake_seed() {
	local tid=${f[3]#title:}
	if grep -q "^$tid ${f[4]}$" "$FAKE/seeds" 2>/dev/null; then
		fakeReply "seed committed ok 00000000 $tid present"
	else
		echo "$tid ${f[4]}" >>"$FAKE/seeds"
		fakeReply "seed committed ok 00000000 $tid added"
	fi
}

# bench [sub] [args] as the fake plays it; FAKE_MODE=release has no such verb.
fakeKnob() { cat "$FAKE/knob-$1" 2>/dev/null || echo "$2"; }
# fakeKnobRow <name>: that knob's `<knob> <default> <ceiling>` row, if the device has one.
fakeKnobRow() { awk -v k="$1" '$1 == k' "$FAKE/knobs"; }
# fakeNamed <word>: ` <word>` for a knob or `capture`, which a refused set echoes (refuseArgs); else nothing.
fakeNamed() { if [ "$1" = capture ] || [ -n "$(fakeKnobRow "$1")" ]; then echo " $1"; fi; }

# fakeTail <sub> <argument>...: prints a sub's reply after its frame, with its keys in the order of the
# device's format string (src/bench/bench.c, hw.c, discover.c), which fakeCheckTails holds each tail to.
# Arguments: rx|tx <bytes>; file <bytes> <writes>; mb <request id> <dir> <rows>; hw <request id> <rows>;
# for any other sub, a knob: <applied> <default>.
fakeTail() {
	local sub=$1
	shift
	case "$sub" in
	state) echo "capture=$(fakeKnob capture 0) mb_rows=$FAKE_MB_ROWS" ;; # after the knobs
	rx | tx) echo "$sub bytes=$1 ms=1000 busy_ms=900 idle_ms=100 calls=$(($1 / 65536)) eagain=0" ;;
	file) echo "file bytes=$1 chunk=1048576 writes=$2 prealloc_ms=0 first_ms=40 steady_ms=$(($2 * 60 - 60)) steady_max_ms=70 flush_ms=1 bytes_done=$1 reads=0 read_first_ms=0 read_steady_ms=0 read_steady_max_ms=0 read_done=0" ;;
	mb) echo "mb req=$1 dir=$2 rows=$3 cols=wall_ms,wait_ms,eagain,sd_kb" ;;
	hw) echo "hw req=$1 rows=$2 calls=2 gaps=1 gap_min_ms=30 call_cols=kind,start_us,end_us,k gap_cols=g,quiet_us,call_us,end_us" ;;
	mem) echo "mem lin_heap=33554432 lin_free=6361344 heap=31621120 heap_touched_max=29360128 tier1=$FAKE_TIER1" ;;
	capture) echo "capture capture=$(fakeKnob capture 0) frames=0 held=0 dropped=0 slots=4" ;;
	led) echo "led rgb=000000 shown=000000 breathes=0 gap_max_ms=0 writes=0 failed=0" ;;
	disc) echo "disc=open name=3drop.local asked=0 answered=0 announced=0 probed=0 drained=0 opened=0 unsent=0" ;;
	*) echo "$sub applied=$1 default=$2" ;;
	esac
}
# fakeCapture <on|off>: a capture set as capture.c's benchCaptureSet does it: the state in RAM, and the
# arm file a boot arms from, made or removed on the SD. capture-nosave is a card that refuses the file.
fakeCapture() {
	local arm
	arm=$(fakeSd "$(fakeAppFile src/bench/capture.c CAPTURE_DIR)/arm")
	case "$1" in on) echo 1 ;; *) echo 0 ;; esac >"$FAKE/knob-capture"
	[ "$FAKE_MODE" != capture-nosave ] || return 0
	if [ "$1" = on ]; then
		mkdir -p "${arm%/*}"
		: >"$arm"
	else
		rm -f "$arm"
	fi
}
# fakeKeys <text>: prints the keys of the `key=` words in a reply or a format string, in order.
fakeKeys() {
	local word keys=
	# shellcheck disable=SC2086 # one word per field
	for word in $1; do
		case "$word" in *=*) keys="$keys${word%%=*} " ;; esac
	done
	echo "$keys"
}
# fakeFormat <source file under src/> <opening>: prints the device's format string that opens with that
# text after its quote, as the source writes it, with adjacent literals joined, up to the closing quote.
fakeFormat() {
	local text
	text=$(<"$ROOT/src/$1")
	[[ "$text" == *\""$2"* ]] || return 1
	text=${text#*\""$2"}
	text=$2${text%%\",*}
	echo "${text//\"/}"
}
# fakeCheckTails: each tail has the keys of the device's format string, read from its source as the
# knob rows are, so a reply the device changed fails here and not in some verb's test far from it.
fakeCheckTails() {
	local sub src open args format got want
	while IFS='|' read -r sub src open args; do
		format=$(fakeFormat "$src" "$open") || fail "no format string in src/$src opens with '$open'"
		# shellcheck disable=SC2086 # the sample arguments are words
		got=$(fakeKeys "$(fakeTail "$sub" $args)") want=$(fakeKeys "$format")
		[ "$got" = "$want" ] || fail "the fake's $sub reply has the keys '$got', src/$src's format string '$want'"
	done <<'EOF'
state|bench/bench.c| state%s |
rx|bench/bench.c| bytes=|1048576
file|bench/bench.c| file bytes=|1048576 1
mb|bench/bench.c| mb req=|- rx 1
hw|bench/hw.c| hw req=|- 3
mem|bench/bench.c| mem lin_heap=|
capture|bench/bench.c| capture capture=|
led|bench/bench.c| led rgb=|
disc|discover.c| disc=%s |
nohash|bench/bench.c| %s applied=%s default=|0 0
EOF
}
fake_bench() {
	local sub=${f[2]:-} bytes got knob def hi nonce named ok fields='' extra=''
	detail=grammar
	if [ "$FAKE_MODE:$n" = mute-state:2 ] && [ -e "$FAKE/relaunched" ]; then # silent after `run`
		cat >/dev/null
		return
	fi
	# A set is `bench <id> <nonce> <knob|capture> [value]`, and no sub's name is 64 hex. The nonce is spent
	# as every change's is, so a captured one cannot set again. Grammar is checked first, so a malformed
	# set keeps its nonce as the device's does; then the fields shift as the device's do.
	if [ "${#sub}" = 64 ]; then
		knob=${f[3]:-}
		named=$(fakeNamed "$knob")
		ok=false
		case "$n:$knob:${f[4]:-}" in
		5:capture:on | 5:capture:off) ok=true ;;
		5:capture:*) ;;
		5:*) [ -z "$named" ] || ok=true ;; # a value past a knob's range is the knob's to refuse, after the spend
		esac
		if ! $ok; then
			fakeReply "bench rejected grammar 00000000$named"
			return
		fi
		detail=challenge
		[ ! -s "$FAKE/nonce" ] || read -r nonce <"$FAKE/nonce"
		if [ "$nonce" != "$sub" ]; then
			fakeRefuse
			return
		fi
		rm -f "$FAKE/nonce"
		spent=true
		f=("${f[0]}" "${f[1]}" "${f[3]}" "${f[4]}")
		n=4 sub=$knob
	# A knob or `capture` with a value and no nonce, the old form, is refused as the device refuses
	# it: only a set that names a nonce changes anything.
	elif [ "$n" = 4 ] && [ -n "$(fakeNamed "$sub")" ]; then
		detail=challenge
		fakeRefuse
		return
	fi
	case "$sub:$n" in
	:2)
		while read -r knob def _; do fields="$fields $knob=$(fakeKnob "$knob" "$def")"; done <"$FAKE/knobs"
		[ "$FAKE_MODE" != state-extra ] || extra=" added=1" # a key appended, as the rule has it
		fakeReply "bench ok ok 00000000 state$fields $(fakeTail state)$extra"
		;;
	rx:4)
		[ "$FAKE_MODE" != mute-rx ] || fakeHold
		bytes=$((f[3] * 1048576))
		got=$(head -c "$bytes" | wc -c | tr -d ' ')
		fakeXfer "$got" rx bench
		if [ "$got" = "$bytes" ]; then fakeReply "bench ok ok 00000000 $(fakeTail rx "$got")"; else
			fakeReply "bench rejected short d963f802 $(fakeTail rx "$got")"
		fi
		;;
	tx:4)
		bytes=$((f[3] * 1048576))
		[ "$FAKE_MODE" != tx-short ] || bytes=$((bytes / 2)) # half the unit asked: header, body and trailer agree, so only a value check sees it (more than asked is cut by wire.sh's own cap)
		head -c "$bytes" /dev/zero >"$FAKE/send"
		fakeXfer "$bytes" tx bench
		fakeStream "$FAKE/send" "$(fakeTail tx "$bytes")" " tx bytes=$bytes"
		;;
	mb:3)
		local of=- dir=rx # `req` is the request line the replies' MACs cover
		bytes=0
		[ ! -s "$FAKE/xfer" ] || read -r of dir bytes _ <"$FAKE/xfer"
		[ "$FAKE_MODE" != foreign-mb ] || of=ffffffffffffffffffffffffffffffff # another request's curve
		awk -v n=$((bytes / 1000000)) 'BEGIN { for (i = 1; i <= n; i++) print i * 700, i * 10, 1, i * 1000 }' >"$FAKE/list"
		fakeStream "$FAKE/list" "$(wc -l <"$FAKE/list" | tr -d ' ') $(unitDigest "$FAKE/list")" " $(fakeTail mb "$of" "$dir" $((bytes / 1000000)))"
		;;
	hw:3) # hw-rows: a header that miscounts its lines
		local of=-
		[ ! -s "$FAKE/xfer" ] || read -r of _ <"$FAKE/xfer"
		printf 'w 1000 178000 1\nd 178100 204000 1\ng 5000 6000 45000\n' >"$FAKE/list"
		fakeStream "$FAKE/list" "3 $(unitDigest "$FAKE/list")" " $(fakeTail hw "$of" "$([ "$FAKE_MODE" = hw-rows ] && echo 4 || echo 3)")"
		;;
	mem:3 | led:3 | disc:3) fakeReply "bench ok ok 00000000 $(fakeTail "$sub")" ;;
	fscalls:3) fakeReply "bench ok ok 00000000 fscalls req=-$FAKE_FS_STEPS" ;; # no change yet: every step took none
	file:[4-8])
		bytes=$((f[3] * 1048576))
		fakeReply "bench ok ok 00000000 $(fakeTail file "$bytes" "${f[3]}")"
		;;
	capture:[34])
		case "${f[3]:-}" in on | off) fakeCapture "${f[3]}" ;; "") ;; *)
			fakeReply "bench rejected grammar 00000000 capture"
			return
			;;
		esac
		fakeReply "bench ok ok 00000000 $(fakeTail capture)"
		;;
	*:[34])
		read -r _ def hi <<<"$(fakeKnobRow "$sub")"
		if [ -z "$def" ]; then
			fakeReply "bench rejected grammar 00000000"
			return
		fi
		if [ "$n" = 4 ]; then
			if [[ "${f[3]}" == *[!0-9]* ]] || [ "${f[3]}" -gt "$hi" ]; then
				fakeReply "bench rejected range 00000000 $sub applied=$(fakeKnob "$sub" "$def")"
				return
			fi
			knob=${f[3]}
			[ "$knob" != 0 ] || knob=$def
			echo "$knob" >"$FAKE/knob-$sub"
		fi
		# A socket knob reads its request's own socket too, which the fake has none of.
		fakeReply "bench ok ok 00000000 $(fakeTail "$sub" "$(fakeKnob "$sub" "$def")" "$def")$(case "$sub" in rcvbuf | sndbuf) echo " client=-" ;; esac)"
		;;
	*) fakeReply "bench rejected grammar 00000000" ;;
	esac
}

# screenshot <top|bottom>: $FAKE/<screen>.bmp in get's frame.
fakeCheck_screenshot() {
	detail=grammar
	[ "${f[3]}" = top ] || [ "${f[3]}" = bottom ]
}
fake_screenshot() {
	file="$FAKE/${f[3]}.bmp"
	fakeStream "$file" "$(unitDigest "$file") $(fileSize "$file")" " $(fileSize "$file") raw" committed
}

# Every test's device listens on a real port too, since a stream always comes over /dev/tcp.
if [ -n "${fakeOwn:-}" ]; then
	fakeCheckTails
	trap fakeStop EXIT
	fakeListen
	export THREEDROP_PORT=$fakePort
fi

# shellcheck shell=bash
# lib.sh holds what the host tools share: dying and usage, files, private folders, a tool's scratch
# and its child processes. A tool sources it first and wire.sh sources it again; sourced a second
# time it would unset SCRATCH, so the guard below returns.
[ -z "${_libSourced:-}" ] || return 0
_libSourced=1
ROOT="$(CDPATH='' cd "${BASH_SOURCE[0]%/*}/.." && pwd)" # CDPATH is cleared because cd would search it first
# How a tool names itself in its own messages on stderr: drop.sh says `drop:`.
TOOL=${0##*/}
TOOL=${TOOL%.sh}
# The tool's own arguments, taken here because the tool sources this from its top level, before it
# shifts any. scratchTool runs the tool again with them.
TOOL_ARGS=("$@")

die() { dieWith 1 "$@"; }
# dieWith <status> <message>: prints the message on stderr as the tool's own, then exits with the
# status of what failed, as the exit table has it: 1 (the console refused), 3 (a benchmark run on
# this PC holds the console), 4 (no answer), 124 (timed out).
dieWith() {
	local st=$1
	shift
	echo "$TOOL: $*" >&2
	exit "$st"
}

# usage [status [why]]: prints the script's header comment, which opens with its usage line, up to
# its first blank line. When the tool cannot take a call, the header goes to stderr after a line
# saying why (where there is one), and the tool exits 2. --help (`usage 0`) prints it on stdout and
# exits 0, with one line more for --version: if the header has a table, a row under its -h row and
# in its columns; otherwise a line at the end.
usage() {
	local st=${1:-2} fd=2
	[ "$st" != 0 ] || fd=1
	[ -z "${2:-}" ] || echo "$TOOL: $2" >&2
	awk -v help="$((st == 0))" 'NR > 1 {
		if (!/^#/) exit
		sub(/^# ?/, "")
		print
		if (help && match($0, /^ +-h, --help +/)) {
			row = substr($0, 1, RLENGTH)
			sub(/-h, --help/, "--version ", row)
			print row "this copy\047s release"
			help = 0
		}
	}
	END { if (help) print "--version: print this copy\047s release and exit" }' "$0" >&"$fd"
	exit "$st"
}

# releaseName [word]: prints the release this copy is, named as git names the tag it was cloned at
# (the README's install), with -<n>-g<hash> past the tag and -dirty for an edit. This is what
# build.sh gives the console to show. A copy with no .git of its own (one from a zip, or one inside
# another repository, which git would answer for) or with no git gets `word`, or unknown.
# shellcheck disable=SC2120 # build.sh passes its word
releaseName() {
	if [ -e "$ROOT/.git" ]; then git -C "$ROOT" describe --tags --always --dirty 2>/dev/null && return; fi
	echo "${1:-unknown}"
}

# This answers -h, --help and --version for every tool alike, before the tool makes a scratch or
# asks for a key. After `--` a first word like these is the tool's own (a bench label), so only the
# `--` is dropped, and TOOL_ARGS keeps it for scratchTool's rerun. (A tool sources this with no
# arguments, so the shift acts on the tool's own.)
case "${1:-}" in
-h | --help) usage 0 ;;
--version) releaseName && exit 0 ;;
--) shift ;;
esac

# With a native docker.exe under Git Bash, MSYS rewrites each argument that looks like a POSIX path
# as it starts a Windows program (`-w /work` becomes C:/Program Files/Git/work), and the container's
# paths are not this PC's. So a docker call sets MSYS2_ARG_CONV_EXCL='*' for itself alone (as a
# prefix, never exported, because other native programs, such as openssl, need their paths
# converted) and names the bind source the way Windows does, by hostPath. Elsewhere
# MSYS2_ARG_CONV_EXCL is a variable docker ignores and hostPath returns the path as it is.
# hostPath <path>: prints the path the way a native program takes it, C:/src/3Drop for Git Bash's
# /c/src/3Drop.
hostPath() {
	case $OSTYPE in
	msys* | cygwin*) cygpath -m "$1" ;;
	*) printf '%s\n' "$1" ;;
	esac
}

# isUint <text>: succeeds for decimal digits only. Check with it before any arithmetic, which would
# evaluate a name (`x[$(cmd)]` runs cmd).
isUint() { [[ -n "$1" && "$1" != *[!0-9]* ]]; }
isPositive() { isUint "$1" && [[ $1 == *[1-9]* ]]; }

# isPort <text>: succeeds for a TCP port, 1 to 65535, in its one spelling, with no leading 0.
isPort() {
	local LC_ALL=C
	[[ $1 =~ ^[1-9][0-9]{0,4}$ ]] && [ "$1" -le 65535 ]
}

fileSize() {
	local size
	size=$(exec wc -c <"$1") || return # without exec, bash forks once more for a redirect it runs itself
	size="${size//[[:space:]]/}"
	if ! isUint "$size"; then
		echo "$TOOL: invalid size for $1" >&2
		return 1
	fi
	echo "$size"
}

# fileFrom <file> <offset>: prints its bytes from the offset on. dd reads the bytes before the
# offset, which leaves the file's one offset at the point where cat goes on; BSD tail -c +N would
# copy them slowly instead.
fileFrom() {
	{
		[ "$2" = 0 ] || dd bs="$2" count=1 2>/dev/null >/dev/null
		cat
	} <"$1"
}

# fileHex <file> [offset count]: prints its bytes as lowercase hex, since a bash variable cannot
# hold a NUL.
fileHex() { od -An -v -tx1 ${2:+-j "$2" -N "$3"} "$1" | tr -d ' \n'; }

hexDecode() {
	local out="" i
	for ((i = 0; i < ${#1}; i += 2)); do
		out+="\\x${1:i:2}"
	done
	printf '%b' "$out"
}

# filePatch <file> <offset> <hex>: writes those bytes over the file's own, at the offset.
filePatch() { hexDecode "$3" | dd of="$1" bs=1 seek="$2" conv=notrunc 2>/dev/null; }

# These folders are per-user and closed to others, because in a shared $TMPDIR another local user
# could plant a file the tools read. Bulk scratch stays in mktemp's own directories, because
# XDG_RUNTIME_DIR is a small tmpfs. _privateDir <root> prints <root>/3drop, made once per shell.
# _privateDirs lists each folder made, once mkdir and chmod have both gone through, as a whole path,
# since a path may hold any character but NUL (a | too). A $(...) runs in a subshell, whose list
# dies with it, so a tool asks once outside one first (wire.sh's head, and wireKey for a key it
# stretches itself), and each $(cacheDir) after that makes nothing. A tool whose parent stretched
# its key skips that first ask, so each of its $(cacheDir)s makes the folder again.
_privateDirs=()
_privateDir() {
	local root=$1 dir made
	while [[ $root == */ ]]; do root=${root%/}; done # an extra trailing slash is the same folder
	dir=$root/3drop
	for made in ${_privateDirs[@]+"${_privateDirs[@]}"}; do
		if [ "$made" = "$dir" ]; then
			echo "$dir"
			return 0
		fi
	done
	mkdir -p "$dir" && chmod 700 "$dir" && _privateDirs+=("$dir") || return
	echo "$dir"
}
cacheDir() { _privateDir "${XDG_CACHE_HOME:-$HOME/.cache}"; }
runDir() { { [ -n "${XDG_RUNTIME_DIR:-}" ] && _privateDir "$XDG_RUNTIME_DIR" 2>/dev/null; } || cacheDir; }

# cDefine <src-file> <NAME>: prints a device constant's value, so a host copy is the one the device
# builds with. The value is read whole or refused, so no part of one (the 0 of `0x305B14ULL`) passes
# for it, and it must hold only integers and operators, so the arithmetic evaluates no name.
cDefine() {
	local LC_ALL=C value
	value=$(sed -n "s|^#define $2[[:blank:]]\{1,\}||p" "$ROOT/src/$1" | sed 's|[[:blank:]]*//.*||; s|[[:blank:]]*/\*.*||')
	if ! [[ $value =~ ^([[:blank:]()*/+<>-]|(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*)+$ && $value == *[0-9]* ]]; then
		echo "$TOOL: src/$1 has no #define $2 of integers alone${value:+: $value}" >&2
		return 1
	fi
	echo $((${value//[uUlL]/})) # strips the suffixes, and no other u or l is left
}

# One line per build put on a console, tab-separated: its address, its version, the ELF's SHA-256, the
# tree, the flavor and the time. bench.sh refuses a series on a build the console's last line does
# not name, since that is unknown code under test.
armsLedger() { echo "${THREEDROP_ARMS:-$(cacheDir)/arms.tsv}"; }
# armsRecord <ip> <version> <elf-sha256> <tree> <bench|release>: appends that build's line, stamped
# with the time now.
armsRecord() { printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$(date '+%F %T')" >>"$(armsLedger)"; }
# armsLast <ip>: prints the version from the console's last line, or nothing when it has none.
armsLast() { awk -F'\t' -v ip="$1" '$1 == ip { last = $2 } END { print last }' "$(armsLedger)" 2>/dev/null || :; }

# watchOwner <pid> <tick> <stop...>: runs in the background ($!). Each second while pid runs, it
# runs the tick, then runs stop once the tick fails or this shell is gone, since bash starts both
# with Ctrl-C and Ctrl-\ ignored.
watchOwner() {
	local owner=${BASHPID:-$(exec sh -c 'echo "$PPID"')} # a subshell's own pid, since bash 3.2 has no BASHPID
	while :; do
		sleep 1 || : # bash 5.2 lets a Ctrl-C reach a background subshell's commands
		kill -0 "$1" 2>/dev/null || break
		if ! kill -0 "$owner" 2>/dev/null || ! "$2"; then
			"${@:3}"
			break
		fi
	done >/dev/null 2>&1 &
}

# killTree <pid>: terminates it and every process under it. Each is stopped before its children are
# listed, so no parent forks more or responds to a child's death (a put's reconcile read) before its
# own TERM. Children are found from the PID and PPID columns of ps -ef, since Git Bash has no pgrep.
killTree() {
	local child
	kill -STOP "$1" 2>/dev/null || return 0
	for child in $(ps -ef | awk -v p="$1" '$2 ~ /^[0-9]+$/ && $3 == p { print $2 }'); do
		killTree "$child"
	done
	# It may be gone since its STOP (a zombie reaped); that must never trigger set -e's exit.
	kill -TERM "$1" 2>/dev/null || :
	kill -CONT "$1" 2>/dev/null || :
}
# killChildren: terminates each process this shell started, along with its own children, for an EXIT
# trap. A TERM to this shell alone runs the trap at once, and what it runs in the background never
# hears a Ctrl-C.
killChildren() {
	local child
	for child in $(ps -ef | awk -v p="$$" '$2 ~ /^[0-9]+$/ && $3 == p { print $2 }'); do killTree "$child" || :; done
}

# SCRATCH is the tool's own, set by scratch. One in the environment is some other program's.
unset SCRATCH
# A scratch is named for its process. A KILL runs no trap, so a later run removes the scratch once
# that process is gone, and never while it lives. Anything with no process in its name, such as an
# older version's or a caller's that made no scratch, is litter, removed once it is a day old.
_scratchSweep() {
	local dir pid
	for dir in "${TMPDIR:-/tmp}"/3drop.*.??????; do
		pid=${dir##*/3drop.} pid=${pid%%.*}
		if isUint "$pid" && [ -O "$dir" ] && ! kill -0 "$pid" 2>/dev/null; then rm -rf "$dir"; fi
	done
	find "${TMPDIR:-/tmp}" -maxdepth 1 \( -name '3drop-reply.??????' -o -name '3drop-reply.??????.*' -o -name '3drop-work.??????' \
		-o -name '3drop-deadline.??????' -o -name '3drop-bench.??????' \) -user "$UID" -mmin +1440 -exec rm -rf {} + 2>/dev/null || :
	# A replacement's temp that a cut write left (a tree's manifest, the address book), and a
	# ghost's fifos. They are looked for in runDir's two places, which are named here rather than
	# made, because the parent that sweeps has not sourced wire.sh.
	find ${XDG_RUNTIME_DIR:+"$XDG_RUNTIME_DIR/3drop"} "${XDG_CACHE_HOME:-$HOME/.cache}/3drop" -maxdepth 1 \( -name 'fifo.??????' \
		-o -name '*.new.??????' \) -mmin +1440 -exec rm -f {} + 2>/dev/null || :
}
# scratch: makes this process's scratch, SCRATCH, and the EXIT trap that removes it. Call it from the
# script's top level, never inside $(...), where the variable and the trap would die with the subshell.
scratch() {
	[ -z "${SCRATCH:-}" ] || return 0
	_scratchSweep
	SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/3drop.$$.XXXXXX")
	trap _scratchEnd EXIT
}
# _scratchEnd: stops the tool's child (scratchTool), then removes the scratch. A TERM to this
# process alone would leave the child running, since bash runs this trap at once. Windows refuses a
# delete while a child's trap still has one pending in the scratch, so a failed delete is tried
# again a second later.
_scratchEnd() {
	local child gone=${SCRATCH%.*}.gone00 # still matches 3drop.<pid>.?????? for the sweep
	if read -r child 2>/dev/null <"$SCRATCH/tool" && isUint "$child"; then
		killTree "$child" || :
		# Reap the child here, quietly, because otherwise bash prints "Terminated" for it.
		wait "$child" 2>/dev/null || :
	fi
	# Move it out of its name first, because a process that a group's signal missed (macOS, as it
	# forks) keeps writing by name.
	mv "$SCRATCH" "$gone" 2>/dev/null || gone=$SCRATCH
	rm -rf "$gone" 2>/dev/null || {
		sleep 1
		rm -rf "$gone"
	}
}
# scratchTool: makes the tool's scratch, then runs the tool again in a child that works in it, and
# exits with the child's status. This process keeps the EXIT trap that removes the scratch (it runs
# on a TERM from an interrupt or a deadline too), and the child has none, since bash 3.2 exits 0
# from an unset variable in a process that has one. A tool calls this before it sources wire.sh, so
# only the child parses that.
scratchTool() {
	[ -z "${SCRATCH:-}" ] || return 0 # a second call would run the tool again, in a child of its child
	if [[ "${THREEDROP_SCRATCH:-}" == */3drop."$PPID".?????? ]] && [ -d "$THREEDROP_SCRATCH" ]; then
		SCRATCH=$THREEDROP_SCRATCH
		unset THREEDROP_SCRATCH
		echo "$$" >"$SCRATCH/tool"
		return 0
	fi
	scratch
	local st
	THREEDROP_SCRATCH=$SCRATCH "$BASH" "$0" ${TOOL_ARGS[@]+"${TOOL_ARGS[@]}"} && st=0 || st=$?
	rm -f "$SCRATCH/tool" # the child is reaped, so its pid may be another process's now
	exit "$st"
}

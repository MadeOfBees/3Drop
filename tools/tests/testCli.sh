#!/usr/bin/env bash
# Every tool a user runs answers --version and --help alone: on stdout, exit 0, nothing on stderr.
# --version is the release it came from, the one build.sh gives the console to show: git's name for the
# tag a clone was made at, and `unknown` for a copy with no .git of its own, never an error. --help is
# the tool's usage.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

command -v git >/dev/null 2>&1 || fail "no git, which the README's install needs"
# The user's own git settings (signing) have no part in the repositories made here.
g() { git -c user.name=test -c user.email=test@example.com -c commit.gpgsign=false -c tag.gpgSign=false "$@"; }

# The tools: each script with a shebang (the rest are sourced).
tools=()
for script in "$ROOT"/tools/*.sh "$ROOT"/tools/bench/*.sh; do
	[ "$(head -c 2 "$script")" = '#!' ] || continue
	tools+=("${script#"$ROOT"/}")
done
[ "${#tools[@]}" -ge 20 ] || fail "found ${#tools[@]} tools, where there were 20"

# --version and --help are answered before a tool asks for a key or makes a scratch, a cache or a run
# path. So `alone` runs a tool with no key, and with TMPDIR, XDG_CACHE_HOME and XDG_RUNTIME_DIR all in a
# folder that is not there. A scratch fails in it, and any path made leaves the folder behind. The .env
# that versions leaves in each tree, which dies when read, is never reached.
none=$TMP/none
alone=(env -u THREEDROP_KEY TMPDIR="$none" XDG_CACHE_HOME="$none" XDG_RUNTIME_DIR="$none")

# versions <tree> <release>: each tool of the tree, asked --version, says the release and a newline on
# stdout, and nothing else.
versions() {
	local tool
	printf 'a key goes in as THREEDROP_KEY=<key>, so this line is not one\n' >"$1/.env"
	for tool in "${tools[@]}"; do
		run 0 "${alone[@]}" bash "$1/$tool" --version
		printf '%s\n' "$2" | cmp -s - "$TMP/out" || fail "$tool --version in $1 said '$(cat "$TMP/out")', want '$2'"
		[ ! -s "$TMP/err" ] || fail "$tool --version in $1 also said: $(cat "$TMP/err")"
		[ ! -e "$none" ] || fail "$tool --version in $1 made $none"
	done
}

# The tools as a clone holds them, in a repository at a release tag (without tests, which it does not need).
mkdir "$TMP/repo"
cp -R "$ROOT/tools" "$TMP/repo/tools"
rm -rf "$TMP/repo/tools/tests"
g -C "$TMP/repo" init -q
g -C "$TMP/repo" add -A
g -C "$TMP/repo" commit -q -m tools
g -C "$TMP/repo" tag v7

# The README's install is a clone of the tag. An edit to a tracked file shows up in the version.
g -c advice.detachedHead=false clone -q --branch v7 "$TMP/repo" "$TMP/clone"
versions "$TMP/clone" v7
echo '# an edit' >>"$TMP/clone/tools/lib.sh"
versions "$TMP/clone" v7-dirty

# A commit past the tag, and a clone with no tag at all, which git still names by its commit.
g -C "$TMP/repo" commit -q --allow-empty -m more
run 0 bash "$TMP/repo/tools/device.sh" --version
[[ "$(cat "$TMP/out")" =~ ^v7-1-g[0-9a-f]+$ ]] || fail "one commit past v7 is '$(cat "$TMP/out")'"
g clone -q --no-tags "$TMP/repo" "$TMP/branch"
run 0 bash "$TMP/branch/tools/device.sh" --version
[[ "$(cat "$TMP/out")" =~ ^[0-9a-f]{7,}$ ]] || fail "a clone with no tag is '$(cat "$TMP/out")'"

# A copy with no .git of its own is unknown, whether it is a zip of the source or the tools inside
# another repository (which git would name for that repository's own tag).
mkdir "$TMP/zip"
cp -R "$TMP/repo/tools" "$TMP/zip/tools"
versions "$TMP/zip" unknown
mkdir "$TMP/clone/inside"
cp -R "$TMP/repo/tools" "$TMP/clone/inside/tools"
versions "$TMP/clone/inside" unknown
# If git fails, as it does when it cannot run, the version is unknown too.
mkdir "$TMP/nogit"
printf '#!/bin/sh\nexit 128\n' >"$TMP/nogit/git"
chmod +x "$TMP/nogit/git"
PATH="$TMP/nogit:$PATH" versions "$TMP/clone" unknown

# build.sh gives the console the release that --version names, so the two lines in a bug report can be
# compared. Where there is no release, the console says dev, its own word for a build with no release.
mkdir "$TMP/bin"
# shellcheck disable=SC2016 # the fake's own $1 and $@
printf '#!/bin/sh\n[ "$1" = info ] || printf "%%s\\n" "$@" >"$DOCKER_CALLS"\n' >"$TMP/bin/docker"
chmod +x "$TMP/bin/docker"
# builtWith <tree> prints the APP_VERSION that its build.sh passes to make.
builtWith() {
	rm -f "$TMP/dockerCalls"
	DOCKER_CALLS=$TMP/dockerCalls PATH="$TMP/bin:$PATH" bash "$1/tools/build.sh" >/dev/null || fail "build.sh under a fake docker in $1"
	sed -n 's/^APP_VERSION=//p' "$TMP/dockerCalls"
}
run 0 env -u THREEDROP_KEY bash "$TMP/clone/tools/device.sh" --version
expect "$(builtWith "$TMP/clone")" "$(cat "$TMP/out")" "the APP_VERSION build.sh gives the console, in a clone"
expect "$(builtWith "$TMP/zip")" dev "the APP_VERSION of a copy with no .git"
expect "$(builtWith "$TMP/clone/inside")" dev "the APP_VERSION of a copy inside another repository"

# --help and -h print the usage: the tool's own lines, ending with the one for --version. If the usage
# is a table whose last row is -h's, the --version line is a row in its columns. They are also asked
# before any other flag, where a tool takes flags before its arguments.
for tool in "${tools[@]}"; do
	name=${tool##*/}
	run 0 "${alone[@]}" bash "$ROOT/$tool" --help
	cp "$TMP/out" "$TMP/help"
	[ ! -s "$TMP/err" ] || fail "$name --help also said: $(cat "$TMP/err")"
	case "$(head -n 1 "$TMP/help")" in
	"$name "* | "$name:"*) ;;
	*) fail "$name --help opens with '$(head -n 1 "$TMP/help")'" ;;
	esac
	last="--version: print this copy's release and exit"
	if help=$(grep -e '^ *-h, --help  *this text$' "$TMP/help"); then
		last=${help/-h, --help/--version }
		last="${last%this text}this copy's release"
	fi
	[ "$(tail -n 1 "$TMP/help")" = "$last" ] ||
		fail "$name --help ends with '$(tail -n 1 "$TMP/help")', not '$last'"
	run 0 "${alone[@]}" bash "$ROOT/$tool" -h
	cmp -s "$TMP/help" "$TMP/out" || fail "$name -h is not $name --help"
	[ ! -s "$TMP/err" ] || fail "$name -h also said: $(cat "$TMP/err")"
	[ ! -e "$none" ] || fail "$name --help or -h made $none"
done
for pair in drop.sh:--run dropBatch.sh:--new; do
	run 0 bash "$ROOT/tools/${pair%%:*}" "${pair#*:}" --help
	[[ "$(head -n 1 "$TMP/out")" == "${pair%%:*} "* && ! -s "$TMP/err" ]] || fail "${pair%%:*} ${pair#*:} --help said: $(head -n 1 "$TMP/out") $(cat "$TMP/err")"
done

# After `--`, a first word named like an option is the tool's own: a bench label named --help gets
# scored, and a tool that reruns itself in a scratch (scratchTool) is rerun with the `--` too, which the
# rerun takes off.
(
	export XDG_CACHE_HOME=$TMP/cache XDG_RUNTIME_DIR=$TMP/cache
	# shellcheck source=tools/bench/lib.sh
	. "$ROOT/tools/bench/lib.sh"
	benchRow "$TMP/results.tsv" series=--help rep=1 time=- uid=- title=- size_bytes=1048576 wall_s=10 rate_mbs=1 outcome=committed:ok \
		ver=- inst=- knobs=- capture=- am_titles=- am_tickets=- free_bytes=- air=- card_before=- card_after=- stats=- mb=-
)
# shellcheck disable=SC2031 # the repo's root: the bench library's stayed in the subshell above, the same
run 0 bash "$ROOT/tools/bench/seriesScore.sh" -- --help "$TMP/results.tsv" 1
grep -q '^--help n=1 ' "$TMP/out" || fail "seriesScore.sh -- --help scored: $(cat "$TMP/out")"
# shellcheck disable=SC2016,SC2031 # the script's own $# and $*; the repo's root, as above
printf '%s\n' '#!/usr/bin/env bash' ". \"$ROOT/tools/lib.sh\"" scratchTool 'echo "$#:$*"' >"$TMP/echoArgs.sh"
run 0 bash "$TMP/echoArgs.sh" -- --help x
expect "$(cat "$TMP/out")" "2:--help x" "the arguments after -- of a tool that reruns itself in a scratch"
echo "testCli: PASS"

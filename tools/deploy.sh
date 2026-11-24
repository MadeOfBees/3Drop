#!/usr/bin/env bash
# deploy.sh <ip>: build, package at the installed version + 1 (a self-update must be strictly
# newer), self-update, wait for the new build, and record it in the arms ledger for bench.sh.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
scratchTool
. "$ROOT/tools/wire.sh"

ip="${1:-}"
[ -n "$ip" ] || usage
wireConsole "$ip"
ip=$wireIP port=$wirePort
# This check comes after wireConsole, whose .env may set the wait.
isUint "${THREEDROP_DEPLOY_WAIT:-60}" || dieWith 2 "THREEDROP_DEPLOY_WAIT must be a whole number of seconds"
installed=$(wireWaitUp "$ip" "$port" "" 0) ||
	dieWith $? "no version from 3Drop at $ip (no answer, or none in stats op or stats self: reinstall 3Drop once by hand)"
next=$((installed + 1))
# Release vN is title version N x 1024 (tools/release.sh), so a build deployed on it outranks only
# the builds before it, and the next release outranks them all; a build numbered there would not.
if [ $((next % 1024)) = 0 ]; then
	on="on top of release v$((installed / 1024))"
	[ "$installed" -ge 1024 ] || on="before release v1"
	# v63 is the last release (tools/release.sh), so there is no next one to offer.
	if [ "$installed" -ge $((63 * 1024)) ]; then
		die "3Drop has 1023 builds $on, all one release leaves room for, and v63 is the last release: for a new run, reinstall a release with FBI"
	fi
	die "3Drop has 1023 builds $on, all one release leaves room for: for a new run, install the next release if it is out (scan the update QR in 3Drop, or drop.sh --self), or reinstall a release with FBI"
fi

"$ROOT/tools/build.sh"
BUILD_NUMBER="$next" "$ROOT/tools/makeCIA.sh"
reply=$("$ROOT/tools/drop.sh" --self "$ROOT/3drop.cia" "$ip" "$port") ||
	dieWith $? "the console did not confirm the install"
echo "$reply"
[[ "$reply" == "put committed "*" self" ]] || die "the console did not confirm the install"

# The reply comes before the relaunch.
t0=$SECONDS
wireWaitUp "$ip" "$port" "$next" "${THREEDROP_DEPLOY_WAIT:-60}" >/dev/null ||
	dieWith $? "installed build $next, but it is not the running build; check the console"
# A tree without .git (a transferred copy) is still a successful deploy.
tree=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null) && { git -C "$ROOT" diff --quiet HEAD || tree+=-dirty; } || tree=nogit
armsRecord "$ip" "$next" "$(sha256File "$ROOT/3drop.elf")" "$tree" "$([ "${THREEDROP_TEST_VERBS:-0}" = 1 ] && echo bench || echo release)"
echo "deploy: 3Drop build $next (was $installed) is running on $ip, up $((SECONDS - t0)) s after its install"

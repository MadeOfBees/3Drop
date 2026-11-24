#!/usr/bin/env bash
# build.sh [make args]: builds 3drop.elf in the pinned devkitARM image (needs Docker)

# APP_VERSION, the release the console shows, is display only; the title version gates self-updates.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
docker info >/dev/null 2>&1 || die "docker is not running"

APP_VERSION="$(releaseName dev)"
# IMAGE is the lint image's base, pinned by digest, so a plain build and the gate share one toolchain.
IMAGE="$(sed -n 's/^FROM //p' "$ROOT/tools/lint.Dockerfile")"
[ -n "$IMAGE" ] || die "no FROM in tools/lint.Dockerfile"

# The container's paths stay as typed; the bind source is the host's own path (lib.sh: hostPath).
hostRoot=$(hostPath "$ROOT")
MSYS2_ARG_CONV_EXCL='*' exec docker run --rm --user "$(id -u):$(id -g)" --env HOME=/tmp \
	-v "$hostRoot":/work -w /work \
	"$IMAGE" make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
	APP_VERSION="$APP_VERSION" \
	THREEDROP_TEST_VERBS="${THREEDROP_TEST_VERBS:-0}" "$@"

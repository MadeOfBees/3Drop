#!/usr/bin/env bash
# lint.sh [check|format|image]: `format` rewrites the C sources and scripts in place; `image` builds the
# image alone and prints its name.
set -euo pipefail

. "$(dirname "$0")/lib.sh"
IMAGE=3drop-lint:bookworm
MODE="${1:-check}"
case "$MODE" in check | format | image) ;; *) usage ;; esac

if [ "${THREEDROP_LINT_INSIDE:-0}" != 1 ]; then
	# The file list comes from git on the host, because the container may not see the repository's git
	# directory. src/quirc is upstream's, byte for byte (src/quirc/UPSTREAM.txt), so it is neither formatted
	# nor linted.
	files() { git -C "$ROOT" ls-files --cached --others --exclude-standard "$@" | grep -v '^src/quirc/' | tr '\n' ' '; }
	docker build --quiet --file "$ROOT/tools/lint.Dockerfile" --tag "$IMAGE" "$ROOT/tools" >/dev/null
	[ "$MODE" != image ] || exec echo "$IMAGE"
	echo "-- lint: image ready ($SECONDS s)"
	# The build above is left to Git Bash's conversion, because every argument of it is this PC's path.
	hostRoot=$(hostPath "$ROOT")
	MSYS2_ARG_CONV_EXCL='*' exec docker run --rm ${LINT_CONTAINER:+--name "$LINT_CONTAINER"} --user "$(id -u):$(id -g)" --env HOME=/tmp --env THREEDROP_LINT_INSIDE=1 \
		--env LINT_C="$(files '*.c' '*.h')" --env LINT_SH="$(files '*.sh')" \
		--volume "$hostRoot:/work" --workdir /work "$IMAGE" tools/lint.sh "$MODE"
fi
cd "$ROOT"
# Prints the end of each stage, in seconds since the container started, to show where the gate's time goes.
stamp() { echo "-- lint: $1 ended ($SECONDS s)"; }
# shellcheck disable=SC2086 # the lists are space-separated paths without spaces
if [ "$MODE" = format ]; then
	clang-format-14 -i $LINT_C
	shfmt -w -i 0 $LINT_SH
	exit 0
fi
# shellcheck disable=SC2086
clang-format-14 --dry-run --Werror $LINT_C
stamp clang-format
cppcheck --quiet --enable=warning,style,performance,portability --error-exitcode=1 \
	--suppress=missingIncludeSystem --suppress='*:src/quirc/*' -I src src/*.c src/*.h src/bench/*.c \
	src/bench/*.h tools/bench/*.c tools/tests/stubs/3ds.h
stamp "cppcheck src"
# The tests get a separate run. src is covered by the run above. In the tests, asserts always run (fakes.h),
# even a close inside one, and locals shadow helpers.
cppcheck --quiet --enable=warning,style,performance,portability --error-exitcode=1 --inline-suppr \
	--suppress=missingIncludeSystem --suppress='*:src/*' --suppress=assertWithSideEffect \
	--suppress=assignmentInAssert --suppress=resourceLeak --suppress=shadowFunction \
	-DTESTS_DIR='""' -I src -I tools/tests/stubs tools/tests/*.c tools/tests/stubs/fakes.c
stamp "cppcheck tests"
# One process per file, one per CPU at most: a single clang-tidy holds every AST at once and runs the
# container out of memory. A header is checked in each file that includes it (HeaderFilterRegex).
gcc=/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc
# shellcheck source=tools/cia.sh
self=$(. "$ROOT/tools/cia.sh" && selfTitleID)
tidy=(-DSELF_TITLE_ID="0x${self}ULL" -DAPP_VERSION=\"lint\" --target=arm-none-eabi -x c -std=gnu11 -D__3DS__ -I/work/src
	-I/opt/devkitpro/libctru/include -I/opt/devkitpro/portlibs/3ds/include
	-isystem "$($gcc -print-sysroot)/include" -isystem "$($gcc -print-file-name=include)")
printf '%s\n' src/*.c tools/bench/*.c | xargs -P "$(nproc)" -I{} clang-tidy-14 --quiet {} -- -DTHREEDROP_TEST_VERBS=0 "${tidy[@]}"
printf '%s\n' src/bench/*.c | xargs -P "$(nproc)" -I{} clang-tidy-14 --quiet {} -- -DTHREEDROP_TEST_VERBS=1 "${tidy[@]}"
stamp clang-tidy
# shellcheck disable=SC2086
shellcheck $LINT_SH
stamp shellcheck
# shellcheck disable=SC2086
shfmt -d -i 0 $LINT_SH
stamp shfmt
# The system clock steps, so every timer on the console reads uptimeMs (src/app.h), never osGetTime.
if grep -nE 'osGetTime\(' src/*.c src/*.h src/bench/*.c src/bench/*.h tools/bench/*.c; then exit 1; fi
# Raw MCU register writes can damage hardware, so the LED goes through the typed pattern call. The
# one read allowed is the bench's LED read-back (src/bench/ledRead.c), never in a release build.
if grep -nE 'MCUHWC_WriteRegister|mcuHwcWriteReg' src/*.c src/bench/*.c tools/bench/*.c; then exit 1; fi
if grep -nE 'MCUHWC_ReadRegister|mcuHwcReadReg' src/*.c src/bench/*.c tools/bench/*.c |
	grep -v '^src/bench/ledRead\.c:'; then exit 1; fi

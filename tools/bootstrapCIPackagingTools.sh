#!/usr/bin/env bash
# bootstrapCIPackagingTools.sh: installs CI's packaging tools (bannertool, makerom and 3dstool, Linux x86-64)
#   in $RUNNER_TEMP/3drop-ci-tools/install/bin (else under $TMPDIR or /tmp) and puts them on the PATH of the
#   GitHub job that runs it. Checksums pin each download, and each binary comes from a fixed path inside it,
#   so a same-named file that an upstream release adds is never run.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

case "$(uname -s):$(uname -m)" in
Linux:x86_64) ;;
*)
	echo "bootstrapCIPackagingTools: Linux x86_64 is required" >&2
	exit 1
	;;
esac

for requiredTool in curl sha256sum tar unzip install; do
	command -v "$requiredTool" >/dev/null 2>&1 || {
		echo "bootstrapCIPackagingTools: required tool not found: $requiredTool" >&2
		exit 1
	}
done

TOOLS_DIR="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/3drop-ci-tools"
DOWNLOAD_DIR="$TOOLS_DIR/downloads"
EXTRACT_DIR="$TOOLS_DIR/extracted"
INSTALL_DIR="$TOOLS_DIR/install"
mkdir -p "$DOWNLOAD_DIR" "$EXTRACT_DIR" "$INSTALL_DIR/bin"

downloadVerified() {
	curl --location --fail --silent --show-error --output "$2" "$1"
	printf '%s  %s\n' "$3" "$2" | sha256sum --check --status || {
		echo "bootstrapCIPackagingTools: checksum failed: $2" >&2
		exit 1
	}
}

bannertoolArchive="$DOWNLOAD_DIR/bannertool-1.2.3-linux.tar.gz"
downloadVerified \
	https://github.com/carstene1ns/3ds-bannertool/releases/download/1.2.3/bannertool-1.2.3-linux.tar.gz \
	"$bannertoolArchive" \
	748519d200519db18e9fd00c332ac32f5411e41230258291e89953a76c1f7155
rm -rf "$EXTRACT_DIR/bannertool"
mkdir -p "$EXTRACT_DIR/bannertool"
tar -xzf "$bannertoolArchive" -C "$EXTRACT_DIR/bannertool"
install -m 0755 \
	"$EXTRACT_DIR/bannertool/bannertool-1.2.3-linux/bannertool" \
	"$INSTALL_DIR/bin/bannertool"

makeromArchive="$DOWNLOAD_DIR/makerom-v0.19.0-ubuntu_x86_64.zip"
downloadVerified \
	https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.19.0/makerom-v0.19.0-ubuntu_x86_64.zip \
	"$makeromArchive" \
	287b809dec064e0ad597e3d272c49ecb7eed41693d5ee6fef9d8a8aa24c2497e
rm -rf "$EXTRACT_DIR/makerom"
mkdir -p "$EXTRACT_DIR/makerom"
unzip -q "$makeromArchive" -d "$EXTRACT_DIR/makerom"
install -m 0755 "$EXTRACT_DIR/makerom/makerom" "$INSTALL_DIR/bin/makerom"

threeDsToolArchive="$DOWNLOAD_DIR/3dstool_linux_x86_64.tar.gz"
downloadVerified \
	https://github.com/dnasdw/3dstool/releases/download/v1.2.6/3dstool_linux_x86_64.tar.gz \
	"$threeDsToolArchive" \
	d42e3de7766ee552b3609ac5acb7ad3f07209dd91bc45a21d3fc2b049790ede2
rm -rf "$EXTRACT_DIR/3dstool"
mkdir -p "$EXTRACT_DIR/3dstool"
tar -xzf "$threeDsToolArchive" -C "$EXTRACT_DIR/3dstool"
install -m 0755 "$EXTRACT_DIR/3dstool/3dstool" "$INSTALL_DIR/bin/3dstool"

if [ -n "${GITHUB_PATH:-}" ]; then
	printf '%s\n' "$INSTALL_DIR/bin" >>"$GITHUB_PATH"
fi

echo "Installed CI packaging tools to $INSTALL_DIR/bin"

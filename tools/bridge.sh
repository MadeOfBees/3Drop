#!/usr/bin/env bash
# bridge.sh [ip]: the console's SD card for GUI file clients (FileZilla, WinSCP, Cyberduck; Finder
# reads), as an FTP server on 127.0.0.1 alone, over device.sh's commands, until Ctrl-C. It prints
# its address, user and password, a new password each run. With no <ip> it finds the console as
# discover does. Set the client to Binary and one connection: a second transfer is refused.
# THREEDROP_BRIDGE_PORT is its port (2121); THREEDROP_PIECE the transfers' pieces (8388608 bytes).
set -euo pipefail
# wire.sh brings in lib.sh. The scratch comes from scratch, not scratchTool as in the other
# tools, because this shell becomes perl (exec below) and perl removes it.
. "$(dirname "$0")/wire.sh"
port=${THREEDROP_BRIDGE_PORT:-2121}
# 0 means any free port, and the printed address names it.
if [ "$#" -gt 1 ] || { [ "$port" != 0 ] && ! isPort "$port"; }; then usage; fi
command -v perl >/dev/null 2>&1 || die "the bridge runs in perl, which this PC lacks"
wireConsoleAt "${1:-}"
# A client waits on a piece with nothing moving, and gives up after 15 s (KIO, WinSCP).
export THREEDROP_PIECE=${THREEDROP_PIECE:-8388608}
password=$(requireHex 24 "$(openssl rand -hex 12)" password) || exit
echo "bridge: the console at $wireIP:$wirePort" >&2
# The scratch is named for this process, which perl becomes. It holds the transfer lock and the
# sessions' replies.
scratch
THREEDROP_BRIDGE_PASSWORD=$password THREEDROP_BRIDGE_DIR=$SCRATCH \
	exec perl "$ROOT/tools/bridge.pl" "$wireIP:$wirePort" "$port" "$WIRE_PUT_TEMP"

#!/usr/bin/env bash
# stampUnistore.sh <version> <revision> [manifest]: the UniStore manifest (3drop.unistore) stamped
# in place for a release: the entry's version and last_updated (now), and the store's revision,
# which makes Universal Updater fetch the store again. Needs jq.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
if ! { [ "$#" = 2 ] || [ "$#" = 3 ]; } || [ -z "$1" ] || ! isUint "$2"; then usage; fi
manifest=${3:-$ROOT/3drop.unistore}
# UU shows "%d-%d-%d at %d:%d" as a date, and offers an update when the entry's last_updated sorts
# after the installed one's as a string. A date alone would tie two releases on one day.
updated=$(date -u '+%Y-%m-%d at %H:%M (UTC)')
jq --arg v "$1" --arg d "$updated" --argjson r "$2" \
	'.storeContent[0].info.version = $v | .storeContent[0].info.last_updated = $d | .storeInfo.revision = $r' \
	"$manifest" >"$manifest.new"
mv "$manifest.new" "$manifest"

#!/usr/bin/env bash
# Build the macOS app icon (.icns) from the site's favicon (docs/public/favicon.png, the Ballance ball).
# Used by tools/package-gasm.sh (Ballance (gasm).app). Needs macOS iconutil.
#   tools/make-icns.sh <out.icns>
set -euo pipefail
OUT=${1:?usage: make-icns.sh <out.icns>}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC=${OPENBALLANCE_ICON:-$ROOT/docs/public/favicon.png}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
SET="$TMP/openballance.iconset"; mkdir -p "$SET"
for s in 16 32 128 256 512; do
  python3 "$ROOT/tools/icon.py" "$s" "$SET/icon_${s}x${s}.png" "$SRC"
  python3 "$ROOT/tools/icon.py" $((s * 2)) "$SET/icon_${s}x${s}@2x.png" "$SRC"
done
iconutil -c icns "$SET" -o "$OUT"

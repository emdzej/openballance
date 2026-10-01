#!/usr/bin/env bash
# Check a gasm bundle from tools/package-gasm.sh without game data (CI runs it on each bundle's own OS):
#   tools/smoke-gasm-bundle.sh <openballance-gasm-...-macos-universal.zip | ...-linux-<arch>.tar.gz>
# Unpacks it, checks the files, runs the bundled gasm-run without data (it must stop with OpenBallance's
# "game data not found" message and exit code 1, not crash), and the launcher's --help and --dry-run paths
# against fake data (an installed folder, a CD folder, an .iso, a .bin; no dialogs: OPENBALLANCE_DATA, --dry-run, a
# scratch HOME).
# With OPENBALLANCE_SMOKE_DATA=<folder or .iso/.bin> it also runs the game to the main menu (990 frames, headless).
set -euo pipefail
ARCHIVE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
case "$ARCHIVE" in
  *.zip) (cd "$T" && unzip -q "$ARCHIVE") ;;
  *.tar.gz) tar xzf "$ARCHIVE" -C "$T" ;;
  *) fail "unknown archive $ARCHIVE" ;;
esac
DIR=$(find "$T" -mindepth 1 -maxdepth 1 -type d -name 'openballance-gasm-*' | head -n 1)
[ -n "$DIR" ] || fail "no openballance-gasm-* folder in the archive"
for f in README.txt LICENSE LICENSE-gasm; do [ -s "$DIR/$f" ] || fail "missing $f"; done
if [ -d "$DIR/Ballance (gasm).app" ]; then
  APP="$DIR/Ballance (gasm).app"
  RUN="$APP/Contents/MacOS/gasm-run"; WASM="$APP/Contents/Resources/openballance.wasm"; L="$APP/Contents/MacOS/OpenBallance"
  codesign --verify --strict "$APP" || fail "code signature"
  [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$APP/Contents/Info.plist")" = pl.emdzej.openballance.gasm ] ||
    fail "bundle id"
  [ -s "$APP/Contents/Resources/openballance.icns" ] || fail "icon"
  lipo -info "$RUN"
else
  RUN="$DIR/gasm-run"; WASM="$DIR/openballance.wasm"; L="$DIR/openballance.sh"
  [ -s "$DIR/openballance.png" ] || fail "icon"
fi
if [ ! -x "$RUN" ] || [ ! -s "$WASM" ] || [ ! -x "$L" ]; then fail "gasm-run, openballance.wasm or the launcher missing"; fi
grep -q "gasm-run [0-9]" "$DIR/README.txt" || fail "README does not name the gasm version"

# 1. No data: a clean error from the game, not a crash.
set +e
out=$(cd "$T" && "$RUN" "$WASM" --headless 5 --mute 2>&1); rc=$?
set -e
echo "$out"
[ "$rc" = 1 ] || fail "gasm-run without data: exit $rc, expected 1"
echo "$out" | grep -q "game data not found" || fail "no 'game data not found' message"

# 2. Launcher: help, and the command it builds (no dialogs, scratch HOME so nothing real is touched).
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" XDG_STATE_HOME="$T/home/.local/state"
mkdir -p "$HOME"
"$L" --help | grep -q "OpenBallance" || fail "--help"
mkdir -p "$T/installed" "$T/cd/setup" "$T/empty"
: > "$T/installed/Base.CMO"; : > "$T/cd/setup/Data1.hdr"   # any letter case
OPENBALLANCE_DATA="$T/installed" "$L" --dry-run --param unlockall=1 | tee "$T/cmd"
{ grep -q -- "--asset-dir" "$T/cmd" && grep -q "unlockall=1" "$T/cmd"; } || fail "dry run with an installed folder"
OPENBALLANCE_DATA="$T/cd" "$L" --dry-run | grep -q -- "--asset-dir" || fail "dry run with a CD folder"
: > "$T/Ballance.ISO"
OPENBALLANCE_DATA="$T/Ballance.ISO" "$L" --dry-run | grep -q -- "--rom" || fail "dry run with an .iso"
: > "$T/ballance.BIN"
OPENBALLANCE_DATA="$T/ballance.BIN" "$L" --dry-run | grep -q -- "--rom" || fail "dry run with a .bin"
: > "$T/ballance.mdf"
if OPENBALLANCE_DATA="$T/ballance.mdf" "$L" --dry-run 2>/dev/null; then fail ".mdf accepted"; fi
: > "$T/ballance.cue"
if OPENBALLANCE_DATA="$T/ballance.cue" "$L" --dry-run 2>/dev/null; then fail ".cue accepted"; fi
if OPENBALLANCE_DATA="$T/empty" "$L" --dry-run 2>/dev/null; then fail "a folder without base.cmo or Setup/data1.hdr accepted"; fi
if "$L" --dry-run </dev/null >/dev/null 2>&1; then fail "no data, dry run: should exit non-zero"; fi
if [ -e "$XDG_CONFIG_HOME/openballance/data-location" ] ||
   [ -e "$HOME/Library/Application Support/OpenBallance/data-location" ]; then
  fail "a dry run saved the data location"
fi

# 3. Optional: the real game with the user's data, to the main menu.
if [ -n "${OPENBALLANCE_SMOKE_DATA:-}" ]; then
  if [ -d "$OPENBALLANCE_SMOKE_DATA" ]; then data=(--asset-dir "$OPENBALLANCE_SMOKE_DATA"); else data=(--rom "$OPENBALLANCE_SMOKE_DATA"); fi
  "$RUN" "$WASM" "${data[@]}" --headless 990 --mute 2>&1 | tee "$T/run" | grep -E "game data from|fnv" || true
  grep -q "game data from" "$T/run" || fail "the game did not find the data"
  grep -q "frames=990 " "$T/run" || fail "the game did not run 990 frames"
fi
echo "PASS $(basename "$ARCHIVE")"

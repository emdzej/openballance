#!/usr/bin/env bash
# Package openballance.wasm with a released gasm-run into a double-clickable bundle (no game data inside).
#   tools/package-gasm.sh <version> <platform> <gasm-run dir> <out dir>
# <platform>: macos-universal, linux-x86_64, linux-arm64 or windows-x86_64. <gasm-run dir> holds gasm-run[.exe]
# and gasm's LICENSE (tools/fetch-gasm-runner.sh <platform> makes one). OPENBALLANCE_WASM=<file> picks the
# module (default build-gasm/openballance.wasm); it is always bundled as openballance.wasm, which is also
# gasm's storage namespace for the saves. Produces in <out dir>:
#   macos-universal  Ballance (gasm).app, openballance-gasm-<version>-macos-universal.zip
#   linux-<arch>     openballance-gasm-<version>-linux-<arch>.tar.gz
#   windows-x86_64   openballance-gasm-<version>-windows-x86_64.zip
# each archive with a .sha256 next to it. The macOS app is signed ad hoc when codesign is available.
# The launchers are in tools/gasm-bundle/. No AOT .cwasm: gasm-run --compile only targets the host it runs
# on, so it can't be made for the other platforms (or both halves of the universal app) at packaging time.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
usage="usage: package-gasm.sh <version> <macos-universal|linux-x86_64|linux-arm64|windows-x86_64> <gasm-run dir> <out dir>"
VERSION=${1:?$usage}; PLATFORM=${2:?$usage}; RUNDIR=${3:?$usage}; OUT=${4:?$usage}
WASM=${OPENBALLANCE_WASM:-$ROOT/build-gasm/openballance.wasm}
ICON=${OPENBALLANCE_ICON:-$ROOT/docs/public/favicon.png}
GASM_VERSION=$(cat "$RUNDIR/VERSION" 2>/dev/null || "$ROOT/tools/fetch-gasm-sdk.sh" --version)
SRC="$ROOT/tools/gasm-bundle"
EXE=""; [[ "$PLATFORM" == windows-* ]] && EXE=.exe
case "$PLATFORM" in macos-universal | linux-x86_64 | linux-arm64 | windows-x86_64) ;; *) echo "$usage" >&2; exit 2 ;; esac
for f in "$WASM" "$RUNDIR/gasm-run$EXE" "$RUNDIR/LICENSE" "$ROOT/LICENSE"; do
  [ -f "$f" ] || { echo "missing $f (tools/fetch-gasm-runner.sh $PLATFORM fetches the runner and its LICENSE)" >&2; exit 1; }
done
[[ "$PLATFORM" == windows-* ]] || [ -f "$ICON" ] || { echo "missing $ICON (the app icon)" >&2; exit 1; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
NAME="openballance-gasm-$VERSION-$PLATFORM"
export OPENBALLANCE_ICON="$ICON"

# fill <template> <out>: substitute the versions
fill() { sed -e "s|@VERSION@|$VERSION|g" -e "s|@GASM_VERSION@|$GASM_VERSION|g" "$1" > "$2"; }
crlf() { sed -e 's/\r*$/\r/' "$1" > "$1.tmp" && mv "$1.tmp" "$1"; }
sha() { (cd "$OUT" && if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi > "$1.sha256"); }

# readme <out file>: README.txt for this platform (~, $USER and backslashes are meant literally)
# shellcheck disable=SC2088,SC2016,SC1003
readme() {
  local start data_how change saves logs keymapdir lic=""
  [ -n "$EXE" ] && lic=.txt
  case "$PLATFORM" in
    macos-*)
      start='Open "Ballance (gasm).app". It is signed ad hoc, not notarized: the first time,
right-click it and choose Open (or: xattr -dr com.apple.quarantine "Ballance (gasm).app").'
      data_how='The first time, the app asks for your game data: "Choose your Ballance folder..." for the
CD itself, a mounted disc image (double-click an .iso; it appears under /Volumes), a folder you
copied the CD to, or the folder of an installed Ballance (the one with base.cmo); "Choose disc
image (.iso, .bin)..." for an .iso file or the .bin of a .bin/.cue pair.'
      change='Hold Option while opening the app to choose other data, or delete
  ~/Library/Application Support/OpenBallance/data-location
From Terminal: "Ballance (gasm).app/Contents/MacOS/OpenBallance" --help'
      saves='~/Library/Application Support/gasm/openballance/'
      logs='~/Library/Logs/OpenBallance/gasm.log'
      keymapdir='~/Library/Application Support/gasm/keymap.txt' ;;
    linux-*)
      start='Run ./openballance.sh (from a terminal or your file manager). ./openballance.sh
--install-desktop adds a menu entry. gasm-run needs ALSA (libasound2, package libasound2t64 on
newer Debian and Ubuntu) and a Vulkan capable graphics driver (Ballance is drawn on the GPU).'
      data_how='Give it your game data once: ./openballance.sh /media/$USER/BALLANCE (the CD or a mounted
image), a folder you copied the CD to, the folder of an installed Ballance (the one with
base.cmo), or an .iso or .bin image file. Without an argument it opens a chooser (zenity or kdialog) if one is
installed.'
      change='./openballance.sh --change-data, or give it other data as the argument, or delete
  ${XDG_CONFIG_HOME:-~/.config}/openballance/data-location
./openballance.sh --help lists the options; anything after the data goes to gasm-run.'
      saves='~/.local/share/gasm/openballance/'
      logs='the terminal, or ~/.local/state/openballance/gasm.log when started from a menu'
      keymapdir='~/.local/share/gasm/keymap.txt' ;;
    windows-*)
      start='Double-click OpenBallance.cmd. (If Windows SmartScreen warns about gasm-run.exe: More info,
Run anyway.)'
      data_how='The first time, it asks for your game data: "Choose your Ballance folder..." for the CD
drive, a mounted disc image (right-click an .iso, Mount), a folder you copied the CD to, or the
folder of an installed Ballance (the one with base.cmo); "Choose disc image (.iso, .bin)..." for
an .iso or .bin file. From a command prompt: OpenBallance.cmd D:\'
      change='OpenBallance.cmd --change-data, or delete %APPDATA%\OpenBallance\data-location.
OpenBallance.cmd --help lists the options; anything after the data goes to gasm-run.'
      saves='%APPDATA%\gasm\openballance\'
      logs='the console window'
      keymapdir='%APPDATA%\gasm\keymap.txt' ;;
  esac
  cat > "$1" <<TXT
OpenBallance $VERSION for gasm ($PLATFORM)
https://openballance.emdzej.pl

OpenBallance is a faithful reimplementation of Ballance (Cyparade / Atari, 2004): a Virtools runtime
that runs the game's original scripts, with the Ipion physics engine ported from the original. This
bundle runs it as a WebAssembly module (openballance.wasm) in the gasm runtime: gasm-run
$GASM_VERSION is included (https://gasm.emdzej.pl). It is the same game as the browser player.

You need your own copy of Ballance: the CD, an .iso or .bin image of it, or an installed game
folder. None of the original game's files are included.

START
$start

YOUR GAME DATA
$data_how
The choice is remembered (the folder or file must still be there next time; a CD or mounted
image has to be inserted or mounted again).

CHANGE THE GAME DATA
$change

CONTROLS (keyboard; a gamepad works too)
  Arrows            roll the ball, move in the menus
  X or Enter        Enter: select
  Z                 Esc: pause menu, back (Esc itself closes gasm-run)
  Q or W            Shift: hold with Left / Right to rotate the camera
  S                 Space: raise the camera for an overview
  A                 Q: skip the tutorial
  Right Shift       F1
Typed letters go to the highscore name entry. Other keys: write a layout file (gasm-run
--print-keymap prints the default) and save it as $keymapdir
Launch options (after the data): --param unlockall=1 (all levels), --param language=0..4
(German, English, Spanish, Italian, French), --param mode=viewer --param level=1..12 (level viewer).
Full guide: https://openballance.emdzej.pl/guide/

SAVES (highscores, settings, unlocked levels)
$saves

LOG
$logs

LICENSES
OpenBallance is GPL-3.0 (LICENSE$lic). gasm-run $GASM_VERSION is MIT (LICENSE-gasm$lic),
https://github.com/emdzej/gasm. Ballance is (c) 2004 Cyparade / Atari.
TXT
}

case "$PLATFORM" in
macos-*)
  APP="$OUT/Ballance (gasm).app"
  rm -rf "$APP"
  mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
  cp "$RUNDIR/gasm-run" "$APP/Contents/MacOS/gasm-run"
  fill "$SRC/launch-macos.sh" "$APP/Contents/MacOS/OpenBallance"
  chmod +x "$APP/Contents/MacOS/OpenBallance" "$APP/Contents/MacOS/gasm-run"
  cp "$WASM" "$APP/Contents/Resources/openballance.wasm"
  cp "$ROOT/LICENSE" "$APP/Contents/Resources/LICENSE"
  cp "$RUNDIR/LICENSE" "$APP/Contents/Resources/LICENSE-gasm"
  "$ROOT/tools/make-icns.sh" "$APP/Contents/Resources/openballance.icns"
  cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Ballance (gasm)</string>
  <key>CFBundleDisplayName</key><string>Ballance (gasm)</string>
  <key>CFBundleIdentifier</key><string>pl.emdzej.openballance.gasm</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleGetInfoString</key><string>OpenBallance $VERSION on gasm-run $GASM_VERSION</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>OpenBallance</string>
  <key>CFBundleIconFile</key><string>openballance</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.puzzle-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>OpenBallance contributors, GPL-3.0; gasm-run MIT. Ballance is (c) 2004 Cyparade / Atari.</string>
</dict></plist>
PLIST
  if command -v codesign >/dev/null; then
    codesign --force --sign - "$APP/Contents/MacOS/gasm-run"
    codesign --force --sign - "$APP"   # ad hoc (not notarized)
    codesign --verify --strict "$APP"
  fi
  STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
  mkdir "$STAGE/$NAME"
  cp -R "$APP" "$STAGE/$NAME/"
  readme "$STAGE/$NAME/README.txt"
  cp "$ROOT/LICENSE" "$STAGE/$NAME/LICENSE"
  cp "$RUNDIR/LICENSE" "$STAGE/$NAME/LICENSE-gasm"
  rm -f "$OUT/$NAME.zip"
  if command -v ditto >/dev/null; then (cd "$STAGE" && ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$NAME" "$OUT/$NAME.zip")
  else (cd "$STAGE" && zip -qry "$OUT/$NAME.zip" "$NAME"); fi
  sha "$NAME.zip"
  echo "$APP"; echo "$OUT/$NAME.zip" ;;
linux-*)
  STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
  D="$STAGE/$NAME"; mkdir "$D"
  cp "$RUNDIR/gasm-run" "$D/"
  cp "$WASM" "$D/openballance.wasm"
  fill "$SRC/openballance.sh" "$D/openballance.sh"
  cp "$SRC/openballance-gasm.desktop" "$D/"
  python3 "$ROOT/tools/icon.py" 256 "$D/openballance.png" "$ICON"
  chmod 755 "$D/gasm-run" "$D/openballance.sh"; chmod 644 "$D/openballance.wasm"
  readme "$D/README.txt"
  cp "$ROOT/LICENSE" "$D/LICENSE"; cp "$RUNDIR/LICENSE" "$D/LICENSE-gasm"
  if tar --version 2>/dev/null | grep -q GNU; then own=(--owner=0 --group=0 --numeric-owner); else own=(--uid 0 --gid 0 --no-xattrs --no-mac-metadata); fi
  COPYFILE_DISABLE=1 tar "${own[@]}" -C "$STAGE" -czf "$OUT/$NAME.tar.gz" "$NAME"
  sha "$NAME.tar.gz"
  echo "$OUT/$NAME.tar.gz" ;;
windows-*)
  STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
  D="$STAGE/$NAME"; mkdir "$D"
  cp "$RUNDIR/gasm-run.exe" "$D/"
  cp "$WASM" "$D/openballance.wasm"
  fill "$SRC/OpenBallance.cmd" "$D/OpenBallance.cmd"; fill "$SRC/openballance.ps1" "$D/openballance.ps1"
  readme "$D/README.txt"
  cp "$ROOT/LICENSE" "$D/LICENSE.txt"; cp "$RUNDIR/LICENSE" "$D/LICENSE-gasm.txt"
  for f in OpenBallance.cmd openballance.ps1 README.txt LICENSE.txt LICENSE-gasm.txt; do crlf "$D/$f"; done
  rm -f "$OUT/$NAME.zip"
  (cd "$STAGE" && zip -qr "$OUT/$NAME.zip" "$NAME")
  sha "$NAME.zip"
  echo "$OUT/$NAME.zip" ;;
esac

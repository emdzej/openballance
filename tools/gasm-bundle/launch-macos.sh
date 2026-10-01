#!/bin/bash
# Launcher of Ballance (gasm).app: OpenBallance @VERSION@ (openballance.wasm) on the bundled gasm-run @GASM_VERSION@.
# Installed as Contents/MacOS/OpenBallance by tools/package-gasm.sh. macOS ships bash 3.2: no bash 4 features.
#
# Finds the Ballance game data (remembered in ~/Library/Application Support/OpenBallance/data-location), asks
# for it with a dialog when it is missing, then runs gasm-run; its output goes to
# ~/Library/Logs/OpenBallance/gasm.log.
# Test hooks (no dialogs): OPENBALLANCE_DATA=<folder|image> uses that data without saving it,
# OPENBALLANCE_DRY_RUN=1 or --dry-run prints the gasm-run command instead of running it. --help: the options.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RES="$(cd "$HERE/../Resources" && pwd)"
CONF="$HOME/Library/Application Support/OpenBallance"
LOCFILE="$CONF/data-location"
LOGDIR="$HOME/Library/Logs/OpenBallance"
LOG="$LOGDIR/gasm.log"
TITLE="Ballance (gasm)"
DRY=${OPENBALLANCE_DRY_RUN:-0}
CHANGE=0

usage() {
  cat <<EOF
Ballance (gasm).app: OpenBallance @VERSION@ on gasm-run @GASM_VERSION@

  open "Ballance (gasm).app" [--args [options] [DATA] [gasm-run options...]]
  "Ballance (gasm).app/Contents/MacOS/OpenBallance" [options] [DATA] [gasm-run options...]

DATA is your copy of Ballance: the CD or a mounted disc image (/Volumes/...; it has Setup/data1.hdr), a
folder you copied the CD to, the installed game's folder (it has base.cmo), or a disc image file (.iso,
or the .bin of a .bin/.cue pair). It is remembered in
  $LOCFILE
Without one the app asks for it. Hold Option while opening the app (or pass --change-data) to pick
another; --forget-data deletes the saved location.

Options:
  --change-data  ask for the game data even if a location is saved
  --forget-data  delete the saved location and exit
  --dry-run      print the gasm-run command instead of running it (also OPENBALLANCE_DRY_RUN=1)
  --help         this text
Anything after DATA goes to gasm-run, e.g. --param unlockall=1, --param language=0, --keymap FILE, --mute.
OPENBALLANCE_DATA=<DATA> uses that data for one run without saving it.

Log: $LOG
More: https://openballance.emdzej.pl/guide/
EOF
}

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

# ci_find <dir> <name> <f|d>: the entry of <dir> called <name> in any letter case
ci_find() { find "$1" -mindepth 1 -maxdepth 1 -iname "$2" -type "$3" -print 2>/dev/null | head -n 1; }

# check_data <path>: exit 0 if usable; otherwise prints why
check_data() {
  local p=$1 setup
  if [ -d "$p" ]; then
    [ -n "$(ci_find "$p" base.cmo f)" ] && return 0                                    # the installed game
    setup=$(ci_find "$p" Setup d)
    if [ -n "$setup" ] && [ -n "$(ci_find "$setup" data1.hdr f)" ]; then return 0; fi   # the CD
    echo "This folder is neither the Ballance CD (it has Setup/data1.hdr) nor the installed game (it has base.cmo):"
    echo "$p"
    return 1
  fi
  if [ -f "$p" ]; then
    case "$(lower "$p")" in
      *.iso | *.bin) return 0 ;;
      *.cue) echo "This is the cue sheet: choose the .bin file next to it."; return 1 ;;
      *) echo "Not a disc image: use an .iso/.bin disc image or the CD/installed folder."; echo "$p"; return 1 ;;
    esac
  fi
  echo "The Ballance game data was not found at:"
  echo "$p"
  echo "Insert or mount the CD, or choose it again."
  return 1
}

# absolute <path>: absolute path without a trailing slash
absolute() {
  local p=$1
  case "$p" in /*) ;; *) p="$PWD/$p" ;; esac
  while [ "${#p}" -gt 1 ] && [ "${p%/}" != "$p" ]; do p=${p%/}; done
  printf '%s' "$p"
}

option_held() { # the Option key is down (NSEvent modifier flag 1 << 19)
  [ "$(osascript -l JavaScript -e 'ObjC.import("AppKit"); ($.NSEvent.modifierFlags & 0x80000) ? "1" : "0"' 2>/dev/null)" = 1 ]
}

# ask <message>: a dialog, then a folder or file chooser; prints the chosen path (empty on Quit)
ask() {
  local choice
  choice=$(osascript -e 'on run argv' \
    -e 'button returned of (display dialog (item 1 of argv) with title (item 2 of argv) buttons {"Quit", "Choose disc image (.iso, .bin)…", "Choose your Ballance folder…"} default button 3 cancel button 1 with icon note)' \
    -e 'end run' "$1" "$TITLE" 2>/dev/null) || return 0
  case "$choice" in
    "Choose your Ballance folder…")
      osascript -e 'POSIX path of (choose folder with prompt "Choose the Ballance CD (the folder with Setup) or the installed game (the folder with base.cmo):" default location (POSIX file "/Volumes" as alias))' 2>/dev/null ;;
    "Choose disc image (.iso, .bin)…")
      osascript -e 'POSIX path of (choose file with prompt "Choose the Ballance CD image (.iso, or the .bin of a .bin/.cue pair):")' 2>/dev/null ;;
  esac
}

INTRO="OpenBallance needs your copy of Ballance (it is not included).

Choose the CD itself or a mounted disc image (they appear under /Volumes), a folder you copied the CD to, the folder of an installed Ballance (with base.cmo), or an .iso or .bin image file.

Your choice is remembered. To change it later, hold Option while opening the app."

# Launcher options, then optional game data, then gasm-run's options.
while [ $# -gt 0 ]; do
  case "$1" in
    --help | -h) usage; exit 0 ;;
    --dry-run) DRY=1; shift ;;
    --change-data | --change-cd) CHANGE=1; shift ;;
    --forget-data | --forget-cd) rm -f "$LOCFILE"; echo "forgot the game data location ($LOCFILE)"; exit 0 ;;
    -psn_*) shift ;;   # process serial number from older Finder launches
    *) break ;;
  esac
done
DATAP="" SAVE=0
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then DATAP=$(absolute "$1"); SAVE=1; shift; fi
if [ -z "$DATAP" ] && [ -n "${OPENBALLANCE_DATA:-}" ]; then DATAP=$(absolute "$OPENBALLANCE_DATA"); fi
if [ -z "$DATAP" ] && [ "$CHANGE" = 0 ] && [ -f "$LOCFILE" ]; then
  DATAP=$(head -n 1 "$LOCFILE")
  if [ "$DRY" = 0 ] && option_held; then DATAP=""; fi
fi

MSG=$INTRO
[ -n "$DATAP" ] && { MSG=$(check_data "$DATAP") || true; }
while [ -z "$DATAP" ] || ! check_data "$DATAP" >/dev/null; do
  if [ "$DRY" = 1 ] || [ -n "${OPENBALLANCE_DATA:-}" ]; then   # never open dialogs in test runs
    echo "no usable Ballance game data${DATAP:+: $MSG}" >&2
    exit 2
  fi
  DATAP=$(ask "$MSG")
  [ -n "$DATAP" ] || exit 0
  DATAP=$(absolute "$DATAP")
  SAVE=1
  MSG=$(check_data "$DATAP") || true
done
if [ "$SAVE" = 1 ] && [ "$DRY" = 0 ]; then
  mkdir -p "$CONF" && printf '%s\n' "$DATAP" > "$LOCFILE"
fi

if [ -d "$DATAP" ]; then DATA=(--asset-dir "$DATAP"); else DATA=(--rom "$DATAP"); fi
CMD=("$HERE/gasm-run" "$RES/openballance.wasm" "${DATA[@]}" --window 1280x960 "$@")
if [ "$DRY" = 1 ]; then printf '%q ' "${CMD[@]}"; echo; exit 0; fi

mkdir -p "$LOGDIR"
{ echo "--- $(date '+%Y-%m-%d %H:%M:%S') OpenBallance @VERSION@, gasm-run @GASM_VERSION@"; printf '%q ' "${CMD[@]}"; echo; } >>"$LOG"
"${CMD[@]}" >>"$LOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
  osascript -e 'on run argv' \
    -e 'display alert "OpenBallance stopped with an error" message (item 1 of argv) as critical' -e 'end run' \
    "$(tail -n 4 "$LOG")

Full log: $LOG" >/dev/null 2>&1
fi
exit "$rc"

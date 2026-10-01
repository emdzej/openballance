#!/bin/sh
# OpenBallance @VERSION@ (openballance.wasm) on the bundled gasm-run @GASM_VERSION@: Linux launcher.
#   ./openballance.sh [options] [DATA] [gasm-run options...]          (./openballance.sh --help)
# The game data location comes from the argument (then saved), else
# $XDG_CONFIG_HOME/openballance/data-location, else a zenity or kdialog chooser (when a display is
# available), else the usage text. Test hooks (no dialogs): OPENBALLANCE_DATA=<folder|image> uses that data
# without saving it, OPENBALLANCE_DRY_RUN=1 or --dry-run prints the gasm-run command instead of running it.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/openballance"
LOCFILE="$CONF/data-location"
LOGDIR="${XDG_STATE_HOME:-$HOME/.local/state}/openballance"
LOG="$LOGDIR/gasm.log"
TITLE="Ballance (gasm)"
DRY=${OPENBALLANCE_DRY_RUN:-0}
CHANGE=0

usage() {
  cat <<EOF
OpenBallance @VERSION@ on gasm-run @GASM_VERSION@

  $0 [options] [DATA] [gasm-run options...]

DATA is your copy of Ballance: the CD or a mounted disc image (e.g. /media/${USER:-you}/BALLANCE; it has
Setup/data1.hdr), a folder you copied the CD to, the installed game's folder (it has base.cmo), or a
disc image file (.iso, or the .bin of a .bin/.cue pair). It is saved in
  $LOCFILE
so later runs need no argument. Without one, a chooser opens (zenity or kdialog).

Options:
  --change-data      ask for the game data even if a location is saved
  --forget-data      delete the saved location and exit
  --install-desktop  add a menu entry (~/.local/share/applications/openballance-gasm.desktop) and exit
  --dry-run          print the gasm-run command instead of running it (also OPENBALLANCE_DRY_RUN=1)
  --help             this text
Anything after DATA goes to gasm-run, e.g. --param unlockall=1, --param language=0, --keymap FILE, --mute.
OPENBALLANCE_DATA=<DATA> uses that data for one run without saving it.

More: https://openballance.emdzej.pl/guide/
EOF
}

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

# ci_find <dir> <name> <f|d>: the entry of <dir> called <name> in any letter case
ci_find() { find "$1" -mindepth 1 -maxdepth 1 -iname "$2" -type "$3" -print 2>/dev/null | head -n 1; }

# check_data <path>: exit 0 if usable; otherwise prints why
check_data() {
  if [ -d "$1" ]; then
    [ -n "$(ci_find "$1" base.cmo f)" ] && return 0                                    # the installed game
    setup=$(ci_find "$1" Setup d)
    if [ -n "$setup" ] && [ -n "$(ci_find "$setup" data1.hdr f)" ]; then return 0; fi   # the CD
    printf 'This folder is neither the Ballance CD (it has Setup/data1.hdr) nor the installed game (it has base.cmo):\n%s\n' "$1"
    return 1
  fi
  if [ -f "$1" ]; then
    case "$(lower "$1")" in
      *.iso | *.bin) return 0 ;;
      *.cue) echo "This is the cue sheet: choose the .bin file next to it."; return 1 ;;
      *) printf 'Not a disc image: use an .iso/.bin disc image or the CD/installed folder.\n%s\n' "$1"; return 1 ;;
    esac
  fi
  printf 'The Ballance game data was not found at:\n%s\nInsert or mount the CD, or choose it again.\n' "$1"
  return 1
}

absolute() { # absolute path without a trailing slash
  p=$1
  case "$p" in /*) ;; *) p="$PWD/$p" ;; esac
  while [ "${#p}" -gt 1 ] && [ "${p%/}" != "$p" ]; do p=${p%/}; done
  printf '%s' "$p"
}

have_gui() { [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && { command -v zenity >/dev/null 2>&1 || command -v kdialog >/dev/null 2>&1; }; }

# ask <message>: prints the chosen folder or image (empty on Quit)
ask() {
  if command -v zenity >/dev/null 2>&1; then
    choice=$(zenity --question --title "$TITLE" --text "$1" --ok-label "Choose your Ballance folder" \
      --cancel-label Quit --extra-button "Choose disc image (.iso, .bin)" 2>/dev/null)
    rc=$?
    if [ "$rc" = 0 ]; then
      zenity --file-selection --directory --title "Choose the Ballance CD (with Setup) or the installed game (with base.cmo)" 2>/dev/null
    elif [ "$choice" = "Choose disc image (.iso, .bin)" ]; then
      zenity --file-selection --title "Choose the Ballance CD image (.iso, or the .bin of a .bin/.cue pair)" \
        --file-filter "Disc images | *.iso *.ISO *.bin *.BIN" --file-filter "All files | *" 2>/dev/null
    fi
  else
    kdialog --title "$TITLE" --yesnocancel "$1" --yes-label "Choose your Ballance folder" \
      --no-label "Choose disc image (.iso, .bin)" --cancel-label Quit 2>/dev/null
    case $? in
      0) kdialog --title "Choose the Ballance CD (with Setup) or the installed game (with base.cmo)" --getexistingdirectory "$HOME" 2>/dev/null ;;
      1) kdialog --title "Choose the Ballance CD image (.iso, or the .bin of a .bin/.cue pair)" --getopenfilename "$HOME" "*.iso *.ISO *.bin *.BIN" 2>/dev/null ;;
    esac
  fi
}

error_box() {
  if command -v zenity >/dev/null 2>&1; then zenity --error --title "$TITLE" --text "$1" 2>/dev/null
  elif command -v kdialog >/dev/null 2>&1; then kdialog --title "$TITLE" --error "$1" 2>/dev/null; fi
}

install_desktop() {
  dir="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
  mkdir -p "$dir"
  sed -e "s|@DIR@|$HERE|g" "$HERE/openballance-gasm.desktop" > "$dir/openballance-gasm.desktop"
  echo "installed $dir/openballance-gasm.desktop"
}

INTRO="OpenBallance needs your copy of Ballance (it is not included).

Choose the CD itself or a mounted disc image (for example under /media/${USER:-you}), a folder you copied the CD to, the folder of an installed Ballance (with base.cmo), or an .iso or .bin image file.

Your choice is remembered; run openballance.sh --change-data to pick another."

while [ $# -gt 0 ]; do
  case "$1" in
    --help | -h) usage; exit 0 ;;
    --dry-run) DRY=1; shift ;;
    --change-data | --change-cd) CHANGE=1; shift ;;
    --forget-data | --forget-cd) rm -f "$LOCFILE"; echo "forgot the game data location ($LOCFILE)"; exit 0 ;;
    --install-desktop) install_desktop; exit 0 ;;
    *) break ;;
  esac
done
DATAP="" SAVE=0
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then DATAP=$(absolute "$1"); SAVE=1; shift; fi
if [ -z "$DATAP" ] && [ -n "${OPENBALLANCE_DATA:-}" ]; then DATAP=$(absolute "$OPENBALLANCE_DATA"); fi
if [ -z "$DATAP" ] && [ "$CHANGE" = 0 ] && [ -f "$LOCFILE" ]; then DATAP=$(head -n 1 "$LOCFILE"); fi

MSG=$INTRO
[ -n "$DATAP" ] && { MSG=$(check_data "$DATAP") || true; }
while [ -z "$DATAP" ] || ! check_data "$DATAP" >/dev/null; do
  if [ "$DRY" = 1 ] || [ -n "${OPENBALLANCE_DATA:-}" ] || ! have_gui; then
    [ -n "$DATAP" ] && printf '%s\n\n' "$MSG" >&2
    [ "$DRY" = 1 ] || [ -n "${OPENBALLANCE_DATA:-}" ] || usage >&2
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

if [ -d "$DATAP" ]; then set -- --asset-dir "$DATAP" "$@"; else set -- --rom "$DATAP" "$@"; fi
set -- "$HERE/gasm-run" "$HERE/openballance.wasm" "$@"
if [ "$DRY" = 1 ]; then
  for a in "$@"; do printf "'%s' " "$(printf '%s' "$a" | sed "s/'/'\\\\''/g")"; done
  echo
  exit 0
fi

if [ -t 1 ] || [ -t 2 ]; then exec "$@"; fi
# Started from a menu or file manager: keep the output in a log and show errors in a dialog.
mkdir -p "$LOGDIR"
{ echo "--- $(date '+%Y-%m-%d %H:%M:%S') OpenBallance @VERSION@, gasm-run @GASM_VERSION@"; echo "$*"; } >>"$LOG"
"$@" >>"$LOG" 2>&1
rc=$?
[ "$rc" = 0 ] || error_box "OpenBallance stopped with an error:

$(tail -n 4 "$LOG")

Full log: $LOG"
exit "$rc"

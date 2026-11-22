#!/bin/bash
# Harry Potter Sorcerer's Stone Installer: the bundle's executable. The game is built on the player's Mac from their own disc image:
#   1. Apple's Command Line Tools (clt.sh)
#   2. the disc image (Xbox, USA): its files into ~/Library/Application Support/Harry Potter Sorcerer's Stone/game, default.xbe checked
#   3. the game's program translated to C and compiled with clang (mac/build_game.sh), the icon from the disc's title image
#   4. "Harry Potter Sorcerer's Stone.app" in /Applications (~/Applications when /Applications isn't writable), then opened
# The installer holds no game code or data. Same flow as the NFS Hot Pursuit 2 port's installer.
# Environment: HP1_IMPORT_ISO=<iso> (no file dialog), HP1_INSTALL_DIR=<folder> (where the app goes), HP1_NO_UI=1 (no
# dialogs, no progress window, no opening: tests), HP1_BUILD_JOBS=<n>.
set -uo pipefail
RES="$(cd "$(dirname "$0")/../Resources" && pwd)"
KIT="$RES/kit"
VERSION="$(cat "$KIT/VERSION")"
USER_DIR="$HOME/Library/Application Support/Harry Potter Sorcerer's Stone"
GAME="$USER_DIR/game"
WORK="$USER_DIR/build"
APP_NAME="Harry Potter Sorcerer's Stone.app"
mkdir -p "$USER_DIR/logs" "$WORK"
LOG="$USER_DIR/logs/install.log"
PROG="$WORK/progress.txt"
exec >"$LOG" 2>&1
echo "installer $VERSION, macOS $(sw_vers -productVersion), $(date)"
: > "$PROG"

ui() { [ -z "${HP1_NO_UI:-}" ]; }
alert() { ui && /usr/bin/osascript -e "display alert \"Harry Potter Sorcerer's Stone\" message \"$1\" as critical" >/dev/null 2>&1; echo "alert: $1"; }
fail() { echo "100 ERROR: $1" >> "$PROG"; alert "$1

The log: ~/Library/Application Support/Harry Potter Sorcerer's Stone/logs/install.log"; exit 1; }
say() { echo "$1 $2" >> "$PROG"; echo "== $2"; }

# where the app goes
if [ -n "${HP1_INSTALL_DIR:-}" ]; then DEST="$HP1_INSTALL_DIR"
elif [ -w /Applications ]; then DEST=/Applications
else DEST="$HOME/Applications"; fi
mkdir -p "$DEST"

# built already: open it, or build it again
if [ -d "$DEST/$APP_NAME" ] && ui; then
  choice=$(/usr/bin/osascript -e "button returned of (display dialog \"Harry Potter Sorcerer's Stone is installed in $DEST.\" buttons {\"Rebuild\", \"Quit\", \"Play\"} default button \"Play\" with title \"Harry Potter Sorcerer's Stone\")" 2>/dev/null)
  case "$choice" in
    Play) open "$DEST/$APP_NAME"; exit 0 ;;
    Rebuild) ;;
    *) exit 0 ;;
  esac
fi

# 1. Command Line Tools
bash "$KIT/clt.sh" "$PROG" || fail "Apple's Command Line Tools are needed to build the game. In Terminal: xcode-select --install"

# 2. the disc
installed() { [ -d "$GAME/resource" ] && /usr/bin/python3 -I "$KIT/mac/xbe_check.py" "$GAME/default.xbe"; }
if ! installed; then
  ISO="${HP1_IMPORT_ISO:-}"
  if [ -z "$ISO" ]; then
    ui || fail "no disc image (HP1_IMPORT_ISO)"
    ISO=$(/usr/bin/osascript -e "POSIX path of (choose file with prompt \"Choose your disc image of Harry Potter and the Sorcerer's Stone (Xbox, USA):\" of type {\"iso\", \"public.iso-image\", \"public.data\"})" 2>/dev/null) || exit 0
  fi
  [ -f "$ISO" ] || fail "No such disc image: $ISO"
  ui && { /usr/bin/osascript -l JavaScript "$KIT/progress.js" "$PROG" >/dev/null 2>&1 & }
  say 2 "Copying the game's files from the disc image (1.8 GB)"
  /usr/bin/python3 -I "$KIT/mac/import_disc.py" "$ISO" "$GAME"
  case $? in
    0) ;;
    3) fail "That file is not an Xbox disc image." ;;
    4) fail "That disc image is not Harry Potter and the Sorcerer's Stone." ;;
    5) fail "This edition of Harry Potter and the Sorcerer's Stone is not supported yet. Supported: the Xbox USA disc." ;;
    *) fail "Copying the disc image failed." ;;
  esac
else
  ui && { /usr/bin/osascript -l JavaScript "$KIT/progress.js" "$PROG" >/dev/null 2>&1 & }
fi

# 3. the game's program as C, compiled; the icon
rm -rf "$WORK/game"
bash "$KIT/mac/build_game.sh" "$KIT/build" "$GAME" "$KIT/libs" "$WORK/game" "$WORK/hp1" "$PROG" \
  || fail "Building the game failed."
say 95 "Making the icon from the disc's title image"
ICON="$WORK/hp1.icns"
bash "$KIT/mac/make_icon.sh" "$GAME/default.xbe" "$ICON" || { echo "no icon"; ICON=-; }

# 4. the app
say 97 "Putting Harry Potter Sorcerer's Stone in $DEST"
bash "$KIT/mac/assemble_app.sh" "$WORK/hp1" "$KIT/libs" "$ICON" "$DEST/$APP_NAME" "$VERSION" \
  || fail "Putting the app in $DEST failed."
rm -rf "$WORK/game" "$WORK/hp1" "$WORK/hp1.icns"   # the build's copy of the game's code goes; the game's files stay
say 100 "Done: Harry Potter Sorcerer's Stone is in $DEST"
echo "installed: $DEST/$APP_NAME"
ui && { sleep 1; open "$DEST/$APP_NAME"; }
exit 0

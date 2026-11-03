#!/usr/bin/env bash
# Adds "Tomodachi Life" to your application menu: a desktop entry in ~/.local/share/applications that starts the native
# Linux build (saves in ~/.local/share/tomodachi-recomp/userdata, the game's output of the last start in
# ~/.local/share/tomodachi-recomp/last_run.log). Inside a distrobox container (Steam Deck) the entry starts the game in it.
#
#   scripts/linux/install_desktop.sh             install (again, after moving this folder)
#   scripts/linux/install_desktop.sh --remove    remove the entry (saves are kept)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/tomodachi-recomp"
ENTRY="$APPS/tomodachi-recomp.desktop"

if [ "${1:-}" = "--remove" ]; then
    rm -f "$ENTRY" "$DATA/launch.sh"
    echo "removed $ENTRY (saves in $DATA/userdata are kept)"
    exit 0
fi
[ -x "$REPO/build-linux/work/tomodachi_native" ] || { echo "no Linux build in $REPO/build-linux: run scripts/linux/install.sh first" >&2; exit 1; }

mkdir -p "$APPS" "$DATA"
RUN="exec \"$REPO/play.sh\" \"\$@\""
if [ -n "${CONTAINER_ID:-}" ] && command -v distrobox-export >/dev/null 2>&1; then     # distrobox: the menu is the host's
    RUN="exec distrobox enter $CONTAINER_ID -- \"$REPO/play.sh\" \"\$@\""
fi
# a small launcher script, so that the desktop entry holds no quoting of paths with spaces
printf '#!/bin/sh\n%s\n' "$RUN" > "$DATA/launch.sh"
chmod +x "$DATA/launch.sh"
ICON=applications-games
[ -f "$REPO/build-linux/work/title-logo.png" ] && ICON="$REPO/build-linux/work/title-logo.png"
cat > "$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Name=Tomodachi Life
Comment=Tomodachi Life: Living the Dream, native Linux build
Exec="$DATA/launch.sh"
Icon=$ICON
Terminal=false
Categories=Game;
EOF
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
echo "installed $ENTRY"

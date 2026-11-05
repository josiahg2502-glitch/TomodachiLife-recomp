#!/usr/bin/env bash
# The whole Linux install in one command (docs/LINUX.md): the build's dependencies (scripts/linux/install_deps.sh, asks
# for sudo once), the game built from your own dump (scripts/setup.py --target linux, about 40 minutes, resumable: run it
# again after an interruption), and "Tomodachi Life" in your application menu (scripts/linux/install_desktop.sh).
#
#   scripts/linux/install.sh <dump folder>     <dump folder> holds exefs/ and romfs/ (README.md: what to dump)
#   scripts/linux/install.sh <dump folder> --no-system     you installed the system packages yourself
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ $# -ge 1 ] && [ -d "$1" ] || { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
DUMP="$(cd "$1" && pwd)"
shift
# SteamOS (Steam Deck): the build runs in a distrobox container (install_deps.sh says why). It is made here and the install goes
# on inside it: a list of commands to paste lost its last line to `distrobox enter`, which starts a shell (issue #39).
. /etc/os-release
if [ "${ID:-}" = steamos ] && [[ " $* " != *" --steamos-host "* ]] && [[ " $* " != *" --no-system "* ]]; then
    command -v distrobox >/dev/null || { echo "This SteamOS has no distrobox (SteamOS 3.5 and newer include it): docs/LINUX.md, Steam Deck" >&2; exit 1; }
    distrobox list | grep -w tomodachi >/dev/null || distrobox create --yes --no-entry --name tomodachi --image docker.io/library/archlinux:latest
    cd "$REPO"
    exec distrobox enter tomodachi -- "$REPO/scripts/linux/install.sh" "$DUMP" "$@"
fi
"$REPO/scripts/linux/install_deps.sh" "$@"
"$REPO/deps/venv/bin/python" "$REPO/scripts/setup.py" --target linux --dump "$DUMP"
"$REPO/scripts/linux/install_desktop.sh"
echo
echo "Installed. Start the game from your application menu (Tomodachi Life) or with: $REPO/play.sh"

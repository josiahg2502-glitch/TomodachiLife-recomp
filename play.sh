#!/usr/bin/env bash
# Starts the native Linux build (docs/LINUX.md). Run it from anywhere:
#
#   ./play.sh                          play; saves in ~/.local/share/tomodachi-recomp/userdata
#   ./play.sh --userdata <folder>      another save folder
#   ./play.sh --help                   every option (they are scripts/run.py's)
#
# The game's output is shown here and also kept in ~/.local/share/tomodachi-recomp/last_run.log (attach it to a bug report).
set -uo pipefail

REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/tomodachi-recomp"
PY="$REPO/deps/venv/bin/python"

[ -x "$PY" ] || { echo "deps/venv is missing: run scripts/linux/install.sh <your dump folder> first" >&2; exit 1; }
mkdir -p "$DATA"
"$PY" "$REPO/scripts/run.py" "$@" 2>&1 | tee "$DATA/last_run.log"
exit "${PIPESTATUS[0]}"

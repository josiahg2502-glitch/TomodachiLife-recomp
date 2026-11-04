#!/usr/bin/env bash
# The Linux updater: moves this folder to the newest release and builds it (your dump, saves and settings stay; a run takes
# from a few minutes to an hour, depending on what changed, and can be run again after an interruption).
#
#   scripts/linux/update.sh           update to the newest release, if there is one
#   scripts/linux/update.sh --check   only say whether there is one (exit 0: there is; 1: up to date or no network)
#   scripts/linux/update.sh --daily   the check play.sh runs in the background, at most once a day: a notice when there is one
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/tomodachi-recomp"
cd "$REPO"

newest() { git ls-remote --tags --refs origin 'v*' 2>/dev/null | sed 's#.*refs/tags/##' | grep -v -e '-beta' | sort -V | tail -1; }
current() { git describe --tags --abbrev=0 2>/dev/null || echo v0; }
newer() { [ -n "$1" ] && [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]; }

case "${1:-}" in
--check)
    n="$(newest)"; c="$(current)"
    if newer "$n" "$c"; then echo "$n is out (this copy: $c): scripts/linux/update.sh installs it"; exit 0; fi
    echo "up to date ($c)"; exit 1 ;;
--daily)
    mkdir -p "$DATA"
    stamp="$DATA/update_checked"
    [ -z "$(find "$stamp" -mmin -1440 2>/dev/null)" ] || exit 0
    touch "$stamp"
    n="$(newest)"; c="$(current)"
    if newer "$n" "$c"; then
        msg="Tomodachi Life $n is out (you have $c). To install it, run: $REPO/scripts/linux/update.sh, or choose \"Update\" on the game's menu entry."
        echo "$msg" > "$DATA/update_available"
        command -v notify-send >/dev/null 2>&1 && notify-send "Tomodachi Life" "$msg" || true
    else rm -f "$DATA/update_available"; fi
    exit 0 ;;
esac

n="$(newest)"; c="$(current)"
if ! newer "$n" "$c"; then echo "up to date ($c)"; exit 0; fi
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "this folder has changes of its own: commit or remove them first (git status)" >&2; exit 1; }
echo "updating $c -> $n"
git fetch --tags origin
git checkout -q "$n"
DUMP="$("$REPO/deps/venv/bin/python" - <<'EOF'
import json, os
try:
    print(os.path.dirname(json.load(open(os.path.join('build-linux', 'state.json')))['dump']['romfs']))
except Exception:
    print('')
EOF
)"
[ -n "$DUMP" ] && [ -d "$DUMP" ] || { echo "the dump folder of the last build was not found: run scripts/linux/install.sh <dump folder>" >&2; exit 1; }
"$REPO/scripts/linux/install.sh" "$DUMP" --no-system
rm -f "$DATA/update_available"
echo "updated to $n"

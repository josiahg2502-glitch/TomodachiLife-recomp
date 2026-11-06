#!/usr/bin/env bash
# Installs what the native Linux build needs (docs/LINUX.md):
#   1. the system packages: clang, SDL3, FFmpeg, glslang, Vulkan loader, OpenGL, Opus (asks for sudo)
#   2. SDL3 3.4.18 built from source into deps/sdl3-linux when the system has no SDL3 (Ubuntu before 25.04, Debian 12)
#   3. a Python virtual environment in deps/venv with the build's Python packages
#   4. deps/Vulkan-Headers (v1.4.329, as the Windows build), deps/imgui (v1.91.9b, the settings menu), and the two files of
#      a yuzu-family source tree the self-test reads (deps/suyu), each checked by its SHA-256
#
#   scripts/linux/install_deps.sh                  everything
#   scripts/linux/install_deps.sh --no-system      skip step 1 (you install the packages yourself)
#   scripts/linux/install_deps.sh --steamos-host   on a Steam Deck: use pacman on SteamOS itself instead of stopping with
#                                                  the advice to build in a distrobox container
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS="$REPO/deps"
SYSTEM=1
STEAMOS_HOST=0
for arg in "$@"; do
    case "$arg" in
        --no-system) SYSTEM=0 ;;
        --steamos-host) STEAMOS_HOST=1 ;;
        -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $arg" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
SUDO=""
[ "$(id -u)" = 0 ] || SUDO=sudo

# ---------------------------------------------------------------------------------------------- 1. system packages
if [ "$SYSTEM" = 1 ]; then
    . /etc/os-release
    # SteamOS (Steam Deck): the system is read-only and every SteamOS update removes what pacman installed. A distrobox
    # container (SteamOS 3.5 and newer include distrobox and podman) keeps the tools in your home folder and survives
    # updates; the game runs from it with the Deck's GPU and sound.
    if [ "${ID:-}" = "steamos" ] && [ "$STEAMOS_HOST" = 0 ]; then
        say "SteamOS (Steam Deck): build inside a distrobox container"
        echo "  In Desktop Mode, open Konsole and run, from this folder (it makes the container and builds inside it):"
        echo
        echo "    scripts/linux/install.sh \"<your dump folder>\""
        echo
        echo "  Keep this folder in your home folder (the container sees your home folder). The application menu entry the"
        echo "  install makes starts the game in the container."
        echo "  To install on SteamOS itself instead (needs a sudo password, 'sudo steamos-readonly disable' and the pacman"
        echo "  keys; a SteamOS update removes the packages again), run this script with --steamos-host."
        exit 1
    fi
    if command -v pacman >/dev/null; then
        say "Arch-based system ($PRETTY_NAME): pacman"
        EXTRA=""
        # a Steam Deck (a fresh Arch container has no GPU driver and no sound client library): Mesa RADV and libpulse,
        # which SDL3 loads to play through the Deck's PipeWire
        if [ "$(cat /sys/devices/virtual/dmi/id/board_vendor 2>/dev/null)" = "Valve" ]; then
            EXTRA="vulkan-radeon libpulse"
            echo "  Steam Deck: also $EXTRA"
        fi
        # shellcheck disable=SC2086
        $SUDO pacman -S --needed clang lld pkgconf sdl3 ffmpeg glslang vulkan-icd-loader libglvnd opus \
            python git curl $EXTRA
    elif command -v apt-get >/dev/null; then
        say "Debian/Ubuntu-based system ($PRETTY_NAME): apt"
        PKGS="clang lld pkg-config libavformat-dev libavcodec-dev libswscale-dev libswresample-dev
              libavutil-dev glslang-dev glslang-tools libvulkan-dev libgl-dev libopus0 python3-venv git curl"
        if apt-cache show libsdl3-dev >/dev/null 2>&1; then
            PKGS="$PKGS libsdl3-dev"
        else                                   # no SDL3 package (Ubuntu before 25.04, Debian 12): step 2 builds it
            PKGS="$PKGS cmake ninja-build libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev
                  libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libegl-dev libgbm-dev libdrm-dev libasound2-dev
                  libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev"
        fi
        $SUDO apt-get update
        # shellcheck disable=SC2086
        $SUDO apt-get install -y $PKGS
    elif command -v dnf >/dev/null; then
        say "Fedora-based system ($PRETTY_NAME): dnf"
        $SUDO dnf install -y clang lld pkgconf-pkg-config SDL3-devel libavformat-free-devel \
            libavcodec-free-devel libswscale-free-devel libswresample-free-devel libavutil-free-devel glslang-devel \
            vulkan-loader-devel libglvnd-devel opus python3 git curl
    else
        say "Unknown distribution ($PRETTY_NAME): install these yourself, then run again with --no-system"
        echo "  clang, pkg-config, development files of: SDL3, FFmpeg (libavformat libavcodec libswscale"
        echo "  libswresample libavutil), glslang, Vulkan (headers + loader), OpenGL (libGL); the Opus library;"
        echo "  Python 3.10+ with venv; git; curl"
        exit 1
    fi
    echo "  optional: zenity or kdialog (text boxes of the PC Settings page for a controller player use a dialog)"
fi

fetch() {          # fetch <url> <file> <sha256>: downloaded, checked, or refused
    curl -L --fail --silent --show-error --retry 3 -o "$2.part" "$1"
    echo "$3  $2.part" | sha256sum -c --quiet - || { rm -f "$2.part"; echo "$1: the download does not match its SHA-256: refused" >&2; exit 1; }
    mv "$2.part" "$2"
}

# ---------------------------------------------------------------------------------------------- 2. SDL3 from source
SDL_VER=3.4.18
SDL_SHA=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3      # GitHub's digest of the release asset
if pkg-config --exists sdl3 2>/dev/null && [ ! -d "$DEPS/sdl3-linux" ]; then
    say "SDL3 $(pkg-config --modversion sdl3): the system's"
elif [ -f "$DEPS/sdl3-linux/lib/pkgconfig/sdl3.pc" ]; then
    say "deps/sdl3-linux: present"
else
    say "SDL3 $SDL_VER: built from source into deps/sdl3-linux (the system has none)"
    SRC="$DEPS/SDL3-$SDL_VER"                  # the source the Android build uses (scripts/android), when it is there
    if [ ! -f "$SRC/CMakeLists.txt" ]; then
        mkdir -p "$DEPS/src"
        fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz" "$DEPS/src/SDL3-$SDL_VER.tar.gz" "$SDL_SHA"
        tar -xzf "$DEPS/src/SDL3-$SDL_VER.tar.gz" -C "$DEPS/src"
        SRC="$DEPS/src/SDL3-$SDL_VER"
    fi
    cmake -S "$SRC" -B "$DEPS/src/SDL3-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$DEPS/sdl3-linux" -DCMAKE_INSTALL_LIBDIR=lib
    cmake --build "$DEPS/src/SDL3-build"
    cmake --install "$DEPS/src/SDL3-build"
    rm -rf "$DEPS/src"
fi

# ---------------------------------------------------------------------------------------------- 3. Python packages
PY="${PYTHON:-python3}"
"$PY" -c 'import sys; sys.exit(sys.version_info < (3, 10))' || { echo "Python 3.10 or newer is needed ($PY)" >&2; exit 1; }
if [ ! -x "$DEPS/venv/bin/python" ]; then
    say "Python virtual environment: deps/venv"
    mkdir -p "$DEPS"
    "$PY" -m venv "$DEPS/venv"
fi
say "Python packages: zstandard lz4 Pillow pycryptodome"
"$DEPS/venv/bin/python" -m pip install --quiet zstandard lz4 Pillow pycryptodome

# ---------------------------------------------------------------------------------------------- 4. headers and sources
if [ -f "$DEPS/Vulkan-Headers/include/vulkan/vulkan.h" ]; then
    say "deps/Vulkan-Headers: present"
else
    say "deps/Vulkan-Headers: Khronos headers v1.4.329"
    rm -rf "$DEPS/Vulkan-Headers"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch v1.4.329 https://github.com/KhronosGroup/Vulkan-Headers.git "$DEPS/Vulkan-Headers"
fi
if [ -f "$DEPS/imgui/imgui.h" ] && [ -f "$DEPS/imgui/imgui.cpp" ]; then
    say "deps/imgui: present"
else
    say "deps/imgui: Dear ImGui v1.91.9b (the in-game settings menu)"
    rm -rf "$DEPS/imgui"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui.git "$DEPS/imgui" || \
        say "WARNING: Dear ImGui could not be fetched: the game builds without the settings menu"
fi
# the two files of suyu-emu/suyu-v0.0.4 (MIT / GPL-3.0) tools/lifter/gen_sdktest.py reads, from this project's files
# repository (scripts/install.py SUYU_FILES: the same hashes)
MIRROR=https://github.com/josiahg2502-glitch/TomodachiLife-recomp-files/releases/download/installer-tools
MII="$DEPS/suyu/src/core/hle/service/mii"
mkdir -p "$MII/types"
[ -f "$MII/types/raw_data.cpp" ] || fetch "$MIRROR/raw_data.cpp" "$MII/types/raw_data.cpp" 0f5b10dcde4038024fffd6079d75cd4bad8d72486efcf1ffa06e6c779803c486
[ -f "$MII/mii_types.h" ] || fetch "$MIRROR/mii_types.h" "$MII/mii_types.h" cf2e0c990b881c238f5b7fd823f33fe2c0a8d0846cdbf012ec6f702b1647f3e1
say "deps/suyu: present"

say "done"

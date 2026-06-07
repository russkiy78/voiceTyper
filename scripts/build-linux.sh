#!/usr/bin/env bash
#
# Configures and builds voiceTyper on Linux, producing the executable at
#   <build-dir>/voiceTyper
#
# Usage:
#   scripts/build-linux.sh [options]
#
# Options:
#   --qt <path>          Qt kit prefix (e.g. ~/Qt/6.11.1/gcc_64). Auto-detected
#                        from ~/Qt/*/gcc_64 if present; otherwise the distro Qt6
#                        is used. May also be set via the QT_PREFIX env var.
#   --build-type <type>  CMake build type (default: Release).
#   --build-dir <dir>    Build directory (default: build).
#   --jobs <N>           Parallel build jobs (default: nproc).
#   --no-whisper         Build without whisper.cpp (UI/plumbing only, NullAsrEngine).
#   --clean              Remove the build directory before configuring.
#   -D<var>=<val> ...    Any extra args are passed through to CMake configure.
#   -h, --help           Show this help.
#
# Build deps (Ubuntu/Debian):
#   sudo apt install build-essential cmake git \
#       libx11-dev libxtst-dev libxcb1-dev libasound2-dev libpulse-dev
#   plus Qt 6 (qt6-base-dev qt6-multimedia-dev) or an online-installer kit.
# Optional GPU (auto-detected): libvulkan-dev + glslc for Vulkan; CUDA toolkit for CUDA.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

QT_PREFIX="${QT_PREFIX:-}"
BUILD_TYPE="Release"
BUILD_DIR="build"
JOBS="$(nproc 2>/dev/null || echo 4)"
WITH_WHISPER="ON"
CLEAN=0
EXTRA_CMAKE_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qt)         QT_PREFIX="$2"; shift 2 ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --build-dir)  BUILD_DIR="$2"; shift 2 ;;
        --jobs)       JOBS="$2"; shift 2 ;;
        --no-whisper) WITH_WHISPER="OFF"; shift ;;
        --clean)      CLEAN=1; shift ;;
        -h|--help)
            sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//; s/^#$//; /^set -euo/d'
            exit 0 ;;
        *)            EXTRA_CMAKE_ARGS+=("$1"); shift ;;
    esac
done

# Absolute build path so the final message is copy-pasteable from anywhere.
case "$BUILD_DIR" in
    /*) BUILD_PATH="$BUILD_DIR" ;;
    *)  BUILD_PATH="$ROOT/$BUILD_DIR" ;;
esac

# Best-effort auto-detect of an online-installer Qt kit; distro Qt needs nothing.
if [[ -z "$QT_PREFIX" ]]; then
    QT_PREFIX="$(ls -d "$HOME"/Qt/*/gcc_64 2>/dev/null | sort -V | tail -n1 || true)"
fi

CMAKE_ARGS=(
    -S "$ROOT"
    -B "$BUILD_PATH"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DVOICETYPER_WITH_WHISPER="$WITH_WHISPER"
)
if [[ -n "$QT_PREFIX" ]]; then
    echo "Using Qt kit: $QT_PREFIX"
    CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
else
    echo "No online-installer Qt kit found; relying on distro Qt6."
fi
CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

if [[ "$CLEAN" == "1" && -d "$BUILD_PATH" ]]; then
    echo "Cleaning $BUILD_PATH"
    rm -rf "$BUILD_PATH"
fi

echo "==> Configuring (whisper=$WITH_WHISPER, type=$BUILD_TYPE)"
cmake "${CMAKE_ARGS[@]}"

echo "==> Building with $JOBS jobs"
cmake --build "$BUILD_PATH" -j "$JOBS"

BIN="$BUILD_PATH/voiceTyper"
echo
echo "Build complete."
echo "Executable: $BIN"

if ! compgen -G "$ROOT/models/ggml-*.bin" >/dev/null; then
    echo
    echo "No speech model found. Download one before running:"
    echo "  scripts/download-model.sh                # small-q5_1 (~180 MB)"
    echo "  scripts/download-model.sh large-v3-turbo-q5_0"
fi

if [[ -n "$QT_PREFIX" ]]; then
    echo
    echo "If it fails to start with a Qt library error, export the kit's libs:"
    echo "  export LD_LIBRARY_PATH=\"$QT_PREFIX/lib:\${LD_LIBRARY_PATH:-}\""
fi

#!/usr/bin/env bash
#
# Builds .deb packages for voiceTyper, one per compute backend:
#   voiceTyper-cpu     CPU only
#   voiceTyper-vulkan  CPU + Vulkan
#   voiceTyper-cuda    CPU + CUDA
#   voiceTyper-all     CPU + Vulkan + CUDA (universal)
#
# Why separate packages: whisper.cpp's GPU backends are linked into the binary,
# and CUDA in particular becomes a hard launch dependency (DT_NEEDED on
# libcudart/libcublas/libcuda). A single "universal" build therefore refuses to
# start on any machine without the CUDA runtime + NVIDIA driver — even for CPU
# or Vulkan users. Splitting keeps each package runnable on its target.
#
# Qt 6.11.1 is BUNDLED (from the Qt Online Installer kit) rather than declared
# as an apt dependency. Ubuntu 24.04 ships Qt 6.4.2 whose GStreamer multimedia
# backend silently fails to capture audio on PipeWire. Qt 6.11.1 uses the
# FFmpeg backend with native PipeWire/PulseAudio support that works correctly.
# ICU 73, the FFmpeg multimedia plugin, and platform plugins (xcb, wayland) are
# all bundled under /usr/lib/voiceTyper/. Only system xcb/wayland/GL/audio
# libraries are declared as apt Depends:.
#
# Usage:
#   scripts/build_deb.sh [variant ...]      # default: cpu vulkan cuda all
#   QT_KIT=~/Qt/6.11.1/gcc_64 scripts/build_deb.sh cpu
#   VOICETYPER_VERSION=0.3.42 scripts/build_deb.sh vulkan   # override the auto-derived version (CI)
#   VOICETYPER_EXTRA_CMAKE_ARGS="-DCMAKE_CXX_COMPILER_LAUNCHER=sccache" scripts/build_deb.sh
#
# Build deps (Ubuntu/Debian):
#   sudo apt install build-essential cmake git \
#       libx11-dev libxtst-dev libxcb1-dev libasound2-dev libpulse-dev
#   Vulkan variant also needs: libvulkan-dev glslc (glslang-tools / shaderc)
#   CUDA variant also needs:   the CUDA toolkit (nvcc, from NVIDIA's apt repo)
#   Qt 6.11.1 via the Qt Online Installer (https://www.qt.io/download-qt-installer)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PROJECT_NAME="voiceTyper"
# Patch is the git commit count (matches VT_VERSION baked into the binary by
# CMakeLists.txt) - NOT the raw ".0" patch literal in project(... VERSION x.y.0),
# which bump-version.sh/.ps1 always leave at .0. VOICETYPER_VERSION lets CI (which
# computes this once, up front, for all three platforms) pass the exact same
# string instead of every script re-deriving it independently.
if [ -n "${VOICETYPER_VERSION:-}" ]; then
    VERSION="$VOICETYPER_VERSION"
else
    MAJOR_MINOR="$(grep -A2 '^project(' "${ROOT_DIR}/CMakeLists.txt" | awk '/VERSION/{print $2}' | cut -d. -f1,2)"
    COMMIT_COUNT="$(git -C "$ROOT_DIR" rev-list --count HEAD 2>/dev/null || echo 0)"
    VERSION="${MAJOR_MINOR}.${COMMIT_COUNT}"
fi
ARCH="amd64"
JOBS="$(nproc 2>/dev/null || echo 4)"

# Extra CMake configure args (space-separated), e.g. sccache compiler launchers.
EXTRA_CMAKE_ARGS=()
if [ -n "${VOICETYPER_EXTRA_CMAKE_ARGS:-}" ]; then
    read -ra EXTRA_CMAKE_ARGS <<< "$VOICETYPER_EXTRA_CMAKE_ARGS"
fi

# Install layout (inside each package):
#   /usr/lib/voiceTyper/bin/voiceTyper           real binary
#   /usr/lib/voiceTyper/bin/commands.default.json
#   /usr/lib/voiceTyper/bin/qt.conf              points Qt to bundled libs/plugins
#   /usr/lib/voiceTyper/lib/libQt6*.so.*         bundled Qt 6.11.1 + ICU 73
#   /usr/lib/voiceTyper/plugins/                 bundled Qt plugins
#   /usr/bin/voiceTyper -> ../lib/voiceTyper/bin/voiceTyper
#
# Whisper models are NOT downloaded by the installer. Place a .bin model into
# /usr/lib/voiceTyper/bin/models/ manually, or use the app's model download UI.
PREFIX_DIR="usr/lib/${PROJECT_NAME}"

VARIANTS=("$@")
if [ ${#VARIANTS[@]} -eq 0 ]; then
    VARIANTS=(cpu vulkan cuda all)
fi

BUILD_DIR="${ROOT_DIR}/build"
mkdir -p "$BUILD_DIR"

cd "$ROOT_DIR"

# Qt installer kit — bundled into each package for correct audio capture.
# Default: ~/Qt/6.11.1/gcc_64. Override with QT_KIT= env var.
QT_KIT="${QT_KIT:-${HOME}/Qt/6.11.1/gcc_64}"
if [ ! -d "$QT_KIT" ]; then
    echo "ERROR: Qt kit not found at $QT_KIT" >&2
    echo "       Install Qt 6.11.1 via the Qt Online Installer or set QT_KIT=/path/to/kit" >&2
    exit 1
fi
echo "Bundling Qt from: $QT_KIT"

# Generate icons if missing.
if [ ! -f "${ROOT_DIR}/voicetyper_icon.png" ]; then
    echo "Generating application icons..."
    python3 "${ROOT_DIR}/generate_icon.py"
fi

# ---------------------------------------------------------------------------
# Derive runtime Depends: ldd the binary, then map every library that resolves
# to a system path (not glibc core) to its providing package via dpkg -S. The
# NVIDIA driver is intentionally excluded — its package name is version-pinned
# (libnvidia-compute-NNN) and must not be hard-coded.
# ---------------------------------------------------------------------------
derive_deps() {
    local pkgroot="$1"; shift
    local glibc_core='ld-linux|/libc\.so|/libm\.so|/libdl\.so|/libpthread|/librt\.so|/libresolv'
    {
        for f in "$@"; do
            ldd "$f" 2>/dev/null
        done
    } \
        | awk '{print $3}' \
        | grep -E '^/' \
        | grep -vF "$pkgroot/" \
        | grep -vE "$glibc_core" \
        | sort -u \
        | while read -r lib; do
            dpkg -S "$(readlink -f "$lib")" 2>/dev/null | cut -d: -f1
        done \
        | tr ',' '\n' \
        | sed 's/^ *//; s/ *$//' \
        | grep -vE 'libnvidia|nvidia-' \
        | sort -u \
        | paste -sd ',' - \
        | sed 's/,/, /g'
}

# ---------------------------------------------------------------------------
# Bundle Qt 6.11.1 libs and plugins into <pkgroot>/lib and <pkgroot>/plugins.
# The binary's RPATH ($ORIGIN/../lib) and a qt.conf file make Qt find them at
# runtime without touching the system Qt installation.
#
# Plugin RUNPATH is already $ORIGIN/../../lib in the installer kit — this
# resolves correctly to <prefix>/lib/ when plugins live under <prefix>/plugins/.
# ---------------------------------------------------------------------------
bundle_qt() {
    local pkgroot="$1"
    local libdir="${pkgroot}/lib"
    local plugdir="${pkgroot}/plugins"
    mkdir -p "$libdir" "$plugdir"

    echo "  Bundling Qt libs..."
    local qt_libs=(
        libQt6Core libQt6Gui libQt6Widgets libQt6Multimedia
        libQt6Network libQt6DBus libQt6Concurrent
        libQt6XcbQpa libQt6WaylandClient
        libicudata libicui18n libicuuc
    )
    for name in "${qt_libs[@]}"; do
        for f in "${QT_KIT}/lib/${name}.so".*; do
            [[ -e "$f" || -L "$f" ]] || continue
            [[ "$f" == *.debug ]] && continue
            cp -P "$f" "$libdir/"
        done
    done

    echo "  Bundling Qt plugins..."
    # Multimedia: FFmpeg backend (PipeWire/PulseAudio)
    mkdir -p "${plugdir}/multimedia"
    cp "${QT_KIT}/plugins/multimedia/libffmpegmediaplugin.so" "${plugdir}/multimedia/"

    # Platform plugins (xcb = X11/Xwayland, wayland = native Wayland)
    mkdir -p "${plugdir}/platforms"
    for name in libqxcb libqwayland; do
        local src="${QT_KIT}/plugins/platforms/${name}.so"
        [ -f "$src" ] && cp "$src" "${plugdir}/platforms/"
    done

    # XCB GL integrations
    mkdir -p "${plugdir}/xcbglintegrations"
    for f in "${QT_KIT}/plugins/xcbglintegrations/"*.so; do
        [ -e "$f" ] && cp "$f" "${plugdir}/xcbglintegrations/"
    done

    # Wayland shell integration
    mkdir -p "${plugdir}/wayland-shell-integration"
    for f in "${QT_KIT}/plugins/wayland-shell-integration/"*.so; do
        [ -e "$f" ] && cp "$f" "${plugdir}/wayland-shell-integration/"
    done

    # qt.conf: tells Qt to find its plugins and libs in the bundled locations
    cat > "${pkgroot}/bin/qt.conf" <<'QT_CONF_EOF'
[Paths]
Prefix = /usr/lib/voiceTyper
Plugins = plugins
Libraries = lib
QT_CONF_EOF
}

# ===========================================================================
# Per-variant build + package
# ===========================================================================
build_one() {
    local variant="$1"
    local with_cuda="OFF" with_vulkan="OFF"
    case "$variant" in
        cpu)    ;;
        vulkan) with_vulkan="ON" ;;
        cuda)   with_cuda="ON" ;;
        all)    with_cuda="ON"; with_vulkan="ON" ;;
        *) echo "ERROR: unknown variant '$variant' (use cpu|vulkan|cuda|all)" >&2; exit 1 ;;
    esac

    local pkg="${PROJECT_NAME}-${variant}"
    local build_dir="${ROOT_DIR}/build-deb-${variant}"
    local deb_dir="${ROOT_DIR}/deb-${variant}"
    local pkgroot="${deb_dir}/${PREFIX_DIR}"
    local out_deb="${BUILD_DIR}/${pkg}_${VERSION}_${ARCH}.deb"

    echo
    echo "==================================================================="
    echo "  Building ${pkg}  (cuda=${with_cuda} vulkan=${with_vulkan})"
    echo "==================================================================="

    # --- Configure + build --------------------------------------------------
    local cmake_args=(
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_PREFIX_PATH="$QT_KIT"
        -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib'
        -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
        -DVOICETYPER_WITH_CUDA="$with_cuda"
        -DVOICETYPER_WITH_VULKAN="$with_vulkan"
        "${EXTRA_CMAKE_ARGS[@]}"
    )

    cmake -S "$ROOT_DIR" -B "$build_dir" "${cmake_args[@]}"
    # Suppress "Clock skew detected" when cmake regenerates older timestamps
    touch "$ROOT_DIR/CMakeLists.txt"
    cmake --build "$build_dir" -j "$JOBS"

    # --- Lay out the package tree ------------------------------------------
    rm -rf "$deb_dir" "$out_deb"
    mkdir -p "$deb_dir/DEBIAN" "$deb_dir/usr/bin"
    mkdir -p "$deb_dir/usr/share/applications"

    # Install binary + default config into <prefix>/bin.
    cmake --install "$build_dir" --prefix "$pkgroot"
    local bin="${pkgroot}/bin/${PROJECT_NAME}"
    if [ ! -f "$bin" ]; then
        echo "ERROR: expected installed binary at $bin" >&2
        exit 1
    fi

    # --- Bundle Qt 6.11.1 -------------------------------------------------
    bundle_qt "$pkgroot"

    # --- /usr/bin launcher symlink -----------------------------------------
    ln -sf "../lib/${PROJECT_NAME}/bin/${PROJECT_NAME}" "$deb_dir/usr/bin/${PROJECT_NAME}"

    # --- Icons + desktop entry ---------------------------------------------
    for sz in 16 32 48 64 128 256; do
        mkdir -p "$deb_dir/usr/share/icons/hicolor/${sz}x${sz}/apps"
        cp "${ROOT_DIR}/voicetyper_${sz}x${sz}.png" \
           "$deb_dir/usr/share/icons/hicolor/${sz}x${sz}/apps/voicetyper.png"
    done
    cat > "$deb_dir/usr/share/applications/voicetyper.desktop" <<'DESKTOP_EOF'
[Desktop Entry]
Type=Application
Name=VoiceTyper
Comment=Local voice typing utility using Qt6 and whisper.cpp
Exec=/usr/bin/voiceTyper
Icon=voicetyper
Terminal=false
Categories=Utility;Accessibility;
Keywords=voice;typing;speech;transcription;
DESKTOP_EOF

    # --- Dependencies -------------------------------------------------------
    # Scan binary + bundled plugins; Qt libs in pkgroot/lib/ are filtered out
    # automatically (they start with $deb_dir/), leaving only system deps.
    local bundled_plugins=()
    while IFS= read -r -d '' f; do
        bundled_plugins+=("$f")
    done < <(find "${pkgroot}/plugins" -name "*.so" -print0 2>/dev/null)
    local deps
    deps="$(derive_deps "$deb_dir" "$bin" "${bundled_plugins[@]}")"
    # libqxcb.so (Qt 6.5+) dlopen's libxcb-cursor.so.0 at runtime — ldd misses it.
    case "$deps" in *libxcb-cursor0*) ;; *) deps="${deps:+$deps, }libxcb-cursor0" ;; esac

    if [ "$with_vulkan" = "ON" ]; then
        # The Vulkan loader must come from the system so it can find the GPU ICDs.
        case "$deps" in *libvulkan1*) ;; *) deps="${deps:+$deps, }libvulkan1" ;; esac
    fi
    if [ "$with_cuda" = "ON" ]; then
        # CUDA runtime libs must be installed from NVIDIA's apt repository.
        # derive_deps picks them up automatically via dpkg -S when CUDA is
        # deb-installed. Warn if they were not found (e.g. CUDA installed via
        # the .run installer without populating dpkg).
        if ! echo "$deps" | grep -qiE 'libcudart|libcublas|cuda'; then
            echo "WARNING: CUDA runtime packages not detected by dpkg -S." >&2
            echo "         Install CUDA from NVIDIA's apt repository so the" >&2
            echo "         Depends field is populated correctly:" >&2
            echo "           https://developer.nvidia.com/cuda-downloads" >&2
        fi
    fi
    echo "Computed Depends: ${deps}"

    local extra_desc=""
    case "$variant" in
        cpu)    extra_desc=" CPU-only build." ;;
        vulkan) extra_desc=" Vulkan GPU build; requires a Vulkan driver (e.g. mesa-vulkan-drivers or the vendor driver)." ;;
        cuda)   extra_desc=" CUDA GPU build; requires the CUDA runtime (libcudart-12-x, libcublas-12-x from NVIDIA's apt repo) and the NVIDIA driver (libcuda.so.1) on the host." ;;
        all)    extra_desc=" Universal build with CPU, Vulkan, and CUDA backends; requires a Vulkan driver, the CUDA runtime (libcudart-12-x, libcublas-12-x), and the NVIDIA driver (libcuda.so.1) on the host." ;;
    esac

    # --- Control file -------------------------------------------------------
    # Provides/Conflicts/Replaces the virtual 'voiceTyper' so only one backend
    # variant can be installed at a time (all ship /usr/bin/voiceTyper).
    local installed_size
    installed_size="$(du -sk "$deb_dir/usr" | cut -f1)"
    cat > "$deb_dir/DEBIAN/control" <<EOF
Package: ${pkg}
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${installed_size}
Depends: ${deps}
Provides: ${PROJECT_NAME}
Conflicts: ${PROJECT_NAME}
Replaces: ${PROJECT_NAME}
Maintainer: VoiceTyper Team
Description: Local voice typing utility using Qt6 and whisper.cpp (${variant})
 A desktop application for voice-to-text transcription with local processing.
 Features global hotkey support, command detection, and offline transcription.
 Qt 6.11.1 is bundled; GPU runtimes are system dependencies.${extra_desc}
EOF

    # --- postinst: refresh icon cache after install ------------------------
    cat > "$deb_dir/DEBIAN/postinst" <<'POSTINST_EOF'
#!/bin/bash
set -e
if [ -x /usr/bin/gtk-update-icon-cache ]; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi
exit 0
POSTINST_EOF
    chmod 755 "$deb_dir/DEBIAN/postinst"

    # --- Build the .deb -----------------------------------------------------
    dpkg-deb --root-owner-group --build "$deb_dir" "$out_deb"
    rm -rf "$deb_dir"
    echo "Created: ${out_deb}"
}

for v in "${VARIANTS[@]}"; do
    build_one "$v"
done

echo
echo "Done. Packages:"
for v in "${VARIANTS[@]}"; do
    echo "  ${BUILD_DIR}/${PROJECT_NAME}-${v}_${VERSION}_${ARCH}.deb"
done
echo
echo "Install (one variant at a time):  sudo apt install ./${PROJECT_NAME}-cpu_${VERSION}_${ARCH}.deb"
echo "Place a Whisper model (.bin) into /usr/lib/${PROJECT_NAME}/bin/models/ before first run."

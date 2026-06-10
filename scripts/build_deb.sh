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
# Qt 6, CUDA, and Vulkan runtimes are NOT bundled — they are declared as apt
# dependencies and pulled in at install time. The NVIDIA driver (libcuda.so.1)
# must be present on the host for CUDA variants.
#
# IMPORTANT: build against system Qt packages (not the Qt Online Installer kit).
# With a private kit in ~/Qt/ the libraries are unknown to dpkg, so derive_deps
# cannot map them to package names and Depends: will be wrong.
#
# Usage:
#   scripts/build_deb.sh [variant ...]      # default: cpu vulkan cuda all
#   QT_PREFIX=/usr scripts/build_deb.sh cpu # explicit system-Qt prefix
#
# Build deps (Ubuntu/Debian):
#   sudo apt install build-essential cmake git \
#       qt6-base-dev qt6-multimedia-dev libqt6widgets6-dev \
#       libx11-dev libxtst-dev libxcb1-dev libasound2-dev libpulse-dev
#   Vulkan variant also needs: libvulkan-dev glslc (glslang-tools / shaderc)
#   CUDA variant also needs:   the CUDA toolkit (nvcc, from NVIDIA's apt repo)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PROJECT_NAME="voiceTyper"
VERSION="0.1.0"
ARCH="amd64"
JOBS="$(nproc 2>/dev/null || echo 4)"

# Install layout (inside each package):
#   /usr/lib/voiceTyper/bin/voiceTyper        real binary
#   /usr/lib/voiceTyper/bin/commands.default.json
#   /usr/bin/voiceTyper -> ../lib/voiceTyper/bin/voiceTyper
#
# Whisper models are NOT downloaded by the installer. Place a .bin model into
# /usr/lib/voiceTyper/bin/models/ manually, or use the app's model download UI.
PREFIX_DIR="usr/lib/${PROJECT_NAME}"

VARIANTS=("$@")
if [ ${#VARIANTS[@]} -eq 0 ]; then
    VARIANTS=(cpu vulkan cuda all)
fi

cd "$ROOT_DIR"

# Optional cmake prefix for Qt. Leave unset to let cmake find system Qt.
# Must point to a dpkg-managed installation (e.g. /usr) so that derive_deps
# can resolve Qt library paths back to package names via dpkg -S.
QT_PREFIX="${QT_PREFIX:-}"
if [ -n "$QT_PREFIX" ]; then
    echo "Using Qt prefix: $QT_PREFIX"
    if [[ "$QT_PREFIX" != /usr* ]]; then
        echo "WARNING: QT_PREFIX '$QT_PREFIX' is not a system path." >&2
        echo "         Qt libs will NOT appear in Depends: because dpkg does" >&2
        echo "         not know about them. Use system Qt (apt install qt6-base-dev)." >&2
    fi
else
    echo "Using system Qt (QT_PREFIX not set)"
fi

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
    local out_deb="${ROOT_DIR}/${pkg}_${VERSION}_${ARCH}.deb"

    echo
    echo "==================================================================="
    echo "  Building ${pkg}  (cuda=${with_cuda} vulkan=${with_vulkan})"
    echo "==================================================================="

    # --- Configure + build --------------------------------------------------
    local cmake_args=(
        -DCMAKE_BUILD_TYPE=Release
        -DVOICETYPER_WITH_CUDA="$with_cuda"
        -DVOICETYPER_WITH_VULKAN="$with_vulkan"
    )
    [ -n "$QT_PREFIX" ] && cmake_args+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")

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
    local deps
    deps="$(derive_deps "$deb_dir" "$bin")"
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
 Qt 6 and GPU runtimes are system dependencies (not bundled).${extra_desc}
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
    echo "  ${ROOT_DIR}/${PROJECT_NAME}-${v}_${VERSION}_${ARCH}.deb"
done
echo
echo "Install (one variant at a time):  sudo apt install ./${PROJECT_NAME}-cpu_${VERSION}_${ARCH}.deb"
echo "Place a Whisper model (.bin) into /usr/lib/${PROJECT_NAME}/bin/models/ before first run."

#!/usr/bin/env bash
#
# Builds self-contained .deb packages for voiceTyper, one per compute backend:
#   voiceTyper-cpu     CPU only
#   voiceTyper-vulkan  CPU + Vulkan
#   voiceTyper-cuda    CPU + CUDA
#
# Why separate packages: whisper.cpp's GPU backends are linked into the binary,
# and CUDA in particular becomes a hard launch dependency (DT_NEEDED on
# libcudart/libcublas/libcuda). A single "universal" build therefore refuses to
# start on any machine without the CUDA runtime + NVIDIA driver — even for CPU
# or Vulkan users. Splitting keeps each package runnable on its target.
#
# Each package bundles the Qt 6 runtime (libs + the xcb/tls/ffmpeg plugins)
# under /usr/lib/voiceTyper, so it runs without Qt installed. The cuda package
# additionally bundles libcudart/libcublas/libcublasLt; the NVIDIA *driver*
# (libcuda.so.1) is still required on the host.
#
# Usage:
#   scripts/build_deb.sh [variant ...]      # default: cpu vulkan cuda
#   QT_PREFIX=~/Qt/6.11.1/gcc_64 scripts/build_deb.sh cpu
#
# Build deps (Ubuntu/Debian):
#   sudo apt install build-essential cmake git \
#       libx11-dev libxtst-dev libxcb1-dev libasound2-dev libpulse-dev
#   Vulkan package also needs: libvulkan-dev glslc (glslang-tools / shaderc)
#   CUDA package also needs:   the CUDA toolkit (nvcc)
#   plus a Qt 6 kit (online installer ~/Qt/<ver>/gcc_64, or set QT_PREFIX).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PROJECT_NAME="voiceTyper"
VERSION="0.1.0"
ARCH="amd64"
JOBS="$(nproc 2>/dev/null || echo 4)"

DEFAULT_MODEL="ggml-large-v3-q5_0.bin"
MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${DEFAULT_MODEL}"

# Install layout (inside each package):
#   /usr/lib/voiceTyper/bin/voiceTyper      real binary (RPATH=$ORIGIN/../lib)
#   /usr/lib/voiceTyper/bin/commands.default.json
#   /usr/lib/voiceTyper/bin/qt.conf
#   /usr/lib/voiceTyper/bin/models/         model downloaded here by postinst
#   /usr/lib/voiceTyper/lib/*.so*           bundled Qt/ffmpeg/(cuda) libraries
#   /usr/lib/voiceTyper/plugins/<group>/    bundled Qt plugins
#   /usr/bin/voiceTyper -> ../lib/voiceTyper/bin/voiceTyper
#
# The model + config live next to the binary because the app autodetects models
# in applicationDirPath()/models and loads commands.default.json from
# applicationDirPath() (see SettingsStore.cpp). /proc/self/exe resolves the
# /usr/bin symlink to the real bin dir, so autodetection works.
PREFIX_DIR="usr/lib/${PROJECT_NAME}"
MODELS_SUBDIR="bin/models"

VARIANTS=("$@")
if [ ${#VARIANTS[@]} -eq 0 ]; then
    VARIANTS=(cpu vulkan cuda)
fi

cd "$ROOT_DIR"

# ---------------------------------------------------------------------------
# Locate the Qt kit (same heuristic as scripts/build-linux.sh).
# ---------------------------------------------------------------------------
QT_PREFIX="${QT_PREFIX:-}"
if [ -z "$QT_PREFIX" ]; then
    QT_PREFIX="$(ls -d "$HOME"/Qt/*/gcc_64 2>/dev/null | sort -V | tail -n1 || true)"
fi
if [ -z "$QT_PREFIX" ] || [ ! -d "$QT_PREFIX/lib" ]; then
    echo "ERROR: could not find a Qt kit. Set QT_PREFIX=/path/to/Qt/<ver>/gcc_64" >&2
    exit 1
fi
QT_LIB="${QT_PREFIX}/lib"
QT_PLUGINS="${QT_PREFIX}/plugins"
echo "Using Qt kit: $QT_PREFIX"

# Qt plugins to bundle: "group/file.so". xcb = the platform plugin (GUI),
# tls = HTTPS for the optional HTTP post-processor, multimedia = the FFmpeg
# media integration that QMediaDevices/QAudioSource require for audio capture.
QT_PLUGIN_FILES=(
    "platforms/libqxcb.so"
    "xcbglintegrations/libqxcb-glx-integration.so"
    "tls/libqopensslbackend.so"
    "tls/libqcertonlybackend.so"
    "multimedia/libffmpegmediaplugin.so"
)

# Generate icons if missing.
if [ ! -f "${ROOT_DIR}/voicetyper_icon.png" ]; then
    echo "Generating application icons..."
    python3 "${ROOT_DIR}/generate_icon.py"
fi

# ---------------------------------------------------------------------------
# Copy every transitive shared-lib dependency of $1 that lives inside the Qt
# kit into $2. ldd already resolves transitively; LD_LIBRARY_PATH=$QT_LIB makes
# it resolve against the kit even for an already-RPATH-rewritten binary.
# ---------------------------------------------------------------------------
copy_kit_libs() {
    local elf="$1" dest="$2"
    LD_LIBRARY_PATH="$QT_LIB" ldd "$elf" 2>/dev/null \
        | awk '{print $3}' \
        | grep "^${QT_PREFIX}/" \
        | sort -u \
        | while read -r p; do
            cp -uL "$p" "$dest/"
        done
}

# ---------------------------------------------------------------------------
# Derive runtime Depends: ldd the binary + bundled plugins against the bundle's
# lib dir, then map every library that still resolves to a *system* path (i.e.
# not bundled, not the glibc core) to its providing package via dpkg -S. The
# NVIDIA driver is intentionally excluded — its package name is version-pinned
# (libnvidia-compute-NNN) and must not be hard-coded.
# ---------------------------------------------------------------------------
derive_deps() {
    local pkgroot="$1"; shift
    local bundle_lib="${pkgroot}/${PREFIX_DIR}/lib"
    local glibc_core='ld-linux|/libc\.so|/libm\.so|/libdl\.so|/libpthread|/librt\.so|/libresolv'
    {
        for f in "$@"; do
            LD_LIBRARY_PATH="$bundle_lib" ldd "$f" 2>/dev/null
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
        *) echo "ERROR: unknown variant '$variant' (use cpu|vulkan|cuda)" >&2; exit 1 ;;
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

    # --- Configure + build (clean, explicit backend flags) -----------------
    # RPATH=$ORIGIN/../lib with old dtags (DT_RPATH) so the executable's rpath
    # also resolves the dlopened plugins' transitive deps (libav*, libQt6*).
    cmake -S "$ROOT_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DVOICETYPER_WITH_CUDA="$with_cuda" \
        -DVOICETYPER_WITH_VULKAN="$with_vulkan" \
        -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib' \
        -DCMAKE_EXE_LINKER_FLAGS='-Wl,--disable-new-dtags'
    # Suppress "Clock skew detected" when cmake regenerates older timestamps
    touch "$ROOT_DIR/CMakeLists.txt"
    cmake --build "$build_dir" -j "$JOBS"

    # --- Lay out the package tree ------------------------------------------
    rm -rf "$deb_dir" "$out_deb"
    mkdir -p "$pkgroot/lib" "$pkgroot/plugins"
    mkdir -p "$deb_dir/DEBIAN" "$deb_dir/usr/bin"
    mkdir -p "$deb_dir/usr/share/applications"

    # Install binary + default config into <prefix>/bin and rewrite RPATH.
    cmake --install "$build_dir" --prefix "$pkgroot"
    local bin="${pkgroot}/bin/${PROJECT_NAME}"
    if [ ! -f "$bin" ]; then
        echo "ERROR: expected installed binary at $bin" >&2
        exit 1
    fi
    mkdir -p "${pkgroot}/${MODELS_SUBDIR}"

    # --- Bundle Qt runtime --------------------------------------------------
    copy_kit_libs "$bin" "$pkgroot/lib"
    local plugin_paths=()
    for rel in "${QT_PLUGIN_FILES[@]}"; do
        local src="${QT_PLUGINS}/${rel}"
        if [ ! -f "$src" ]; then
            echo "WARNING: Qt plugin not found, skipping: $src" >&2
            continue
        fi
        mkdir -p "$pkgroot/plugins/$(dirname "$rel")"
        cp -uL "$src" "$pkgroot/plugins/${rel}"
        copy_kit_libs "$src" "$pkgroot/lib"
        plugin_paths+=("$pkgroot/plugins/${rel}")
    done

    # --- Bundle CUDA runtime libs (driver excluded) ------------------------
    if [ "$with_cuda" = "ON" ]; then
        echo "Bundling CUDA runtime libraries..."
        for soname in libcudart.so.12 libcublas.so.12 libcublasLt.so.12; do
            local p
            p="$(LD_LIBRARY_PATH="$QT_LIB" ldd "$bin" 2>/dev/null \
                 | awk -v s="$soname" '$1==s {print $3}')"
            if [ -z "$p" ]; then
                echo "ERROR: $soname not found in the binary's deps — is the CUDA runtime installed?" >&2
                exit 1
            fi
            cp -uL "$p" "$pkgroot/lib/"
        done
    fi

    # --- qt.conf so Qt finds the bundled plugins ---------------------------
    cat > "${pkgroot}/bin/qt.conf" <<'QTCONF'
[Paths]
Prefix = ..
Libraries = lib
Plugins = plugins
QTCONF

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
    deps="$(derive_deps "$deb_dir" "$bin" "${plugin_paths[@]}")"
    if [ "$with_vulkan" = "ON" ]; then
        # The Vulkan loader must come from the system so it can find the GPU ICDs.
        case "$deps" in *libvulkan1*) ;; *) deps="${deps:+$deps, }libvulkan1" ;; esac
    fi
    echo "Computed Depends: ${deps}"

    local extra_desc=""
    case "$variant" in
        cpu)    extra_desc=" CPU-only build." ;;
        vulkan) extra_desc=" Vulkan GPU build; requires a Vulkan driver (e.g. mesa-vulkan-drivers or the vendor driver)." ;;
        cuda)   extra_desc=" CUDA GPU build; bundles the CUDA runtime but requires the NVIDIA driver (libcuda.so.1) on the host." ;;
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
 Qt 6 is bundled inside the package.${extra_desc}
EOF

    # --- postinst: fetch the default model where the app autodetects it -----
    cat > "$deb_dir/DEBIAN/postinst" <<POSTINST_EOF
#!/bin/bash
set -e

if [ -x /usr/bin/gtk-update-icon-cache ]; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi

MODELS_DIR="/${PREFIX_DIR}/${MODELS_SUBDIR}"
MODEL_PATH="\${MODELS_DIR}/${DEFAULT_MODEL}"
MODEL_URL="${MODEL_URL}"

mkdir -p "\${MODELS_DIR}"

if [ ! -f "\${MODEL_PATH}" ]; then
    echo "Downloading default Whisper model (${DEFAULT_MODEL}, ~1.1 GB)..."
    if command -v wget >/dev/null 2>&1; then
        wget -O "\${MODEL_PATH}" "\${MODEL_URL}" || {
            rm -f "\${MODEL_PATH}"
            echo "Model download failed. Fetch it later with:"
            echo "  sudo wget -O \${MODEL_PATH} \${MODEL_URL}"
        }
    elif command -v curl >/dev/null 2>&1; then
        curl -L -o "\${MODEL_PATH}" "\${MODEL_URL}" || {
            rm -f "\${MODEL_PATH}"
            echo "Model download failed. Fetch it later with:"
            echo "  sudo curl -L -o \${MODEL_PATH} \${MODEL_URL}"
        }
    else
        echo "Neither wget nor curl found; download a model into \${MODELS_DIR} manually."
    fi
    [ -f "\${MODEL_PATH}" ] && chmod 644 "\${MODEL_PATH}"
fi

exit 0
POSTINST_EOF
    chmod 755 "$deb_dir/DEBIAN/postinst"

    # --- postrm: drop the downloaded model on purge ------------------------
    cat > "$deb_dir/DEBIAN/postrm" <<POSTRM_EOF
#!/bin/bash
set -e
if [ "\$1" = "purge" ]; then
    rm -rf "/${PREFIX_DIR}/${MODELS_SUBDIR}"
fi
exit 0
POSTRM_EOF
    chmod 755 "$deb_dir/DEBIAN/postrm"

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
echo "The package downloads ${DEFAULT_MODEL} (~1.1 GB) on install."

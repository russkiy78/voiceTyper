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
# start on any machine without the NVIDIA driver — even for CPU or Vulkan
# users. Splitting keeps each package runnable on its target.
#
# Qt 6.11.1 is BUNDLED (from the Qt Online Installer kit) rather than declared
# as an apt dependency. Ubuntu 24.04 ships Qt 6.4.2 whose GStreamer multimedia
# backend silently fails to capture audio on PipeWire. Qt 6.11.1 uses the
# FFmpeg backend with native PipeWire/PulseAudio support that works correctly.
# The FFmpeg multimedia plugin and platform plugins (xcb, wayland) are bundled
# under /usr/lib/voiceTyper/ together with every kit library they need (Qt
# Quick/Qml for the FFmpeg plugin, the kit's FFmpeg and ICU builds, ...).
# Depends: lists only what those files load from the system, as computed by
# dpkg-shlibdeps, so the package installs on Ubuntu 24.04 and newer.
#
# The CUDA runtime (libcudart/libcublas/libcublasLt/...) is likewise BUNDLED
# for the cuda/all variants, same as Qt — the target does NOT need a matching
# CUDA toolkit installed, and it will not conflict with a newer toolkit
# already on the host (e.g. CUDA 13.x) since RPATH resolves the bundled 12.x
# copies first. The one CUDA-related lib that stays a host dependency is
# libcuda.so.1 itself: it's the NVIDIA driver's own library, tied to the
# exact driver version installed, so it is intentionally never bundled — an
# NVIDIA driver is still required on the host for cuda/all.
#
# Usage:
#   scripts/build_deb.sh [variant ...]      # default: cpu vulkan cuda all
#   QT_KIT=~/Qt/6.11.1/gcc_64 scripts/build_deb.sh cpu
#   VOICETYPER_VERSION=0.3.42 scripts/build_deb.sh vulkan   # override the auto-derived version (CI)
#   VOICETYPER_EXTRA_CMAKE_ARGS="-DCMAKE_CXX_COMPILER_LAUNCHER=sccache" scripts/build_deb.sh
#
# Build deps (Ubuntu/Debian):
#   sudo apt install build-essential dpkg-dev cmake git \
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
#   /usr/lib/voiceTyper/lib/libQt6*.so.*         bundled Qt 6.11.1 + ICU + FFmpeg
#   /usr/lib/voiceTyper/lib/libcudart.so.*, etc  bundled CUDA runtime (cuda/all only)
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
# Derive runtime Depends with dpkg-shlibdeps over every shipped ELF (binary,
# plugins, bundled libs). It maps each file's *direct* DT_NEEDED libraries to
# their packages, with minimum versions, and treats <pkgroot>/lib as private.
#
# The previous scan took the whole ldd closure of the build machine instead,
# which dragged in transitive, release-specific packages - e.g. libflac12t64
# (via libpulse -> libsndfile) exists on Ubuntu 24.04 but not 26.04 (libflac14),
# making the .deb uninstallable there.
#
# The NVIDIA driver is intentionally excluded - its package name is pinned to
# the driver version (libnvidia-compute-NNN). On a build machine without the
# driver (CI), CUDA_STUB (set by bundle_cuda) stands in for libcuda.so.1 as a
# private lib, since dpkg-shlibdeps refuses to continue on a missing library.
# ---------------------------------------------------------------------------
CUDA_STUB=""

derive_deps() {
    local pkgroot="$1"; shift
    local tmp
    tmp="$(mktemp -d)"
    # dpkg-shlibdeps insists on a debian/control; its content is irrelevant.
    mkdir -p "$tmp/debian" "$tmp/stubs"
    printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' \
        "$PROJECT_NAME" "$PROJECT_NAME" > "$tmp/debian/control"
    if [ -n "$CUDA_STUB" ]; then
        ln -s "$CUDA_STUB" "$tmp/stubs/libcuda.so.1"
    fi

    local out
    if ! out="$(cd "$tmp" && dpkg-shlibdeps -O --ignore-missing-info \
            -l"$pkgroot/lib" -l"$tmp/stubs" -e "$@" 2>"$tmp/stderr")"; then
        cat "$tmp/stderr" >&2
        rm -rf "$tmp"
        echo "ERROR: dpkg-shlibdeps could not derive Depends" >&2
        exit 1
    fi
    rm -rf "$tmp"

    # `|| true`: under pipefail, grep's "no match" exit code would otherwise
    # abort the script silently when nothing is filtered out.
    echo "$out" \
        | sed -n 's/^shlibs:Depends=//p' \
        | tr ',' '\n' \
        | sed 's/^ *//; s/ *$//' \
        | (grep -vE 'libnvidia|nvidia-' || true) \
        | paste -sd ',' - \
        | sed 's/,/, /g'
}

# ---------------------------------------------------------------------------
# Bundle Qt plugins into <pkgroot>/plugins, then every library from the kit
# that the binary and those plugins need into <pkgroot>/lib. The binary's
# RPATH ($ORIGIN/../lib) and a qt.conf file make Qt find them at runtime
# without touching the system Qt installation.
#
# The library set follows DT_NEEDED instead of a hand-written list: the FFmpeg
# multimedia plugin alone needs Qt Quick/Qml/OpenGL, the kit's own FFmpeg
# (libavcodec.so.61, ...) and its libQt6FFmpegStub-* shims. A fixed list
# missed all of them, so the plugin failed to load on a clean system (no audio
# capture) or picked up a mismatched system Qt Quick.
#
# Plugin RUNPATH is already $ORIGIN/../../lib in the installer kit — this
# resolves correctly to <prefix>/lib/ when plugins live under <prefix>/plugins/.
# ---------------------------------------------------------------------------
bundle_qt() {
    local pkgroot="$1"
    local libdir="${pkgroot}/lib"
    local plugdir="${pkgroot}/plugins"
    mkdir -p "$libdir" "$plugdir"

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

    echo "  Bundling Qt libs..."
    # Breadth-first over DT_NEEDED: whatever the kit ships is copied under its
    # soname (symlinks resolved) and scanned in turn; everything else has to
    # come from the system and ends up in Depends via derive_deps().
    local queue=("${pkgroot}/bin/${PROJECT_NAME}")
    while IFS= read -r -d '' f; do
        queue+=("$f")
    done < <(find "$plugdir" -name '*.so' -print0)
    while [ ${#queue[@]} -gt 0 ]; do
        local elf="${queue[0]}" soname
        queue=("${queue[@]:1}")
        while read -r soname; do
            [ -e "${libdir}/${soname}" ] && continue
            [ -e "${QT_KIT}/lib/${soname}" ] || continue
            cp -L "${QT_KIT}/lib/${soname}" "${libdir}/${soname}"
            queue+=("${libdir}/${soname}")
        done < <(readelf -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    done

    # qt.conf: tells Qt to find its plugins and libs in the bundled locations
    cat > "${pkgroot}/bin/qt.conf" <<'QT_CONF_EOF'
[Paths]
Prefix = /usr/lib/voiceTyper
Plugins = plugins
Libraries = lib
QT_CONF_EOF
}

# ---------------------------------------------------------------------------
# Bundle the CUDA runtime libs the binary actually links against (cudart,
# cublas, cublasLt, nvJitLink, ...) into <pkgroot>/lib, alongside Qt.
#
# Run this BEFORE derive_deps(): once these libs live under pkgroot/lib,
# ldd resolves them there (via the $ORIGIN/../lib RPATH) and derive_deps's
# existing "starts with pkgroot"-prefix filter drops them from Depends
# automatically — exactly like the bundled Qt libs, no separate exclusion
# logic needed.
#
# libcuda.so.1 (the NVIDIA driver's own lib, not part of the CUDA toolkit)
# is deliberately EXCLUDED — it must match whatever driver is installed on
# the target, so it has to stay a real runtime dependency resolved from the
# host, never bundled.
# ---------------------------------------------------------------------------
bundle_cuda() {
    local pkgroot="$1" bin="$2"
    local libdir="${pkgroot}/lib"
    mkdir -p "$libdir"

    echo "  Bundling CUDA runtime libs..."
    local found=0
    while read -r soname resolved; do
        [ -z "${resolved:-}" ] && continue
        case "$soname" in
            libcuda.so*) continue ;;               # driver stub: must come from the host driver
            libcu*|libnvJitLink*|libnvrtc*) ;;      # CUDA toolkit runtime libs: bundle
            *) continue ;;
        esac
        cp -L "$resolved" "${libdir}/${soname}"
        found=1
        # The toolkit's link stub of the driver lib, for derive_deps().
        local stub
        stub="$(dirname "$(readlink -f "$resolved")")/stubs/libcuda.so"
        [ -e "$stub" ] && CUDA_STUB="$stub"
    done < <(ldd "$bin" 2>/dev/null | awk '{print $1, $3}')

    if [ "$found" -eq 0 ]; then
        echo "ERROR: CUDA was requested but no CUDA runtime libs (libcudart/" >&2
        echo "       libcublas/...) were resolved for $bin - is the CUDA" >&2
        echo "       toolkit that built this binary still on the loader path?" >&2
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Fail the build if a shipped ELF can't resolve a library, or takes a Qt or
# FFmpeg library from outside the package: that would be whatever Qt the build
# machine happens to have (a mismatched build on the user's system, or nothing
# at all on a clean one). libcuda.so.1 is exempt - it is the host's NVIDIA
# driver and absent on CI.
# ---------------------------------------------------------------------------
verify_bundle() {
    local pkgroot="$1" bad=0 elf line
    while IFS= read -r -d '' elf; do
        readelf -h "$elf" >/dev/null 2>&1 || continue
        while IFS= read -r line; do
            case "$line" in
                *libcuda.so.1*) ;;
                *"not found"*)
                    echo "ERROR: ${elf#"$pkgroot"/}:${line}" >&2
                    bad=1 ;;
                *libQt6*|*libavcodec*|*libavformat*|*libavutil*|*libswresample*|*libswscale*)
                    case "$line" in
                        *"=> ${pkgroot}/"*) ;;
                        *) echo "ERROR: ${elf#"$pkgroot"/} loads a library from outside the package:${line}" >&2
                           bad=1 ;;
                    esac ;;
            esac
        done < <(ldd "$elf" 2>/dev/null)
    done < <(find "$pkgroot" -type f -print0)
    if [ "$bad" -ne 0 ]; then
        echo "ERROR: the package would not run on a clean system (see above)" >&2
        exit 1
    fi
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
    # cmake --install also installs whisper.cpp's static libs, headers and
    # CMake/pkg-config files; none of it is needed at runtime.
    rm -rf "${pkgroot}/include" "${pkgroot}/lib/cmake" "${pkgroot}/lib/pkgconfig"
    find "${pkgroot}/lib" -name '*.a' -delete

    # --- Bundle Qt 6.11.1 -------------------------------------------------
    bundle_qt "$pkgroot"

    # --- Bundle CUDA runtime (cuda/all only) --------------------------------
    if [ "$with_cuda" = "ON" ]; then
        bundle_cuda "$pkgroot" "$bin"
    fi

    # --- /usr/bin launcher symlink -----------------------------------------
    ln -sf "../lib/${PROJECT_NAME}/bin/${PROJECT_NAME}" "$deb_dir/usr/bin/${PROJECT_NAME}"

    # --- Icons + desktop entry ---------------------------------------------
    for sz in 16 32 48 64 128 256; do
        mkdir -p "$deb_dir/usr/share/icons/hicolor/${sz}x${sz}/apps"
        cp "${ROOT_DIR}/voicetyper_${sz}x${sz}.png" \
           "$deb_dir/usr/share/icons/hicolor/${sz}x${sz}/apps/voicetyper.png"
    done
    # Named after the app id the XDG portal knows us by (src/core/XdgPortal.h):
    # GNOME only grants Wayland global shortcuts to an id backed by a .desktop.
    cat > "$deb_dir/usr/share/applications/io.github.russkiy78.voiceTyper.desktop" <<'DESKTOP_EOF'
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

    verify_bundle "$pkgroot"

    # --- Dependencies -------------------------------------------------------
    # Every shipped ELF: bundled libs count too, their system deps are ours.
    local elfs=()
    while IFS= read -r -d '' f; do
        readelf -h "$f" >/dev/null 2>&1 && elfs+=("$f")
    done < <(find "$pkgroot" -type f -print0)
    local deps
    deps="$(derive_deps "$pkgroot" "${elfs[@]}")"
    # libqxcb.so (Qt 6.5+) dlopen's libxcb-cursor.so.0 at runtime — ldd misses it.
    case "$deps" in *libxcb-cursor0*) ;; *) deps="${deps:+$deps, }libxcb-cursor0" ;; esac
    # The kit's FFmpeg needs libbz2.so.1, which Ubuntu only ships as a compat
    # symlink to libbz2.so.1.0 - dpkg-shlibdeps can't map that to a package.
    case "$deps" in *libbz2-1.0*) ;; *) deps="${deps:+$deps, }libbz2-1.0" ;; esac

    if [ "$with_vulkan" = "ON" ]; then
        # The Vulkan loader must come from the system so it can find the GPU ICDs.
        case "$deps" in *libvulkan1*) ;; *) deps="${deps:+$deps, }libvulkan1" ;; esac
    fi
    echo "Computed Depends: ${deps}"

    local extra_desc=""
    case "$variant" in
        cpu)    extra_desc=" CPU-only build." ;;
        vulkan) extra_desc=" Vulkan GPU build; requires a Vulkan driver (e.g. mesa-vulkan-drivers or the vendor driver) on the host." ;;
        cuda)   extra_desc=" CUDA GPU build; CUDA runtime bundled — requires only the NVIDIA driver (libcuda.so.1) on the host." ;;
        all)    extra_desc=" Universal build with CPU, Vulkan, and CUDA backends; CUDA runtime bundled — requires a Vulkan driver and the NVIDIA driver (libcuda.so.1) on the host." ;;
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
 Qt 6.11.1 is bundled.${extra_desc}
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

#!/usr/bin/env bash
#
# Builds a distributable voiceTyper.app and packages it into a .dmg for
# direct distribution outside the Mac App Store.
#
# Usage:
#   scripts/package-macos.sh
#   QT_DIR=~/Qt/6.11.0/macos scripts/package-macos.sh
#   CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" scripts/package-macos.sh
#
# Signing:
#   Without a Developer ID, this ad-hoc signs the app (default — no setup
#   needed). Anyone you send it to will see Gatekeeper's "Apple could not
#   verify..." warning on first launch and need to right-click > Open (or
#   System Settings > Privacy & Security > Open Anyway). That's normal for
#   unsigned indie distribution and doesn't block the app from running.
#
#   With a paid Apple Developer Program membership ($99/yr), set
#   CODESIGN_IDENTITY to your "Developer ID Application: ..." identity (list
#   available identities with: security find-identity -v -p codesigning) and
#   this script will also attempt notarization if APPLE_ID, APPLE_TEAM_ID,
#   and APPLE_APP_PASSWORD (an app-specific password from
#   appleid.apple.com > Sign-In and Security > App-Specific Passwords, NOT
#   your Apple ID password) are set in the environment. A notarized, stapled
#   app launches with no Gatekeeper friction at all.
#
#   This identity is independent of the "voiceTyper Dev" self-signed
#   certificate CMakeLists.txt uses for local dev builds (that one only
#   keeps Accessibility permission grants stable on YOUR machine across
#   rebuilds — it means nothing to anyone else's Mac and must never be used
#   for anything you hand out).
#
# Model: bundles the small-q5_1 model (~180 MB) by default so the app works
# out of the box. Set VT_PACKAGE_MODEL=none to ship without one (smaller
# download; first run then prompts the user to pick a model in Settings), or
# VT_PACKAGE_MODEL=<name> for a different one (see download-model.sh).
#
# Requires: cmake, Xcode Command Line Tools (codesign, iconutil, hdiutil),
# Qt6, and python3+Pillow for icon generation (skipped gracefully if
# unavailable — the .app just ships without a custom icon).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

PROJECT_NAME="voiceTyper"
BUNDLE_ID="com.voicetyper.voiceTyper"
VERSION="$(grep -A2 '^project(' CMakeLists.txt | awk '/VERSION/{print $2}')"
COMMIT_COUNT="$(git rev-list --count HEAD 2>/dev/null || echo 0)"
FULL_VERSION="${VERSION%.*}.${COMMIT_COUNT}"   # matches VT_VERSION in CMakeLists.txt
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

QT_DIR="${QT_DIR:-${HOME}/Qt/6.11.0/macos}"
if [ ! -d "$QT_DIR" ]; then
    echo "ERROR: Qt kit not found at $QT_DIR" >&2
    echo "       Install Qt 6.11+ via the Qt Online Installer or set QT_DIR=/path/to/kit" >&2
    exit 1
fi
echo "Using Qt from: $QT_DIR"

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"   # "-" = ad-hoc
VT_PACKAGE_MODEL="${VT_PACKAGE_MODEL:-small-q5_1}"

BUILD_DIR="${ROOT_DIR}/build-package"
STAGE_DIR="${ROOT_DIR}/dist-macos"
APP_DIR="${STAGE_DIR}/${PROJECT_NAME}.app"
OUT_DMG="${ROOT_DIR}/${PROJECT_NAME}-${FULL_VERSION}.dmg"

echo
echo "==================================================================="
echo "  Packaging ${PROJECT_NAME} ${FULL_VERSION}"
echo "==================================================================="

# ---------------------------------------------------------------------------
# 1. Build (Release)
# ---------------------------------------------------------------------------
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_DIR"
cmake --build "$BUILD_DIR" -j "$JOBS"

BIN="${BUILD_DIR}/voiceTyper"
if [ ! -f "$BIN" ]; then
    echo "ERROR: expected built binary at $BIN" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Lay out a clean .app bundle (independent of the dev-only bundle that
#    CMakeLists.txt's POST_BUILD step maintains under build/ for local
#    Accessibility-permission stability).
# ---------------------------------------------------------------------------
rm -rf "$STAGE_DIR" "$OUT_DMG"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"

cp "$BIN" "$APP_DIR/Contents/MacOS/${PROJECT_NAME}"
# Resources/, not MacOS/: codesign expects Contents/MacOS/ to hold only
# executables and rejects the bundle if data files sit there too (see
# SettingsStore::bundledDefaultCommandsPath(), which knows to look here).
cp "$ROOT_DIR/config/commands.default.json" "$APP_DIR/Contents/Resources/commands.default.json"

# ---------------------------------------------------------------------------
# 3. App icon (.icns), generated from generate_icon.py if Pillow is available.
# ---------------------------------------------------------------------------
ICONSET="${BUILD_DIR}/AppIcon.iconset"
if python3 -c "import PIL" >/dev/null 2>&1; then
    echo "Generating app icon..."
    rm -rf "$ICONSET"
    mkdir -p "$ICONSET"
    python3 - "$ICONSET" <<'PYEOF'
import sys
sys.path.insert(0, ".")
from generate_icon import create_icon

iconset = sys.argv[1]
# (filename, pixel size) pairs required by iconutil.
sizes = [
    ("icon_16x16.png", 16), ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256), ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512), ("icon_512x512@2x.png", 1024),
]
for name, px in sizes:
    create_icon(px).save(f"{iconset}/{name}")
print(f"Wrote {len(sizes)} icon images to {iconset}")
PYEOF
    iconutil -c icns "$ICONSET" -o "$APP_DIR/Contents/Resources/AppIcon.icns"
    rm -rf "$ICONSET"
else
    echo "NOTE: python3/Pillow not available — packaging without a custom app icon." >&2
fi

# ---------------------------------------------------------------------------
# 4. Whisper model (optional, bundled by default for an out-of-the-box first
#    run — see SettingsStore::autodetectModelPath(), which checks
#    <bundle>/Contents/Resources/models/, same Resources/-not-MacOS/ reason
#    as commands.default.json above).
# ---------------------------------------------------------------------------
if [ "$VT_PACKAGE_MODEL" != "none" ]; then
    MODEL_FILE="ggml-${VT_PACKAGE_MODEL}.bin"
    MODEL_SRC="${ROOT_DIR}/models/${MODEL_FILE}"
    if [ ! -f "$MODEL_SRC" ]; then
        echo "Downloading bundled model (${VT_PACKAGE_MODEL})..."
        "${SCRIPT_DIR}/download-model.sh" "$VT_PACKAGE_MODEL"
    fi
    mkdir -p "$APP_DIR/Contents/Resources/models"
    cp "$MODEL_SRC" "$APP_DIR/Contents/Resources/models/${MODEL_FILE}"
    echo "Bundled model: ${MODEL_FILE}"
else
    echo "Packaging without a bundled model (VT_PACKAGE_MODEL=none)."
fi

# ---------------------------------------------------------------------------
# 5. Info.plist
# ---------------------------------------------------------------------------
cat > "$APP_DIR/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>${PROJECT_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleName</key>
    <string>${PROJECT_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${PROJECT_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${FULL_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${FULL_VERSION}</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>voiceTyper needs microphone access to transcribe your speech.</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# ---------------------------------------------------------------------------
# 6. Bundle Qt frameworks + plugins with macdeployqt. Without this, the
#    binary dynamically links against Qt at its absolute build-machine path
#    (e.g. /Users/you/Qt/6.11.0/macos/lib/...) — it would only run on THIS
#    machine, and hardened runtime rejects the mismatched signatures on
#    anyone else's anyway. macdeployqt copies the frameworks into
#    Contents/Frameworks and rewrites the binary's link paths to match.
#    Must run before codesigning (it invalidates any existing signature).
# ---------------------------------------------------------------------------
echo "Bundling Qt frameworks..."
"${QT_DIR}/bin/macdeployqt" "$APP_DIR"

# ---------------------------------------------------------------------------
# 7. Code sign, inside-out: every framework/dylib/plugin individually, then
#    the app bundle last. `codesign --deep` looks like it should handle this
#    in one shot, but Apple's own docs call it unsafe for exactly this case —
#    in practice it leaves nested Qt frameworks under their original
#    Qt-Company signature instead of re-signing them with ours. That mismatch
#    is invisible to `codesign --verify` (which only checks each seal is
#    internally valid) but dyld's hardened-runtime library validation rejects
#    it at launch ("different Team IDs"), so the packaged app fails to start
#    on any machine but this one. Hardened runtime (--options runtime) itself
#    is only requested for a real Developer ID, since it's meaningless for
#    ad-hoc signing and is what makes the Team ID mismatch fatal in the first
#    place.
# ---------------------------------------------------------------------------
echo "Signing with identity: ${CODESIGN_IDENTITY}"
SIGN_FLAGS=(--force --sign "$CODESIGN_IDENTITY")
if [ "$CODESIGN_IDENTITY" != "-" ]; then
    SIGN_FLAGS+=(--options runtime)
fi

# Innermost first: plugins, then dylibs directly under Frameworks/, then each
# .framework bundle (by its Versions/Current, which resolves the symlink).
find "$APP_DIR/Contents/PlugIns" -name "*.dylib" -print0 2>/dev/null \
    | xargs -0 -I{} codesign "${SIGN_FLAGS[@]}" "{}"
find "$APP_DIR/Contents/Frameworks" -maxdepth 1 -name "*.dylib" -print0 2>/dev/null \
    | xargs -0 -I{} codesign "${SIGN_FLAGS[@]}" "{}"
find "$APP_DIR/Contents/Frameworks" -maxdepth 1 -name "*.framework" -print0 2>/dev/null \
    | while IFS= read -r -d '' fw; do
        codesign "${SIGN_FLAGS[@]}" "$fw"
    done

# Outermost last: the app bundle itself (signs the main executable + seals
# Info.plist/Resources). No --deep needed — everything nested is already signed.
codesign "${SIGN_FLAGS[@]}" "$APP_DIR"
codesign -dv "$APP_DIR" 2>&1
codesign --verify --deep --strict "$APP_DIR"

# ---------------------------------------------------------------------------
# 8. Notarize (only if a real Developer ID + Apple credentials are present)
# ---------------------------------------------------------------------------
if [ "$CODESIGN_IDENTITY" != "-" ] && [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
    echo "Notarizing (this can take several minutes)..."
    NOTARIZE_ZIP="${BUILD_DIR}/${PROJECT_NAME}-notarize.zip"
    ditto -c -k --keepParent "$APP_DIR" "$NOTARIZE_ZIP"
    xcrun notarytool submit "$NOTARIZE_ZIP" \
        --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" \
        --wait
    xcrun stapler staple "$APP_DIR"
    rm -f "$NOTARIZE_ZIP"
else
    echo "Skipping notarization (ad-hoc signed, or APPLE_ID/APPLE_TEAM_ID/APPLE_APP_PASSWORD not set)."
fi

# ---------------------------------------------------------------------------
# 9. Package into a .dmg
# ---------------------------------------------------------------------------
ln -s /Applications "$STAGE_DIR/Applications"
hdiutil create -volname "$PROJECT_NAME" -srcfolder "$STAGE_DIR" -ov -format UDZO "$OUT_DMG"

if [ "$CODESIGN_IDENTITY" != "-" ]; then
    codesign --force --sign "$CODESIGN_IDENTITY" "$OUT_DMG"
fi

rm -rf "$STAGE_DIR"

echo
echo "==================================================================="
echo "  Done: ${OUT_DMG}"
echo "==================================================================="
if [ "$CODESIGN_IDENTITY" = "-" ]; then
    echo
    echo "This is ad-hoc signed (no Developer ID). Recipients will need to:"
    echo "  right-click the app > Open > Open  (first launch only)"
    echo "or: System Settings > Privacy & Security > \"Open Anyway\""
    echo
    echo "For a friction-free install, get an Apple Developer Program"
    echo "membership (\$99/yr) and re-run with:"
    echo "  CODESIGN_IDENTITY=\"Developer ID Application: ...\" APPLE_ID=... \\"
    echo "  APPLE_TEAM_ID=... APPLE_APP_PASSWORD=... scripts/package-macos.sh"
fi

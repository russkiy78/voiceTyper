#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PROJECT_NAME="voiceTyper"
VERSION="0.1.0"
ARCH="amd64"
PACKAGE_NAME="${PROJECT_NAME}_${VERSION}_${ARCH}"
BUILD_DIR="${ROOT_DIR}/build"
DEB_DIR="${ROOT_DIR}/deb_package"
MODELS_DIR="models"
DEFAULT_MODEL="ggml-large-v3-q5_0.bin"

cd "$ROOT_DIR"

# Clean previous build artifacts
rm -rf "${DEB_DIR}" "${ROOT_DIR}/${PACKAGE_NAME}.deb"

# Generate icons if they don't exist
if [ ! -f "${ROOT_DIR}/voicetyper_icon.png" ]; then
    echo "Generating application icons..."
    python3 "${SCRIPT_DIR}/../generate_icon.py"
fi

# Build project if build directory doesn't exist or is empty
if [ ! -d "${BUILD_DIR}" ] || [ -z "$(ls -A ${BUILD_DIR})" ]; then
    echo "Building project..."
    cmake -S . -B ${BUILD_DIR}
    cmake --build ${BUILD_DIR} --config Release
fi

# Create DEB package structure
mkdir -p "${DEB_DIR}/DEBIAN"
mkdir -p "${DEB_DIR}/usr/bin"
mkdir -p "${DEB_DIR}/usr/share/${PROJECT_NAME}/models"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/16x16/apps"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/32x32/apps"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/48x48/apps"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/64x64/apps"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/128x128/apps"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/256x256/apps"
mkdir -p "${DEB_DIR}/usr/share/applications"

# Copy binary
cp "${BUILD_DIR}/${PROJECT_NAME}" "${DEB_DIR}/usr/bin/"

# Copy default config
cp config/commands.default.json "${DEB_DIR}/usr/share/${PROJECT_NAME}/"

# Copy models directory structure (empty, will be populated by postinst)
mkdir -p "${DEB_DIR}/usr/share/${PROJECT_NAME}/models"

# Copy icons
cp "${ROOT_DIR}/voicetyper_16x16.png" "${DEB_DIR}/usr/share/icons/hicolor/16x16/apps/voicetyper.png"
cp "${ROOT_DIR}/voicetyper_32x32.png" "${DEB_DIR}/usr/share/icons/hicolor/32x32/apps/voicetyper.png"
cp "${ROOT_DIR}/voicetyper_48x48.png" "${DEB_DIR}/usr/share/icons/hicolor/48x48/apps/voicetyper.png"
cp "${ROOT_DIR}/voicetyper_64x64.png" "${DEB_DIR}/usr/share/icons/hicolor/64x64/apps/voicetyper.png"
cp "${ROOT_DIR}/voicetyper_128x128.png" "${DEB_DIR}/usr/share/icons/hicolor/128x128/apps/voicetyper.png"
cp "${ROOT_DIR}/voicetyper_256x256.png" "${DEB_DIR}/usr/share/icons/hicolor/256x256/apps/voicetyper.png"

# Create desktop entry
cat > "${DEB_DIR}/usr/share/applications/voicetyper.desktop" <<'DESKTOP_EOF'
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

# Create control file
cat > "${DEB_DIR}/DEBIAN/control" <<EOF
Package: ${PROJECT_NAME}
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libqt6core6, libqt6gui6, libqt6widgets6, libqt6multimedia6, libqt6network6
Maintainer: VoiceTyper Team
Description: Local voice typing utility using Qt6 and whisper.cpp
 A desktop application for voice-to-text transcription with local processing.
 Features include global hotkey support, command detection, and offline transcription.
EOF

# Create postinst script to install icons and download model if not present
cat > "${DEB_DIR}/DEBIAN/postinst" <<'POSTINST_EOF'
#!/bin/bash
set -e

# Update icon cache
if [ -x /usr/bin/gtk-update-icon-cache ]; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi

MODELS_DIR="/usr/share/voiceTyper/models"
DEFAULT_MODEL="ggml-large-v3-q5_0.bin"
MODEL_PATH="${MODELS_DIR}/${DEFAULT_MODEL}"
MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-q5_0.bin"

# Create models directory if it doesn't exist
mkdir -p "${MODELS_DIR}"

# Check if default model exists
if [ ! -f "${MODEL_PATH}" ]; then
    echo "Downloading default Whisper model (${DEFAULT_MODEL})..."
    echo "This may take a while depending on your connection speed."
    
    # Try to download the model
    if command -v wget &> /dev/null; then
        wget -O "${MODEL_PATH}" "${MODEL_URL}" || {
            echo "Failed to download model. Please download manually:"
            echo "  wget -O ${MODEL_PATH} ${MODEL_URL}"
            exit 0
        }
    elif command -v curl &> /dev/null; then
        curl -L -o "${MODEL_PATH}" "${MODEL_URL}" || {
            echo "Failed to download model. Please download manually:"
            echo "  curl -L -o ${MODEL_PATH} ${MODEL_URL}"
            exit 0
        }
    else
        echo "Neither wget nor curl found. Please install one and download the model manually:"
        echo "  wget -O ${MODEL_PATH} ${MODEL_URL}"
        echo "  or"
        echo "  curl -L -o ${MODEL_PATH} ${MODEL_URL}"
        exit 0
    fi
    
    chmod 644 "${MODEL_PATH}"
    echo "Model downloaded successfully to ${MODEL_PATH}"
else
    echo "Default model already exists at ${MODEL_PATH}, skipping download."
fi

exit 0
POSTINST_EOF

chmod 755 "${DEB_DIR}/DEBIAN/postinst"

# Create postrm script for clean uninstallation
cat > "${DEB_DIR}/DEBIAN/postrm" <<'POSTRM_EOF'
#!/bin/bash
set -e

if [ "$1" = "purge" ]; then
    echo "Removing models directory..."
    rm -rf "/usr/share/voiceTyper"
fi

exit 0
POSTRM_EOF

chmod 755 "${DEB_DIR}/DEBIAN/postrm"

# Build the DEB package
echo "Building DEB package: ${ROOT_DIR}/${PACKAGE_NAME}.deb"
dpkg-deb --build "${DEB_DIR}" "${ROOT_DIR}/${PACKAGE_NAME}.deb"

# Clean up temporary directory
rm -rf "${DEB_DIR}"

echo "DEB package created: ${ROOT_DIR}/${PACKAGE_NAME}.deb"
echo ""
echo "To install:"
echo "  sudo dpkg -i ${PACKAGE_NAME}.deb"
echo ""
echo "The package will attempt to download the default model (${DEFAULT_MODEL}) during installation."
echo "If download fails, you can manually download it to /usr/share/voiceTyper/models/"

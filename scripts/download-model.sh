#!/usr/bin/env bash
#
# Downloads a bundled whisper.cpp GGML model into ./models.
#
# Usage:
#   scripts/download-model.sh [model]
#
# model (default: small-q5_1) is one of the names published at:
#   https://huggingface.co/ggerganov/whisper.cpp
#
# Recommended for the MVP:
#   small-q5_1            ~180 MB  (good size/quality tradeoff, multilingual)
#   medium-q5_0           ~540 MB  (better quality)
#   large-v3-turbo-q5_0   ~570 MB  (best quality / speed, recommended target)
#
set -euo pipefail

MODEL="${1:-small-q5_1}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODELS_DIR="${SCRIPT_DIR}/../models"
BASE_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main"
FILE="ggml-${MODEL}.bin"
URL="${BASE_URL}/${FILE}"

mkdir -p "${MODELS_DIR}"
DEST="${MODELS_DIR}/${FILE}"

if [[ -f "${DEST}" ]]; then
    echo "Model already present: ${DEST}"
    exit 0
fi

echo "Downloading ${URL}"
if command -v curl >/dev/null 2>&1; then
    curl -L --fail -o "${DEST}.part" "${URL}"
elif command -v wget >/dev/null 2>&1; then
    wget -O "${DEST}.part" "${URL}"
else
    echo "Need curl or wget to download the model." >&2
    exit 1
fi

mv "${DEST}.part" "${DEST}"
echo "Saved model to ${DEST}"
echo
echo "Point voiceTyper at it via Settings, or set modelPath in the config."

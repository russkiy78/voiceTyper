#!/usr/bin/env bash
# Bump the MAJOR.MINOR version in CMakeLists.txt.
# Patch is derived automatically from git commit count at build time.
#
# Usage: scripts/bump-version.sh <MAJOR.MINOR>
#   e.g. scripts/bump-version.sh 0.3
set -euo pipefail

NEW_VER="${1:?Usage: $0 <MAJOR.MINOR>  (e.g. 0.3)}"

if ! [[ "${NEW_VER}" =~ ^[0-9]+\.[0-9]+$ ]]; then
    echo "error: version must be MAJOR.MINOR — patch is auto-set from git commit count" >&2
    exit 1
fi

MAJOR="${NEW_VER%%.*}"
MINOR="${NEW_VER##*.}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMAKELISTS="${ROOT}/CMakeLists.txt"

sed -i -E "s/(    VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1${MAJOR}.${MINOR}.0/" "${CMAKELISTS}"

COMMIT_COUNT=$(git -C "${ROOT}" rev-list --count HEAD 2>/dev/null || echo "?")
echo "version: ${MAJOR}.${MINOR}.${COMMIT_COUNT}  (patch will update on every new commit)"

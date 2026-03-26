#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST="${ROOT_DIR}/bindings/go/tachyon/_core_local"

rm -rf "${DEST}"
mkdir -p "${DEST}"
cp -r "${ROOT_DIR}/core/include" "${DEST}/"
cp -r "${ROOT_DIR}/core/src" "${DEST}/"

echo "Vendored core → ${DEST}"
echo "Files:"
find "${DEST}" -type f | sort | sed 's|^|  |'

#!/usr/bin/env bash

set -euo pipefail

EMSDK_VERSION="${1:-latest}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EMSDK_DIR="${PROJECT_ROOT}/.emsdk"

echo "[emsdk] Preparing Emscripten SDK (${EMSDK_VERSION}) in ${EMSDK_DIR}..."

if [[ ! -d "${EMSDK_DIR}" ]]; then
	echo "[emsdk] Cloning emsdk repository..."
	git clone https://github.com/emscripten-core/emsdk.git "${EMSDK_DIR}"
else
	echo "[emsdk] Directory already exists. Pulling latest updates..."
	cd "${EMSDK_DIR}"
	git pull origin main
fi

cd "${EMSDK_DIR}"

echo "[emsdk] Installing version: ${EMSDK_VERSION}..."
./emsdk install "${EMSDK_VERSION}"

echo "[emsdk] Activating version: ${EMSDK_VERSION}..."
./emsdk activate "${EMSDK_VERSION}"

echo ""
echo "[emsdk] Emscripten SDK successfully installed."
echo "[emsdk] To activate the toolchain in your current shell, run:"
echo "source ${EMSDK_DIR}/emsdk_env.sh"

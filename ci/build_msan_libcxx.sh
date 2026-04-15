#!/usr/bin/env bash

set -euo pipefail

LLVM_VERSION="${1:-21}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${PROJECT_ROOT}/.msan_toolchain/llvm-${LLVM_VERSION}"

if [[ -d "${INSTALL_DIR}/include/c++/v1" ]]; then
	echo "[msan] Instrumented libc++ already present in ${INSTALL_DIR}"
	exit 0
fi

echo "[msan] Building libc++ with MemorySanitizer (LLVM ${LLVM_VERSION})..."

CLANG_CC="clang-${LLVM_VERSION}"
CLANG_CXX="clang++-${LLVM_VERSION}"

if ! command -v "${CLANG_CC}" &>/dev/null; then
	if command -v clang &>/dev/null; then
		CLANG_CC="clang"
		CLANG_CXX="clang++"
		echo "[msan] Note: ${CLANG_CC} fallback triggered (typical for Fedora)"
	else
		echo "[msan] Error: Could not find clang-${LLVM_VERSION} or clang in PATH." >&2
		exit 1
	fi
fi

if command -v ninja &>/dev/null; then
	CMAKE_GENERATOR="-G Ninja"
	BUILD_TOOL="ninja"
else
	CMAKE_GENERATOR=""
	BUILD_TOOL="make -j$(nproc)"
fi

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TEMP_DIR}"' EXIT
cd "${TEMP_DIR}"

echo "[msan] Cloning llvm-project release/${LLVM_VERSION}.x..."
git clone --depth=1 -b "release/${LLVM_VERSION}.x" https://github.com/llvm/llvm-project.git
cd llvm-project

echo "[msan] Configuring build with CMake..."
cmake "${CMAKE_GENERATOR}" -S runtimes -B build \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER="${CLANG_CC}" \
	-DCMAKE_CXX_COMPILER="${CLANG_CXX}" \
	-DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
	-DLLVM_USE_SANITIZER=MemoryWithOrigins \
	-DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DLIBCXX_ENABLE_SHARED=OFF \
	-DLIBCXXABI_ENABLE_SHARED=OFF \
	-DLIBCXX_ENABLE_STATIC=ON \
	-DLIBCXXABI_ENABLE_STATIC=ON \
	-DLIBCXX_INCLUDE_TESTS=OFF \
	-DLIBCXXABI_INCLUDE_TESTS=OFF \
	-DLIBUNWIND_ENABLE_SHARED=OFF \
	-DLIBUNWIND_ENABLE_STATIC=ON

echo "[msan] Compiling and installing..."
${BUILD_TOOL} -C build install

echo "[msan] libc++ MSan successfully installed locally in ${INSTALL_DIR}"

#!/usr/bin/env bash
set -euo pipefail

WORKSPACE="/workspace"
BINDING_DIR="${WORKSPACE}/bindings/csharp"
CORE_VENDOR="${BINDING_DIR}/src/TachyonIpc/_core_local"

ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64)  RID="linux-x64"   ;;
    aarch64) RID="linux-arm64" ;;
    *)       echo "[post-create] Unsupported arch: ${ARCH}" >&2; exit 1 ;;
esac

echo "[post-create] Vendoring C++ core..."
bash "${WORKSPACE}/ci/vendor.sh" c#
echo "[post-create] Core vendored: ${CORE_VENDOR}"

echo "[post-create] Building libtachyon (clang-${LLVM_VERSION:-21}, Release)..."
BUILD_DIR="/tmp/build/csharp-release"
cmake -S "${WORKSPACE}" \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="clang-${LLVM_VERSION:-21}" \
    -DCMAKE_CXX_COMPILER="clang++-${LLVM_VERSION:-21}" \
    -DTACHYON_SANITIZER=none \
    -DTACHYON_PORTABLE_BUILD=ON

cmake --build "${BUILD_DIR}" --target tachyon --parallel

NATIVE_DIR="${BINDING_DIR}/src/TachyonIpc/runtimes/${RID}/native"
mkdir -p "${NATIVE_DIR}"
cp "${BUILD_DIR}/core/libtachyon.so" "${NATIVE_DIR}/libtachyon.so"
echo "[post-create] Native library: ${NATIVE_DIR}/libtachyon.so"

echo "[post-create] Restoring .NET packages..."
if [ -f "${BINDING_DIR}/src/TachyonIpc/TachyonIpc.csproj" ]; then
    dotnet restore "${BINDING_DIR}/src/TachyonIpc/TachyonIpc.csproj"
fi

if [ -f "${BINDING_DIR}/tests/TachyonIpc.Tests/TachyonIpc.Tests.csproj" ]; then
    dotnet restore "${BINDING_DIR}/tests/TachyonIpc.Tests/TachyonIpc.Tests.csproj"
fi

if [ -f "${BINDING_DIR}/benchmarks/TachyonIpc.Benchmarks/TachyonIpc.Benchmarks.csproj" ]; then
    dotnet restore "${BINDING_DIR}/benchmarks/TachyonIpc.Benchmarks/TachyonIpc.Benchmarks.csproj"
fi

echo "[post-create] Done."

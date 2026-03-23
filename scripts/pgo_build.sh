#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/cmake-build-pgo}"
CORES="${2:-$(nproc)}"
PGO_DIR="${BUILD_DIR}/pgo-data"
BENCHMARK="${BUILD_DIR}/benchmark/tachyon_benchmark"

BLUE='\033[0;34m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

log() { echo -e "${BLUE}[pgo]${NC} $*"; }
ok() { echo -e "${GREEN}[pgo]${NC} $*"; }
warn() { echo -e "${YELLOW}[pgo]${NC} $*"; }
err() {
	echo -e "${RED}[pgo]${NC} $*" >&2
	exit 1
}

[[ -f "${ROOT_DIR}/CMakeLists.txt" ]] || err "Must be run from the Tachyon root or scripts/ directory."
[[ "${EUID}" -eq 0 ]] && err "Do not run as root. taskset does not require sudo for your own processes."

detect_compiler() {
	local candidate
	if [[ -n "${CXX:-}" ]] && command -v "${CXX}" &>/dev/null; then
		candidate="$(command -v "${CXX}")"
	elif command -v clang++ &>/dev/null; then
		candidate="$(command -v clang++)"
	elif command -v g++ &>/dev/null; then
		candidate="$(command -v g++)"
	else
		err "No C++ compiler found. Set \$CXX or install clang++ / g++."
	fi
	local ver
	ver="$("${candidate}" --version 2>&1 | head -1)"
	if echo "${ver}" | grep -qi "clang"; then
		echo "clang:${candidate}"
	else echo "gcc:${candidate}"; fi
}

COMPILER_INFO="$(detect_compiler)"
COMPILER_FAMILY="${COMPILER_INFO%%:*}"
COMPILER_BIN="${COMPILER_INFO##*:}"

log "Root:      ${ROOT_DIR}"
log "Build dir: ${BUILD_DIR}"
log "PGO data:  ${PGO_DIR}"
log "Cores:     ${CORES}"
log "Compiler:  ${COMPILER_BIN} (${COMPILER_FAMILY})"

LLVM_PROFDATA=""
if [[ "${COMPILER_FAMILY}" == "clang" ]]; then
	CLANG_VER="$("${COMPILER_BIN}" --version 2>&1 | grep -oP 'version \K[0-9]+' | head -1)"
	for candidate in "llvm-profdata-${CLANG_VER}" "llvm-profdata"; do
		if command -v "${candidate}" &>/dev/null; then
			LLVM_PROFDATA="$(command -v "${candidate}")"
			break
		fi
	done
	[[ -n "${LLVM_PROFDATA}" ]] || err "llvm-profdata not found. Install llvm-${CLANG_VER}."
	log "llvm-profdata: ${LLVM_PROFDATA}"
fi

TASKSET_CMD=""
if command -v taskset &>/dev/null; then
	TASKSET_CMD="taskset -c 7,8,9"
	log "CPU pinning: cores 7,8,9"
else
	warn "taskset not found — running unpinned"
fi
echo

cmake_build() {
	local phase="$1"
	cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_COMPILER="${COMPILER_BIN}" \
		-DTACHYON_PGO_PHASE="${phase}" \
		-DTACHYON_PGO_DIR="${PGO_DIR}" \
		-G Ninja --fresh -Wno-dev 2>&1 | grep -v "^--" || true
	cmake --build "${BUILD_DIR}" --target tachyon_benchmark -- -j"${CORES}"
}

log "=== Phase 1: GENERATE ==="
cmake_build GENERATE
ok "Phase 1 complete."
echo

log "=== Profile collection ==="
${TASKSET_CMD} "${BENCHMARK}"

if [[ "${COMPILER_FAMILY}" == "clang" ]]; then
	COUNT=$(find "${PGO_DIR}" -name "*.profraw" 2>/dev/null | wc -l)
	[[ "${COUNT}" -gt 0 ]] || err "No .profraw files in ${PGO_DIR}."
	ok "Collected ${COUNT} .profraw file(s). Merging..."
	"${LLVM_PROFDATA}" merge -output="${PGO_DIR}/merged.profdata" "${PGO_DIR}"/*.profraw
	ok "Merged: ${PGO_DIR}/merged.profdata"
else
	COUNT=$(find "${PGO_DIR}" -name "*.gcda" 2>/dev/null | wc -l)
	[[ "${COUNT}" -gt 0 ]] || err "No .gcda files in ${PGO_DIR}."
	ok "Collected ${COUNT} .gcda file(s)."
fi
echo

log "=== Phase 2: USE ==="
cmake_build USE
ok "Phase 2 complete."
echo

log "=== Validation ==="
${TASKSET_CMD} "${BENCHMARK}"

echo
ok "Binary: ${BENCHMARK}"
ok "Baseline: ${ROOT_DIR}/cmake-build-release/benchmark/tachyon_benchmark"

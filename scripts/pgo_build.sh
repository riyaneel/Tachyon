#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/cmake-build-pgo}"
CORES="${2:-$(nproc)}"
PGO_DIR="${BUILD_DIR}/pgo-data"
BENCHMARK="${BUILD_DIR}/benchmark/tachyon_benchmark"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()  { echo -e "${BLUE}[pgo]${NC} $*"; }
ok()   { echo -e "${GREEN}[pgo]${NC} $*"; }
warn() { echo -e "${YELLOW}[pgo]${NC} $*"; }
err()  { echo -e "${RED}[pgo]${NC} $*" >&2; exit 1; }

[[ -f "${ROOT_DIR}/CMakeLists.txt" ]] || err "Must be run from the Tachyon root or scripts/ directory."

log "Root:      ${ROOT_DIR}"
log "Build dir: ${BUILD_DIR}"
log "PGO data:  ${PGO_DIR}"
log "Cores:     ${CORES}"
echo

log "=== Phase 1: GENERATE ==="
log "Configuring instrumented build..."

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTACHYON_PGO_PHASE=GENERATE \
    -DTACHYON_PGO_DIR="${PGO_DIR}" \
    -G Ninja \
    --fresh \
    -Wno-dev \
    2>&1 | grep -v "^--" || true

log "Building instrumented binary..."
cmake --build "${BUILD_DIR}" --target tachyon_benchmark -- -j"${CORES}"

ok "Phase 1 build complete."
echo

log "=== Profile collection (benchmark run) ==="

TASKSET_CMD=""
if command -v taskset &>/dev/null && sudo -n taskset -c 0 true 2>/dev/null; then
    TASKSET_CMD="sudo taskset -c 7,8,9"
    log "Pinning to cores 7,8,9 via taskset"
else
    warn "taskset not available or requires password — running unpinned"
    warn "Profile quality will be slightly lower but still valid"
fi

log "Running benchmark to collect profiles (~10 seconds)..."
${TASKSET_CMD} "${BENCHMARK}"

PROFILE_COUNT=$(find "${PGO_DIR}" -name "*.gcda" 2>/dev/null | wc -l)
if [[ "${PROFILE_COUNT}" -eq 0 ]]; then
    PROFILE_COUNT=$(find "${PGO_DIR}" -name "*.profraw" 2>/dev/null | wc -l)
fi

if [[ "${PROFILE_COUNT}" -eq 0 ]]; then
    err "No profile data found in ${PGO_DIR}. Benchmark may have failed."
fi

ok "Profile data collected (${PROFILE_COUNT} file(s) in ${PGO_DIR})."
echo

if "${BENCHMARK}" --version 2>/dev/null | grep -q clang; then
    if command -v llvm-profdata &>/dev/null; then
        log "Merging LLVM profile data..."
        llvm-profdata merge -output="${PGO_DIR}/merged.profdata" "${PGO_DIR}"/*.profraw
        ok "LLVM profiles merged."
    else
        warn "llvm-profdata not found — profile merge skipped."
        warn "If using Clang, install llvm-tools and re-run."
    fi
fi

log "=== Phase 2: USE ==="
log "Configuring optimized build using profile data..."

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTACHYON_PGO_PHASE=USE \
    -DTACHYON_PGO_DIR="${PGO_DIR}" \
    -G Ninja \
    --fresh \
    -Wno-dev \
    2>&1 | grep -v "^--" || true

log "Building PGO-optimized binary..."
cmake --build "${BUILD_DIR}" --target tachyon_benchmark -- -j"${CORES}"

ok "Phase 2 build complete."
echo

log "=== Validation: PGO-optimized benchmark run ==="
${TASKSET_CMD} "${BENCHMARK}"

echo
ok "PGO build complete. Binary at: ${BENCHMARK}"
ok "To compare against baseline:"
ok "  sudo taskset -c 7,8,9 ${ROOT_DIR}/cmake-build-release/benchmark/tachyon_benchmark"
ok "  sudo taskset -c 7,8,9 ${BENCHMARK}"

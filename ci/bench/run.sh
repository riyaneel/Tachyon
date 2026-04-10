#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${1:-$(pwd)/build/clang-release}"
OUTPUT_DIR="${2:-$(pwd)/benchmark/results}"
PING_CORE="${PING_CORE:-8}"
PONG_CORE="${PONG_CORE:-9}"
ITERATIONS="${ITERATIONS:-1000000}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
log() { echo -e "${GREEN}[bench]${NC} $*"; }
warn() { echo -e "${YELLOW}[bench]${NC} $*"; }
err() {
	echo -e "${RED}[bench]${NC} $*" >&2
	exit 1
}

command -v taskset >/dev/null 2>&1 || err "taskset not found (install util-linux)"
command -v chrt >/dev/null 2>&1 || err "chrt not found (install util-linux)"
command -v python3 >/dev/null 2>&1 || warn "python3 not found — comparison table disabled"

BENCH_INTRA="${BUILD_DIR}/benchmark/tachyon_bench_intra"
BENCH_INTER="${BUILD_DIR}/benchmark/tachyon_bench_inter"
BENCH_ZMQ="${BUILD_DIR}/benchmark/tachyon_bench_zmq"

[[ -x "${BENCH_INTRA}" ]] || err "Binary not found: ${BENCH_INTRA}\nBuild first: cmake --build ${BUILD_DIR} -t tachyon_bench_intra tachyon_bench_inter"
[[ -x "${BENCH_INTER}" ]] || err "Binary not found: ${BENCH_INTER}"

mkdir -p "${OUTPUT_DIR}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

MAX_CORE=$(($(nproc) - 1))
if [[ "${PING_CORE}" -gt "${MAX_CORE}" || "${PONG_CORE}" -gt "${MAX_CORE}" ]]; then
	warn "Requested cores ${PING_CORE},${PONG_CORE} exceed available (max=${MAX_CORE})"
	warn "Running unpinned — results will show higher jitter"
	TASKSET_INTRA=""
	TASKSET_INTER=""
else
	TASKSET_INTRA="taskset -c ${PING_CORE},${PONG_CORE}"
	TASKSET_INTER="taskset -c ${PING_CORE},${PONG_CORE}"
	log "Core pinning: benchmark=${PING_CORE}, server=${PONG_CORE}"
fi

CHRT_CMD="chrt -f ${SCHED_PRIO:-99}"
if ! chrt -f 1 true 2>/dev/null; then
	warn "SCHED_FIFO unavailable — run with sudo for best results"
	CHRT_CMD=""
fi

log "Build:      ${BUILD_DIR}"
log "Output:     ${OUTPUT_DIR}"
log "Iterations: ${ITERATIONS}"
echo

CPUFREQ_RESTORED=()
for cpu in "${PING_CORE}" "${PONG_CORE}"; do
	GOVPATH="/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
	if [[ -w "${GOVPATH}" ]]; then
		CPUFREQ_RESTORED+=("${cpu}:$(cat ${GOVPATH})")
		echo "performance" >"${GOVPATH}"
		log "CPU${cpu} governor → performance"
	fi
done

restore_cpufreq() {
	for entry in "${CPUFREQ_RESTORED[@]:-}"; do
		local cpu="${entry%%:*}"
		local gov="${entry##*:}"
		local govpath="/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
		[[ -w "${govpath}" ]] && echo "${gov}" >"${govpath}"
	done
}
trap restore_cpufreq EXIT

INTRA_JSON="${OUTPUT_DIR}/intra_${TIMESTAMP}.json"
log "=== Intra-process (two threads, shared arena) ==="

# shellcheck disable=SC2086
TACHYON_SERVER_CORE="${PONG_CORE}" \
	${TASKSET_INTRA} ${CHRT_CMD} \
	"${BENCH_INTRA}" \
	--benchmark_format=json \
	--benchmark_out="${INTRA_JSON}" \
	--benchmark_repetitions=3 \
	--benchmark_display_aggregates_only=true \
	2>&1 | tee "${OUTPUT_DIR}/intra_${TIMESTAMP}.log"

log "Intra JSON: ${INTRA_JSON}"
echo

INTER_JSON="${OUTPUT_DIR}/inter_${TIMESTAMP}.json"
log "=== Inter-process (fork, UDS handshake, two mmap arenas) ==="

# shellcheck disable=SC2086
TACHYON_PING_CORE="${PING_CORE}" TACHYON_PONG_CORE="${PONG_CORE}" \
	${TASKSET_INTER} ${CHRT_CMD} \
	"${BENCH_INTER}" \
	--iterations "${ITERATIONS}" \
	--output "${INTER_JSON}" \
	2>&1 | tee "${OUTPUT_DIR}/inter_${TIMESTAMP}.log"

log "Inter JSON: ${INTER_JSON}"
echo

ZMQ_JSON=""
if [[ -x "${BENCH_ZMQ}" ]]; then
	ZMQ_JSON="${OUTPUT_DIR}/zmq_${TIMESTAMP}.json"
	log "=== ZeroMQ baseline (inproc://) ==="
	# shellcheck disable=SC2086
	TACHYON_SERVER_CORE="${PONG_CORE}" \
		${TASKSET_INTRA} ${CHRT_CMD} \
		"${BENCH_ZMQ}" \
		--benchmark_format=json \
		--benchmark_out="${ZMQ_JSON}" \
		--benchmark_repetitions=3 \
		--benchmark_display_aggregates_only=true \
		2>&1 | tee "${OUTPUT_DIR}/zmq_${TIMESTAMP}.log"
	log "ZMQ JSON: ${ZMQ_JSON}"
	echo
else
	warn "tachyon_bench_zmq not found — ZeroMQ baseline skipped"
	warn "Install libzmq3-dev and rebuild to enable"
	echo
fi

log "=== Summary ==="

extract_p50() {
	local json="$1"
	python3 -c "
import json, sys
with open('${json}') as f:
    data = json.load(f)
for b in data.get('benchmarks', []):
    if 'aggregate_name' not in b or b.get('aggregate_name') == 'mean':
        p = b.get('p50_ns') or b.get('real_time', 0)
        print(f'{p:.1f}')
        break
" 2>/dev/null || echo "N/A"
}

if command -v python3 >/dev/null 2>&1; then
	INTRA_P50=$(extract_p50 "${INTRA_JSON}")
	INTER_P50=$(extract_p50 "${INTER_JSON}")

	printf "\n%-40s %12s\n" "Benchmark" "p50 RTT (ns)"
	printf "%-40s %12s\n" "$(printf '%0.s-' {1..40})" "$(printf '%0.s-' {1..12})"
	printf "%-40s %12s\n" "Tachyon intra-process (two threads)" "${INTRA_P50}"
	printf "%-40s %12s\n" "Tachyon inter-process (two processes)" "${INTER_P50}"

	if [[ -n "${ZMQ_JSON}" ]]; then
		ZMQ_P50=$(extract_p50 "${ZMQ_JSON}")
		printf "%-40s %12s\n" "ZeroMQ inproc://" "${ZMQ_P50}"
	fi
	echo
fi

log "All results written to ${OUTPUT_DIR}/"
log ""
log "Percentile comparison (Tachyon):"

if [[ -n "${ZMQ_JSON}" ]]; then
	log "  python3 ci/bench/compare.py ${INTRA_JSON} ${INTER_JSON} ${ZMQ_JSON}"
else
	log "  python3 ci/bench/compare.py ${INTRA_JSON} ${INTER_JSON}"
fi

#!/usr/bin/env python3

import json
import sys
import os
from typing import Optional, List, Dict

METRICS = ['p50_ns', 'p90_ns', 'p99_ns', 'p99.9_ns', 'p99.99_ns']


def extract_metrics(filepath: str) -> Optional[Dict[str, float]]:
    try:
        with open(filepath, 'r') as f:
            data = json.load(f)

        for b in data.get('benchmarks', []):
            is_gb_median = b.get('run_type') == 'aggregate' and b.get('aggregate_name') == 'median'
            is_tachyon_iter = b.get('run_type') == 'iteration'

            if is_gb_median or is_tachyon_iter:
                metrics = {}
                for m in METRICS:
                    if m in b:
                        metrics[m] = float(b[m])
                if 'p50_ns' in metrics:
                    return metrics

        for b in data.get('benchmarks', []):
            if 'p50_ns' in b:
                return {m: float(b[m]) for m in METRICS if m in b}

    except Exception as e:
        print(f"[ERROR] Failed to parse {filepath}: {e}", file=sys.stderr)

    return None


def get_bench_name(filepath: str) -> str:
    basename = os.path.basename(filepath)
    name_part = basename.split('_')[0]
    return name_part.upper()


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: python ci/bench/compare.py <baseline.json> <target1.json> [target2.json...]", file=sys.stderr)
        sys.exit(1)

    baseline_file: str = sys.argv[1]
    targets: List[str] = sys.argv[2:]

    baseline_name: str = get_bench_name(baseline_file)
    baseline_metrics: Optional[Dict[str, float]] = extract_metrics(baseline_file)

    if not baseline_metrics:
        print(f"[ERROR] Could not extract valid metrics from baseline: {baseline_file}", file=sys.stderr)
        sys.exit(1)

    print("=" * 85)
    print(" Tachyon Benchmark Comparison")
    print("=" * 85)
    print(f" Baseline : {baseline_name}")

    for target_file in targets:
        target_name: str = get_bench_name(target_file)
        target_metrics: Optional[Dict[str, float]] = extract_metrics(target_file)

        print("-" * 85)
        print(f" [ Target : {target_name} ]")
        print(f" {'Metric':<12} | {'Baseline (ns)':<15} | {'Target (ns)':<15} | {'Speedup':<10} | {'Latency Change'}")
        print("-" * 85)

        if not target_metrics:
            print(f" ERROR: Could not parse metrics for {target_name}")
            continue

        for metric in METRICS:
            base_val = baseline_metrics.get(metric)
            tgt_val = target_metrics.get(metric)
            if base_val is None or tgt_val is None:
                print(f" {metric:<12} | {'N/A':<15} | {'N/A':<15} | {'N/A':<10} | N/A")
                continue

            speedup_factor: float = (base_val / tgt_val) if tgt_val > 0 else float('inf')
            latency_diff_pct: float = ((tgt_val - base_val) / base_val) * 100
            speedup_str: str = f"{speedup_factor:.1f}x"
            change_str: str = f"{latency_diff_pct:+.2f}%"
            print(f" {metric:<12} | {base_val:<15.1f} | {tgt_val:<15.1f} | {speedup_str:<10} | {change_str}")

    print("=" * 85)
    print()


if __name__ == "__main__":
    main()

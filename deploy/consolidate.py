#!/usr/bin/env python3
"""Consolidate benchmark results from multiple pensieve-bench clients."""

import glob
import json
import os
import sys
import time

RESULTS_DIR = os.environ.get("RESULTS_DIR", "/results")
EXPECTED = int(os.environ.get("EXPECTED_CLIENTS", "3"))
POLL_INTERVAL = 2
TIMEOUT = 300

BUCKET_LABELS = [
    "  < 50us ", "  < 100us", "  < 250us", "  < 500us",
    "  <   1ms", "  <   5ms", "  <  10ms", "  <  50ms",
    "  < 100ms", "  >= 100ms",
]


def wait_for_results():
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        files = sorted(glob.glob(os.path.join(RESULTS_DIR, "*.json")))
        print(f"[consolidator] found {len(files)}/{EXPECTED} result files...",
              flush=True)
        if len(files) >= EXPECTED:
            return files
        time.sleep(POLL_INTERVAL)
    files = sorted(glob.glob(os.path.join(RESULTS_DIR, "*.json")))
    if files:
        return files
    print("[consolidator] timed out waiting for results", flush=True)
    sys.exit(1)


def merge_phase(results, phase):
    total_ops = sum(r[phase]["total_ops"] for r in results)
    total_errors = sum(r[phase]["errors"] for r in results)
    throughput = sum(r[phase]["throughput"] for r in results)
    elapsed = max(r[phase]["elapsed_secs"] for r in results)
    p50 = max(r[phase]["p50"] for r in results)
    p95 = max(r[phase]["p95"] for r in results)
    p99 = max(r[phase]["p99"] for r in results)
    lat_min = min(r[phase]["min"] for r in results)
    lat_max = max(r[phase]["max"] for r in results)

    num_buckets = len(results[0][phase]["buckets"])
    buckets = [0] * num_buckets
    for r in results:
        for i, v in enumerate(r[phase]["buckets"]):
            buckets[i] += v

    return {
        "total_ops": total_ops,
        "errors": total_errors,
        "throughput": throughput,
        "elapsed_secs": elapsed,
        "p50": p50,
        "p95": p95,
        "p99": p99,
        "min": lat_min,
        "max": lat_max,
        "buckets": buckets,
    }


def print_histogram(buckets):
    max_count = max(buckets) if buckets else 0
    total = sum(buckets)
    bar_width = 40
    print("\n  Latency distribution:")
    for i, count in enumerate(buckets):
        bw = (count * bar_width // max_count) if max_count > 0 else 0
        pct = 100.0 * count / total if total > 0 else 0.0
        bar = "#" * bw
        label = BUCKET_LABELS[i] if i < len(BUCKET_LABELS) else f"  bucket {i}"
        print(f"    {label} |{bar} {count} ({pct:.1f}%)")


def print_phase(label, data):
    print(f"\n===== {label} (consolidated) =====")
    print(f"  Total ops:    {data['total_ops']}")
    print(f"  Errors:       {data['errors']}")
    print(f"  Elapsed:      {data['elapsed_secs']:.3f} s (slowest client)")
    print(f"  Throughput:   {data['throughput']:.0f} ops/sec (sum)")
    print(f"  p50:          {data['p50']} us (max across clients)")
    print(f"  p95:          {data['p95']} us (max across clients)")
    print(f"  p99:          {data['p99']} us (max across clients)")
    print(f"  min:          {data['min']} us")
    print(f"  max:          {data['max']} us")
    print_histogram(data["buckets"])


def main():
    print(f"[consolidator] waiting for {EXPECTED} result files "
          f"in {RESULTS_DIR}...", flush=True)
    files = wait_for_results()

    results = []
    for f in files:
        with open(f) as fh:
            results.append(json.load(fh))
        print(f"[consolidator] loaded {f}", flush=True)

    total_threads = sum(r.get("threads", 1) for r in results)
    total_keys = sum(r.get("num_keys", 0) for r in results)

    print("\n" + "=" * 60)
    print("  CONSOLIDATED BENCHMARK REPORT")
    print("=" * 60)
    print(f"  Clients:        {len(results)}")
    print(f"  Total threads:  {total_threads}")
    print(f"  Total keys:     {total_keys}")

    put_merged = merge_phase(results, "put")
    get_merged = merge_phase(results, "get")

    print_phase("PUT", put_merged)
    print_phase("GET", get_merged)

    print("\n--- Per-Client Summary ---")
    for i, (f, r) in enumerate(zip(files, results)):
        name = os.path.basename(f)
        pt = r["put"]["throughput"]
        gt = r["get"]["throughput"]
        print(f"  {name}: PUT {pt:.0f} ops/s, GET {gt:.0f} ops/s")

    print(flush=True)


if __name__ == "__main__":
    main()

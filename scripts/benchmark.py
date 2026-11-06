#!/usr/bin/env python3
"""Reproducible local measurement; no remote services or third-party packages."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_e2e import Server  # Same real-process harness as the behavioral suite.


def percentile(values, probability):
    return sorted(values)[int((len(values)-1)*probability)]


def environment():
    cpu = platform.processor()
    if sys.platform == "darwin":
        cpu = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
    elif Path("/proc/cpuinfo").exists():
        cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                    if line.startswith("model name")), cpu)
    return {"os": platform.system(), "os_release": platform.release(), "machine": platform.machine(),
            "cpu": cpu, "logical_cpus": os.cpu_count(), "python": platform.python_version(),
            "storage": "local temporary directory; no physical-device power-loss test",
            "filesystem_block_bytes": os.statvfs(tempfile.gettempdir()).f_bsize}


def build_metadata(name):
    binary = ROOT / "build" / name
    metadata = json.loads(Path(str(binary)+".build.json").read_text())
    if hashlib.sha256(binary.read_bytes()).hexdigest() != metadata["binary_sha256"]:
        raise RuntimeError(f"{name} changed after its build metadata was recorded; rebuild with make")
    for source, digest in metadata["source_sha256"].items():
        if hashlib.sha256((ROOT/source).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"{name} was built from different {source}; rebuild with make")
    return metadata


def tcp_measure(directory, count):
    server = Server(directory, "--timeout-ms", "10000")
    try:
        latencies = []
        with server.connect() as sock, sock.makefile("rb") as fp:
            started = time.perf_counter()
            for i in range(count):
                start = time.perf_counter()
                sock.sendall(f"PUT durable {i} {'ab'*32}\n".encode())
                assert fp.readline() == f"OK {i+1}\n".encode()
                latencies.append((time.perf_counter()-start)*1e6)
            elapsed = time.perf_counter()-started
        stats = json.loads(server.command("STATS"))
        assert stats["durable_sequence"] == count
        return {"writes": count, "clients": 1, "persistent_connection": True,
                "writes_per_second": count/elapsed, "elapsed_seconds": elapsed,
                "latency": {"p50_us": percentile(latencies, .50), "p95_us": percentile(latencies, .95),
                            "p99_us": percentile(latencies, .99)}}
    finally:
        server.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--records", type=int, default=250000)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--writes", type=int, default=1000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output", type=Path, default=ROOT / "evidence/benchmark.json")
    args = parser.parse_args()
    if args.runs < 1 or args.runs > 10:
        parser.error("runs must be between 1 and 10")
    results = {"measured_at_utc": datetime.now(timezone.utc).isoformat(), "environment": environment(),
               "builds": {name: build_metadata(name) for name in ("strata", "strata-bench")},
               "method": {"index_dataset_seed": 20261023, "query_seed": 813492,
                          "query_order": "alternate index-first and scan-first; warm in-memory data",
                          "query_shape": "128 streams, shuffled inserts, duplicate timestamps, 1-20 second windows, limits 1-60; 7/135 stream names absent",
                          "comparison": "every returned Record field compared for every query",
                          "dataset_load": "buffered, followed by explicit fsync; excluded from durable write result",
                          "durable_writes": "one fsync per acknowledged record, 32-byte payload, no group commit",
                          "scope": "single-process local measurements; not disk reads, concurrent throughput, or power-loss proof"},
               "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted(list((ROOT / "src").glob("*.cpp")) + list((ROOT / "include").glob("*.hpp")))},
               "runs": []}
    for iteration in range(args.runs):
        with tempfile.TemporaryDirectory(prefix="strata-benchmark-") as temporary:
            native = subprocess.run([str(ROOT / "build/strata-bench"), str(Path(temporary) / "native"),
                                     str(args.records), str(args.queries), str(args.writes)],
                                    capture_output=True, text=True, check=True, timeout=90)
            result = {"native": json.loads(native.stdout),
                      "tcp_durable": tcp_measure(Path(temporary) / "tcp", args.writes)}
            results["runs"].append(result)
            print(f"run {iteration+1}: index {result['native']['indexed']['p50_us']:.2f} us; "
                  f"scan {result['native']['full_scan']['p50_us']:.2f} us; "
                  f"TCP durable {result['tcp_durable']['writes_per_second']:.0f}/s", flush=True)
    results["summary"] = {
        "runs": args.runs,
        "median_index_query_p50_us": statistics.median(r["native"]["indexed"]["p50_us"] for r in results["runs"]),
        "median_scan_query_p50_us": statistics.median(r["native"]["full_scan"]["p50_us"] for r in results["runs"]),
        "median_speedup": statistics.median(r["native"]["median_query_speedup"] for r in results["runs"]),
        "median_native_durable_writes_per_second": statistics.median(r["native"]["durable"]["writes_per_second"] for r in results["runs"]),
        "median_tcp_durable_writes_per_second": statistics.median(r["tcp_durable"]["writes_per_second"] for r in results["runs"])}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results["summary"], indent=2))


if __name__ == "__main__":
    main()

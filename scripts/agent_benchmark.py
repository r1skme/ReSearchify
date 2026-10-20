#!/usr/bin/env python3
"""Optional development benchmark. Python is not a ReSearchify runtime dependency."""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
import time


def summary(samples: list[float]) -> dict[str, float | int]:
    ordered = sorted(samples)
    return {"iterations": len(samples), "p50_ms": statistics.median(samples),
            "p95_ms": ordered[math.ceil(0.95 * len(samples)) - 1],
            "min_ms": ordered[0], "max_ms": ordered[-1]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--query", default="needle_42")
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 10000:
        parser.error("iterations must be 1..10000")
    exe, index = str(args.executable.resolve()), str(args.index.resolve())
    command = [exe, "find", args.query, "--index", index, "--json", "--limit", "8"]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, timeout=120)
    standalone = []
    for _ in range(args.iterations):
        start = time.perf_counter()
        result = subprocess.run(command, check=True, capture_output=True, timeout=120)
        standalone.append((time.perf_counter() - start) * 1000)
        response = json.loads(result.stdout)
        if response["status"] != "ok" or result.stderr:
            raise RuntimeError("Invalid CLI benchmark response")
    messages = [{"id": i, "command": "find", "query": args.query, "limit": 8}
                for i in range(args.iterations)]
    data = "".join(json.dumps(request, ensure_ascii=False) + "\n" for request in messages)
    start = time.perf_counter()
    batch = subprocess.run([exe, "serve", "--stdio", "--index", index], input=data.encode(),
                           check=True, capture_output=True, timeout=120)
    batch_ms = (time.perf_counter() - start) * 1000
    responses = [json.loads(line) for line in batch.stdout.splitlines()]
    if len(responses) != args.iterations or batch.stderr:
        raise RuntimeError("Protocol contamination or missing JSONL records")
    for i, response in enumerate(responses):
        if response.get("id") != i or response["status"] != "ok":
            raise RuntimeError("JSONL request/response mismatch")
    report = {"method": "warm filesystem cache; each CLI call is a fresh process; one JSONL process includes startup and shutdown",
              "query": args.query, "standalone": summary(standalone),
              "jsonl_total_ms": batch_ms, "jsonl_amortized_ms": batch_ms / args.iterations,
              "matching_files": responses[-1]["total"]}
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Alternate baseline/current persistent JSONL readers of ONE immutable index.

Uses only the Python standard library. Run after the index-format compatibility
checks: both readers must support the supplied current-writer index. First
requests, additional warmups and measured exchanges are recorded separately.
No corpus/index is created, updated or deleted and no OS caches are flushed.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import platform
import queue
import random
import subprocess
import sys
import tempfile
import threading
import time

import perf_benchmark as bench


class Service:
    """One bounded-lifetime JSONL process; requests are strictly sequential."""

    def __init__(self, argv: list[str], timeout: float):
        self.timeout = timeout
        self.errors = tempfile.TemporaryFile()
        try:
            self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=self.errors, text=True, encoding="utf-8", bufsize=1)
        except BaseException:
            self.errors.close()
            raise
        self.lines: queue.Queue = queue.Queue()
        self.next_id = 0
        self.closed = False
        self.reader = threading.Thread(target=self._read_lines, daemon=True)
        self.reader.start()

    def _read_lines(self):
        try:
            for line in self.process.stdout:
                self.lines.put(line)
        except BaseException as error:
            self.lines.put(error)
        finally:
            self.lines.put(None)

    def request(self, request: dict) -> dict:
        if self.closed:
            raise RuntimeError("Request after JSONL service shutdown")
        request_id = self.next_id
        self.next_id += 1
        payload = json.dumps({**request, "id": request_id}, ensure_ascii=False) + "\n"
        begin = time.perf_counter()
        self.process.stdin.write(payload)
        self.process.stdin.flush()
        line = self.lines.get(timeout=self.timeout)
        seconds = time.perf_counter() - begin
        if isinstance(line, BaseException):
            raise RuntimeError("JSONL stdout reader failed") from line
        if line is None:
            raise RuntimeError("JSONL service exited before responding")
        response = json.loads(line)
        if response.get("id") != request_id or response.get("status") != "ok":
            raise RuntimeError("Unexpected JSONL response: " + line[:2000])
        if not response.get("execution_complete"):
            raise RuntimeError("Incomplete JSONL search")
        engine_seconds = response.get("metrics", {}).get("seconds")
        if (not isinstance(engine_seconds, (int, float)) or isinstance(engine_seconds, bool)
                or not math.isfinite(engine_seconds) or engine_seconds < 0):
            raise RuntimeError("Missing or invalid internal search duration")
        return {"request_wall_seconds": seconds, "engine_seconds": engine_seconds,
                "response": response}

    def close(self, validate: bool = True) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            if self.process.stdin and not self.process.stdin.closed:
                self.process.stdin.close()
            if not validate and self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=self.timeout)
            self.reader.join(timeout=self.timeout)
            if validate:
                if self.reader.is_alive():
                    raise RuntimeError("Unfinished JSONL stdout reader")
                tail = self.lines.get(timeout=self.timeout)
                if isinstance(tail, BaseException):
                    raise RuntimeError("JSONL stdout reader failed during shutdown") from tail
                if tail is not None:
                    raise RuntimeError("Unexpected extra JSONL records or unfinished reader")
                self.errors.seek(0)
                error_text = self.errors.read().decode("utf-8", "replace")
                if self.process.returncode or error_text:
                    raise RuntimeError("JSONL service failed: " + error_text[-4000:])
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            if self.process.stdout:
                self.process.stdout.close()
            self.errors.close()


def paired_orders(repetitions: int, rng: random.Random) -> list[list[str]]:
    order = ["baseline", "current"]
    rng.shuffle(order)
    # Balance AB/BA exactly for even N and to within one first position for odd N.
    return [list(order if iteration % 2 == 0 else reversed(order)) for iteration in range(repetitions)]


def check_same(sample: dict, reference: dict) -> None:
    if bench.normalized(sample["response"]) != bench.normalized(reference):
        raise RuntimeError("Persistent readers returned different counts/results")


def summarize(samples: list[dict]) -> dict:
    return {"request_wall_ms": bench.distribution([sample["request_wall_seconds"] * 1000 for sample in samples]),
            "engine_wall_ms": bench.distribution([sample["engine_seconds"] * 1000 for sample in samples]),
            "raw": samples}


def compare_case(executables: dict[str, str], index: str, case: dict, repetitions: int,
                 warmups: int, memory_mb: int, timeout: float, rng: random.Random) -> dict:
    request = {"command": "find", "query": case["query"], "limit": 0, "snippets": 0,
               "memory_mb": memory_mb, "regex_steps": 18446744073709551615}
    services, first, warming = {}, {}, {name: [] for name in executables}
    startup_order = list(executables)
    rng.shuffle(startup_order)
    success = False
    try:
        # Construct each process immediately before its first request. Starting
        # both before querying would give the second one hidden startup time.
        for name in startup_order:
            services[name] = Service([executables[name], "serve", "--stdio", "--index", index], timeout)
            first[name] = services[name].request(request)
        reference = first["baseline"]["response"]
        for sample in first.values():
            check_same(sample, reference)
        warmup_order = paired_orders(warmups, rng)
        for order in warmup_order:
            for name in order:
                sample = services[name].request(request)
                check_same(sample, reference)
                warming[name].append(sample)
        samples = {name: [] for name in executables}
        orders = paired_orders(repetitions, rng)
        for order in orders:
            for name in order:
                sample = services[name].request(request)
                check_same(sample, reference)
                samples[name].append(sample)
        success = True
        return {"name": case["name"], "query": case["query"], "matching_files": reference["total"],
                "request": request, "startup_order": startup_order, "first_request": first,
                "warmup_order": warmup_order, "warmups": warming, "paired_order": orders,
                "tools": {name: summarize(values) for name, values in samples.items()}}
    finally:
        errors = []
        for service in services.values():
            try:
                service.close(validate=success)
            except BaseException as error:
                errors.append(error)
        if success and errors:
            raise RuntimeError("Persistent session shutdown failed") from errors[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-executable", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True, help="ONE index used unchanged by both binaries")
    parser.add_argument("--corpus", type=Path, required=True, help="Matching explicit cs_bench generated source")
    parser.add_argument("--term-iterations", type=int, default=201)
    parser.add_argument("--literal-iterations", type=int, default=21)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--seed", type=int, default=1729)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (not 1 <= args.term_iterations <= 10000 or not 1 <= args.literal_iterations <= 10000
            or not 1 <= args.warmups <= 100 or args.memory_mb < 16 or args.timeout <= 0):
        parser.error("Invalid repetitions, warmups, memory budget or timeout")
    if args.output.exists():
        parser.error("Output already exists; preserve the previous evidence and choose a new path")
    executables = {"baseline": str(args.baseline_executable.resolve(strict=True)),
                   "current": str(args.executable.resolve(strict=True))}
    index = str(args.index.resolve(strict=True))
    corpus = args.corpus.resolve(strict=True)
    metadata = (corpus.parent / "benchmark.meta").read_text(encoding="utf-8").split()
    if len(metadata) != 3 or metadata[0] != "RESEARCHIFY_BENCH_1":
        parser.error("Requires the explicit cs_bench generated corpus")
    status = {name: bench.success(bench.command([exe, "status", "--index", index, "--json"], args.timeout))
              for name, exe in executables.items()}
    stable_fields = ("generation", "profile", "files", "lines", "bytes", "root")
    for name, value in status.items():
        if Path(value["root"]).resolve() != corpus or value["files"] != int(metadata[1]):
            parser.error(name + " reader/index does not cover this corpus")
        if any(value[key] != status["baseline"][key] for key in stable_fields):
            parser.error("Readers disagree about the same index's metadata")
    report = {"format": "researchify-paired-jsonl-v1", "complete": False,
              "platform": platform.platform(), "machine": platform.machine(),
              "python": platform.python_version(), "seed": args.seed, "index_path": index,
              "index": {key: status["baseline"][key] for key in stable_fields},
              "binaries": {name: {"path": exe, "sha256": bench.binary_identity(exe)}
                           for name, exe in executables.items()},
              "conditions": {
                  "readers": "Two concurrent persistent processes, strictly sequential request execution",
                  "index": "Same immutable explicitly supplied index for both readers; format compatibility required",
                  "cache": "OS cache uncontrolled; first request and additional warmups excluded; NOT cold-cache",
                  "ordering": "Seeded balanced alternating AB/BA pair order, at most one extra first position",
                  "first_request": "Each service starts immediately before its first request; remaining startup included",
                  "wall": "Client write/flush to complete response line; JSON parsing excluded",
                  "engine": "Unprofiled response metrics.seconds; internal wall time, NOT CPU time",
                  "correctness": "Exact count-only result fields compared between readers and on every exchange",
                  "scope": "No positions, ranking, snippets, full per-file oracle, CPU or RSS claim in this harness",
                  "resources": "Both processes remain resident; use isolated perf_benchmark runs for per-process CPU/RSS",
                  "memory_mb_per_reader": args.memory_mb,
                  "threads": "Each executable's default; profiles and both startup samples retained separately",
              }, "queries": []}
    rng = random.Random(args.seed)
    for case in bench.cases(status["baseline"]["files"]):
        if case["name"] not in ("term", "literal"):
            continue
        repetitions = args.term_iterations if case["name"] == "term" else args.literal_iterations
        row = compare_case(executables, index, case, repetitions, args.warmups,
                           args.memory_mb, args.timeout, rng)
        report["queries"].append(row)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"{case['name']}: {repetitions} alternating persistent pairs completed", file=sys.stderr, flush=True)
    for name, exe in executables.items():
        if bench.binary_identity(exe) != report["binaries"][name]["sha256"]:
            raise RuntimeError(name + " binary changed; incomplete checkpoint preserved")
        final = bench.success(bench.command([exe, "status", "--index", index, "--json"], args.timeout))
        if any(final[key] != status[name][key] for key in stable_fields):
            raise RuntimeError("Shared index changed; incomplete checkpoint preserved")
    report["complete"] = True
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"complete": True, "output": str(args.output),
                      "profile": report["index"]["profile"], "queries": len(report["queries"])}))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Paired fresh-process baseline/current/rg measurements on one immutable corpus.

Requires separately built baseline/current indexes. Every repetition randomizes
tool order with a recorded deterministic seed. No build time is hidden in a
search number. Warm JSONL measurements remain in perf_benchmark.py.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import random
import shutil
import sys

import perf_benchmark as bench


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-executable", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--baseline-index", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--search-threads", type=int)
    parser.add_argument("--rg-threads", type=int)
    parser.add_argument("--query", action="append")
    parser.add_argument("--seed", type=int, default=1729)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--rg", default=shutil.which("rg"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (not 1 <= args.iterations <= 10000 or args.memory_mb < 16 or args.timeout <= 0
            or (args.search_threads is not None and args.search_threads < 0)
            or (args.rg_threads is not None and args.rg_threads < 1)):
        parser.error("invalid iterations, memory, timeout or worker count")
    executables = {"baseline": str(args.baseline_executable.resolve()), "current": str(args.executable.resolve())}
    indexes = {"baseline": str(args.baseline_index.resolve()), "current": str(args.index.resolve())}
    corpus = args.corpus.resolve()
    meta = (corpus.parent / "benchmark.meta").read_text(encoding="utf-8").split()
    if len(meta) != 3 or meta[0] != "RESEARCHIFY_BENCH_1":
        parser.error("requires the explicit cs_bench generated corpus")
    status = {name: bench.success(bench.command([exe, "status", "--index", indexes[name], "--json"], args.timeout))
              for name, exe in executables.items()}
    for name, value in status.items():
        if Path(value["root"]).resolve() != corpus or value["files"] != int(meta[1]):
            parser.error(name + " index is not from this generated corpus")
    for key in ("files", "lines", "bytes", "profile"):
        if status["baseline"][key] != status["current"][key]:
            parser.error("baseline/current corpus differs: " + key)
    suite = bench.cases(status["baseline"]["files"])
    selected = args.query or ["term", "literal", "no_match", "broad_literal", "broad_regex"]
    if set(selected) - {case["name"] for case in suite}:
        parser.error("unknown query case")
    suite = [case for case in suite if case["name"] in selected]
    report = {
        "format": "researchify-paired-v1", "platform": platform.platform(), "machine": platform.machine(),
        "seed": args.seed, "iterations": args.iterations,
        "binaries": {name: {"path": exe, "sha256": bench.binary_identity(exe)} for name, exe in executables.items()},
        "indexes": {name: {**{key: value.get(key) for key in ("generation", "profile", "files", "lines", "bytes")},
                           "status_directory_bytes_including_scratch": value.get("index_bytes"),
                           **bench.index_storage(indexes[name])}
                    for name, value in status.items()},
        "conditions": {
            "execution": "fresh target process per sample, helper startup excluded",
            "cache": "OS cache uncontrolled, explicit warm-up; NOT cold-cache",
            "ordering": "deterministically shuffled baseline/current/rg order for each paired repetition",
            "correctness": "matching-file count on every sample; full path/count checks separately in perf_benchmark.py",
            "comparison": "RS count-only; rg complete occurrence scans via --count-matches, not -l early exit",
            "cpu": "target process CPU", "rss": "process high-water; POSIX can include Python inherited pre-exec floor",
            "build": "excluded; baseline/current creation must be timed separately", "memory_mb": args.memory_mb,
            "current_threads": args.search_threads, "rg_threads": args.rg_threads,
        }, "queries": [],
    }
    if args.rg:
        version = bench.command([args.rg, "--version"], args.timeout)
        if version["returncode"]:
            raise RuntimeError("ripgrep --version failed")
        report["ripgrep"] = version["stdout"].splitlines()[0]
    rng = random.Random(args.seed)
    for case in suite:
        commands = {name: [exe, "find", case["query"], "--index", indexes[name], "--json", "--limit", "0",
                           "--snippets", "0", "--memory-mb", str(args.memory_mb), "--regex-steps", "18446744073709551615"]
                    for name, exe in executables.items()}
        if args.search_threads is not None:
            commands["current"] += ["--threads", str(args.search_threads)]
        if args.rg and "rg" in case:
            commands["rg"] = [args.rg, "--no-config", "--no-ignore", "--hidden", "--text", "--color", "never",
                              "--case-sensitive", "--count-matches", "--null"]
            if args.rg_threads is not None:
                commands["rg"] += ["--threads", str(args.rg_threads)]
            if case.get("fixed"):
                commands["rg"].append("--fixed-strings")
            commands["rg"] += ["--regexp", case["rg"], "."]
        samples = {name: [] for name in commands}
        counts = {}
        metrics = {"baseline": [], "current": []}
        def run(name):
            sample = bench.command(commands[name], args.timeout, str(corpus) if name == "rg" else None)
            if name == "rg":
                if sample["returncode"] not in (0, 1) or sample["stderr"]:
                    raise RuntimeError("ripgrep failed: " + sample["stderr"])
                count = len(bench.parse_rg_counts(sample["stdout"]))
            else:
                response = bench.success(sample)
                count = response["total"]
                sample["engine_metrics"] = response["metrics"]
            if counts and count != next(iter(counts.values())):
                raise RuntimeError("Different matching-file count for " + case["name"] + ": " + name)
            counts[name] = count
            return sample
        # One explicit warm-up per tool; not included in any distribution.
        for name in commands:
            run(name)
        orders = []
        for iteration in range(args.iterations):
            order = list(commands)
            rng.shuffle(order)
            orders.append(order)
            for name in order:
                sample = run(name)
                if name in metrics:
                    metrics[name].append(sample.pop("engine_metrics"))
                samples[name].append(sample)
            print(f"{case['name']}: paired repetition {iteration + 1}/{args.iterations}", file=sys.stderr, flush=True)
        row = {"name": case["name"], "query": case["query"], "matching_files": counts["baseline"],
               "commands": commands, "order": orders, "engine_metrics": metrics,
               "tools": {name: bench.metric_summary(values) for name, values in samples.items()}}
        report["queries"].append(row)
        # Checkpoint completed cases; interrupted runs never masquerade as full.
        report["complete"] = False
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for name, exe in executables.items():
        if bench.binary_identity(exe) != report["binaries"][name]["sha256"]:
            raise RuntimeError(name + " executable changed during measurement; incomplete checkpoint preserved")
        final_status = bench.success(bench.command([exe, "status", "--index", indexes[name], "--json"], args.timeout))
        if any(final_status[key] != status[name][key] for key in ("generation", "files", "lines", "bytes", "profile")):
            raise RuntimeError(name + " index changed during measurement; incomplete checkpoint preserved")
    report["complete"] = True
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

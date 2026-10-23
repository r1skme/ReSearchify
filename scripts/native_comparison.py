#!/usr/bin/env python3
"""Bounded native macOS comparisons using the existing measurement drivers.

All inputs are explicitly supplied local binaries. Generated data, raw reports
and driver logs are preserved; no OS cache flush or speed threshold is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

import perf_benchmark as bench


ORIGINAL = "eb858348169157686e70104af6b64f9539a39fe8"
PREVIOUS = "dbcddf76708b614c26e212fa27b48efa33ddfd58"
SCHEMA = "researchify-native-comparison-v1"


def output(argv):
    return subprocess.run(argv, check=True, capture_output=True, text=True, timeout=60).stdout.strip()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def brief(value):
    return {key: number for key, number in value.items() if key != "samples"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("original", "previous", "current"):
        parser.add_argument("--" + name, type=Path, required=True)
        parser.add_argument("--" + name + "-checkout", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="New directory, never removed")
    parser.add_argument("--machine", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--rg", default="rg")
    parser.add_argument("--files", type=int, default=20000)
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--term-iterations", type=int, default=201)
    args = parser.parse_args()
    if args.files < 64 or not 1 <= args.iterations <= 100 or not 1 <= args.term_iterations <= 1000:
        parser.error("Use >=64 files and bounded positive repetition counts")
    if platform.system() != "Darwin" or output(["uname", "-m"]) != args.machine:
        parser.error("Requires the requested native macOS host, not cross-compilation")
    work = args.output_dir.resolve()
    if work.exists():
        parser.error("Output directory exists; preserve it and use a new path")
    binaries = {name: str(getattr(args, name).resolve(strict=True)) for name in ("original", "previous", "current")}
    generator = str(args.generator.resolve(strict=True))
    rg = shutil.which(args.rg)
    if not rg:
        parser.error("ripgrep is required for the independent comparison")
    provenance = {}
    for name, executable in binaries.items():
        checkout = getattr(args, name + "_checkout").resolve(strict=True)
        commit = output(["git", "-C", str(checkout), "rev-parse", "HEAD"])
        expected = {"original": ORIGINAL, "previous": PREVIOUS}.get(name)
        if expected and commit != expected:
            parser.error(name + " checkout does not match its pinned revision")
        arch = output(["lipo", "-archs", executable])
        if arch != args.machine:
            parser.error(name + " binary is not a single-architecture native executable")
        provenance[name] = {"commit": commit, "path": executable,
                            "sha256": bench.binary_identity(executable), "mach_o_arch": arch}
    if output(["lipo", "-archs", generator]) != args.machine:
        parser.error("Corpus generator is not native")
    work.mkdir(parents=True)
    reports = work / "results"
    reports.mkdir()
    summary = {"format": SCHEMA, "complete": False, "platform": platform.platform(),
               "machine": args.machine, "logical_cpus": os.cpu_count(), "python": platform.python_version(),
               "compiler": output(["clang++", "--version"]), "ripgrep": output([rg, "--version"]),
               "binaries": provenance, "generator_sha256": bench.binary_identity(generator),
               "conditions": {"cache": "Uncontrolled OS cache, explicit excluded warmups; NOT cold-cache",
                              "host": "Native GitHub runner; not a reproduction on a user's Mac",
                              "build": "One separate index-create sample per writer/profile; compilation and generation excluded",
                              "memory_mb": 256, "index_threads": 3, "search_threads": "Executable default",
                              "gates": "Correctness/completion only; no timing threshold on shared runners",
                              "rss": "Timed child RSS may include the Python pre-exec floor; JSONL has no per-reader CPU/RSS claim",
                              "correctness": "Full matching paths/counts vs rg outside timing; exact count-only responses inside drivers"},
               "builds": [], "measurements": []}

    def emit(kind, value):
        print("RESEARCHIFY_NATIVE_SUMMARY " + json.dumps({"format": SCHEMA, "kind": kind, **value}), flush=True)

    def driver(script, name, arguments):
        target = reports / (name + ".json")
        command = [sys.executable, str(Path(__file__).with_name(script)), *map(str, arguments), "--output", str(target)]
        with (reports / (name + ".stdout.log")).open("w") as stdout, (reports / (name + ".stderr.log")).open("w") as stderr:
            subprocess.run(command, stdout=stdout, stderr=stderr, check=True, timeout=1200)
        result = json.loads(target.read_text(encoding="utf-8"))
        if result.get("complete") is not True:
            raise RuntimeError(name + " did not complete")
        for query in result["queries"]:
            row = {"report": target.name, "query": query["name"], "matching_files": query["matching_files"],
                   "binaries": result["binaries"], "tools": {}}
            for tool, metrics in query["tools"].items():
                row["tools"][tool] = {key: brief(value) for key, value in metrics.items()
                                     if key in ("wall_ms", "cpu_ms", "peak_rss_bytes", "request_wall_ms", "engine_wall_ms")}
            if "engine_metrics" in query:
                row["counters"] = {tool: {key: sorted({sample[key] for sample in samples})
                                          for key in ("candidates", "scanned_bytes")}
                                   for tool, samples in query["engine_metrics"].items()}
            summary["measurements"].append(row)
            emit("query", row)
        save(reports / "summary.json", summary)

    try:
        emit("provenance", {key: value for key, value in summary.items() if key not in ("builds", "measurements")})
        save(reports / "summary.json", summary)
        fixture = work / "fixture"
        generated = bench.command([generator, "--work", str(fixture), "--phase", "generate",
                                   "--files", str(args.files), "--lines", "50"], 180)
        save(reports / "generation.json", generated)
        if generated["returncode"] or generated["stderr"]:
            raise RuntimeError("Corpus generation failed")
        corpus = fixture / "source"
        indexes = {}
        for profile, order in (("compact", ("original", "current")), ("fast", ("current", "original"))):
            for name in order:
                index = work / (name + "-" + profile)
                indexes[name, profile] = str(index)
                argv = [binaries[name], "create", "--root", str(corpus), "--index", str(index),
                        "--profile", profile, "--threads", "3", "--memory-mb", "256", "--json"]
                sample = bench.command(argv, 180)
                save(reports / (name + "-" + profile + "-create.json"), {"command": argv, "sample": sample})
                response = bench.success(sample)
                if response.get("execution_complete") is not True or response["files"] != args.files or response["lines"] != args.files * 50:
                    raise RuntimeError("Incomplete or incorrect index creation")
                row = {"writer": name, "profile": profile, "command": argv, "sample": sample,
                       "storage": bench.index_storage(str(index))}
                summary["builds"].append(row)
                save(reports / "index-builds.json", summary["builds"])
                emit("index-build", {"writer": name, "profile": profile,
                                     "wall_ms": sample["wall_seconds"] * 1000,
                                     "cpu_ms": sample["cpu_seconds"] * 1000 if sample["cpu_seconds"] is not None else None,
                                     "peak_rss_bytes": sample["peak_rss_bytes"], "storage": row["storage"]})

        selected = {"compact": ["term", "literal", "no_match", "broad_literal", "broad_regex"],
                    "fast": ["term", "literal", "no_match"]}
        cases = {case["name"]: case for case in bench.cases(args.files)}
        expected = {}
        checks = []
        for profile, queries in selected.items():
            for query in queries:
                case = cases[query]
                if query not in expected:
                    argv = [rg, "--no-config", "--no-ignore", "--hidden", "--text", "--color", "never",
                            "--case-sensitive", "--count-matches", "--null"]
                    if case.get("fixed"):
                        argv.append("--fixed-strings")
                    sample = bench.command(argv + ["--regexp", case["rg"], "."], 120, str(corpus))
                    if sample["returncode"] not in (0, 1) or sample["stderr"]:
                        raise RuntimeError("Independent rg check failed")
                    expected[query] = bench.parse_rg_counts(sample["stdout"])
                    save(reports / ("expected-" + query + ".json"), expected[query])
                for reader in ("original", "current", "previous"):
                    writer = "current" if reader == "previous" else reader
                    actual = bench.all_results(binaries[reader], indexes[writer, profile], case, 120, 256)
                    if actual != expected[query]:
                        save(reports / "mismatching-paths.json", {"reader": reader, "profile": profile, "query": query,
                                                                 "expected": expected[query], "actual": actual})
                        raise RuntimeError("Full path/count mismatch: " + reader + "/" + profile + "/" + query)
                    checks.append({"reader": reader, "writer": writer, "profile": profile, "query": query,
                                   "matching_files": len(actual), "status": "passed",
                                   "path_counts_sha256": hashlib.sha256(json.dumps(actual, sort_keys=True).encode()).hexdigest()})
                    save(reports / "correctness.json", checks)
        emit("correctness", {"passed_full_path_count_checks": len(checks)})
        summary["correctness"] = {"passed_full_path_count_checks": len(checks), "report": "correctness.json"}
        save(reports / "summary.json", summary)

        for profile, queries in selected.items():
            driver("compare_benchmarks.py", "original-" + profile,
                   ["--baseline-executable", binaries["original"], "--executable", binaries["current"],
                    "--baseline-index", indexes["original", profile], "--index", indexes["current", profile],
                    "--corpus", corpus, "--iterations", args.iterations, "--memory-mb", 256, "--timeout", 120,
                    "--rg", rg, *[part for query in queries for part in ("--query", query)]])
        driver("compare_benchmarks.py", "previous-compact",
               ["--baseline-executable", binaries["previous"], "--executable", binaries["current"],
                "--baseline-index", indexes["current", "compact"], "--index", indexes["current", "compact"],
                "--corpus", corpus, "--iterations", args.iterations, "--memory-mb", 256, "--timeout", 120,
                "--rg", "", "--query", "literal", "--query", "broad_regex"])
        for profile in selected:
            driver("compare_sessions.py", "previous-jsonl-" + profile,
                   ["--baseline-executable", binaries["previous"], "--executable", binaries["current"],
                    "--index", indexes["current", profile], "--corpus", corpus, "--memory-mb", 256,
                    "--term-iterations", args.term_iterations, "--literal-iterations", args.iterations,
                    "--warmups", 3, "--timeout", 120])
        if any(bench.binary_identity(exe) != provenance[name]["sha256"] for name, exe in binaries.items()):
            raise RuntimeError("Binary changed during comparison")
        summary["complete"] = True
    except BaseException as error:
        summary["error"] = str(error)
        raise
    finally:
        save(reports / "summary.json", summary)
        emit("completion", {"complete": summary["complete"], "error": summary.get("error"),
                            "summary": "summary.json", "query_rows": len(summary["measurements"])})


if __name__ == "__main__":
    main()

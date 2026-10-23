#!/usr/bin/env python3
"""Native before/after/rg comparison on a reproducible tiny-file surrogate.

The supplied Windows measurements did not include their exact source fixture or
queries. This is an explicitly different, generated eight-line workload. Timing
uses 256 MiB and count-only requests; full correctness pages use 1 GiB outside
timing to avoid 100 repeated scans for 100,000 matching files.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import time

import perf_benchmark as bench
from compare_sessions import Service


CASES = ("term", "broad_term", "literal", "broad_regex", "anchored_regex", "missing_term")


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def process_batch(spec):
    """One isolated helper, sequential target processes, no child overlap."""
    started = time.perf_counter()
    durations, cpus, peaks = [], [], []
    for _ in range(spec["iterations"]):
        sample = bench.worker({"kind": "command", "argv": spec["argv"],
                               "cwd": spec.get("cwd"), "timeout": spec["timeout"]})
        if spec["tool"] == "rg":
            if sample["returncode"] not in (0, 1) or sample["stderr"]:
                raise RuntimeError("rg failed during sequential process batch")
            if bench.parse_rg_counts(sample["stdout"]) != spec["expected"]:
                raise RuntimeError("rg matching paths/counts changed during batch")
        else:
            response = bench.success(sample)
            if not response.get("execution_complete") or response["total"] != len(spec["expected"]):
                raise RuntimeError("Search was incomplete or changed during batch")
        durations.append(sample["wall_seconds"] * 1000)
        cpus.append(sample["cpu_seconds"])
        peaks.append(sample["peak_rss_bytes"])
    # POSIX getrusage is cumulative for this otherwise child-free helper.
    # Windows counters belong to each target handle and must instead be summed.
    cpu = None if any(value is None for value in cpus) else (sum(cpus) if os.name == "nt" else cpus[-1])
    return {"iterations": spec["iterations"], "command": spec["argv"], "cwd": spec.get("cwd"),
            "batch_wall_seconds": time.perf_counter() - started,
            "individual_process_wall_ms": bench.distribution(durations), "target_cpu_seconds": cpu,
            "max_target_peak_rss_bytes": None if any(value is None for value in peaks) else max(peaks),
            "conditions": "One sequential batch, target process startup included. Batch wall includes Python orchestration/result checks; individual process wall excludes post-exit result checks. POSIX RSS may include inherited Python high-water floor. No OS-cache reset."}


def measured_batch(spec):
    completed = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--_batch-worker"],
                               input=json.dumps(spec), capture_output=True, text=True, encoding="utf-8",
                               timeout=spec["timeout"] * spec["iterations"] + 30, check=True)
    return json.loads(completed.stdout)


def architecture(path: Path) -> dict:
    """Check the native executable, not just the compiler target setting."""
    with path.open("rb") as stream:
        head = stream.read(64)
        if platform.system() == "Windows":
            if head[:2] != b"MZ" or len(head) < 64:
                raise ValueError("Not a PE executable")
            stream.seek(struct.unpack_from("<I", head, 0x3c)[0])
            header = stream.read(6)
            if header[:4] != b"PE\0\0" or struct.unpack_from("<H", header, 4)[0] != 0x8664:
                raise ValueError("Requires a native x86-64 PE executable")
            return {"format": "PE", "machine": "x86_64"}
        if platform.system() == "Linux":
            if head[:5] != b"\x7fELF\x02" or head[5] != 1:
                raise ValueError("Requires a little-endian 64-bit ELF executable")
            machine = {62: "x86_64", 183: "aarch64"}.get(struct.unpack_from("<H", head, 18)[0])
            if machine != platform.machine():
                raise ValueError("ELF machine differs from native host")
            return {"format": "ELF", "machine": machine}
    if platform.system() == "Darwin":
        machine = subprocess.check_output(["lipo", "-archs", str(path)], text=True).strip()
        if machine != platform.machine():
            raise ValueError("Mach-O machine differs from native host")
        return {"format": "Mach-O", "machine": machine}
    raise ValueError("Unsupported native platform")


def all_hits(executable, index, case, timeout):
    # Separate correctness budget, never used in the performance distributions.
    request = {"command": "find", "query": case["query"], "limit": 10000,
               "snippets": 0, "sort": "path", "fields": ["path", "matches"],
               "max_output_bytes": 1048576, "memory_mb": 1024,
               "regex_steps": 18446744073709551615}
    service = Service([executable, "serve", "--stdio", "--index", str(index)], timeout)
    found, cursors = {}, set()
    try:
        while True:
            response = service.request(request)["response"]
            for hit in response["results"]:
                if hit["path"] in found:
                    raise RuntimeError("Duplicate correctness path")
                found[hit["path"]] = hit["matches"]
            if not response["has_more"]:
                if len(found) != response["total"] or response.get("output_truncated"):
                    raise RuntimeError("Incomplete correctness result")
                return found
            cursor = response.get("next_cursor")
            if not response["results"] or not cursor or cursor in cursors:
                raise RuntimeError("Non-progressing correctness pagination")
            cursors.add(cursor)
            request["cursor"] = cursor
    finally:
        service.close()


def snippet_check(executable, index, corpus, case, rg, timeout):
    """Check returned lines, byte columns and text against rg's real JSON hits."""
    response = bench.success(bench.command(
        [executable, "find", case["query"], "--index", str(index), "--json", "--limit", "16",
         "--snippets", "32", "--snippet-bytes", "4096", "--sort", "path",
         "--memory-mb", "256", "--regex-steps", "18446744073709551615"], timeout))
    checked = []
    for hit in response["results"]:
        argv = [rg, "--no-config", "--text", "--case-sensitive", "--json"]
        if case.get("fixed"):
            argv.append("--fixed-strings")
        sample = bench.command(argv + ["--regexp", case["rg"], "--", hit["path"]], timeout, str(corpus))
        if sample["returncode"] or sample["stderr"]:
            raise RuntimeError("rg JSON match retrieval failed")
        lines = {}
        for line in sample["stdout"].splitlines():
            event = json.loads(line)
            if event["type"] != "match":
                continue
            data = event["data"]
            # Tiny fixture has LF endings, ASCII and no truncated snippets.
            text = data["lines"]["text"].removesuffix("\n")
            first = data["submatches"][0]["start"] + 1
            lines[data["line_number"]] = {"line": data["line_number"], "column": first, "text": text}
        actual = hit["snippets"]
        if actual != [lines[number] for number in sorted(lines)][:32]:
            raise RuntimeError("Actual match position/text mismatch: " + hit["path"])
        checked.append({"path": hit["path"], "matches": hit["matches"], "snippets": actual})
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--baseline-revision", required=True)
    parser.add_argument("--current-revision", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--files", type=int, default=100000)
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--session-iterations", type=int, default=1000)
    parser.add_argument("--fresh-processes", type=int, default=1000)
    parser.add_argument("--index-threads", type=int, default=4)
    parser.add_argument("--rg", default="rg")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    if (not 64 <= args.files <= 1000000 or not 1 <= args.iterations <= 100
            or not 1 <= args.session_iterations <= 1000 or not 1 <= args.fresh_processes <= 1000):
        parser.error("Invalid bounded fixture/repetition count")
    if not 1 <= args.index_threads <= 32 or args.timeout <= 0:
        parser.error("Invalid thread/timeout value")
    if platform.system() == "Windows" and platform.machine().lower() not in ("amd64", "x86_64"):
        parser.error("Requires native Windows x86-64")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    reports = work / "results"
    reports.mkdir()
    binaries = {name: str(getattr(args, name).resolve(strict=True)) for name in ("baseline", "current")}
    rg = shutil.which(args.rg)
    if not rg:
        parser.error("ripgrep is required")
    summary = {"format": "researchify-tiny-comparison-v1", "complete": False,
               "platform": platform.platform(), "machine": platform.machine(), "logical_cpus": os.cpu_count(),
               "binaries": {name: {"path": exe, "revision": getattr(args, name + "_revision"),
                                   "sha256": bench.binary_identity(exe), "native": architecture(Path(exe))}
                            for name, exe in binaries.items()},
               "ripgrep": bench.command([rg, "--version"], args.timeout)["stdout"],
               "conditions": {"fixture": "Generated eight-line tiny-file surrogate, NOT the supplied Windows corpus",
                              "cache": "Uncontrolled OS cache with excluded warmups; NOT cold-cache",
                              "host": "Native execution; host is recorded, not assumed to be the user's PC",
                              "timed_memory_mb": 256, "correctness_memory_mb": 1024,
                              "correctness": "All matching paths/counts; real rg JSON line/byte-column/text checks for first 16 files per query",
                              "timing": "Fresh process count-only samples, paired shuffled order; separate warm JSONL session",
                              "build": "One independent sample per writer/profile; not a stable build p95",
                              "gates": "Correctness/completion, no performance threshold on shared runners"},
               "builds": [], "correctness": [], "reports": [], "sequential_process_batches": []}
    save(reports / "summary.json", summary)
    try:
        generator = Path(__file__).with_name("generate_tiny_corpus.py")
        generated = bench.command([sys.executable, str(generator), "--work", str(work / "fixture"), "--files", str(args.files)], args.timeout)
        if generated["returncode"] or generated["stderr"]:
            raise RuntimeError("Surrogate generation failed")
        summary["fixture"] = json.loads(generated["stdout"])
        corpus = work / "fixture" / "source"
        cases = {case["name"]: case for case in bench.cases(args.files)}
        for profile, order in (("compact", ("baseline", "current")), ("fast", ("current", "baseline"))):
            indexes = {name: work / (name + "-" + profile) for name in binaries}
            for name in order:
                argv = [binaries[name], "create", "--root", str(corpus), "--index", str(indexes[name]),
                        "--profile", profile, "--threads", str(args.index_threads), "--memory-mb", "256", "--json"]
                sample = bench.command(argv, args.timeout)
                response = bench.success(sample)
                if not response.get("execution_complete") or any(response[field] != summary["fixture"][field] for field in ("files", "bytes", "lines")):
                    raise RuntimeError("Incomplete surrogate indexing")
                summary["builds"].append({"writer": name, "profile": profile, "command": argv,
                                           "sample": sample, "storage": bench.index_storage(str(indexes[name]))})
                save(reports / "summary.json", summary)
            for case_name in CASES:
                case = cases[case_name]
                argv = [rg, "--no-config", "--no-ignore", "--hidden", "--text", "--case-sensitive", "--count-matches", "--null"]
                if case.get("fixed"):
                    argv.append("--fixed-strings")
                expected_sample = bench.command(argv + ["--regexp", case["rg"], "."], args.timeout, str(corpus))
                if expected_sample["returncode"] not in (0, 1) or expected_sample["stderr"]:
                    raise RuntimeError("rg correctness scan failed")
                expected = bench.parse_rg_counts(expected_sample["stdout"])
                for name in binaries:
                    actual = all_hits(binaries[name], indexes[name], case, args.timeout)
                    if actual != expected:
                        save(reports / "mismatch.json", {"tool": name, "profile": profile, "case": case, "actual": actual, "expected": expected})
                        raise RuntimeError("Full matching path/count mismatch")
                    snippets = snippet_check(binaries[name], indexes[name], corpus, case, rg, args.timeout)
                    summary["correctness"].append({"tool": name, "profile": profile, "case": case_name,
                                                   "matching_files": len(actual), "status": "passed", "sampled_actual_hits": snippets,
                                                   "path_counts_sha256": hashlib.sha256(json.dumps(actual, sort_keys=True).encode()).hexdigest()})
                    save(reports / "summary.json", summary)
                    print("correctness", profile, name, case_name, len(actual), flush=True)
            target = reports / (profile + "-process.json")
            argv = [sys.executable, str(Path(__file__).with_name("compare_benchmarks.py")),
                    "--baseline-executable", binaries["baseline"], "--executable", binaries["current"],
                    "--baseline-index", str(indexes["baseline"]), "--index", str(indexes["current"]),
                    "--corpus", str(corpus), "--iterations", str(args.iterations), "--memory-mb", "256",
                    "--timeout", str(args.timeout), "--rg", rg, "--output", str(target),
                    *[part for name in CASES for part in ("--query", name)]]
            with (reports / (profile + "-process.log")).open("w", encoding="utf-8") as output:
                subprocess.run(argv, stdout=output, stderr=output, check=True, timeout=1800)
            measured = json.loads(target.read_text(encoding="utf-8"))
            if not measured.get("complete"):
                raise RuntimeError("Incomplete timing experiment")
            summary["reports"].append(target.name)
            for row in measured["queries"]:
                print("RESEARCHIFY_TINY_SUMMARY " + json.dumps({"profile": profile, "query": row["name"],
                    "tools": {name: {key: {k: v for k, v in value.items() if k != "samples"}
                                      for key, value in metrics.items() if isinstance(value, dict)}
                              for name, metrics in row["tools"].items()}}), flush=True)
            target = reports / (profile + "-sessions.json")
            argv = [sys.executable, str(Path(__file__).with_name("compare_sessions.py")),
                    "--baseline-executable", binaries["baseline"], "--executable", binaries["current"],
                    "--index", str(indexes["current"]), "--corpus", str(corpus), "--memory-mb", "256",
                    "--term-iterations", str(args.session_iterations), "--literal-iterations", str(args.iterations),
                    "--warmups", "3", "--timeout", str(args.timeout), "--output", str(target)]
            with (reports / (profile + "-sessions.log")).open("w", encoding="utf-8") as output:
                subprocess.run(argv, stdout=output, stderr=output, check=True, timeout=1800)
            if not json.loads(target.read_text(encoding="utf-8")).get("complete"):
                raise RuntimeError("Incomplete session experiment")
            summary["reports"].append(target.name)
            term = cases["term"]
            expected_term = {"group_0/module_42.cpp": 2}
            # Both generated occurrences are on separate lines. The fixture
            # minimum is 64 files, so needle_42 is always present exactly once.
            for name in order:
                argv = [binaries[name], "find", term["query"], "--index", str(indexes[name]),
                        "--json", "--limit", "0", "--snippets", "0", "--memory-mb", "256"]
                row = measured_batch({"tool": name, "argv": argv, "timeout": args.timeout,
                                      "iterations": args.fresh_processes, "expected": expected_term})
                summary["sequential_process_batches"].append({"tool": name, "profile": profile, **row})
                save(reports / "summary.json", summary)
            if profile == "compact":
                argv = [rg, "--no-config", "--no-ignore", "--hidden", "--text", "--case-sensitive",
                        "--count-matches", "--null", "--regexp", term["rg"], "."]
                row = measured_batch({"tool": "rg", "argv": argv, "cwd": str(corpus), "timeout": args.timeout,
                                      "iterations": args.fresh_processes, "expected": expected_term})
                summary["sequential_process_batches"].append({"tool": "rg", "profile": "unindexed", **row})
            save(reports / "summary.json", summary)
        if any(bench.binary_identity(exe) != summary["binaries"][name]["sha256"] for name, exe in binaries.items()):
            raise RuntimeError("Executable changed during benchmark")
        summary["complete"] = True
    except BaseException as error:
        summary["error"] = str(error)
        raise
    finally:
        save(reports / "summary.json", summary)


if __name__ == "__main__":
    if sys.argv[1:] == ["--_batch-worker"]:
        print(json.dumps(process_batch(json.load(sys.stdin))))
    else:
        main()

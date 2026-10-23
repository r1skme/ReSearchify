#!/usr/bin/env python3
"""Update-only native comparison on a new owned tiny-file fixture.

This intentionally runs no search benchmark, rg batch or JSONL latency series.
Its independent runner window must not be merged with search-job timings.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import sys

import perf_benchmark as bench
from tiny_comparison import architecture, brief, run_update_comparisons, save


def corpus_state(source, files):
    """Verify every generated path/content, including unexpected leftover files."""
    expected = {f"group_{i // 1000}/module_{i}.cpp" for i in range(files)}
    actual = set()
    for path in source.rglob("*"):
        if path.is_symlink():
            raise RuntimeError("Generated source contains a symlink")
        if path.is_file():
            actual.add(path.relative_to(source).as_posix())
    if actual != expected:
        raise RuntimeError("Generated source has missing or unexpected files")
    digest = hashlib.sha256()
    size = lines = 0
    for i in range(files):
        relative = f"group_{i // 1000}/module_{i}.cpp"
        data = (source / relative).read_bytes()
        digest.update(relative.encode("ascii") + b"\0" + data + b"\0")
        size += len(data)
        lines += data.count(b"\n")
    return {"files": files, "bytes": size, "lines": lines,
            "path_and_content_sha256": digest.hexdigest()}


def run(args):
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    reports = work / "results"
    reports.mkdir()
    source = work / "fixture/source"
    edit = source / "group_0/module_42.cpp"
    summary = {"format": "researchify-tiny-update-comparison-v1", "scope": "updates-only", "complete": False,
               "started_utc": datetime.now(timezone.utc).isoformat(),
               "platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
               "logical_cpus": os.cpu_count(),
               "ci": {key: os.environ.get(key) for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_JOB", "RUNNER_NAME", "RUNNER_OS", "RUNNER_ARCH")},
               "binaries": {}, "drivers": {}, "builds": [], "reports": [], "update_reports": [],
               "source_restoration": {"checked": False, "restored": None},
               "conditions": {"fixture": "New owned generated eight-line tiny-file surrogate, not the user's corpus",
                              "window": "Independent update-only runner; never combine these timings with the search job as one runner session",
                              "cache": "Uncontrolled OS cache with excluded setup/warmups; not cold-cache",
                              "timing": "Existing paired update driver; complete CLI lifetime; equal scopes; full correctness and restoration checks",
                              "index": "Only current-writer compact/fast base indexes; both versions receive independent copies",
                              "build": "One current-writer setup sample per profile, separate from update timing",
                              "memory_mb": 256, "index_threads": args.index_threads, "update_iterations": args.update_iterations,
                              "search_series": False, "gates": "Correctness/completion only, no throughput threshold"}}
    save(reports / "summary.json", summary)
    failure = None
    original_mtime = None
    try:
        binaries = {name: str(getattr(args, name).resolve(strict=True)) for name in ("baseline", "current")}
        summary["binaries"] = {name: {"path": exe, "revision": getattr(args, name + "_revision"),
                                      "sha256": bench.binary_identity(exe), "native": architecture(Path(exe))}
                               for name, exe in binaries.items()}
        driver_names = ("tiny_update_comparison.py", "generate_tiny_corpus.py", "tiny_comparison.py",
                        "update_benchmark.py", "perf_benchmark.py", "compare_sessions.py", "maintenance_benchmark.py")
        summary["drivers"] = {name: {"path": str(Path(__file__).with_name(name)),
                                     "sha256": bench.binary_identity(str(Path(__file__).with_name(name)))}
                              for name in driver_names}
        save(reports / "summary.json", summary)
        command = [sys.executable, str(Path(__file__).with_name("generate_tiny_corpus.py")),
                   "--work", str(work / "fixture"), "--files", str(args.files)]
        generated = bench.command(command, args.timeout)
        summary["generation"] = {"command": command, "sample": generated}
        save(reports / "summary.json", summary)
        if generated["returncode"] or generated["stderr"]:
            raise RuntimeError("Surrogate generation failed")
        summary["fixture"] = json.loads(generated["stdout"])
        if summary["fixture"].get("format") != "researchify-tiny-corpus-v1" or summary["fixture"].get("files") != args.files:
            raise RuntimeError("Unexpected generated fixture manifest")
        save(reports / "fixture.json", summary["fixture"])
        original_mtime = edit.stat().st_mtime_ns
        for profile in ("compact", "fast"):
            index = work / ("current-" + profile)
            command = [binaries["current"], "create", "--root", str(source), "--index", str(index),
                       "--profile", profile, "--threads", str(args.index_threads), "--memory-mb", "256", "--json"]
            record = {"writer": "current", "profile": profile, "command": command, "complete": False}
            summary["builds"].append(record)
            save(reports / "summary.json", summary)
            record["sample"] = bench.command(command, args.timeout)
            response = bench.success(record["sample"])
            if not response.get("execution_complete") or any(response[field] != summary["fixture"][field] for field in ("files", "bytes", "lines")):
                raise RuntimeError("Incomplete surrogate indexing")
            record["storage"] = bench.index_storage(str(index))
            record["complete"] = True
            save(reports / "summary.json", summary)
        run_update_comparisons(args, binaries, work, reports, summary)
        if ([row["profile"] for row in summary["update_reports"]] != ["compact", "fast"]
                or not all(row["complete"] and row["source_restored"] for row in summary["update_reports"])):
            raise RuntimeError("Incomplete update-only profile series")
        for name, executable in binaries.items():
            if bench.binary_identity(executable) != summary["binaries"][name]["sha256"]:
                raise RuntimeError("Executable changed during update comparison")
        for name, identity in summary["drivers"].items():
            if bench.binary_identity(identity["path"]) != identity["sha256"]:
                raise RuntimeError("Driver changed during update comparison: " + name)
        summary["complete"] = True
    except BaseException as error:
        failure = error
        summary["error"] = type(error).__name__ + ": " + str(error)
    finally:
        if "fixture" in summary and source.is_dir() and original_mtime is not None:
            try:
                actual = corpus_state(source, args.files)
                actual_mtime = edit.stat().st_mtime_ns
                restored = all(actual[key] == summary["fixture"][key] for key in actual) and actual_mtime == original_mtime
                summary["source_restoration"] = {"checked": True, "restored": restored,
                                                  "actual": actual, "original_edit_mtime_ns": original_mtime,
                                                  "final_edit_mtime_ns": actual_mtime}
                if not restored:
                    raise RuntimeError("Full generated source was not restored")
            except BaseException as error:
                summary["source_restoration"].update({"checked": True, "restored": False,
                                                       "error": type(error).__name__ + ": " + str(error)})
                if failure is None:
                    failure = error
                    summary["error"] = type(error).__name__ + ": " + str(error)
        if failure is not None or not summary["source_restoration"]["restored"]:
            summary["complete"] = False
        summary["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(reports / "summary.json", summary)
    if failure is not None:
        raise failure
    if not summary["complete"]:
        raise RuntimeError("Incomplete update-only experiment")
    print("RESEARCHIFY_TINY_UPDATES_ONLY_SUMMARY " + json.dumps({
        "format": summary["format"], "scope": summary["scope"], "complete": True,
        "started_utc": summary["started_utc"], "finished_utc": summary["finished_utc"], "ci": summary["ci"],
        "binaries": summary["binaries"], "fixture": summary["fixture"], "conditions": summary["conditions"],
        "source_restoration": summary["source_restoration"], "update_reports": brief(summary["update_reports"])}), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--baseline-revision", required=True)
    parser.add_argument("--current-revision", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--files", type=int, default=100000)
    parser.add_argument("--update-iterations", type=int, default=7)
    parser.add_argument("--index-threads", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    if not 64 <= args.files <= 1000000 or not 1 <= args.update_iterations <= 100 or not 1 <= args.index_threads <= 32 or args.timeout <= 0:
        parser.error("Invalid bounded fixture/repetition/thread/timeout setting")
    if platform.system() == "Windows" and platform.machine().lower() not in ("amd64", "x86_64"):
        parser.error("Requires native Windows x86-64")
    run(args)


if __name__ == "__main__":
    main()

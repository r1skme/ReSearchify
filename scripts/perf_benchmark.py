#!/usr/bin/env python3
"""Reproducible CLI/JSONL benchmark; Python is a development-only dependency.

Example (after cs_bench --phase generate/create):
  python scripts/perf_benchmark.py --executable build/rs --index bench/index \
      --corpus bench/source --iterations 21 --output bench/results.json

Use a baseline-built index for an old/new binary comparison when the format is
compatible. Index construction is deliberately NOT hidden in search timings;
cs_bench reports it separately. No OS caches are flushed by this script.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import queue
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time


def binary_identity(path: str) -> str:
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def distribution(samples: list[float]) -> dict:
    if not samples:
        raise ValueError("empty distribution")
    ordered = sorted(samples)
    return {"iterations": len(samples), "median": statistics.median(samples),
            "p95": ordered[math.ceil(.95 * len(samples)) - 1],
            "min": ordered[0], "max": ordered[-1], "samples": samples}


def index_storage(directory: str) -> dict:
    """Logical file bytes; classify leftover transactions separately, never delete."""
    base = Path(directory)
    committed = scratch = spill = coordination = literal = 0
    for path in base.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        size = path.stat().st_size
        parts = path.relative_to(base).parts
        first = parts[0]
        if first.startswith("transaction-"):
            scratch += size
        elif len(parts) >= 3 and first.startswith("segment-") and parts[1] in ("sort", "literal-sort"):
            spill += size
        elif first in ("pins", "writer.lock"):
            coordination += size
        else:
            committed += size
            if path.name == "literal.tbl":
                literal += size
    return {"committed_index_bytes": committed, "transaction_scratch_bytes": scratch,
            "spill_scratch_bytes": spill,
            "coordination_bytes": coordination, "literal_sidecar_bytes": literal,
            "total_file_bytes": committed + scratch + spill + coordination,
            "measurement": "logical file sizes; transaction-* scratch, nested segment sort/literal-sort spills and reader/writer coordination separated; no deletion"}


def windows_handle(pid: int):
    if sys.platform != "win32":
        return None
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    return kernel.OpenProcess(0x0400 | 0x0010, False, pid)


def resources(handle) -> dict:
    """Called in an otherwise child-free worker, AFTER its only target exits."""
    if sys.platform != "win32":
        import resource
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        # Linux reports KiB; Darwin reports bytes. Do not subtract high-water marks.
        multiplier = 1 if sys.platform == "darwin" else 1024
        return {"cpu_seconds": usage.ru_utime + usage.ru_stime,
                "peak_rss_bytes": int(usage.ru_maxrss * multiplier),
                "resource_method": "isolated child getrusage(RUSAGE_CHILDREN); RSS may include inherited Python pre-exec high-water floor"}
    result = {"cpu_seconds": None, "peak_rss_bytes": None,
              "resource_method": "Windows process counters (null if inaccessible)"}
    if not handle:
        return result
    from ctypes import wintypes
    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    created, exited, kernel_time, user_time = (wintypes.FILETIME() for _ in range(4))
    if kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                              ctypes.byref(kernel_time), ctypes.byref(user_time)):
        ticks = lambda value: (value.dwHighDateTime << 32) + value.dwLowDateTime
        result["cpu_seconds"] = (ticks(kernel_time) + ticks(user_time)) / 10_000_000
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
        result["peak_rss_bytes"] = counters.PeakWorkingSetSize
    kernel.CloseHandle(handle)
    return result


def worker(spec: dict) -> dict:
    """One fresh Python helper per target: helper startup is outside timed work."""
    timeout = spec["timeout"]
    started = time.perf_counter()
    if spec["kind"] == "command":
        process = subprocess.Popen(spec["argv"], cwd=spec.get("cwd"),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        handle = windows_handle(process.pid)
        try:
            output, errors = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            resources(handle)
            raise
        wall = time.perf_counter() - started
        return {"wall_seconds": wall, "returncode": process.returncode,
                "stdout": output.decode("utf-8", "strict"), "stderr": errors.decode("utf-8", "replace"),
                **resources(handle)}
    responses, latencies = [], []
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(spec["argv"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=errors, text=True, encoding="utf-8", bufsize=1)
        handle = windows_handle(process.pid)
        lines: queue.Queue = queue.Queue()
        def read_lines():
            try:
                for line in process.stdout:
                    lines.put(line)
            except BaseException as error:
                lines.put(error)
            finally:
                lines.put(None)
        reader = threading.Thread(target=read_lines, daemon=True)
        reader.start()
        try:
            for request in spec["requests"]:
                payload = json.dumps(request, ensure_ascii=False) + "\n"
                begin = time.perf_counter()
                process.stdin.write(payload)
                process.stdin.flush()
                line = lines.get(timeout=timeout)
                latencies.append(time.perf_counter() - begin)
                if isinstance(line, BaseException):
                    raise RuntimeError("JSONL stdout reader failed") from line
                if line is None:
                    raise RuntimeError("JSONL subprocess exited before its response")
                response = json.loads(line)
                if response.get("id") != request["id"] or response.get("status") != "ok":
                    raise RuntimeError("Invalid JSONL response: " + line)
                responses.append(response)
            process.stdin.close()
            process.wait(timeout=timeout)
            reader.join(timeout=timeout)
            if reader.is_alive():
                raise RuntimeError("Unfinished JSONL stdout reader")
            tail = lines.get(timeout=timeout)
            if isinstance(tail, BaseException):
                raise RuntimeError("JSONL stdout reader failed during shutdown") from tail
            if tail is not None:
                raise RuntimeError("Unexpected extra JSONL records or incomplete shutdown")
            errors.seek(0)
            error_text = errors.read().decode("utf-8", "replace")
            if process.returncode or error_text:
                raise RuntimeError("JSONL process failed: " + error_text)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        measured = resources(handle)
    return {"wall_seconds": time.perf_counter() - started, "responses": responses,
            "request_seconds": latencies, **measured}


def measure(spec: dict) -> dict:
    count = len(spec.get("requests", [])) or 1
    completed = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--_worker"],
                               input=json.dumps(spec), text=True, encoding="utf-8",
                               capture_output=True, timeout=spec["timeout"] * count + 30)
    if completed.returncode:
        raise RuntimeError("Measurement worker failed:\n" + completed.stderr)
    return json.loads(completed.stdout)


def command(argv: list[str], timeout: float, cwd: str | None = None) -> dict:
    return measure({"kind": "command", "argv": argv, "cwd": cwd, "timeout": timeout})


def success(result: dict) -> dict:
    if result["returncode"] or result["stderr"]:
        raise RuntimeError("Command failed: " + result["stderr"] + result["stdout"])
    response = json.loads(result["stdout"])
    if response.get("status") != "ok":
        raise RuntimeError("Unsuccessful response: " + result["stdout"])
    return response


def cases(files: int) -> list[dict]:
    first, second = min(42, files - 1), min(43, files - 1)
    needle = f"needle_{first}"
    return [
        {"name": "term", "query": needle, "rg": rf"\b{needle}\b"},
        {"name": "literal", "query": f'"{needle} = 0;"', "rg": f"{needle} = 0;", "fixed": True},
        {"name": "short_literal", "query": '"0;"', "rg": "0;", "fixed": True},
        {"name": "no_match", "query": '"ReSearchify_missing_2026"', "rg": "ReSearchify_missing_2026", "fixed": True},
        {"name": "and", "query": f"bucket{first % 100} AND common"},
        {"name": "or", "query": f"{needle} OR needle_{second}" if first != second else needle,
         "rg": rf"\b(?:{needle}|needle_{second})\b"},
        {"name": "not", "query": f"{needle} AND NOT absent_symbol"},
        {"name": "phrase", "query": f'{needle} AND "return common;"'},
        {"name": "regex", "query": f"{needle} AND /function_[0-9]+_[0-9]+/"},
        {"name": "broad_term", "query": "common", "rg": r"\bcommon\b"},
        {"name": "broad_literal", "query": '"return common;"', "rg": "return common;", "fixed": True},
        {"name": "broad_regex", "query": "/needle_[0-9]+/", "rg": "needle_[0-9]+"},
        {"name": "anchored_regex", "query": "/^int needle_[0-9]+ = 0;$/", "rg": "^int needle_[0-9]+ = 0;$"},
        {"name": "missing_term", "query": "ReSearchify_missing_2026", "rg": r"\bReSearchify_missing_2026\b"},
        {"name": "regex_no_match", "query": "/missing_[0-9]+/", "rg": "missing_[0-9]+"},
    ]


def normalized(response: dict) -> dict:
    """Exact non-timing response content to detect changes between samples."""
    return {key: response.get(key) for key in ("total", "returned", "has_more", "next_cursor", "results")}


def validate_noop_session(responses: list[dict], reference: dict) -> None:
    if not responses or len(responses) % 2 != 1:
        raise RuntimeError("Incomplete update/find session")
    if any(normalized(response) != normalized(reference) for response in responses[::2]):
        raise RuntimeError("Results changed during strict no-op update session")
    for response in responses[1::2]:
        if any(response.get(field) != 0 for field in ("added", "changed", "removed", "tokenized")):
            raise RuntimeError("Update changed the index; this is not a strict no-op benchmark")


def parse_rg_counts(output: str) -> dict[str, int]:
    counts = {}
    position = 0
    while position < len(output):
        separator = output.find("\0", position)
        end = output.find("\n", separator + 1) if separator >= 0 else -1
        if separator < 0 or end < 0:
            raise ValueError("Malformed ripgrep --null --count-matches output")
        # Never copy the remaining output: large result sets otherwise turn
        # correctness parsing into O(n^2) work outside the target measurements.
        path, value = output[position:separator], output[separator + 1:end]
        position = end + 1
        normalized_path = path.replace("\\", "/")
        if normalized_path.startswith("./"):
            normalized_path = normalized_path[2:]
        if normalized_path in counts:
            raise ValueError("Duplicate ripgrep path")
        counts[normalized_path] = int(value)
    return counts


def metric_summary(samples: list[dict]) -> dict:
    result = {"wall_ms": distribution([s["wall_seconds"] * 1000 for s in samples])}
    for field, scale, name in (("cpu_seconds", 1000, "cpu_ms"), ("peak_rss_bytes", 1, "peak_rss_bytes")):
        result[name] = distribution([s[field] * scale for s in samples]) if all(s[field] is not None for s in samples) else None
    result["resource_method"] = samples[0]["resource_method"]
    return result


def correctness_page_limit(memory_mb: int) -> int:
    # Leave ample room for maximum lines, reader/catalog state and bookkeeping.
    # Use the stricter current result-buffer share; this also fits older readers.
    return max(1, min(1000, (memory_mb * 1048576 // 3 - 8 * 1048576) // (32768 + 512)))


def all_results(executable: str, index: str, case: dict, timeout: float, memory_mb: int) -> dict[str, int]:
    # Correctness pass is deliberately outside all timings. Keyset pages cannot
    # silently discard files when an output-size limit truncates a page.
    # The core budgets worst-case path storage, not actual short generated paths.
    page_limit = correctness_page_limit(memory_mb)
    request = {"id": 0, "command": "find", "query": case["query"], "limit": page_limit,
               "snippets": 0, "sort": "path", "fields": ["path", "matches"],
               "max_output_bytes": 1048576, "regex_steps": 18446744073709551615,
               "memory_mb": memory_mb}
    found = {}
    while True:
        response = measure({"kind": "jsonl", "argv": [executable, "serve", "--stdio", "--index", index],
                            "requests": [request], "timeout": timeout})["responses"][0]
        for hit in response["results"]:
            if hit["path"] in found:
                raise RuntimeError("Duplicate path in paginated correctness pass")
            found[hit["path"]] = hit["matches"]
        if not response["has_more"]:
            if len(found) != response["total"]:
                raise RuntimeError("Incomplete paginated correctness pass")
            return found
        cursor = response["next_cursor"]
        if not cursor or cursor == request.get("cursor"):
            raise RuntimeError("Pagination made no progress")
        request["cursor"] = cursor
        request["id"] += 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, help="cs_bench generated source directory; enables equivalent ripgrep checks")
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--search-threads", type=int, default=None)
    parser.add_argument("--query", action="append", help="Benchmark case name (repeatable); default all")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--rg", default=shutil.which("rg"))
    parser.add_argument("--rg-threads", type=int, help="Optional explicit ripgrep worker cap; default uses ripgrep's adaptive default")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--skip-full-check", action="store_true", help="Skip untimed full path/count comparison; never claims full correctness")
    parser.add_argument("--interleave-update", action="store_true", help="Also measure explicit writable JSONL update/find sessions on the generated corpus")
    args = parser.parse_args()
    if (not 1 <= args.iterations <= 10000 or args.timeout <= 0 or args.memory_mb < 16
            or (args.search_threads is not None and args.search_threads < 0)
            or (args.rg_threads is not None and args.rg_threads < 1)):
        parser.error("iterations must be 1..10000, timeout positive, memory-mb >= 16")
    executable, index = str(args.executable.resolve()), str(args.index.resolve())
    metadata = success(command([executable, "status", "--index", index, "--json"], args.timeout))
    suite = cases(metadata["files"])
    if not metadata["files"]:
        parser.error("empty index is not a generated benchmark corpus")
    if args.query:
        unknown = set(args.query) - {case["name"] for case in suite}
        if unknown:
            parser.error("unknown cases: " + ", ".join(sorted(unknown)))
        suite = [case for case in suite if case["name"] in args.query]
    schema = success(command([executable, "schema", "find", "--json"], args.timeout))["schema"]
    fields = schema.get("inputSchema", {}).get("properties", {})
    if args.search_threads is not None and "threads" not in fields:
        parser.error("this executable does not support find --threads; omit it for baseline binaries")
    corpus = args.corpus.resolve() if args.corpus else None
    if args.interleave_update and not corpus:
        parser.error("--interleave-update requires the explicit generated --corpus")
    if corpus:
        meta = (corpus.parent / "benchmark.meta").read_text(encoding="utf-8").split()
        if len(meta) != 3 or meta[0] != "RESEARCHIFY_BENCH_1" or int(meta[1]) != metadata["files"]:
            parser.error("ripgrep comparison requires the matching cs_bench generated corpus")
        if Path(metadata["root"]).resolve() != corpus:
            parser.error("index and ripgrep corpus root differ")
    report = {"format": "researchify-perf-v1", "executable": executable,
              "executable_sha256": binary_identity(executable),
              "platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
              "index": {key: metadata.get(key) for key in ("generation", "files", "lines", "bytes", "profile", "index_bytes")},
              "index_storage": index_storage(index),
              "conditions": {"os_cache": "uncontrolled; explicit warm-up; NOT cold-cache",
                             "memory_mb": args.memory_mb,
                             "search_threads": args.search_threads if args.search_threads is not None else "executable default",
                             "rg_threads": args.rg_threads if args.rg_threads is not None else "ripgrep default",
                             "standalone": "fresh target process per sample, including startup and shutdown",
                             "jsonl": "one persistent process; first request/warmups excluded from latency distribution",
                             "rss": "per process high-water mark, not allocator budget or per-request RSS; POSIX fallback can include inherited Python pre-exec memory floor",
                             "cpu": "target process CPU; JSONL reports whole-session CPU, not per-request CPU",
                             "build": "excluded; measure create/update separately with cs_bench",
                             "comparison": "count-only matching-file result; rg --count-matches forces complete occurrence scans, never -l early exit",
                             "full_rg_checks_requested": not args.skip_full_check,
                             "update_session": "strict no-op update then find; no source changes" if args.interleave_update else "not measured"}, "queries": []}
    rg_version = None
    if corpus and args.rg:
        version = command([args.rg, "--version"], args.timeout)
        if version["returncode"]:
            raise RuntimeError("ripgrep --version failed")
        rg_version = version["stdout"].splitlines()[0]
    report["ripgrep"] = rg_version
    for case in suite:
        argv = [executable, "find", case["query"], "--index", index, "--json", "--limit", "0", "--snippets", "0",
                "--memory-mb", str(args.memory_mb), "--regex-steps", "18446744073709551615"]
        if args.search_threads is not None:
            argv += ["--threads", str(args.search_threads)]
        warmup = success(command(argv, args.timeout))
        samples, metrics = [], []
        for _ in range(args.iterations):
            sample = command(argv, args.timeout)
            response = success(sample)
            if normalized(response) != normalized(warmup):
                raise RuntimeError("CLI results changed between repetitions: " + case["name"])
            samples.append(sample)
            metrics.append(response["metrics"])
        row = {"name": case["name"], "query": case["query"], "command": argv, "matching_files": warmup["total"],
               "standalone": metric_summary(samples), "engine_metrics": metrics,
               "rg": None, "correctness": "repeated CLI/JSONL exact count consistency"}
        request = {"command": "find", "query": case["query"], "limit": 0, "snippets": 0,
                   "memory_mb": args.memory_mb, "regex_steps": 18446744073709551615}
        if args.search_threads is not None:
            request["threads"] = args.search_threads
        session = measure({"kind": "jsonl", "argv": [executable, "serve", "--stdio", "--index", index],
                           "requests": [{**request, "id": i} for i in range(args.iterations + 1)], "timeout": args.timeout})
        if any(normalized(response) != normalized(warmup) for response in session["responses"]):
            raise RuntimeError("CLI/JSONL results differ: " + case["name"])
        row["jsonl"] = {"request_wall_ms": distribution([value * 1000 for value in session["request_seconds"][1:]]),
                        "first_request_ms": session["request_seconds"][0] * 1000,
                        "first_request_engine_metrics": session["responses"][0]["metrics"],
                        "engine_metrics": [response["metrics"] for response in session["responses"][1:]],
                        "whole_session_wall_ms": session["wall_seconds"] * 1000,
                        "whole_session_cpu_ms": session["cpu_seconds"] * 1000 if session["cpu_seconds"] is not None else None,
                        "whole_session_peak_rss_bytes": session["peak_rss_bytes"]}
        if "profile" in fields:
            profiled = measure({"kind": "jsonl", "argv": [executable, "serve", "--stdio", "--index", index],
                                "requests": [{**request, "id": 0, "profile": True}], "timeout": args.timeout})["responses"][0]
            if normalized(profiled) != normalized(warmup):
                raise RuntimeError("Profiling changed search results: " + case["name"])
            row["separate_profile_sample"] = profiled.get("profile", profiled.get("metrics", {}).get("profile"))
        if args.interleave_update:
            mixed_requests = [{**request, "id": 0}]
            for iteration in range(args.iterations):
                mixed_requests += [{"id": iteration * 2 + 1, "command": "update", "memory_mb": args.memory_mb},
                                   {**request, "id": iteration * 2 + 2}]
            mixed = measure({"kind": "jsonl", "argv": [executable, "serve", "--stdio", "--allow-write", "--index", index],
                             "requests": mixed_requests, "timeout": args.timeout})
            validate_noop_session(mixed["responses"], warmup)
            row["jsonl_update_find"] = {
                "update_wall_ms": distribution([value * 1000 for value in mixed["request_seconds"][1::2]]),
                "find_after_update_wall_ms": distribution([value * 1000 for value in mixed["request_seconds"][2::2]]),
                "whole_session_cpu_ms": mixed["cpu_seconds"] * 1000 if mixed["cpu_seconds"] is not None else None,
                "whole_session_peak_rss_bytes": mixed["peak_rss_bytes"],
                "note": "serve --allow-write only; MCP is read-only; no-op updates still explicitly refresh Service"}
        if corpus and args.rg and "rg" in case:
            rg_argv = [args.rg, "--no-config", "--no-ignore", "--hidden", "--text", "--color", "never",
                       "--case-sensitive", "--count-matches", "--null"]
            if args.rg_threads is not None:
                rg_argv += ["--threads", str(args.rg_threads)]
            if case.get("fixed"):
                rg_argv.append("--fixed-strings")
            rg_argv += ["--regexp", case["rg"], "."]
            rg_reference = command(rg_argv, args.timeout, str(corpus))
            if rg_reference["returncode"] not in (0, 1) or rg_reference["stderr"]:
                raise RuntimeError("ripgrep failed: " + rg_reference["stderr"])
            expected = parse_rg_counts(rg_reference["stdout"])
            if len(expected) != warmup["total"]:
                raise RuntimeError("ripgrep/RS matching file counts differ: " + case["name"])
            if not args.skip_full_check:
                actual = all_results(executable, index, case, args.timeout, args.memory_mb)
                if actual != expected:
                    raise RuntimeError("ripgrep/RS paths or occurrence counts differ: " + case["name"])
                row["correctness"] = "full matching path + occurrence-count equality against ripgrep, outside timing"
            rg_samples = []
            for _ in range(args.iterations):
                sample = command(rg_argv, args.timeout, str(corpus))
                if sample["returncode"] not in (0, 1) or sample["stderr"] or parse_rg_counts(sample["stdout"]) != expected:
                    raise RuntimeError("ripgrep result changed")
                rg_samples.append(sample)
            row["rg"] = {"command": rg_argv, **metric_summary(rg_samples),
                         "note": "rg reports all per-file counts; RS returns total only; no ranking/snippets comparison"}
        report["queries"].append(row)
        print(f"{case['name']}: {warmup['total']} files; CLI median {row['standalone']['wall_ms']['median']:.3f} ms; "
              f"JSONL median {row['jsonl']['request_wall_ms']['median']:.3f} ms", file=sys.stderr, flush=True)
    if binary_identity(executable) != report["executable_sha256"]:
        raise RuntimeError("Executable changed during measurement; rerun against an immutable build")
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    if sys.argv[1:] == ["--_worker"]:
        print(json.dumps(worker(json.load(sys.stdin)), ensure_ascii=False))
    else:
        main()

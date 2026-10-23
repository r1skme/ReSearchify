#!/usr/bin/env python3
"""Capture and benchmark only the explicitly approved ReSearchify source tree.

This standalone standard-library development tool copies src/include/tests/docs
from --source-checkout into a new scratch directory. It never indexes the user's
workspace broadly, modifies source files, flushes caches, or deletes artifacts.
Use --phase capture and later --phase measure to coordinate quiet timing windows.
The supplied executables must be trusted, already-built baseline/current rs.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import shutil
import statistics
import subprocess
import sys
import time


FORMAT = "researchify-real-source-v1"
ROOTS = ("src", "include", "tests", "docs")
EXTENSIONS = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".md", ".cmake", ".txt"}
EXCLUDED = {".git", "build", "bench", "benchfixtures", "bench-fixtures", "benchmark-fixtures"}
ABSENT = "rs_real_source_absent_7d94cfe84f4b439e"
CASES = (
    {"name": "term_return", "query": "return", "kind": "term", "pattern": "return"},
    {"name": "literal_std_string", "query": '"std::string"', "kind": "literal", "pattern": "std::string"},
    {"name": "literal_std_scope", "query": '"std::"', "kind": "literal", "pattern": "std::"},
    {"name": "absent_literal", "query": '"' + ABSENT + '"', "kind": "literal", "pattern": ABSENT},
    {"name": "regex_error_codes", "query": "/E_[A-Z_]+/", "kind": "regex", "pattern": "E_[A-Z_]+"},
)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def dump_new(path: Path, value: dict) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, ensure_ascii=False)
        output.write("\n")


def checkpoint(path: Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    dump_new(temporary, value)
    os.replace(temporary, path)


def distribution(values: list[float]) -> dict:
    ordered = sorted(values)
    return {"n": len(values), "median": statistics.median(values),
            "p95": ordered[math.ceil(len(values) * .95) - 1],
            "min": ordered[0], "max": ordered[-1], "samples": values}


def normalized_lines(data: bytes) -> list[str]:
    # Match the engine's LF-only line splitting, first-line BOM and trailing CR.
    if not data:
        return []
    lines = data.decode("utf-8", "strict").split("\n")
    if lines[-1] == "" and data.endswith(b"\n"):
        lines.pop()
    lines = [line[:-1] if line.endswith("\r") else line for line in lines]
    if lines and lines[0].startswith("\ufeff"):
        lines[0] = lines[0][1:]
    return lines


def capture(checkout: Path, work: Path) -> dict:
    if work.exists():
        raise ValueError("Capture destination must not exist; no existing artifacts are overwritten")
    checkout = checkout.resolve(strict=True)
    if not (checkout / "src" / "engine.cpp").is_file():
        raise ValueError("--source-checkout must be the approved ReSearchify checkout")
    # Make the caller choose a scratch directory outside the source checkout.
    if work.resolve().is_relative_to(checkout):
        raise ValueError("Place --output-dir outside the source checkout")
    selected = []
    for top in ROOTS:
        start = checkout / top
        if start.is_symlink() or not start.is_dir():
            raise ValueError("Missing or symlinked approved source directory: " + top)
        for directory, directories, filenames in os.walk(start, followlinks=False):
            directories[:] = sorted(name for name in directories
                                    if name.lower() not in EXCLUDED
                                    and not name.lower().startswith("build-")
                                    and not (Path(directory) / name).is_symlink())
            for name in sorted(filenames):
                path = Path(directory) / name
                if path.suffix.lower() in EXTENSIONS and not path.is_symlink() and path.is_file():
                    selected.append(path)
    if not selected or len(selected) > 10000:
        raise ValueError("Expected a small, nonempty real source fixture (at most 10000 files)")
    work.mkdir(parents=True, exist_ok=False)
    corpus = work / "source"
    corpus.mkdir()
    entries = []
    for path in sorted(selected):
        relative = path.relative_to(checkout)
        before = path.stat()
        data = path.read_bytes()
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise RuntimeError("Source changed while capturing: " + relative.as_posix())
        if b"\0" in data:
            raise ValueError("Binary data in selected source file: " + relative.as_posix())
        lines = normalized_lines(data)
        if any(len(line.encode("utf-8")) > 1024 * 1024 for line in lines):
            raise ValueError("Source line exceeds benchmark index policy: " + relative.as_posix())
        target = corpus / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as output:
            output.write(data)
        entries.append({"path": relative.as_posix(), "bytes": len(data), "lines": len(lines),
                        "sha256": hashlib.sha256(data).hexdigest()})
    serialized = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode("utf-8")
    revision = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=10, check=False)
    manifest = {"format": FORMAT, "source_checkout": str(checkout),
                "source_revision": revision.stdout.strip() if revision.returncode == 0 else None,
                "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "approved_roots": list(ROOTS), "extensions": sorted(EXTENSIONS),
                "files": entries, "file_count": len(entries),
                "bytes": sum(entry["bytes"] for entry in entries),
                "lines": sum(entry["lines"] for entry in entries),
                "manifest_sha256": hashlib.sha256(serialized).hexdigest()}
    dump_new(work / "manifest.json", manifest)
    return manifest


def validate_fixture(work: Path) -> dict:
    manifest = json.loads((work / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("format") != FORMAT or manifest.get("approved_roots") != list(ROOTS):
        raise ValueError("Output directory is not this benchmark's captured fixture")
    source = work / "source"
    actual = {path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file()}
    expected = {entry["path"] for entry in manifest["files"]}
    if actual != expected:
        raise RuntimeError("Captured fixture file set changed")
    for entry in manifest["files"]:
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts or relative.parts[0] not in ROOTS:
            raise ValueError("Invalid captured fixture path")
        path = source / relative
        if path.is_symlink() or not path.resolve().is_relative_to(source.resolve()):
            raise ValueError("Captured fixture must not contain symlinked files/directories")
        if path.stat().st_size != entry["bytes"] or digest(path) != entry["sha256"]:
            raise RuntimeError("Captured source content changed: " + entry["path"])
    serialized = json.dumps(manifest["files"], sort_keys=True, separators=(",", ":")).encode("utf-8")
    if hashlib.sha256(serialized).hexdigest() != manifest["manifest_sha256"]:
        raise RuntimeError("Captured manifest checksum mismatch")
    return manifest


def expected_counts(work: Path, manifest: dict, case: dict) -> dict[str, int]:
    result = {}
    # ReSearchify treats ALL non-ASCII scalars as word characters, not just \w.
    tokens = re.compile(r"[A-Za-z0-9_\u0080-\U0010ffff]+")
    regex = re.compile(case["pattern"])
    for entry in manifest["files"]:
        lines = normalized_lines((work / "source" / entry["path"]).read_bytes())
        if case["kind"] == "term":
            count = sum(token.group() == case["pattern"] for line in lines for token in tokens.finditer(line))
        elif case["kind"] == "regex":
            # The engine counts matching LINES for a regex leaf, not occurrences.
            count = sum(regex.search(line) is not None for line in lines)
        else:
            count = sum(line.count(case["pattern"]) for line in lines)
        if count:
            result[entry["path"]] = count
    return result


def sample_worker(spec: dict) -> dict:
    started = time.perf_counter()
    result = subprocess.run(spec["argv"], cwd=spec.get("cwd"), stdin=subprocess.DEVNULL,
                            capture_output=True, timeout=spec["timeout"], check=False)
    sample = {"wall_seconds": time.perf_counter() - started, "returncode": result.returncode,
              "stdout": result.stdout.decode("utf-8", "strict"),
              "stderr": result.stderr.decode("utf-8", "replace"),
              "cpu_seconds": None, "peak_rss_bytes": None}
    if os.name == "posix":
        import resource
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        sample["cpu_seconds"] = usage.ru_utime + usage.ru_stime
        sample["peak_rss_bytes"] = int(usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024))
    return sample


def command(argv: list[str], timeout: float, cwd: Path | None = None) -> dict:
    # Each helper has exactly one child: RSS is not a cumulative previous-child
    # maximum, though fork/exec may impose an inherited Python high-water floor.
    spec = {"argv": argv, "timeout": timeout, "cwd": str(cwd) if cwd else None}
    result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--sample-worker"],
                            input=json.dumps(spec), capture_output=True, text=True,
                            encoding="utf-8", timeout=timeout + 20, check=False)
    if result.returncode:
        raise RuntimeError("Measurement helper failed: " + result.stderr[-4000:])
    return json.loads(result.stdout)


def success(sample: dict) -> dict:
    if sample["returncode"] or sample["stderr"]:
        raise RuntimeError("ReSearchify invocation failed: " + sample["stderr"] + sample["stdout"][:2000])
    response = json.loads(sample["stdout"])
    if response.get("status") != "ok" or not response.get("execution_complete"):
        raise RuntimeError("Incomplete ReSearchify operation: " + sample["stdout"][:2000])
    return response


def rg_counts(sample: dict) -> dict[str, int]:
    if sample["returncode"] not in (0, 1) or sample["stderr"]:
        raise RuntimeError("ripgrep failed: " + sample["stderr"])
    result = {}
    text = sample["stdout"]
    offset = 0
    while offset < len(text):
        separator = text.index("\0", offset)
        end = text.index("\n", separator)
        name = text[offset:separator].replace("\\", "/")
        if name.startswith("./"):
            name = name[2:]
        count = int(text[separator + 1:end])
        if count:
            if name in result:
                raise RuntimeError("Duplicate ripgrep file: " + name)
            result[name] = count
        offset = end + 1
    return result


def summarized(samples: list[dict]) -> dict:
    result = {"wall_ms": distribution([sample["wall_seconds"] * 1000 for sample in samples])}
    for source, field, multiplier in (("cpu_seconds", "cpu_ms", 1000), ("peak_rss_bytes", "peak_rss_bytes", 1)):
        values = [sample[source] * multiplier for sample in samples if sample[source] is not None]
        result[field] = distribution(values) if len(values) == len(samples) else None
    return result


def storage_bytes(index: Path) -> dict:
    totals = {"committed_bytes": 0, "transaction_scratch_bytes": 0, "coordination_bytes": 0,
              "spill_scratch_bytes": 0, "literal_sidecar_bytes": 0}
    for path in index.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        size = path.stat().st_size
        parts = path.relative_to(index).parts
        first = parts[0]
        if first.startswith("transaction-"):
            kind = "transaction_scratch_bytes"
        elif len(parts) >= 3 and first.startswith("segment-") and parts[1] in ("sort", "literal-sort"):
            kind = "spill_scratch_bytes"
        elif first in ("pins", "writer.lock", "access.lock"):
            kind = "coordination_bytes"
        else:
            kind = "committed_bytes"
        totals[kind] += size
        if kind == "committed_bytes" and path.name == "literal.tbl":
            totals["literal_sidecar_bytes"] += size
    totals["total_file_bytes"] = sum(value for name, value in totals.items() if name != "literal_sidecar_bytes")
    return totals


def all_hits(executable: str, index: Path, case: dict, args, expected_files: int) -> tuple[list, int]:
    # Bound requested result buffers independently of corpus size and paginate
    # outside timing. This also avoids assuming that every response fits 1 MiB.
    page_limit = max(1, min(1000, (args.memory_mb * 1048576 // 3 - 8 * 1048576) // (32768 + 512)))
    base = [executable, "find", case["query"], "--index", str(index), "--json",
            "--limit", str(min(page_limit, expected_files)), "--snippets", "0",
            "--max-output-bytes", "1048576", "--memory-mb", str(args.memory_mb),
            "--regex-steps", "18446744073709551615"]
    hits, cursors = [], set()
    cursor = None
    identity = None
    for _ in range(expected_files + 1):
        response = success(command(base + (["--cursor", cursor] if cursor else []), args.timeout))
        current = (response["snapshot"]["generation"], response["total"])
        if identity is None:
            identity = current
        elif identity != current:
            raise RuntimeError("Snapshot or total changed during correctness pagination")
        hits.extend(response["results"])
        if not response.get("has_more"):
            if response.get("output_truncated") or len(hits) != response["total"]:
                raise RuntimeError("Full correctness result was truncated")
            return hits, response["total"]
        cursor = response.get("next_cursor")
        if not response["results"] or not cursor or cursor in cursors or len(hits) > expected_files:
            raise RuntimeError("Invalid or non-progressing correctness cursor")
        cursors.add(cursor)
    raise RuntimeError("Correctness pagination exceeded fixture file count")


def measure(args, work: Path) -> dict:
    manifest = validate_fixture(work)
    report_path = work / "results.json"
    if report_path.exists() or (work / "indexes").exists():
        raise ValueError("Measurement artifacts already exist; capture to a new directory for another run")
    executables = {"baseline": str(args.baseline.resolve(strict=True)),
                   "current": str(args.current.resolve(strict=True))}
    rg = shutil.which(args.rg) if args.rg else None
    if not rg:
        raise ValueError("A trusted ripgrep executable is required for the independent comparison")
    rg = str(Path(rg).resolve(strict=True))
    rng = random.Random(args.seed)
    report = {"format": FORMAT, "complete": False, "platform": platform.platform(),
              "machine": platform.machine(), "python": platform.python_version(), "manifest": manifest,
              "iterations": args.iterations, "build_iterations": args.build_iterations, "seed": args.seed,
              "binaries": {name: {"path": path, "sha256": digest(Path(path))}
                           for name, path in {**executables, "rg": rg}.items()},
              "conditions": {
                  "fixture": "Unmodified captured baseline src/include/tests/docs, never generated source",
                  "cache": "OS cache uncontrolled; explicit warmups; NOT cold-cache",
                  "timing": "Fresh CLI process; target startup/shutdown included, Python helper startup excluded",
                  "ordering": "Deterministically shuffled tool order within each paired repetition",
                  "comparison": "RS count-only vs rg complete count scans; result-set correctness checked separately",
                  "regex_counts": "Matching lines: RS one match per line, ripgrep --count (not --count-matches)",
                  "term_counts": "Exact RS tokens; rg -w -F equivalence verified on this fixture, not universal Unicode claim",
                  "correctness": "Independent Python per-file counts + rg; old/new ordered full hits on each SAME index",
                  "cpu": "POSIX target process CPU via isolated helper; unavailable on Windows",
                  "rss": "POSIX child high-water, may include inherited Python pre-exec floor; unavailable on Windows",
                  "memory_mb": args.memory_mb, "index_threads": args.index_threads,
                  "search_threads": "Executable default", "rg_threads": "ripgrep default",
                  "small_corpus_caveat": "Startup, scheduling and timer noise can dominate; 21-sample nearest-rank p95 is coarse",
                  "coverage": "One real source tree only; this is not evidence for every language, repository or platform",
                  "build": "Separate create samples, never included in query latency; few repetitions are not stable p95 evidence",
              }, "builds": [], "correctness": [], "queries": []}
    report["ripgrep_version"] = command([rg, "--version"], args.timeout)["stdout"].splitlines()[0]
    dump_new(report_path, report)
    indexes = {}
    for profile in ("compact", "fast"):
        for iteration in range(args.build_iterations):
            order = list(executables)
            rng.shuffle(order)
            for name in order:
                index = work / "indexes" / f"{name}-{profile}-{iteration}"
                argv = [executables[name], "create", str(work / "source"), str(index), "--profile", profile,
                        "--threads", str(args.index_threads), "--memory-mb", str(args.memory_mb), "--json"]
                sample = command(argv, args.timeout)
                response = success(sample)
                if response["files"] != manifest["file_count"] or response["bytes"] != manifest["bytes"]:
                    raise RuntimeError("Index does not cover the complete captured corpus")
                report["builds"].append({"writer": name, "profile": profile, "iteration": iteration,
                                         "command": argv, "sample": {key: sample[key] for key in
                                         ("wall_seconds", "cpu_seconds", "peak_rss_bytes")},
                                         "engine_report": response, "storage": storage_bytes(index)})
                if iteration == 0:
                    indexes[(name, profile)] = index
                print(f"Built real-source {name}/{profile} sample {iteration + 1}", file=sys.stderr, flush=True)
        checkpoint(report_path, report)
    for profile in ("compact", "fast"):
        for writer in executables:
            index = indexes[(writer, profile)]
            for name, executable in executables.items():
                checked = success(command([executable, "verify", "--index", str(index), "--json"], args.timeout))
                if checked["files"] != manifest["file_count"]:
                    raise RuntimeError("Cross-reader verification changed the file count")
    for case in CASES:
        expected = expected_counts(work, manifest, case)
        if case["name"] == "absent_literal" and expected:
            raise RuntimeError("Declared absent literal exists in the captured source")
        rg_argv = [rg, "--no-config", "--no-ignore", "--hidden", "--text", "--color", "never",
                   "--case-sensitive", "--with-filename", "--null",
                   "--count" if case["kind"] == "regex" else "--count-matches"]
        if case["kind"] != "regex":
            rg_argv.append("--fixed-strings")
        if case["kind"] == "term":
            rg_argv.append("--word-regexp")
        rg_argv += ["--regexp", case["pattern"], "."]
        if rg_counts(command(rg_argv, args.timeout, work / "source")) != expected:
            raise RuntimeError("ripgrep and independent source oracle differ: " + case["name"])
        for profile in ("compact", "fast"):
            for writer in executables:
                index = indexes[(writer, profile)]
                full = {}
                for name, executable in executables.items():
                    hits, total = all_hits(executable, index, case, args, manifest["file_count"])
                    if total != len(expected):
                        raise RuntimeError("Wrong matching file count: " + case["name"])
                    counts = {hit["path"]: hit["matches"] for hit in hits}
                    if len(hits) != len(counts) or counts != expected:
                        raise RuntimeError("RS and independent per-file counts differ: " + case["name"])
                    full[name] = hits
                if full["baseline"] != full["current"]:
                    raise RuntimeError("Same-index ordered paths/counts/scores/content IDs differ: " + case["name"])
                report["correctness"].append({"case": case["name"], "index_writer": writer, "profile": profile,
                                              "readers": ["baseline", "current"], "matching_files": len(expected),
                                              "matches": sum(expected.values()), "status": "passed",
                                              "ordered_hit_sha256": hashlib.sha256(json.dumps(full["baseline"],
                                              sort_keys=True, separators=(",", ":")).encode()).hexdigest()})
            commands = {name: [executable, "find", case["query"], "--index", str(indexes[(name, profile)]),
                               "--json", "--limit", "0", "--snippets", "0", "--memory-mb", str(args.memory_mb),
                               "--regex-steps", "18446744073709551615"]
                        for name, executable in executables.items()}
            commands["rg"] = rg_argv
            samples = {name: [] for name in commands}
            metrics = {name: [] for name in executables}

            def run(name):
                value = command(commands[name], args.timeout, work / "source" if name == "rg" else None)
                if name == "rg":
                    if rg_counts(value) != expected:
                        raise RuntimeError("ripgrep results changed during timing")
                else:
                    response = success(value)
                    if response["total"] != len(expected):
                        raise RuntimeError("RS matching file count changed during timing")
                    value["engine_metrics"] = response["metrics"]
                return value

            for _ in range(args.warmups):
                for name in commands:
                    run(name)
            orders = []
            for _ in range(args.iterations):
                order = list(commands)
                rng.shuffle(order)
                orders.append(order)
                for name in order:
                    value = run(name)
                    if name in metrics:
                        metrics[name].append(value["engine_metrics"])
                    samples[name].append(value)
            report["queries"].append({**case, "profile": profile, "matching_files": len(expected),
                                       "matches": sum(expected.values()), "commands": commands,
                                       "paired_order": orders, "engine_metrics": metrics,
                                       "tools": {name: summarized(values) for name, values in samples.items()}})
            checkpoint(report_path, report)
            print(f"Measured real-source {profile}/{case['name']}: {args.iterations} paired repetitions",
                  file=sys.stderr, flush=True)
    validate_fixture(work)
    for name, details in report["binaries"].items():
        if digest(Path(details["path"])) != details["sha256"]:
            raise RuntimeError(name + " binary changed during measurements")
    report["build_summary"] = [{"writer": name, "profile": profile,
                                **summarized([row["sample"] for row in report["builds"]
                                              if row["writer"] == name and row["profile"] == profile])}
                               for profile in ("compact", "fast") for name in executables]
    report["complete"] = True
    checkpoint(report_path, report)
    return report


def main() -> None:
    if sys.argv[1:] == ["--sample-worker"]:
        print(json.dumps(sample_worker(json.load(sys.stdin))))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-checkout", type=Path, help="Approved baseline ReSearchify checkout; capture/all only")
    parser.add_argument("--output-dir", type=Path, required=True, help="New scratch directory; existing captured one for measure")
    parser.add_argument("--baseline", type=Path, help="Trusted built baseline executable; measure/all only")
    parser.add_argument("--current", type=Path, help="Trusted built current executable; measure/all only")
    parser.add_argument("--phase", choices=("capture", "measure", "all"), default="all")
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--build-iterations", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--index-threads", type=int, default=4)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--seed", type=int, default=1729)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--rg", default=shutil.which("rg"))
    args = parser.parse_args()
    if (args.iterations < 1 or args.build_iterations < 1 or args.warmups < 1
            or not 1 <= args.index_threads <= 256 or args.memory_mb < 16 or args.timeout <= 0):
        parser.error("Invalid repetitions, workers, memory budget or timeout")
    if args.phase != "measure" and args.source_checkout is None:
        parser.error("--source-checkout is required to capture the approved files")
    if args.phase != "capture" and (args.baseline is None or args.current is None):
        parser.error("--baseline and --current are required for measurements")
    work = args.output_dir.absolute()
    if args.phase in ("capture", "all"):
        manifest = capture(args.source_checkout, work)
        print(f"Captured {manifest['file_count']} approved source files, {manifest['bytes']} bytes",
              file=sys.stderr, flush=True)
    if args.phase in ("measure", "all"):
        report = measure(args, work)
        print(json.dumps({"complete": report["complete"], "results": str(work / "results.json"),
                          "manifest_sha256": report["manifest"]["manifest_sha256"]}, indent=2))


if __name__ == "__main__":
    main()

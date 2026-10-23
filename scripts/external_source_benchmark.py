#!/usr/bin/env python3
"""Paired baseline/current/ripgrep benchmark of prepared pinned C/H sources.

First run prepare_external_source.py. This driver accepts trusted, already-built
executables and never downloads, rebuilds binaries, flushes caches, or modifies
captured source. Build, full correctness, and timing can use separate exclusive
runtime windows via --phase build/validate/measure and --resume. Index creation
is measured separately; search samples use 256 MiB by default. Full path/count
correctness uses 1024 MiB and keyset pages of 10000 outside all latency samples.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import shutil
import sys

import perf_benchmark as bench
import prepare_external_source as fixture
from compare_sessions import Service


FORMAT = "researchify-external-source-benchmark-v1"
MAX_REGEX_STEPS = 18446744073709551615
MAX_OUTPUT_BYTES = 1048576


def checkpoint(path: Path, value) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(value, output, indent=2, ensure_ascii=True)
        output.write("\n")
    os.replace(temporary, path)


def success(sample: dict) -> dict:
    response = bench.success(sample)
    if not response.get("execution_complete"):
        raise RuntimeError("Incomplete operation: " + sample["stdout"][:2000])
    return response


def rg_counts(sample: dict) -> dict[str, int]:
    if sample["returncode"] not in (0, 1) or sample["stderr"]:
        raise RuntimeError("ripgrep failed: " + sample["stderr"])
    return bench.parse_rg_counts(sample["stdout"])


def measurements(sample: dict) -> dict:
    return {key: sample[key] for key in ("wall_seconds", "cpu_seconds", "peak_rss_bytes", "resource_method")}


def count_digest(counts: dict[str, int]) -> str:
    return hashlib.sha256(fixture.canonical(counts)).hexdigest()


def rg_command(executable: str, case: dict, threads: int) -> list[str]:
    command = [executable, "--no-config", "--no-ignore", "--hidden", "--text", "--color", "never",
               "--case-sensitive", "--with-filename", "--null", "--threads", str(threads),
               "--count" if case["kind"] == "regex" else "--count-matches"]
    if case["kind"] != "regex":
        command.append("--fixed-strings")
    if case["kind"] == "term":
        command.append("--word-regexp")
    return command + ["--regexp", case["pattern"], "."]


def search_command(executable: str, index: Path, case: dict, memory: int,
                   limit: int = 0, snippets: int = 0) -> list[str]:
    return [executable, "find", case["query"], "--index", str(index), "--json",
            "--limit", str(limit), "--snippets", str(snippets), "--memory-mb", str(memory),
            "--max-output-bytes", str(MAX_OUTPUT_BYTES), "--regex-steps", str(MAX_REGEX_STEPS)]


def all_counts(executable: str, index: Path, case: dict, expected: dict[str, int],
               timeout: float) -> dict:
    # 10000 * worst-case path/result storage fits the stricter one-third
    # result-memory share at 1024 MiB. The envelope may still shorten a page;
    # next_cursor is followed, never an assumed numeric offset.
    command = [executable, "serve", "--stdio", "--index", str(index)]
    request = {"command": "find", "query": case["query"], "memory_mb": 1024, "limit": 10000,
               "snippets": 0, "sort": "path", "fields": ["path", "matches"],
               "regex_steps": MAX_REGEX_STEPS, "max_output_bytes": MAX_OUTPUT_BYTES}
    found, cursors, pages = {}, set(), 0
    snapshot = None
    previous_path = None
    service = Service(command, timeout)
    finished = False
    try:
        while True:
            response = service.request(request)["response"]
            identity = (response["snapshot"]["generation"], response["total"])
            if snapshot is not None and snapshot != identity:
                raise RuntimeError("Snapshot changed during full correctness pagination")
            snapshot = identity
            pages += 1
            for hit in response["results"]:
                if hit["path"] in found:
                    raise RuntimeError("Duplicate path during correctness pagination")
                # Valid UTF-8 has the same lexical order as Unicode scalar
                # order. Keep this boundary across pages, not just within one
                # envelope: dict equality alone would accept reordered hits.
                if previous_path is not None and hit["path"] <= previous_path:
                    raise RuntimeError("Out-of-order path during correctness pagination")
                found[hit["path"]] = hit["matches"]
                previous_path = hit["path"]
            if not response["has_more"]:
                if response.get("output_truncated") or len(found) != response["total"] or found != expected:
                    missing = list(sorted(set(expected) - set(found)))[:5]
                    extra = list(sorted(set(found) - set(expected)))[:5]
                    different = [path for path in expected if path in found and expected[path] != found[path]][:5]
                    raise RuntimeError(f"Full path/count mismatch: {case['name']}; missing={missing}, extra={extra}, different={different}")
                finished = True
                return {"pages": pages, "matching_files": len(found), "matches": sum(found.values()),
                        "path_count_sha256": count_digest(found), "service_command": command,
                        "initial_request": {key: value for key, value in request.items() if key != "cursor"},
                        "memory_mb": 1024, "page_limit": 10000, "max_output_bytes": MAX_OUTPUT_BYTES}
            cursor = response.get("next_cursor")
            if not response["results"] or not cursor or cursor in cursors or len(found) > len(expected):
                raise RuntimeError("Non-progressing correctness pagination")
            cursors.add(cursor)
            request["cursor"] = cursor
    finally:
        service.close(validate=finished)


def normalized_lines(data: bytes) -> list[bytes]:
    if not data:
        return []
    lines = data.split(b"\n")
    if data.endswith(b"\n"):
        lines.pop()
    lines = [line[:-1] if line.endswith(b"\r") else line for line in lines]
    if lines and lines[0].startswith(b"\xef\xbb\xbf"):
        lines[0] = lines[0][3:]
    return lines


def clip_snippet(line: bytes, column: int, maximum: int = 240) -> str:
    if len(line) <= maximum:
        return line.decode("utf-8")
    start = max(0, column - maximum // 3)
    while start and start < len(line) and line[start] & 0xc0 == 0x80:
        start -= 1
    end = min(len(line), start + maximum)
    while end > start and end < len(line) and line[end] & 0xc0 == 0x80:
        end -= 1
    return ("..." if start else "") + line[start:end].decode("utf-8") + ("..." if end < len(line) else "")


def expected_snippets(data: bytes, case: dict, limit: int = 3) -> tuple[int, list[dict]]:
    pattern = (fixture.exact_term_regex(case["pattern"]) if case["kind"] == "term" else
               re.compile(case["pattern"].encode("ascii")) if case["kind"] == "regex" else None)
    literal = case["pattern"].encode("ascii")
    total, snippets = 0, []
    for number, line in enumerate(normalized_lines(data), 1):
        if pattern is None:
            count = line.count(literal)
            column = line.find(literal)
        elif case["kind"] == "regex":
            match = pattern.search(line)
            count = int(match is not None)
            column = match.start() if match else -1
        else:
            matches = list(pattern.finditer(line))
            count = len(matches)
            column = matches[0].start() if matches else -1
        total += count
        if count and len(snippets) < limit:
            snippets.append({"line": number, "column": column + 1, "text": clip_snippet(line, column)})
    return total, snippets


def snippet_check(executable: str, index: Path, source: Path, case: dict,
                  expected: dict[str, int], timeout: float) -> dict:
    selected = sorted(expected)[:16]
    if not selected:
        return {"files_checked": 0, "status": "passed: empty result set"}
    request = {"id": 1, "command": "find", "query": case["query"],
               "limit": len(selected), "snippets": 3, "snippet_bytes": 240, "sort": "path",
               "memory_mb": 1024, "max_output_bytes": MAX_OUTPUT_BYTES, "regex_steps": MAX_REGEX_STEPS}
    # A bare basename is a basename glob in RS, not an exact root-file path.
    # Restrict to exact relative paths only when every selected file has a
    # slash; otherwise take the first 16 from the full path-sorted query.
    scoped = all("/" in path for path in selected)
    if scoped:
        request["path"] = selected
    result = bench.measure({"kind": "jsonl", "argv": [executable, "serve", "--stdio", "--index", str(index)],
                            "requests": [request], "timeout": timeout})
    response = result["responses"][0]
    if (not response.get("execution_complete") or response["has_more"] != (not scoped and len(expected) > len(selected)) or
            response.get("output_truncated") or response["total"] != (len(selected) if scoped else len(expected)) or
            [hit["path"] for hit in response["results"]] != selected):
        raise RuntimeError("Sample snippet result omitted selected matching paths")
    for hit in response["results"]:
        count, snippets = expected_snippets((source / hit["path"]).read_bytes(), case)
        if hit["matches"] != count or expected[hit["path"]] != count or hit["snippets"] != snippets:
            raise RuntimeError("Independent byte-column/line/snippet mismatch: " + hit["path"] + " / " + case["name"])
    return {"files_checked": len(selected), "paths": selected, "request": request,
            "method": "Independent source-byte matcher on first 16 sorted matching files; LF/CR/BOM and UTF-8 clipping accounted for; regex matching lines, term/literal occurrences",
            "status": "passed", "results_sha256": hashlib.sha256(fixture.canonical(response["results"])).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--baseline-commit", required=True)
    parser.add_argument("--current-commit", required=True)
    parser.add_argument("--rg", default=shutil.which("rg"))
    parser.add_argument("--phase", choices=("build", "validate", "measure", "all"), default="all")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--profile", choices=("compact", "fast"), action="append")
    parser.add_argument("--query", action="append")
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--build-iterations", type=int, default=1)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--index-threads", type=int, default=8)
    parser.add_argument("--rg-threads", type=int, default=8)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--seed", type=int, default=7727)
    parser.add_argument("--timeout", type=float, default=3600)
    args = parser.parse_args()
    if (not 1 <= args.iterations <= 10000 or not 1 <= args.build_iterations <= 3 or
            not 0 <= args.warmups <= 100 or not 16 <= args.memory_mb <= 1048576 or
            not 1 <= args.index_threads <= 256 or args.rg_threads < 1 or args.timeout <= 0):
        parser.error("Invalid iteration, worker, memory or timeout option")
    for commit in (args.baseline_commit, args.current_commit):
        if not re.fullmatch("[0-9a-f]{40}", commit):
            parser.error("Full lowercase 40-digit source commit IDs are required")
    manifest = fixture.validate_fixture(args.fixture_dir.resolve(strict=True))
    source = Path(manifest["source"])
    suites = manifest["cases"]
    selected = set(args.query or [case["name"] for case in suites])
    if selected - {case["name"] for case in suites}:
        parser.error("Unknown query case")
    suites = [case for case in suites if case["name"] in selected]
    profiles = list(dict.fromkeys(args.profile or ["compact", "fast"]))
    executables = {"baseline": str(args.baseline.resolve(strict=True)),
                   "current": str(args.current.resolve(strict=True))}
    rg_path = shutil.which(args.rg) if args.rg else None
    if not rg_path:
        parser.error("A trusted ripgrep executable is required")
    rg = str(Path(rg_path).resolve(strict=True))
    binaries = {name: {"path": path, "sha256": fixture.sha256(Path(path))}
                for name, path in {**executables, "rg": rg}.items()}
    binaries["baseline"]["source_commit"] = args.baseline_commit
    binaries["current"]["source_commit"] = args.current_commit
    options = {"iterations": args.iterations, "build_iterations": args.build_iterations, "warmups": args.warmups,
               "index_threads": args.index_threads, "rg_threads": args.rg_threads, "memory_mb": args.memory_mb,
               "seed": args.seed, "timeout": args.timeout, "profiles": profiles, "cases": suites}
    work = args.output_dir.absolute()
    report_path = work / "results.json"
    if report_path.exists():
        if not args.resume:
            parser.error("Existing report requires --resume; artifacts are never silently overwritten")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if (report.get("format") != FORMAT or report.get("binaries") != binaries or report.get("options") != options or
                report.get("fixture_manifest_sha256") != manifest["manifest_sha256"]):
            raise ValueError("Resume identity/options differ from the checkpoint")
    else:
        if work.exists():
            parser.error("New --output-dir must not already exist")
        work.mkdir(parents=True)
        report = {"format": FORMAT, "complete": False, "platform": platform.platform(), "machine": platform.machine(),
                  "python": platform.python_version(), "binaries": binaries, "options": options,
                  "fixture_manifest_sha256": manifest["manifest_sha256"], "fixture_manifest": str(args.fixture_dir.resolve() / "manifest.json"),
                  "fixture": {key: manifest[key] for key in ("project", "tag", "commit", "archive_url", "archive_sha256", "content_manifest_sha256", "file_count", "bytes", "lines", "skip_counts")},
                  "conditions": {
                      "source": "Unmodified pinned upstream regular UTF-8 C/H source subset; excluded members listed in capture manifest",
                      "cache": "OS cache uncontrolled, explicit warmups; NOT cold-cache",
                      "timing": "Fresh target process each sample; startup/shutdown included, isolated Python helper startup excluded",
                      "comparison": "RS count-only versus rg complete count scans. rg per-file output is included in wall-time; -l early-exit is never used",
                      "regex": "Matching lines: rg --count, not --count-matches. Only portable ASCII expressions",
                      "terms": "rg -wF equivalence checked against capture-time byte-exact RS token reference; no general Unicode-equivalence assumption",
                      "correctness": "Full per-file counts outside timings at 1024 MiB/pages 10000/1 MiB envelopes; same-snapshot top16 ranking and independent first16 source snippets",
                      "resources": "Isolated target CPU/RSS via perf_benchmark.py. POSIX RSS may include inherited Python pre-exec floor",
                      "build": "Separate randomized create samples; 1-3 samples are descriptive, not stable build p95 evidence",
                      "profiles": "Each executable searches its own immutable profile index; same current snapshot also read by both binaries for ranking",
                      "coverage": "This host/compiler and two explicit pinned source subsets only; no claim of user fixture reproduction or unexecuted platform testing",
                  }, "builds": [], "oracles": [], "correctness": [], "queries": [], "phases": {}}
        report["ripgrep_version"] = bench.command([rg, "--version"], args.timeout)["stdout"].splitlines()[0]
        checkpoint(report_path, report)

    def index_path(name, profile, iteration=0):
        return work / "indexes" / f"{name}-{profile}-{iteration}"

    if args.phase in ("build", "all"):
        rng = random.Random(args.seed)
        for profile in profiles:
            for iteration in range(args.build_iterations):
                order = list(executables)
                rng.shuffle(order)
                for name in order:
                    if any(row["writer"] == name and row["profile"] == profile and row["iteration"] == iteration for row in report["builds"]):
                        continue
                    index = index_path(name, profile, iteration)
                    if index.exists():
                        raise ValueError("Unrecorded partial index exists; preserve it and choose a new output directory: " + str(index))
                    policy = manifest["index_policy"]
                    argv = [executables[name], "create", str(source), str(index), "--profile", profile,
                            "--threads", str(args.index_threads), "--memory-mb", str(args.memory_mb),
                            "--max-file-bytes", str(policy["max_file_bytes"]), "--max-line-bytes", str(policy["max_line_bytes"]), "--json"]
                    measured = bench.command(argv, args.timeout)
                    response = success(measured)
                    if any(response[field] != manifest[key] for field, key in (("files", "file_count"), ("bytes", "bytes"), ("lines", "lines"))):
                        raise RuntimeError("Built index does not cover the whole captured source fixture")
                    report["builds"].append({"writer": name, "profile": profile, "iteration": iteration,
                                             "pair_order": order, "command": argv, "sample": measurements(measured),
                                             "engine_report": response, "storage": bench.index_storage(str(index))})
                    checkpoint(report_path, report)
                    print(f"Built {manifest['project']} {name}/{profile} {iteration + 1}/{args.build_iterations}", file=sys.stderr, flush=True)
        report["build_summary"] = [{"writer": name, "profile": profile,
                                    **bench.metric_summary([row["sample"] for row in report["builds"]
                                                           if row["writer"] == name and row["profile"] == profile])}
                                   for profile in profiles for name in executables]
        report["phases"]["build"] = True
        checkpoint(report_path, report)
    if not report["phases"].get("build"):
        raise ValueError("Complete --phase build before validation or measurement")

    if args.phase in ("validate", "measure", "all"):
        for profile in profiles:
            for name, executable in executables.items():
                state = success(bench.command([executable, "status", "--index", str(index_path(name, profile)), "--json"], args.timeout))
                if (state["files"] != manifest["file_count"] or state["bytes"] != manifest["bytes"] or
                        state["lines"] != manifest["lines"] or state["profile"] != profile or Path(state["root"]).resolve() != source):
                    raise RuntimeError("Prepared index identity differs from the captured fixture")

    for case_number, case in enumerate(suites):
        if args.phase == "build":
            break
        rg_argv = rg_command(rg, case, args.rg_threads)
        expected = rg_counts(bench.command(rg_argv, args.timeout, str(source)))
        if case["kind"] == "term" and expected != manifest["term_reference"][case["name"]]:
            raise RuntimeError("rg -wF and RS token semantics differ on this fixture: " + case["name"])
        if case["name"] == "negative_literal" and expected:
            raise RuntimeError("Declared absent literal unexpectedly exists in fixture")
        if case["name"] != "negative_literal" and not expected:
            raise RuntimeError("Representative positive case has no matches in fixture: " + case["name"])
        oracle = {"case": case["name"], "command": rg_argv, "matching_files": len(expected),
                  "matches": sum(expected.values()), "path_count_sha256": count_digest(expected)}
        existing_oracle = next((row for row in report["oracles"] if row["case"] == case["name"]), None)
        if existing_oracle and existing_oracle != oracle:
            raise RuntimeError("Independent oracle changed since the previous phase")
        if not existing_oracle:
            report["oracles"].append(oracle)
            checkpoint(report_path, report)
        for profile_number, profile in enumerate(profiles):
            checked = next((row for row in report["correctness"] if row["case"] == case["name"] and row["profile"] == profile), None)
            if args.phase in ("validate", "all") and not checked:
                primary = {name: all_counts(executable, index_path(name, profile), case, expected, args.timeout)
                           for name, executable in executables.items()}
                same_index = index_path("current", profile)
                ranking = {}
                for name, executable in executables.items():
                    response = success(bench.command(search_command(executable, same_index, case, 1024, 16), args.timeout))
                    if response["total"] != len(expected) or len(response["results"]) != min(16, len(expected)):
                        raise RuntimeError("Same-snapshot ranked sample has wrong total/size")
                    ranking[name] = response["results"]
                if ranking["baseline"] != ranking["current"]:
                    raise RuntimeError("Same-snapshot ranked paths/counts/scores/content identities differ: " + case["name"])
                snippets = {name: snippet_check(executable, index_path(name, profile), source, case, expected, args.timeout)
                            for name, executable in executables.items()}
                checked = {"case": case["name"], "profile": profile, "status": "passed", "full_path_counts": primary,
                           "same_snapshot_ranking": {"index": str(same_index), "files": len(ranking["baseline"]),
                                                     "sha256": hashlib.sha256(fixture.canonical(ranking["baseline"])).hexdigest()},
                           "snippets": snippets}
                report["correctness"].append(checked)
                checkpoint(report_path, report)
                print(f"Validated {manifest['project']} {profile}/{case['name']}", file=sys.stderr, flush=True)
            if args.phase not in ("measure", "all"):
                continue
            if not checked:
                raise ValueError("Complete full correctness validation before timing: " + case["name"] + "/" + profile)
            if any(row["name"] == case["name"] and row["profile"] == profile for row in report["queries"]):
                continue
            commands = {name: search_command(executable, index_path(name, profile), case, args.memory_mb)
                        for name, executable in executables.items()}
            commands["rg"] = rg_argv
            samples, metrics = {name: [] for name in commands}, {name: [] for name in executables}

            def run(name):
                measured = bench.command(commands[name], args.timeout, str(source) if name == "rg" else None)
                if name == "rg":
                    if rg_counts(measured) != expected:
                        raise RuntimeError("ripgrep counts changed during timing")
                else:
                    response = success(measured)
                    if response["total"] != len(expected):
                        raise RuntimeError("Matching-file count changed during timing")
                    measured["engine_metrics"] = response["metrics"]
                return measured

            for _ in range(args.warmups):
                for name in commands:
                    run(name)
            rng = random.Random(args.seed + 1009 * case_number + 37 * profile_number)
            orders = []
            for iteration in range(args.iterations):
                order = list(commands)
                rng.shuffle(order)
                orders.append(order)
                for name in order:
                    measured = run(name)
                    if name in metrics:
                        metrics[name].append(measured["engine_metrics"])
                    samples[name].append(measurements(measured))
                print(f"{manifest['project']} {profile}/{case['name']}: pair {iteration + 1}/{args.iterations}", file=sys.stderr, flush=True)
            report["queries"].append({**case, "profile": profile, "matching_files": len(expected), "matches": sum(expected.values()),
                                       "commands": commands, "order": orders, "engine_metrics": metrics,
                                       "tools": {name: bench.metric_summary(values) for name, values in samples.items()}})
            checkpoint(report_path, report)

    fixture.validate_fixture(args.fixture_dir.resolve(strict=True))
    for name, binary in binaries.items():
        if fixture.sha256(Path(binary["path"])) != binary["sha256"]:
            raise RuntimeError(name + " binary changed during this run")
    for profile in profiles:
        for name, executable in executables.items():
            state = success(bench.command([executable, "status", "--index", str(index_path(name, profile)), "--json"], args.timeout))
            built = next(row for row in report["builds"] if row["writer"] == name and row["profile"] == profile and row["iteration"] == 0)
            if any(state[field] != built["engine_report"][field] for field in ("generation", "files", "lines", "bytes")):
                raise RuntimeError("Immutable index changed during this run")
    expected_rows = len(profiles) * len(suites)
    report["phases"]["validate"] = len(report["correctness"]) == expected_rows
    report["phases"]["measure"] = len(report["queries"]) == expected_rows
    report["complete"] = all(report["phases"].get(phase) for phase in ("build", "validate", "measure"))
    checkpoint(report_path, report)
    print(json.dumps({"report": str(report_path), "complete": report["complete"], "phases": report["phases"]}, indent=2))


if __name__ == "__main__":
    main()

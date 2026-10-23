#!/usr/bin/env python3
"""Bounded paired update/compaction measurements with pinned-snapshot checks.

Only explicitly supplied trusted local binaries run. Every corpus/index is new;
the harness changes/deletes only its own generated fixture files and preserves
all benchmark indexes and raw results. No caches are flushed and no GC is run.
"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import platform
import queue
import re
import subprocess
import tempfile
import threading

from perf_benchmark import binary_identity, command, distribution, index_storage, success


def valid(response):
    if response.get("status") != "ok" or not response.get("execution_complete"):
        raise AssertionError("Unsuccessful response: " + json.dumps(response))
    if response.get("output_truncated") or response.get("truncated"):
        raise AssertionError("Unexpected output truncation")
    return response


def argv(binary, index, memory_mb, operation, **fields):
    result = [str(binary), operation, "--index", str(index), "--memory-mb", str(memory_mb), "--json"]
    for name, value in fields.items():
        values = value if isinstance(value, (list, tuple)) else [value]
        for item in values:
            result += ["--" + name.replace("_", "-"), str(item)]
    return result


def fresh(binary, index, memory_mb, timeout, operation, **fields):
    process = subprocess.run(argv(binary, index, memory_mb, operation, **fields),
                             text=True, encoding="utf-8", capture_output=True, timeout=timeout)
    if process.returncode or process.stderr:
        raise RuntimeError(process.stderr + process.stdout)
    return valid(json.loads(process.stdout))


class PinnedSession:
    """An idle read-only process keeps its first snapshot open during writers."""

    def __init__(self, binary, index, generation, memory_mb, timeout):
        self.generation, self.memory_mb, self.timeout = generation, memory_mb, timeout
        self.counter = 0
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen([str(binary), "serve", "--stdio", "--index", str(index)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errors,
                                        text=True, encoding="utf-8", bufsize=1)
        self.lines = queue.Queue()

        def reader():
            try:
                for line in self.process.stdout:
                    self.lines.put(line)
            except BaseException as error:
                self.lines.put(error)
            finally:
                self.lines.put(None)

        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()

    def __enter__(self):
        return self

    def request(self, operation, **fields):
        self.counter += 1
        message = {"id": self.counter, "command": operation, "generation": self.generation,
                   "memory_mb": self.memory_mb, "max_output_bytes": 1048576, **fields}
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        line = self.lines.get(timeout=self.timeout)
        if line is None or isinstance(line, BaseException):
            raise RuntimeError("Pinned reader exited or failed before its response")
        response = valid(json.loads(line))
        if response.get("id") != self.counter:
            raise AssertionError("Pinned response ID mismatch")
        return response

    def __exit__(self, exception_type, *_):
        failure = None
        try:
            self.process.stdin.close()
            self.process.wait(timeout=self.timeout)
            self.reader.join(timeout=self.timeout)
            if self.reader.is_alive() or self.lines.get(timeout=self.timeout) is not None:
                raise RuntimeError("Pinned reader did not finish cleanly")
            self.errors.seek(0)
            stderr = self.errors.read().decode("utf-8", "replace")
            if self.process.returncode or stderr:
                raise RuntimeError("Pinned reader failed: " + stderr)
        except Exception as error:
            failure = error
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.reader.join(timeout=self.timeout)
            self.process.stdout.close()
            self.errors.close()
        if failure is not None and exception_type is None:
            raise failure


def filename(number):
    return f"group_{number // 1000}/module_{number}.cpp"


def document(number, epoch=0):
    token = f"needle_{number}" if epoch == 0 else f"updated_{epoch}_{number}"
    return (f"// generated file {number}\nnamespace bucket{number % 100} {{ int {token} = 0;\n"
            + "".join(f"int function_{number}_{line}(int value) {{ int common = value + {line}; return common; }}\n"
                      for line in range(2, 49)) + "}\n")


def write_fixture_file(source, name, text):
    target = source / name
    if target.is_symlink():
        raise ValueError("Fixture file unexpectedly replaced by a symlink")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(text.encode("utf-8"))


def mutate(source, contents, files, epoch):
    affected = []
    for number in range(42, 50):
        name = filename(number)
        affected.append(name)
        contents[name] = document(number, epoch)
        write_fixture_file(source, name, contents[name])
    for offset in range(3):
        name = filename(400 + (epoch - 1) * 3 + offset)
        affected.append(name)
        target = source / name
        if target.is_symlink() or not target.is_file() or name not in contents:
            raise ValueError("Refusing to delete a non-fixture file")
        target.unlink()
        del contents[name]
        number = files + (epoch - 1) * 3 + offset
        name = filename(number)
        affected.append(name)
        contents[name] = document(number)
        write_fixture_file(source, name, contents[name])
    if len(set(affected)) != 14:
        raise AssertionError("Mutation must affect exactly fourteen distinct paths")
    return affected


def checks(contents, files):
    current_marker = re.search(r"\bint ([A-Za-z0-9_]+) = 0;", contents[filename(42)]).group(1)
    definitions = [
        ("rare_changed", "needle_42", r"\bneedle_42\b", 1),
        ("rare_deleted", "needle_400", r"\bneedle_400\b", 1),
        ("rare_added", f"needle_{files}", rf"\bneedle_{files}\b", 1),
        ("update_1", "updated_1_42", r"\bupdated_1_42\b", 1),
        ("update_2", "updated_2_42", r"\bupdated_2_42\b", 1),
        ("update_3", "updated_3_42", r"\bupdated_3_42\b", 1),
        ("literal", f'"{current_marker} = 0;"', re.escape(f"{current_marker} = 0;"), 1),
        ("regex", "(updated_1_42 OR updated_2_42 OR updated_3_42) AND /updated_[123]_42/",
         r"updated_[123]_42", 2),
    ]
    result = [(name, query, {path for path, text in contents.items() if re.search(pattern, text)}, count)
              for name, query, pattern, count in definitions]
    result.append(("common", "common", set(contents), 94))
    result.append(("not", "common AND NOT needle_400",
                   {path for path, text in contents.items() if not re.search(r"\bneedle_400\b", text)}, 94))
    return result


def semantic(response):
    return {"total": response["total"], "results": response["results"]}


def digest(response):
    return hashlib.sha256(json.dumps(semantic(response), sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def assert_query(response, paths, matches, generation):
    if response["snapshot"]["generation"] != generation or response["total"] != len(paths):
        raise AssertionError("Query generation or exact total differs")
    hits = response["results"]
    if len(hits) != min(3, len(paths)) or any(hit["path"] not in paths or hit["matches"] != matches for hit in hits):
        raise AssertionError("Query path/occurrence count differs")
    if len(paths) <= 3 and {hit["path"] for hit in hits} != paths:
        raise AssertionError("Query full path set differs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="New fixture directory; never removed")
    parser.add_argument("--output", type=Path, required=True, help="New report under an existing parent outside --work")
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--files", type=int, default=2000)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    binaries = {"baseline": args.baseline.resolve(strict=True), "candidate": args.candidate.resolve(strict=True)}
    work, output = args.work.resolve(), args.output.resolve()
    if work.exists() or output.exists() or output.is_relative_to(work) or not output.parent.is_dir():
        raise ValueError("Require a new workspace and new external report under an existing parent")
    if args.files < 500 or args.iterations < 1 or args.threads < 1 or args.memory_mb < 16:
        raise ValueError("Use >=500 files, positive repetitions/workers and >=16MiB")
    identities = {name: binary_identity(str(binary)) for name, binary in binaries.items()}
    report = {"status": "running", "platform": platform.platform(), "binary_sha256": identities,
              "files_per_fixture": args.files, "lines_per_file": 50, "iterations": args.iterations,
              "threads": args.threads, "memory_mb": args.memory_mb,
              "conditions": "Alternating paired order, fresh independent indexes, uncontrolled OS cache; no GC",
              "resource_caveat": "Timed maintenance child only; two idle pinned readers excluded; POSIX RSS may include helper pre-exec floor",
              "score_contract": "Exact baseline/candidate hits and scores compared on the SAME snapshot; independent post-update layouts can have different approximate df",
              "query_contract": "Exact independently expected totals and occurrence counts; first three ranked hits/snippets compared in full, with complete path sets for queries having at most three results",
              "size_contract": "Committed bytes include retained generations; compaction does not GC pinned/history data",
              "tail_caveat": "With seven samples nearest-rank p95 equals max; it is not a robust tail estimate",
              "mutation_contract": "Each update changes eight, deletes three and adds three files; updates 1/3 use full strict scans, update 2 uses exactly fourteen supplied paths in strict mode",
              "pairs": []}

    def save():
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    work.mkdir()
    save()
    try:
        for profile in ("compact", "fast"):
            for iteration in range(args.iterations):
                pair_root = work / f"{profile}-{iteration}"
                source = pair_root / "source"
                source.mkdir(parents=True)
                contents = {filename(number): document(number) for number in range(args.files)}
                original = contents.copy()
                for name, text in contents.items():
                    write_fixture_file(source, name, text)
                indexes = {name: pair_root / name for name in binaries}
                order = ("baseline", "candidate") if iteration % 2 == 0 else ("candidate", "baseline")
                pair = {"profile": profile, "iteration": iteration, "order": order, "root": str(pair_root),
                        "operations": [], "checks": []}
                report["pairs"].append(pair)
                generations = {}

                def timed(name, stage, operation, **fields):
                    print(f"START {profile} pair={iteration + 1} {stage} {name}", flush=True)
                    call = argv(binaries[name], indexes[name], args.memory_mb, operation, **fields)
                    measured = command(call, timeout=args.timeout)
                    measured.update({"binary": name, "stage": stage, "command": call})
                    if operation == "update":
                        measured["update_scope"] = "targeted" if "changed_files" in fields else "global"
                        if "changed_files" in fields:
                            measured["supplied_paths"] = fields["changed_files"]
                    pair["operations"].append(measured)
                    save()
                    response = valid(success(measured))
                    measured["storage"] = index_storage(str(indexes[name]))
                    measured["transaction_directories"] = [p.name for p in indexes[name].glob("transaction-*")]
                    generations[name] = response["generation"]
                    if response["files"] != args.files or response["lines"] != args.files * 50:
                        raise AssertionError("Maintenance corpus counts differ")
                    if response["bytes"] != sum(len(text.encode()) for text in contents.values()):
                        raise AssertionError("Maintenance source byte count differs")
                    save()
                    return response

                for name in order:
                    timed(name, "create", "create", root=source, profile=profile, mode="strict", threads=args.threads)
                initial_generations = generations.copy()
                with ExitStack() as stack:
                    pinned = {name: stack.enter_context(PinnedSession(binaries[name], indexes[name], generations[name],
                                                                     args.memory_mb, args.timeout)) for name in binaries}
                    initial_results = {name: {} for name in binaries}
                    original_checks = checks(original, args.files)
                    for name in binaries:
                        for label, query, paths, matches in original_checks:
                            result = pinned[name].request("find", query=query, limit=3, snippets=3)
                            assert_query(result, paths, matches, initial_generations[name])
                            initial_results[name][label] = semantic(result)

                    def check_state(stage):
                        state = {"stage": stage, "readers": {}}
                        expected = checks(contents, args.files)
                        for name in binaries:
                            other = "candidate" if name == "baseline" else "baseline"
                            saved = {"fresh": {}, "pinned": {}}
                            state["readers"][name] = saved
                            for label, query, paths, matches in expected:
                                result = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout,
                                               "find", query=query, limit=3, snippets=3)
                                assert_query(result, paths, matches, generations[name])
                                cross = fresh(binaries[other], indexes[name], args.memory_mb, args.timeout,
                                              "find", generation=generations[name], query=query, limit=3, snippets=3)
                                if semantic(result) != semantic(cross):
                                    raise AssertionError("Same-snapshot baseline/candidate exact hits/scores differ")
                                saved["fresh"][label] = {"total": result["total"], "exact_hit_sha256": digest(result)}
                            for label, query, paths, matches in original_checks:
                                result = pinned[name].request("find", query=query, limit=3, snippets=3)
                                assert_query(result, paths, matches, initial_generations[name])
                                if semantic(result) != initial_results[name][label]:
                                    raise AssertionError("Pinned hits/scores changed after maintenance")
                                saved["pinned"][label] = {"total": result["total"], "exact_hit_sha256": digest(result)}
                            old_text = pinned[name].request("read", file=filename(42), first=1, last=50, source="snapshot")
                            new_text = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout,
                                             "read", file=filename(42), first=1, last=50, source="snapshot")
                            if old_text["text"] != original[filename(42)] or new_text["text"] != contents[filename(42)]:
                                raise AssertionError("Fresh or pinned snapshot text differs")
                            verified = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout, "verify")
                            old_verified = pinned[name].request("verify")
                            if verified["files"] != args.files or old_verified["files"] != args.files:
                                raise AssertionError("Fresh or pinned verification file count differs")
                            if verified["generation"] != generations[name] or old_verified["generation"] != initial_generations[name]:
                                raise AssertionError("Verification used the wrong generation")
                            saved["verified_generations"] = [verified["generation"], old_verified["generation"]]
                        pair["checks"].append(state)
                        save()

                    for name in order:
                        response = timed(name, "noop", "update", profile=profile, mode="strict", threads=args.threads)
                        if response["generation"] != initial_generations[name] or any(response[key] for key in ("added", "changed", "removed", "tokenized")):
                            raise AssertionError("No-op update unexpectedly changed indexed contents/generation")
                    check_state("noop")
                    for epoch in (1, 2, 3):
                        affected = mutate(source, contents, args.files, epoch)
                        scope = "targeted" if epoch == 2 else "global"
                        stage = f"update_{epoch}_{scope}"
                        for name in order:
                            previous = generations[name]
                            fields = {"profile": profile, "mode": "strict", "threads": args.threads}
                            if epoch == 2:
                                fields["changed_files"] = affected
                            response = timed(name, stage, "update", **fields)
                            if response["generation"] == previous or [response[key] for key in ("changed", "removed", "added")] != [8, 3, 3]:
                                raise AssertionError("Mixed update counters/generation differ")
                            if response["global_scan"] != (scope == "global"):
                                raise AssertionError("Update did not use the requested global/targeted scope")
                        check_state(stage)
                    for name in order:
                        previous = generations[name]
                        response = timed(name, "compact", "compact", threads=args.threads)
                        if response["generation"] == previous:
                            raise AssertionError("Compaction failed to publish a new snapshot")
                    check_state("compact")
                pair["status"] = "passed"
                save()
                print(f"PASS {profile} pair={iteration + 1}: maintenance, fresh and pinned checks", flush=True)
        summaries = []
        for profile in ("compact", "fast"):
            for stage in ("create", "noop", "update_1_global", "update_2_targeted", "update_3_global", "compact"):
                matching = [pair for pair in report["pairs"] if pair["profile"] == profile]
                samples = {name: [next(op for op in pair["operations"] if op["stage"] == stage and op["binary"] == name)
                                  for pair in matching] for name in binaries}
                summary = {"profile": profile, "stage": stage}
                for name in binaries:
                    summary[name] = {field: distribution([row[field] for row in samples[name] if row[field] is not None])
                                     for field in ("wall_seconds", "cpu_seconds", "peak_rss_bytes")
                                     if any(row[field] is not None for row in samples[name])}
                    summary[name]["retained_committed_bytes"] = distribution([row["storage"]["committed_index_bytes"] for row in samples[name]])
                paired = [before["wall_seconds"] / after["wall_seconds"]
                          for before, after in zip(samples["baseline"], samples["candidate"])]
                summary["paired_wall_speedup"] = distribution(paired)
                summary["candidate_faster_pairs"] = sum(value > 1 for value in paired)
                summaries.append(summary)
        report["summary"] = summaries
        if identities != {name: binary_identity(str(binary)) for name, binary in binaries.items()}:
            raise AssertionError("A benchmark binary changed during the run")
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        save()
    print(output, flush=True)


if __name__ == "__main__":
    main()

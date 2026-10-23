#!/usr/bin/env python3
"""Compare identical update operations on a marked, disposable source fixture.

The JSON fixture manifest must be outside its source directory and contain
format="researchify-update-fixture-v1", source (absolute path), edit_file
(relative regular file), edit_sha256, files, and description. This is an
explicit mutation contract for a benchmark fixture, never a live project.
Only the named file and two initially absent sibling filenames are changed.
The original bytes and modification timestamp are restored in finally; base indexes are
copied, never written. Timing excludes copying, correctness checks and cleanup.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import random
import shutil
import time

import perf_benchmark as bench
from compare_sessions import paired_orders
from maintenance_benchmark import fresh, valid


def digest(data):
    return hashlib.sha256(data).hexdigest()


def relative_file(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or "\\" in value or ":" in value or any(part in ("", ".", "..") for part in value.split("/")):
        raise ValueError("Fixture edit_file must be a safe relative filename")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--base-index", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="New result/index directory outside the source fixture")
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100 or not 1 <= args.threads <= 256 or args.memory_mb < 16 or args.timeout <= 0:
        parser.error("Invalid bounded iteration/thread/memory/timeout setting")
    manifest_path = args.fixture.resolve(strict=True)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("format") != "researchify-update-fixture-v1" or not manifest.get("description"):
        parser.error("Explicit disposable-fixture manifest is required")
    source = Path(manifest["source"]).resolve(strict=True)
    work = args.work.resolve()
    base_index = args.base_index.resolve(strict=True)
    if manifest_path.is_relative_to(source) or work.is_relative_to(source) or work.is_relative_to(base_index) or work.exists():
        parser.error("Manifest and a NEW --work must be outside the source fixture; work must also be outside the base index")
    relative = relative_file(manifest["edit_file"])
    edit = source.joinpath(*relative.parts)
    if edit.is_symlink() or not edit.is_file() or edit.resolve() != edit or not edit.resolve().is_relative_to(source):
        parser.error("Fixture edit file must not traverse symlinks")
    original = edit.read_bytes()
    saved_stat = edit.stat()
    if digest(original) != manifest["edit_sha256"]:
        parser.error("Fixture edit bytes differ from the explicit manifest")
    files = int(manifest["files"])
    if files < 1:
        parser.error("Fixture must have indexed files")
    binaries = {"baseline": str(args.baseline.resolve(strict=True)), "candidate": str(args.candidate.resolve(strict=True))}
    identities = {name: bench.binary_identity(executable) for name, executable in binaries.items()}
    for lock in ("writer.lock", "access.lock"):
        if (base_index / lock).exists():
            parser.error("Base index is in use by a writer/access gate; retry after it finishes")
    initial = fresh(binaries["baseline"], base_index, args.memory_mb, args.timeout, "status")
    if Path(initial["root"]).resolve() != source or initial["files"] != files:
        parser.error("Base index does not describe the declared fixture")
    added = edit.with_name(edit.stem + "_rs_bench_added" + edit.suffix)
    renamed = edit.with_name(edit.stem + "_rs_bench_renamed" + edit.suffix)
    if any(path.exists() or path.is_symlink() for path in (added, renamed)):
        parser.error("Reserved fixture sibling name already exists")
    work.mkdir(parents=True)
    report = {"format": "researchify-update-workflow-v1", "complete": False,
              "platform": platform.platform(), "python": platform.python_version(),
              "fixture": manifest, "fixture_manifest_sha256": digest(manifest_path.read_bytes()),
              "initial_index": initial, "base_index": str(base_index),
              "binaries": {name: {"path": executable, "sha256": identities[name]} for name, executable in binaries.items()},
              "conditions": {"cache": "Uncontrolled OS cache; explicit setup/no-op warmups excluded; not cold-cache",
                             "mode": "strict for all operations; global and targeted scopes summarized separately",
                             "index": "Both readers start with separate copies of the same immutable base index",
                             "series": "Generations and retained segments grow across identical edit sequences; iterations are not independent reset snapshots",
                             "mutation": "Exactly one file edit; its original timestamp is preserved, including same-size later edits",
                             "timing": "Complete CLI child lifetime; fixture/index copying, query checks and restore excluded",
                             "threads": args.threads, "memory_mb": args.memory_mb,
                             "rss": "Child process high-water mark; may include Python pre-exec floor",
                             "claims": "No cross-scope speedup claim; no claim of user Windows/Mac reproduction"},
              "setup": [], "operations": [], "checks": [], "summaries": [], "mutation_audit": []}
    result_path = work / "results.json"

    def save():
        result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def audit(event, path=edit):
        data = path.read_bytes()
        metadata = path.stat()
        report["mutation_audit"].append({"event": event, "path": str(path), "pid": os.getpid(),
            "time_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns(),
            "bytes": len(data), "sha256": digest(data), "mtime_ns": metadata.st_mtime_ns,
            "ctime_ns": metadata.st_ctime_ns, "inode": metadata.st_ino})
        save()

    def write_edit(data):
        # Only the explicitly marked fixture file; all writes and syncs are
        # outside timed child work. Preserve the same-size/mtime strict test.
        with edit.open("wb") as stream:
            stream.write(data)
            stream.flush()
            os.utime(edit, ns=(saved_stat.st_atime_ns, saved_stat.st_mtime_ns))
            os.fsync(stream.fileno())

    def argv(name, operation, **fields):
        command = [binaries[name], operation, "--index", str(indexes[name]),
                   "--memory-mb", str(args.memory_mb), "--json"]
        for key, values in fields.items():
            for value in values if isinstance(values, list) else [values]:
                command += ["--" + key.replace("_", "-"), str(value)]
        return command

    def timed(name, stage, iteration, fields, expected):
        command = argv(name, "update", mode="strict", threads=args.threads, **fields)
        sample = bench.command(command, args.timeout)
        row = {"binary": name, "stage": stage, "iteration": iteration, "command": command, "sample": sample}
        report["operations"].append(row)
        save()
        response = valid(bench.success(sample))
        row["response"] = response
        for key, value in expected.items():
            if response[key] != value:
                raise AssertionError(f"{name}/{stage}: expected {key}={value}, got {response[key]}")
        save()
        return response

    def check_marker(marker, expected_path):
        observed = {}
        for name in binaries:
            value = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout,
                          "find", query=marker, limit=2, snippets=1, sort="path")
            hits = value["results"]
            if value["total"] != (1 if expected_path else 0) or value.get("has_more"):
                raise AssertionError("Marker match count differs")
            if expected_path and (len(hits) != 1 or hits[0]["path"] != expected_path or hits[0]["matches"] != 1):
                raise AssertionError("Marker path or exact occurrence count differs")
            observed[name] = bench.normalized(value)
        if observed["baseline"] != observed["candidate"]:
            raise AssertionError("Updated hits/scores/snippets differ between binaries")
        report["checks"].append({"marker": marker, "path": expected_path, "results": observed})
        save()

    last_written = original
    added_owned = False
    renamed_owned = False
    indexes = {}
    global_hashed = None
    rng = random.Random(92841)

    def orders():
        for order in paired_orders(args.iterations, rng):
            yield ["candidate" if name == "current" else name for name in order]

    try:
        save()
        audit("initial")
        for name in binaries:
            indexes[name] = work / (name + "-index")
            shutil.copytree(base_index, indexes[name], ignore=shutil.ignore_patterns("pins", "writer.lock", "access.lock", "transaction-*"))
            # Normalize persisted execution policy before timing no-op calls.
            response = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout,
                             "update", mode="strict", threads=args.threads)
            if any(response[key] for key in ("added", "changed", "removed", "tokenized")):
                raise AssertionError("Supplied base index is not current for its source fixture")
            if global_hashed is not None and response["hashed"] != global_hashed:
                raise AssertionError("The two readers do not inspect the same strict-global source set")
            global_hashed = response["hashed"]
            report["setup"].append({"binary": name, "operation": "excluded_strict_noop", "response": response})
            save()

        for iteration, order in enumerate(orders()):
            for name in order:
                timed(name, "strict_noop", iteration, {}, {"global_scan": True, "files": files, "hashed": global_hashed,
                                                          "changed": 0, "added": 0, "removed": 0, "tokenized": 0})

        current_marker = None
        for stage, letter in (("global_one_edit", "g"), ("targeted_one_edit", "t")):
            for iteration, order in enumerate(orders()):
                if edit.read_bytes() != last_written:
                    raise RuntimeError("Fixture edit file was modified by another actor")
                previous_marker = current_marker
                current_marker = f"researchify_update_bench_{letter}_{iteration:04d}"
                last_written = original + b"\n" + current_marker.encode() + b"\n"
                audit(f"before_{stage}_{iteration}")
                write_edit(last_written)
                audit(f"after_{stage}_{iteration}")
                fields = {"changed_file": [str(relative)]} if letter == "t" else {}
                for name in order:
                    timed(name, stage, iteration, fields, {"global_scan": letter == "g", "files": files,
                        "hashed": global_hashed if letter == "g" else 1, "changed": 1, "added": 0, "removed": 0, "tokenized": 1})
                check_marker(current_marker, str(relative))
                if previous_marker:
                    check_marker(previous_marker, None)

        added_relative = added.relative_to(source).as_posix()
        for iteration, order in enumerate(orders()):
            marker = f"researchify_update_bench_added_{iteration:04d}"
            if added.exists() or added.is_symlink():
                raise RuntimeError("Reserved fixture addition appeared unexpectedly")
            with added.open("xb") as stream:
                added_owned = True
                stream.write((marker + "\n").encode())
                stream.flush()
                os.fsync(stream.fileno())
            audit(f"after_add_{iteration}", added)
            for name in order:
                timed(name, "targeted_one_add", iteration, {"changed_file": [added_relative]},
                      {"global_scan": False, "files": files + 1, "hashed": 1, "changed": 0, "added": 1, "removed": 0, "tokenized": 1})
            check_marker(marker, added_relative)
            audit(f"before_delete_{iteration}", added)
            added.unlink()
            added_owned = False
            for name in reversed(order):
                timed(name, "targeted_one_delete", iteration, {"changed_file": [added_relative]},
                      {"global_scan": False, "files": files, "hashed": 0, "changed": 0, "added": 0, "removed": 1, "tokenized": 0})
            check_marker(marker, None)

        renamed_relative = renamed.relative_to(source).as_posix()
        audit("before_rename")
        edit.rename(renamed)
        renamed_owned = True
        audit("after_rename", renamed)
        for name in binaries:
            response = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout, "update", mode="strict",
                             threads=args.threads, changed_file=[str(relative), renamed_relative])
            if response["added"] != 1 or response["removed"] != 1 or response["hashed"] != 1 or response["files"] != files:
                raise AssertionError("Targeted rename did not process both filenames")
            report["checks"].append({"binary": name, "operation": "rename", "response": response})
        check_marker(current_marker, renamed_relative)
        audit("before_reverse_rename", renamed)
        renamed.rename(edit)
        renamed_owned = False
        audit("after_reverse_rename")
        for name in binaries:
            response = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout, "update", mode="strict",
                             threads=args.threads, changed_file=[renamed_relative, str(relative)])
            if response["added"] != 1 or response["removed"] != 1 or response["files"] != files:
                raise AssertionError("Reverse rename failed")
            verified = fresh(binaries[name], indexes[name], args.memory_mb, args.timeout, "verify")
            if verified["files"] != files:
                raise AssertionError("Final index integrity/file count differs")
            report["checks"].append({"binary": name, "operation": "verify", "response": verified})

        for stage in ("strict_noop", "global_one_edit", "targeted_one_edit", "targeted_one_add", "targeted_one_delete"):
            samples = {name: [row["sample"] for row in report["operations"] if row["stage"] == stage and row["binary"] == name] for name in binaries}
            if any(len(values) != args.iterations for values in samples.values()):
                raise AssertionError("Incomplete paired stage")
            ratios = [before["wall_seconds"] / after["wall_seconds"] for before, after in zip(samples["baseline"], samples["candidate"])]
            report["summaries"].append({"stage": stage,
                "tools": {name: bench.metric_summary(values) for name, values in samples.items()},
                "paired_wall_speedup": bench.distribution(ratios), "candidate_faster_pairs": sum(value > 1 for value in ratios)})
        if identities != {name: bench.binary_identity(executable) for name, executable in binaries.items()}:
            raise AssertionError("Executable changed during measurements")
        report["complete"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            if added_owned:
                added.unlink()
            if renamed_owned:
                renamed.rename(edit)
            if edit.read_bytes() != last_written:
                raise RuntimeError("Concurrent source change; refusing to overwrite it")
            audit("before_restore")
            write_edit(original)
            report["source_restored"] = digest(edit.read_bytes()) == manifest["edit_sha256"]
            if not report["source_restored"]:
                raise RuntimeError("Fixture contents changed during restoration")
            audit("after_restore")
            os.utime(edit, ns=(saved_stat.st_atime_ns, saved_stat.st_mtime_ns))
        except BaseException as error:
            report["complete"] = False
            report["restore_error"] = str(error)
            raise
        finally:
            save()
    if not report.get("source_restored"):
        raise RuntimeError("Source restoration failed; inspect results.json")
    print(json.dumps({"format": report["format"], "complete": report["complete"], "report": str(result_path),
                      "operations": len(report["operations"]), "checks": len(report["checks"])}, sort_keys=True))


if __name__ == "__main__":
    main()

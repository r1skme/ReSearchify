#!/usr/bin/env python3
"""Linux file-data-page-cache comparison on an explicitly owned fixture.

The marker JSON must live in a disposable directory and declare format
researchify-cache-fixture-v1, disposable:true, description, source (relative
directory), indexes:{baseline:relative_directory,current:relative_directory}.
Both indexes must describe exactly that source. Nothing is created in the
source or committed index data; ordinary reader pins remain engine-owned.

DONTNEED is applied only to inventoried, owned, single-link regular files.
mincore observes PROT_NONE mappings, so observation does not read/fault in the
file data. Verified cold means zero resident pages in ALL scoped file ranges
at the last observation, not a cold machine, executable, metadata or device.
"""
from __future__ import annotations

import argparse
import ctypes
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import stat
import subprocess
import sys
import time

import perf_benchmark as bench
from tiny_comparison import all_hits, brief


FORMAT = "researchify-cache-fixture-v1"
IDENTITY = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_uid", "st_nlink")
COORDINATION = ("pins", "writer.lock", "access.lock")


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def owned(path, directory=False):
    info = path.lstat()
    expected = stat.S_ISDIR if directory else stat.S_ISREG
    if not expected(info.st_mode) or info.st_uid != os.geteuid():
        raise ValueError("Expected an owned non-symlink " + ("directory: " if directory else "file: ") + str(path))
    if not directory and info.st_nlink != 1:
        raise ValueError("Hard-linked files are outside the disposable-file contract: " + str(path))
    return info


def relative_directory(root, value):
    relative = Path(value)
    if relative.is_absolute() or not relative.parts or any(part in (".", "..") for part in relative.parts):
        raise ValueError("Fixture directories must be strict relative descendants")
    current = root
    for part in relative.parts:
        current /= part
        owned(current, directory=True)
    return current


def load_fixture(marker):
    marker = Path(os.path.abspath(marker))
    # Ancestor symlinks could point an innocent-looking marker outside its owner.
    if marker.resolve(strict=True) != marker:
        raise ValueError("Fixture marker or an ancestor is a symlink")
    owned(marker)
    root = marker.parent
    owned(root, directory=True)
    value = json.loads(marker.read_text(encoding="utf-8"))
    if value.get("format") != FORMAT or value.get("disposable") is not True or not value.get("description"):
        raise ValueError("An explicit disposable fixture marker and description are required")
    if set(value.get("indexes", {})) != {"baseline", "current"}:
        raise ValueError("Fixture requires baseline and current indexes")
    source = relative_directory(root, value["source"])
    indexes = {name: relative_directory(root, item) for name, item in value["indexes"].items()}
    directories = [source, *indexes.values()]
    for i, left in enumerate(directories):
        for right in directories[i + 1:]:
            if left == right and left != source:
                continue  # Compatible readers may share the very same immutable index.
            if left == right or left in right.parents or right in left.parents:
                raise ValueError("Source/index directories must not overlap")
    return root, source, indexes, value


def inventory(directory, index=False):
    """Hash outside measurements; retain identities for non-reading guards."""
    files = []
    for parent, directories, names in os.walk(directory, followlinks=False):
        parent = Path(parent)
        for name in list(directories):
            path = parent / name
            owned(path, directory=True)
            relative = path.relative_to(directory)
            if index and relative.parts[0] in COORDINATION:
                directories.remove(name)
            elif index and (name.startswith("transaction-") or name in ("sort", "literal-sort")):
                raise ValueError("Index contains transaction/sort scratch; prepare a clean immutable index")
        for name in sorted(names):
            path = parent / name
            relative = path.relative_to(directory)
            if index and relative.parts[0] in COORDINATION:
                owned(path)
                continue
            if index and (name.startswith("transaction-") or name.endswith(".tmp")):
                raise ValueError("Index contains unfinished scratch: " + str(relative))
            before = owned(path)
            digest = bench.binary_identity(str(path))
            after = owned(path)
            if any(getattr(before, key) != getattr(after, key) for key in IDENTITY):
                raise RuntimeError("Fixture changed while hashing: " + str(path))
            files.append({"path": str(path), "relative": relative.as_posix(), "sha256": digest,
                          **{key: getattr(after, key) for key in IDENTITY}})
    if not files:
        raise ValueError("Empty fixture data directory: " + str(directory))
    return sorted(files, key=lambda item: item["relative"])


def checked_open(item):
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(item["path"], flags)
    try:
        value = os.fstat(fd)
        if (not stat.S_ISREG(value.st_mode) or value.st_uid != os.geteuid() or value.st_nlink != 1
                or any(getattr(value, key) != item[key] for key in IDENTITY)):
            raise RuntimeError("Fixture identity changed: " + item["path"])
    except BaseException:
        os.close(fd)
        raise
    return fd


class Residency:
    """Linux ABI only. PROT_NONE mappings cannot accidentally read file pages."""
    def __init__(self):
        if sys.platform != "linux" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise OSError("mincore verification requires 64-bit Linux ABI")
        self.page_size = os.sysconf("SC_PAGE_SIZE")
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_long]
        self.libc.mmap.restype = ctypes.c_void_p
        self.libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_ubyte)]
        self.libc.mincore.restype = ctypes.c_int
        self.libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        self.libc.munmap.restype = ctypes.c_int

    def pages(self, fd, length):
        if not length:
            return 0, 0
        count = (length + self.page_size - 1) // self.page_size
        address = self.libc.mmap(None, length, 0, 1, fd, 0)  # PROT_NONE, MAP_SHARED
        if address == ctypes.c_void_p(-1).value:
            raise OSError(ctypes.get_errno(), "mmap(PROT_NONE) failed")
        resident = 0
        try:
            for offset in range(0, count, 16384):
                pages = min(16384, count - offset)
                vector = (ctypes.c_ubyte * pages)()
                span = min(pages * self.page_size, length - offset * self.page_size)
                if self.libc.mincore(address + offset * self.page_size, span, vector):
                    raise OSError(ctypes.get_errno(), "mincore failed")
                resident += sum(value & 1 for value in vector)
        finally:
            if self.libc.munmap(address, length):
                raise OSError(ctypes.get_errno(), "munmap failed")
        return count, resident


def snapshot(files, probe):
    start = time.monotonic_ns()
    result = {"files": len(files), "files_checked": 0, "pages_checked": 0, "resident_pages": 0,
              "resident_examples": [], "errors": [], "observation_started_monotonic_ns": start}
    for item in files:
        fd = checked_open(item)
        try:
            if probe is None:
                raise OSError("mincore unavailable")
            pages, resident = probe.pages(fd, item["st_size"])
            result["files_checked"] += 1
            result["pages_checked"] += pages
            result["resident_pages"] += resident
            if resident and len(result["resident_examples"]) < 16:
                result["resident_examples"].append({"path": item["relative"], "resident_pages": resident})
        except (OSError, AttributeError) as error:
            if len(result["errors"]) < 16:
                result["errors"].append({"path": item["relative"], "error": str(error)})
        finally:
            os.close(fd)
    result["complete"] = result["files_checked"] == len(files)
    result["observation_finished_monotonic_ns"] = time.monotonic_ns()
    return result


def advise(files):
    result = {"attempted_files": len(files), "advised_files": 0, "errors": []}
    for item in files:
        fd = checked_open(item)
        try:
            # Only this already-owned file, from offset 0 through its entire data.
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            result["advised_files"] += 1
        except (OSError, AttributeError) as error:
            if len(result["errors"]) < 16:
                result["errors"].append({"path": item["relative"], "error": str(error)})
        finally:
            os.close(fd)
    result["complete"] = result["advised_files"] == len(files)
    return result


def cache_label(mode, advice, observed):
    if mode == "warm":
        return "warm-query-replay"
    if observed["complete"] and observed["pages_checked"] and observed["resident_pages"] == 0:
        return "verified-cold-file-data-pages"
    return "advice-only" if advice["complete"] else "unverified-cache-state"


def worker(spec):
    import resource
    ready = time.monotonic_ns()
    # This fresh helper never loads the potentially large fixture inventory.
    # Exactly one target child contributes to CPU/RSS and the OS I/O counters.
    measured = bench.worker({"kind": "command", "argv": spec["argv"],
                             "cwd": spec.get("cwd"), "timeout": spec["timeout"]})
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    measured["os_io"] = {"input_block_operations": usage.ru_inblock,
                         "output_block_operations": usage.ru_oublock,
                         "major_page_faults": usage.ru_majflt, "minor_page_faults": usage.ru_minflt,
                         "method": "isolated target getrusage(RUSAGE_CHILDREN); OS block-operation counters, not device-byte measurements"}
    measured["target_worker_ready_monotonic_ns"] = ready
    return measured


def measured(spec):
    files = json.loads(Path(spec["inventory"]).read_text(encoding="utf-8"))
    try:
        probe, unavailable = Residency(), None
    except (OSError, AttributeError) as error:
        probe, unavailable = None, str(error)
    before_advice = snapshot(files, probe) if spec["cache"] == "cold" else None
    advice = advise(files) if spec["cache"] == "cold" else None
    before = snapshot(files, probe)
    ready = time.monotonic_ns()
    completed = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--_worker"],
                               input=json.dumps({key: spec[key] for key in ("argv", "cwd", "timeout")}),
                               text=True, encoding="utf-8", capture_output=True, timeout=spec["timeout"] + 30)
    if completed.returncode:
        raise RuntimeError("Resource worker failed: " + completed.stderr)
    result = json.loads(completed.stdout)
    result["cache"] = {"requested": spec["cache"], "label": cache_label(spec["cache"], advice, before),
                         "probe_unavailable": unavailable, "before_advice": before_advice, "advice": advice,
                         "before_target": before, "target_ready_monotonic_ns": ready,
                         "after_target": snapshot(files, probe)}
    return result


def validate(sample, name, expected):
    if name == "rg":
        if sample["returncode"] not in (0, 1) or sample["stderr"] or bench.parse_rg_counts(sample["stdout"]) != expected:
            raise RuntimeError("ripgrep failed or changed paths/counts")
    else:
        response = bench.success(sample)
        if not response.get("execution_complete") or response["total"] != len(expected):
            raise RuntimeError("Search incomplete or matching-file count changed")
        sample["engine_metrics"] = response.get("metrics")


def rg_command(executable, case):
    # Engine regex reports matching lines; terms/literals count occurrences.
    counter = "--count" if case["name"] == "broad_regex" else "--count-matches"
    result = [executable, "--no-config", "--no-ignore", "--hidden", "--text", "--color", "never",
              "--case-sensitive", counter, "--null"]
    if case.get("fixed"):
        result.append("--fixed-strings")
    return result + ["--regexp", case["rg"], "."]


def finish_sample(sample, name, expected, oracle_digest):
    # Preserve full offending stdout on validation failure. Successful rg output
    # is represented by hashes and the shared complete compressed oracle.
    validate(sample, name, expected)
    if name == "rg":
        data = sample.pop("stdout").encode("utf-8")
        sample["stdout_bytes"] = len(data)
        sample["stdout_sha256"] = hashlib.sha256(data).hexdigest()
        sample["path_count_sha256"] = oracle_digest
        sample["matching_files"] = len(expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--rg", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="New output directory outside source/index trees")
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--query", action="append", choices=("literal", "broad_regex", "term"))
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--seed", type=int, default=1729)
    args = parser.parse_args()
    if sys.platform != "linux" or not 1 <= args.iterations <= 100 or args.timeout <= 0:
        parser.error("Requires Linux, 1..100 repetitions and positive timeout")
    root, source, indexes, fixture = load_fixture(args.fixture)
    work = args.work.resolve()
    if any(work == path or path in work.parents for path in (source, *indexes.values())):
        parser.error("Output cannot be inside source or index data")
    work.mkdir(parents=True, exist_ok=False)
    binaries = {name: str(getattr(args, name).resolve(strict=True)) for name in ("baseline", "current", "rg")}
    report = {"format": "researchify-cache-comparison-v1", "complete": False,
              "host": {"platform": platform.platform(), "machine": platform.machine(),
                       "python": platform.python_version(), "logical_cpus": os.cpu_count()},
              "fixture": fixture, "fixture_marker": str(args.fixture.resolve()), "seed": args.seed,
              "fixture_marker_sha256": bench.binary_identity(str(args.fixture)),
              "drivers": {name: {"path": str(Path(__file__).with_name(name).resolve()),
                                 "sha256": bench.binary_identity(str(Path(__file__).with_name(name)))}
                          for name in ("cache_benchmark.py", "perf_benchmark.py", "tiny_comparison.py", "compare_sessions.py")},
              "binaries": {name: {"path": path, "sha256": bench.binary_identity(path)} for name, path in binaries.items()},
              "conditions": {"cache_scope": "scoped source/index file data pages only; metadata is touched by setup/probes and not evicted; binaries, shared libraries and device caches are uncontrolled",
                             "cold": "owned-file posix_fadvise DONTNEED then PROT_NONE mincore; verified only if all observed ranges have zero resident pages",
                             "observation_window": "ranges observed sequentially; another process or readahead could repopulate earlier ranges before target launch",
                             "warm": "one identical fresh-process query replay immediately before each warm measurement",
                             "process": "fresh target and isolated resource helper per sample; helper and cache setup excluded from target timing",
                             "rss": "POSIX high-water can include inherited Python pre-exec floor; resource helper never loads fixture inventories",
                             "comparison": "RS count-only; rg occurrence counts for term/literal and matching-line counts for regex; rg emits per-file counts, included in target timing; untimed full path/count equivalence first",
                             "index_build": "excluded, indexes must be constructed separately", "memory_mb": 256,
                             "threading": "each executable's default", "p95": "nearest rank; with 7 repetitions p95 is the maximum"},
              "inventory": {}, "correctness": [], "rows": []}
    output = work / "results.json"
    save(output, report)
    try:
        scopes = {"source": source, **indexes}
        inventories = {name: inventory(path, index=name != "source") for name, path in scopes.items()}
        for name, rows in inventories.items():
            path = work / (name + "-inventory.json")
            save(path, rows)
            report["inventory"][name] = {"path": str(path), "sha256": bench.binary_identity(str(path)),
                                         "files": len(rows), "bytes": sum(item["st_size"] for item in rows)}
        states = {name: bench.success(bench.command([binaries[name], "status", "--index", str(indexes[name]), "--json"], args.timeout))
                  for name in indexes}
        for name, state in states.items():
            if (Path(state["root"]).resolve() != source or state["files"] != len(inventories["source"])
                    or state["bytes"] != report["inventory"]["source"]["bytes"]):
                raise RuntimeError("Index must cover the complete fixture source: " + name)
        if any(states["baseline"][key] != states["current"][key] for key in ("files", "lines", "bytes", "profile")):
            raise RuntimeError("Indexes have different source/profile contracts")
        report["indexes"] = states
        report["ripgrep_version"] = bench.command([binaries["rg"], "--version"], args.timeout)
        suite = [case for case in bench.cases(states["baseline"]["files"]) if case["name"] in (args.query or ["literal", "broad_regex", "term"])]
        rng = random.Random(args.seed)
        for case in suite:
            commands = {name: [binaries[name], "find", case["query"], "--index", str(indexes[name]), "--json",
                                "--limit", "0", "--snippets", "0", "--memory-mb", "256",
                                "--regex-steps", "18446744073709551615"] for name in indexes}
            commands["rg"] = rg_command(binaries["rg"], case)
            reference = bench.command(commands["rg"], args.timeout, str(source))
            if reference["returncode"] not in (0, 1) or reference["stderr"]:
                raise RuntimeError("Correctness ripgrep command failed")
            expected = bench.parse_rg_counts(reference["stdout"])
            if not expected:
                raise RuntimeError("Generated-fixture query unexpectedly has no matches: " + case["name"])
            for name in indexes:
                if all_hits(binaries[name], indexes[name], case, args.timeout) != expected:
                    raise RuntimeError("Full path/count mismatch: " + name + "/" + case["name"])
            oracle = json.dumps(expected, sort_keys=True).encode()
            oracle_digest = hashlib.sha256(oracle).hexdigest()
            oracle_path = work / (case["name"] + "-oracle.json.gz")
            oracle_path.write_bytes(gzip.compress(oracle, mtime=0))
            report["correctness"].append({"query": case, "files": len(expected), "complete_paths_counts_equal": True,
                                          "path_count_sha256": oracle_digest,
                                          "oracle": {"path": str(oracle_path), "sha256": bench.binary_identity(str(oracle_path)),
                                                     "uncompressed_bytes": len(oracle)}})
            for mode in ("cold", "warm"):
                row = {"query": case, "mode": mode, "commands": commands, "orders": [], "tools": {name: [] for name in binaries}}
                report["rows"].append(row)
                for iteration in range(args.iterations):
                    order = list(binaries)
                    rng.shuffle(order)
                    row["orders"].append(order)
                    for name in order:
                        cwd = str(source) if name == "rg" else None
                        if mode == "warm":
                            warmup = bench.command(commands[name], args.timeout, cwd)
                            validate(warmup, name, expected)
                        sample = measured({"cache": mode, "argv": commands[name], "cwd": cwd, "timeout": args.timeout,
                                           "inventory": report["inventory"]["source" if name == "rg" else name]["path"]})
                        row["tools"][name].append(sample)  # Preserve an offending raw sample too.
                        finish_sample(sample, name, expected, oracle_digest)
                        save(output, report)
                    print(f"{case['name']} {mode}: {iteration + 1}/{args.iterations}", file=sys.stderr, flush=True)
                # Never mix verified-cold and unverified/advice-only timings.
                row["summaries"] = {}
                for name, samples in row["tools"].items():
                    labels = sorted({sample["cache"]["label"] for sample in samples})
                    row["summaries"][name] = {label: bench.metric_summary([s for s in samples if s["cache"]["label"] == label]) for label in labels}
                save(output, report)
        for name, directory in scopes.items():
            if inventory(directory, index=name != "source") != inventories[name]:
                raise RuntimeError("Fixture contents or identities changed during benchmark: " + name)
        for name, path in binaries.items():
            if bench.binary_identity(path) != report["binaries"][name]["sha256"]:
                raise RuntimeError("Executable changed during benchmark: " + name)
        if bench.binary_identity(str(args.fixture)) != report["fixture_marker_sha256"]:
            raise RuntimeError("Fixture marker changed during benchmark")
        for name, identity in report["drivers"].items():
            if bench.binary_identity(identity["path"]) != identity["sha256"]:
                raise RuntimeError("Benchmark driver changed during measurement: " + name)
        report["complete"] = True
        save(output, report)
        print("RESEARCHIFY_CACHE_SUMMARY " + json.dumps({"format": "researchify-cache-summary-v1", "complete": True,
              "host": report["host"], "binaries": report["binaries"], "conditions": report["conditions"],
              "rows": [{"query": row["query"]["name"], "mode": row["mode"], "summaries": brief(row["summaries"])} for row in report["rows"]]}))
    except BaseException as error:
        report["error"] = type(error).__name__ + ": " + str(error)
        save(output, report)
        raise


if __name__ == "__main__":
    if sys.argv[1:] == ["--_worker"]:
        print(json.dumps(worker(json.load(sys.stdin))))
    else:
        main()

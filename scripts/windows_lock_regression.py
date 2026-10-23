#!/usr/bin/env python3
"""Prove the Windows delete-pending lock regression with old and fixed cores.

The original checkout is temporarily given the current compatible v2 test file.
Only its test target is built; no core source or comparison CLI is replaced.
This is an expected-failure correctness experiment, not a performance benchmark.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import time


BASELINE = "eb858348169157686e70104af6b64f9539a39fe8"
READER = "windows_reader_waits_for_delete_pending_access_lock"
WRITER = "windows_writer_does_not_steal_delete_pending_lock"
OLD_MESSAGE = "access.lock is held or could not be created; recover only after stopping owners"


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(work, report):
    temporary = work / "summary.json.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(work / "summary.json")


def execute(work, report, label, command, timeout):
    log = work / "logs" / f"{len(report['commands']):02d}-{label}.log"
    row = {"label": label, "command": [str(x) for x in command], "log": str(log.relative_to(work)),
           "complete": False, "started_utc": datetime.now(timezone.utc).isoformat()}
    report["commands"].append(row)
    save(work, report)
    started = time.perf_counter()
    try:
        with log.open("wb") as output:
            result = subprocess.run(row["command"], stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=False)
        row.update(returncode=result.returncode, complete=True)
    except BaseException as error:
        row["error"] = type(error).__name__ + ": " + str(error)
        raise
    finally:
        row["wall_seconds"] = time.perf_counter() - started
        if log.exists():
            row.update(log_bytes=log.stat().st_size, log_sha256=sha256(log))
        save(work, report)
    return row, log.read_text(encoding="utf-8", errors="replace")


def checked(work, report, label, command, timeout):
    row, output = execute(work, report, label, command, timeout)
    if row["returncode"] != 0:
        raise RuntimeError(label + " failed; see " + row["log"])
    return output.strip()


def case_names(source):
    names = re.findall(r'^\s*\{"([A-Za-z0-9_]+)",\s*\[\]\s*\{', source, re.MULTILINE)
    if len(names) != len(set(names)) or not {READER, WRITER, "concurrent_gc_and_reader_open_are_coordinated"}.issubset(names):
        raise RuntimeError("Cannot identify complete v2 test case inventory")
    return names


def validate_run(output, returncode, expected_names, legacy):
    """An arbitrary nonzero exit or a different assertion is never a valid control."""
    cases = re.findall(r"^(PASS|FAIL) ([A-Za-z0-9_]+)(?:: ([^\r\n]*))?$", output, re.MULTILINE)
    names = [name for _, name, _ in cases]
    if len(names) != len(set(names)) or set(names) != set(expected_names):
        raise RuntimeError("Missing, duplicate or unexpected test results")
    failures = {name: message for status, name, message in cases if status == "FAIL"}
    wanted = {READER: OLD_MESSAGE} if legacy else {}
    if failures != wanted or returncode != (1 if legacy else 0):
        raise RuntimeError("Unexpected exit or test failure(s): " + json.dumps(failures, sort_keys=True))
    totals = re.findall(r"^(\d+)/(\d+) cases; (\d+) assertions$", output, re.MULTILINE)
    expected_passes = len(expected_names) - len(wanted)
    if len(totals) != 1 or tuple(map(int, totals[0][:2])) != (expected_passes, len(expected_names)) or int(totals[0][2]) <= 0:
        raise RuntimeError("Incomplete or inconsistent test summary")
    proofs = re.findall(r"^DELETE_PENDING (access\.lock|writer\.lock) created=0 error=([^:\s]+):(\d+) \(([^\r\n]*)\) legacy_wait_would_reject=1$",
                        output, re.MULTILINE)
    if len(proofs) != 2 or {row[0] for row in proofs} != {"access.lock", "writer.lock"} or not all(int(row[2]) > 0 for row in proofs):
        raise RuntimeError("Missing actual delete-pending filesystem-error proof")
    return {"cases": len(expected_names), "passed": expected_passes, "assertions": int(totals[0][2]),
            "failures": failures, "delete_pending": [{"lock": name, "category": category, "value": int(value), "message": message}
                                                     for name, category, value, message in proofs]}


def cmake_cache(build, source):
    values = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^/#:][^:]*):[^=]+=(.*)$", line)
        if match:
            values[match[1]] = match[2]
    if (Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve() != source
            or values.get("CMAKE_GENERATOR") != "Visual Studio 17 2022"
            or values.get("CMAKE_GENERATOR_PLATFORM") != "x64"
            or values.get("RESEARCHIFY_STATIC_RUNTIME") != "ON"):
        raise RuntimeError("Unexpected source/generator/architecture/runtime in CMake cache")
    return {key: value for key, value in values.items() if key.startswith(("CMAKE_CXX", "CMAKE_GENERATOR"))
            or key in ("CMAKE_HOME_DIRECTORY", "BUILD_TESTING", "RESEARCHIFY_STATIC_RUNTIME", "RESEARCHIFY_LTO")}


def inventory(source, names):
    return {name: sha256(source / name) for name in names}


def run(args):
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    (work / "logs").mkdir()
    report = {"format": "researchify-windows-lock-regression-v1", "complete": False,
              "started_utc": datetime.now(timezone.utc).isoformat(), "platform": platform.platform(),
              "machine": platform.machine(), "python": platform.python_version(),
              "ci": {key: os.environ.get(key) for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_JOB", "RUNNER_NAME", "RUNNER_OS", "RUNNER_ARCH")},
              "scope": "Native Windows correctness control: same v2 test source, original versus fixed core; not performance",
              "commands": [], "restoration": {"checked": False, "restored": False}, "checks": {}}
    report["driver"] = {"path": str(Path(__file__).resolve()), "sha256": sha256(__file__)}
    save(work, report)
    original_test = test_path = original_stat = None
    baseline = current = None
    core_names = {}
    binaries = {}
    failure = None
    try:
        if platform.system() != "Windows" or platform.machine().lower() not in ("amd64", "x86_64"):
            raise RuntimeError("The lock regression gate requires native Windows x86-64")
        baseline = args.baseline_source.resolve(strict=True)
        current = args.current_source.resolve(strict=True)
        if baseline == current:
            raise RuntimeError("Control checkout must be separate from current source")
        builds = {"baseline": (baseline / "build").resolve(strict=True), "current": args.current_build.resolve(strict=True)}
        sources = {"baseline": baseline, "current": current}
        report["sources"] = {}
        report["cmake"] = {}
        for name, source in sources.items():
            head = checked(work, report, name + "-revision", ["git", "-C", source, "rev-parse", "HEAD"], args.timeout)
            if name == "baseline" and head != BASELINE:
                raise RuntimeError("Control checkout is not the pinned original revision")
            dirty = checked(work, report, name + "-status", ["git", "-C", source, "status", "--porcelain", "--untracked-files=no"], args.timeout)
            if dirty:
                raise RuntimeError(name + " tracked source is already modified")
            paths = checked(work, report, name + "-core-files",
                            ["git", "-C", source, "ls-files", "-z", "src", "include", "cmake", "CMakeLists.txt", "tests/test_support.hpp"], args.timeout)
            core_names[name] = [path for path in paths.split("\0") if path]
            if not {"src/engine.cpp", "CMakeLists.txt", "tests/test_support.hpp"}.issubset(core_names[name]):
                raise RuntimeError("Incomplete tracked core inventory")
            report["sources"][name] = {"directory": str(source), "revision": head,
                                       "core_sha256": inventory(source, core_names[name])}
            report["cmake"][name] = cmake_cache(builds[name], source)
        test_path = baseline / "tests/v2_tests.cpp"
        current_test = current / "tests/v2_tests.cpp"
        if test_path.is_symlink() or current_test.is_symlink():
            raise RuntimeError("Test source must be a regular checkout file")
        original_test = test_path.read_bytes()
        original_stat = test_path.stat()
        test = current_test.read_bytes()
        names = case_names(test.decode("utf-8-sig"))
        if (baseline / "tests/test_support.hpp").read_bytes() != (current / "tests/test_support.hpp").read_bytes():
            raise RuntimeError("Control and fixed test support differ")
        report["test"] = {"source": str(current_test), "sha256": sha256(current_test),
                          "original_baseline_sha256": sha256(test_path), "cases": names}
        binaries = {"baseline_cli": builds["baseline"] / "Release/rs.exe",
                    "current_cli": builds["current"] / "Release/rs.exe",
                    "current_test": builds["current"] / "Release/cs_v2_tests.exe",
                    "current_core": builds["current"] / "Release/researchify_core.lib"}
        report["binaries"] = {name: {"path": str(path), "sha256": sha256(path)} for name, path in binaries.items()}
        control_core = builds["baseline"] / "Release/researchify_core.lib"
        report["baseline_core_before_sha256"] = sha256(control_core)
        save(work, report)
        test_path.write_bytes(test)
        if test_path.read_bytes() != test:
            raise RuntimeError("Control test copy was not byte-identical")
        checked(work, report, "configure-control", ["cmake", "-S", baseline, "-B", builds["baseline"],
                "-G", "Visual Studio 17 2022", "-A", "x64", "-DBUILD_TESTING=ON", "-DRESEARCHIFY_STATIC_RUNTIME=ON"], args.timeout)
        checked(work, report, "build-control-test", ["cmake", "--build", builds["baseline"], "--target", "cs_v2_tests",
                "--config", "Release", "--parallel", "4"], args.timeout)
        control_test = builds["baseline"] / "Release/cs_v2_tests.exe"
        report["control_binaries"] = {name: {"path": str(path), "sha256": sha256(path)}
                                       for name, path in (("core", control_core), ("test", control_test))}
        for name, executable, legacy in (("original-control", control_test, True), ("fixed-control", binaries["current_test"], False)):
            row, output = execute(work, report, name, [executable], args.timeout)
            report["checks"][name] = validate_run(output, row["returncode"], names, legacy)
            save(work, report)
        report["complete"] = True
    except BaseException as error:
        failure = error
        report["error"] = type(error).__name__ + ": " + str(error)
    finally:
        try:
            if original_test is not None:
                test_path.write_bytes(original_test)
                os.utime(test_path, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
                restored = test_path.read_bytes() == original_test
                report["restoration"] = {"checked": True, "restored": restored, "sha256": sha256(test_path)}
                if not restored:
                    raise RuntimeError("Original baseline test source was not restored")
            for name, source in (("baseline", baseline), ("current", current)):
                if name in core_names and name in report.get("sources", {}):
                    if inventory(source, core_names[name]) != report["sources"][name]["core_sha256"]:
                        raise RuntimeError(name + " core source changed during the experiment")
            for name, path in binaries.items():
                if name in report.get("binaries", {}) and sha256(path) != report["binaries"][name]["sha256"]:
                    raise RuntimeError(name + " executable/library changed during the experiment")
            for name, identity in report.get("control_binaries", {}).items():
                if sha256(identity["path"]) != identity["sha256"]:
                    raise RuntimeError("Control " + name + " binary changed during the experiment")
            if "test" in report and sha256(current / "tests/v2_tests.cpp") != report["test"]["sha256"]:
                raise RuntimeError("Current test source changed during the experiment")
            if sha256(__file__) != report["driver"]["sha256"]:
                raise RuntimeError("Control driver changed during the experiment")
            if original_test is not None:
                dirty = checked(work, report, "restored-baseline-status", ["git", "-C", baseline, "status", "--porcelain", "--untracked-files=no"], args.timeout)
                if dirty:
                    raise RuntimeError("Baseline tracked tree is not clean after restoration")
        except BaseException as error:
            report["restoration"]["error"] = type(error).__name__ + ": " + str(error)
            if failure is None:
                failure = error
                report["error"] = report["restoration"]["error"]
        if failure is not None or not report["restoration"]["restored"]:
            report["complete"] = False
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(work, report)
    if failure is not None:
        raise failure
    if not report["complete"]:
        raise RuntimeError("Incomplete Windows lock control experiment")
    print("RESEARCHIFY_WINDOWS_LOCK_REGRESSION " + json.dumps({
        "format": report["format"], "complete": True, "ci": report["ci"], "checks": report["checks"],
        "platform": report["platform"], "machine": report["machine"], "driver": report["driver"],
        "sources": {name: {"revision": identity["revision"], "engine_sha256": identity["core_sha256"]["src/engine.cpp"]}
                    for name, identity in report["sources"].items()},
        "test": report["test"], "binaries": report["binaries"], "control_binaries": report["control_binaries"],
        "restoration": report["restoration"]}), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-source", type=Path, required=True)
    parser.add_argument("--current-source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--current-build", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    if not 0 < args.timeout <= 600:
        parser.error("timeout must be in (0, 600]")
    run(args)


if __name__ == "__main__":
    main()

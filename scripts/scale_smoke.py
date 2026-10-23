#!/usr/bin/env python3
"""Generate a million-tiny-file fixture, then test indexing and targeted searches.

Generation is a separate command so it does not overlap timed query workloads.
Only explicitly supplied, trusted local binaries are executed. Existing corpus,
index, or result targets are never removed or overwritten.
"""

import argparse
import json
from pathlib import Path
import platform

from perf_benchmark import binary_identity, command, index_storage, success


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("generate", "run"))
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--rs", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New JSON report under an existing directory, outside the source and index")
    parser.add_argument("--index", default="optimized-compact")
    parser.add_argument("--files", type=int, default=1_000_000)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--memory-mb", type=int, default=256)
    parser.add_argument("--timeout", type=float, default=1800)
    args = parser.parse_args()
    bench = args.bench.resolve(strict=True)
    work = args.work.resolve()
    output = args.output.resolve()
    if output.exists():
        raise RuntimeError("Report exists; refusing to overwrite: " + str(output))
    if args.files < 1_000_000 or args.threads < 1 or args.memory_mb < 16:
        raise ValueError("Use at least one million files, a positive worker count, and at least 16MiB")
    if args.index in ("", ".", "..", "source") or any(c in args.index for c in "/\\:"):
        raise ValueError("Index must be a simple non-source directory name")
    if output == work or output.is_relative_to(work / "source") or output.is_relative_to(work / args.index):
        raise ValueError("Report must not be the workspace, source contents, or index contents")
    if not output.parent.is_dir():
        raise ValueError("Report parent directory must already exist; put the report outside the new workspace")
    if args.phase == "generate" and work.exists():
        raise RuntimeError("Generation requires a new workspace directory")
    if args.phase == "run" and (work / args.index).exists():
        raise RuntimeError("Target index exists; refusing to overwrite")
    report = {"phase": args.phase, "platform": platform.platform(),
              "machine": platform.machine(), "benchmark_binary_sha256": binary_identity(str(bench)),
              "work": str(work), "samples_per_operation": 1,
              "conditions": "Scalability/correctness smoke, not a repeated speed benchmark; OS cache is uncontrolled",
              "memory_budget_bytes": args.memory_mb * 1024 * 1024,
              "resource_caveat": "Isolated helper measurement; POSIX RSS can include Python pre-exec high-water floor",
              "status": "running", "operations": []}

    def save():
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    def measured(name, argv, check_memory=True):
        print("START " + name, flush=True)
        result = command([str(value) for value in argv], timeout=args.timeout)
        result.update({"name": name, "command": [str(value) for value in argv]})
        report["operations"].append(result)
        save()
        if result["returncode"] or result["stderr"]:
            raise RuntimeError("Operation failed: " + name + ": " + result["stderr"] + result["stdout"])
        rss = result.get("peak_rss_bytes")
        if check_memory and rss is not None and rss > report["memory_budget_bytes"]:
            raise AssertionError(f"{name}: actual peak RSS {rss} exceeds the requested memory budget")
        print(f"PASS {name}: wall={result['wall_seconds']:.6f}s peak_rss_bytes={rss}", flush=True)
        return result

    save()
    try:
        if args.phase == "generate":
            measured("generate_fixture", [bench, "--phase", "generate", "--work", work,
                                          "--files", args.files, "--lines", 5], check_memory=False)
        else:
            if args.rs is None:
                raise ValueError("--rs is required for the run phase")
            rs = args.rs.resolve(strict=True)
            report["rs_binary_sha256"] = binary_identity(str(rs))
            meta = (work / "benchmark.meta").read_text(encoding="utf-8").split()
            if meta != ["RESEARCHIFY_BENCH_1", str(args.files), "5"]:
                raise AssertionError("Fixture metadata does not describe the requested million-file/five-line corpus")
            index = work / args.index
            measured("create_compact", [bench, "--phase", "create", "--work", work, "--index", args.index,
                                         "--profile", "compact", "--threads", args.threads,
                                         "--memory-mb", args.memory_mb])
            report["storage_immediately_after_build"] = index_storage(str(index))
            report["transaction_directories_immediately_after_build"] = [p.name for p in index.glob("transaction-*")]
            save()

            def cli(name, verb, *arguments):
                return success(measured(name, [rs, verb, "--index", index, "--memory-mb", args.memory_mb,
                                                "--json", *arguments]))

            status = cli("status", "status")
            if status["files"] != args.files or status["lines"] != args.files * 5:
                raise AssertionError("Index corpus counts differ from generator metadata")
            last = args.files - 1
            cases = [
                ("rare_first", "needle_42", 1, {"group_0/module_42.cpp"}, 1),
                ("rare_last", f"needle_{last}", 1, {f"group_{last // 1000}/module_{last}.cpp"}, 1),
                ("missing", "absent_unique_symbol_no_match", 0, set(), None),
                ("common", "common", args.files, None, 4),
                ("and", "needle_42 AND common", 1, {"group_0/module_42.cpp"}, 5),
                ("or", f"needle_42 OR needle_{last}", 2,
                 {"group_0/module_42.cpp", f"group_{last // 1000}/module_{last}.cpp"}, 1),
                ("not", "common AND NOT needle_42", args.files - 1, None, 4),
                ("literal", 'needle_42 AND "needle_42 = 0;"', 1, {"group_0/module_42.cpp"}, 2),
                ("regex", "needle_42 AND /needle_[0-9]+/", 1, {"group_0/module_42.cpp"}, 2),
                ("literal_full_scan", '"needle_42 = 0;"', 1, {"group_0/module_42.cpp"}, 1),
            ]
            for name, query, total, paths, matches in cases:
                profile_arguments = ["--profile"] if name == "literal_full_scan" else []
                response = cli("find_" + name, "find", "--query", query, "--limit", 3,
                               "--snippets", 1, "--threads", args.threads, *profile_arguments)
                if response["total"] != total or not response["execution_complete"] or response["output_truncated"]:
                    raise AssertionError("Incomplete or incorrect result count for " + name)
                results = response["results"]
                if len(results) != min(total, 3):
                    raise AssertionError("Incorrect bounded result count for " + name)
                if paths is not None and {hit["path"] for hit in results} != paths:
                    raise AssertionError("Incorrect matching paths for " + name)
                if matches is not None and any(hit["matches"] != matches for hit in results):
                    raise AssertionError("Incorrect occurrence counts for " + name)
                if name.startswith("rare_") and any(hit["snippets"][0]["line"] != 2 for hit in results):
                    raise AssertionError("Incorrect rare-term line position")
                if name == "literal_full_scan":
                    report["literal_full_scan"] = {
                        "instrumentation_enabled": True,
                        "conditions": "One instrumented full-scan scalability check, not a paired speed comparison",
                        "metrics": response["metrics"],
                    }
                    save()
            snapshot = cli("read_last_snapshot", "read", "--file", f"group_{last // 1000}/module_{last}.cpp",
                           "--first", 1, "--last", 5)
            expected_text = (f"// generated file {last}\nnamespace bucket{last % 100} {{ int needle_{last} = 0;\n"
                             + "".join(f"int function_{last}_{line}(int value) {{ int common = value + {line}; return common; }}\n"
                                       for line in (2, 3)) + "}\n")
            if snapshot["text"] != expected_text or snapshot["first_line"] != 1 or snapshot["last_line"] != 5:
                raise AssertionError("Stored snapshot does not match generated source content")
            verified = cli("full_verify", "verify")
            if verified["files"] != args.files:
                raise AssertionError("Full verification did not cover every generated file")
            report["query_cases_passed"] = len(cases)
            rss_values = [row.get("peak_rss_bytes") for row in report["operations"]]
            report["all_operation_rss_available"] = all(value is not None for value in rss_values)
            report["peak_rss_bytes_max"] = max((value for value in rss_values if value is not None), default=None)
            report["memory_budget_verified"] = (report["all_operation_rss_available"]
                                                and report["peak_rss_bytes_max"] is not None
                                                and report["peak_rss_bytes_max"] <= report["memory_budget_bytes"])
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

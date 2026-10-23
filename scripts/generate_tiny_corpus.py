#!/usr/bin/env python3
"""Create a deterministic eight-line tiny-file performance surrogate.

This is not the user's original Windows corpus. It deliberately exercises the
same small-document regime and uses the existing benchmark metadata contract.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time


def contents(number: int) -> bytes:
    return (f"// generated tiny file {number}\n"
            f"namespace bucket{number % 100} {{\n"
            f"int needle_{number} = 0;\n"
            f"int common = needle_{number};\n"
            f"int function_{number}_4(int value) {{return value;}}\n"
            f"int get_{number}() {{ return common; }}\n"
            "}\n// end\n").encode("utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="New benchmark directory")
    parser.add_argument("--files", type=int, default=100000)
    args = parser.parse_args()
    if not 64 <= args.files <= 1000000:
        parser.error("Use between 64 and 1,000,000 files")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    source = work / "source"
    source.mkdir()
    start = time.perf_counter()
    digest = hashlib.sha256()
    total = 0
    for number in range(args.files):
        relative = f"group_{number // 1000}/module_{number}.cpp"
        target = source / relative
        if number % 1000 == 0:
            target.parent.mkdir()
        data = contents(number)
        target.write_bytes(data)
        total += len(data)
        digest.update(relative.encode("ascii") + b"\0" + data + b"\0")
    (work / "benchmark.meta").write_text(f"RESEARCHIFY_BENCH_1\n{args.files} 8\n", encoding="ascii")
    result = {"format": "researchify-tiny-corpus-v1", "kind": "synthetic-surrogate",
              "files": args.files, "lines": 8 * args.files, "bytes": total,
              "path_and_content_sha256": digest.hexdigest(),
              "generation_seconds": time.perf_counter() - start}
    (work / "fixture.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()

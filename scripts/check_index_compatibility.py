#!/usr/bin/env python3
"""Check old/new binary readers against indexes written by either binary.

Supply locally built, trusted executables. No downloads, release publication, or
existing user indexes are touched; all fixtures live in a temporary directory.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def request(executable, index, command, **arguments):
    message = {"command": command, "index": str(index), "memory_mb": 64, **arguments}
    process = subprocess.run([str(executable), "request", "--stdin"],
                             input=json.dumps(message, ensure_ascii=False) + "\n",
                             text=True, encoding="utf-8", capture_output=True, timeout=60)
    if process.returncode:
        raise RuntimeError(f"{executable.name} {command} exited {process.returncode}: "
                           f"{process.stdout} {process.stderr}")
    response = json.loads(process.stdout)
    if response.get("status") != "ok" or not response.get("execution_complete"):
        raise RuntimeError(f"{executable.name} {command}: {response}")
    if response.get("output_truncated") or response.get("has_more"):
        raise RuntimeError(f"Fixture response unexpectedly truncated: {response}")
    return response


def check(old, new):
    comparisons = []
    with tempfile.TemporaryDirectory(prefix="researchify-compat-") as temporary:
        work = Path(temporary)
        source = work / "source"
        source.mkdir()
        content = {
            "a.cpp": "\ufeffcommon needle_42 = 0;\r\n" + "return common;\r\n" * 80,
            "тест/é.cpp": "common other_identifier\n" + "Привет world;\n" * 80,
            "small.cpp": "common xneedle_42 = 0; needle_420;\n",
        }
        for name, text in content.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode("utf-8"))
        queries = ["common", "needle_42", '"needle_42 = 0;"', '"Привет world;"',
                   "/needle_[0-9]+/", "common AND NOT other_identifier", "absent_symbol"]
        for profile in ("compact", "fast"):
            for writer_name, writer in (("old", old), ("new", new)):
                index = work / f"{writer_name}-{profile}"
                request(writer, index, "create", root=str(source), profile=profile, threads=2)
                if profile == "compact" and writer_name == "new" and not list(index.rglob("literal.tbl")):
                    raise AssertionError("New compact fixture did not exercise the optional sidecar")
                verifications = [request(reader, index, "verify") for reader in (old, new)]
                if any(result["files"] != len(content) for result in verifications):
                    raise AssertionError("Reader verified a different file count")
                old_files = request(old, index, "files", limit=100)
                new_files = request(new, index, "files", limit=100)
                if old_files["files"] != new_files["files"]:
                    raise AssertionError("File catalog/content identities differ between readers")
                for query in queries:
                    before = request(old, index, "find", query=query, limit=100, snippets=3)
                    after = request(new, index, "find", query=query, limit=100, snippets=3)
                    for field in ("total", "results"):
                        if before[field] != after[field]:
                            raise AssertionError(f"{writer_name}/{profile} query={query!r}: {field} differs")
                for name in content:
                    before = request(old, index, "read", file=name, first=1, last=100)
                    after = request(new, index, "read", file=name, first=1, last=100)
                    for field in ("path", "content_id", "text", "first_line", "last_line"):
                        if before[field] != after[field]:
                            raise AssertionError(f"Snapshot reader differs for {name}: {field}")
                comparisons.append({"writer": writer_name, "profile": profile,
                                    "readers": ["old", "new"], "queries_compared": len(queries),
                                    "files_compared": len(content), "status": "passed"})
    return {"old_binary": str(old), "new_binary": str(new),
            "old_sha256": hashlib.sha256(old.read_bytes()).hexdigest(),
            "new_sha256": hashlib.sha256(new.read_bytes()).hexdigest(),
            "comparisons": comparisons}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old", type=Path, required=True, help="Trusted baseline rs executable")
    parser.add_argument("--new", type=Path, required=True, help="Trusted candidate rs executable")
    parser.add_argument("--output", type=Path, help="Optional JSON report; must not already exist")
    arguments = parser.parse_args()
    report = check(arguments.old.resolve(strict=True), arguments.new.resolve(strict=True))
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if arguments.output:
        with arguments.output.open("x", encoding="utf-8") as output:
            output.write(text)
    print(text, end="")


if __name__ == "__main__":
    main()

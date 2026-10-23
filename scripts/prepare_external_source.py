#!/usr/bin/env python3
"""Capture regular UTF-8 C/H sources from a fixed CPython or Linux archive.

Only an explicitly supplied local archive is read. No downloader, extractall,
symlink/hardlink extraction, overwrites, permissions, or archive timestamps are
used. The output is a new directory containing source/, manifest.json and an
update-fixture.json usable by update_benchmark.py. Every excluded member and
every retained file checksum are recorded. Original source bytes are preserved.
"""
from __future__ import annotations

import argparse
from collections import Counter
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import time


FORMAT = "researchify-external-source-v1"
PROJECTS = {
    "cpython": {
        "repository": "https://github.com/python/cpython", "tag": "v3.13.0",
        "commit": "60403a5409ff2c3f3b07dd2ca91a7a3e096839c7",
        "archive_sha256": "37c8e18b35e659da6ff4db74caea50ed4170cf66c6d51f97d22e436e338d7b5b",
        "selective_term": "PyImport_ImportModuleLevelObject",
        "selective_regex": "PyImport_[A-Za-z0-9_]+",
    },
    "linux": {
        "repository": "https://github.com/torvalds/linux", "tag": "v6.12",
        "commit": "adc218676eef25575469234709c2d87185ca223a",
        "archive_sha256": "8787cc90ca7740ab7c955b6fad83010dc600f14a6d94511b548703e4f0f40caa",
        "selective_term": "vfs_getattr_nosec",
        "selective_regex": "vfs_getattr_[A-Za-z0-9_]+",
    },
}
ABSENT = "researchify_absent_6e9ae0ce70a44e78bdf088c197fb7134"
WORD_BYTES = b"A-Za-z0-9_\x80-\xff"


def cases(project: str) -> list[dict]:
    details = PROJECTS[project]
    term = details["selective_term"]
    return [
        {"name": "selective_term", "kind": "term", "pattern": term, "query": term},
        {"name": "common_term", "kind": "term", "pattern": "return", "query": "return"},
        {"name": "selective_literal", "kind": "literal", "pattern": term + "(",
         "query": '"' + term + '("'},
        {"name": "common_literal", "kind": "literal", "pattern": "return ", "query": '"return "'},
        {"name": "negative_literal", "kind": "literal", "pattern": ABSENT, "query": '"' + ABSENT + '"'},
        {"name": "selective_regex", "kind": "regex", "pattern": details["selective_regex"],
         "query": "/" + details["selective_regex"] + "/"},
        {"name": "wide_regex", "kind": "regex", "pattern": "(return|if|while)[ \\t]",
         "query": "/(return|if|while)[ \\t]/"},
    ]


def sha256(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("ascii")


def dump_new(path: Path, value) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True)
        stream.write("\n")


def path_problem(name: str) -> str | None:
    """Conservative, portable relative paths; never resolve an archive link."""
    try:
        name.encode("utf-8", "strict")
    except UnicodeError:
        return "non_utf8_path"
    if not name or "\\" in name or "\0" in name or name.startswith("/"):
        return "unsafe_path"
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts):
        return "unsafe_path"
    reserved = {"CON", "PRN", "AUX", "NUL", *["COM" + str(i) for i in range(1, 10)],
                *["LPT" + str(i) for i in range(1, 10)]}
    for part in parts:
        if (any(ord(char) < 32 or char in '<>:"|?*' for char in part) or
                part.endswith((".", " ")) or part.split(".")[0].upper() in reserved):
            return "nonportable_path"
    if any(part in (".git", ".hg", ".svn", ".researchify") for part in parts[:-1]):
        return "engine_excluded_directory"
    return None


def exact_term_regex(term: str) -> re.Pattern:
    # All non-ASCII scalars are token characters in RS. On validated UTF-8,
    # testing their bytes gives exactly the same boundaries for an ASCII term.
    return re.compile(b"(?<![" + WORD_BYTES + b"])" + re.escape(term.encode("ascii")) +
                      b"(?![" + WORD_BYTES + b"])")


def capture(args) -> dict:
    project = PROJECTS[args.project]
    if args.archive.is_symlink():
        raise ValueError("--archive must not be a symlink")
    archive = args.archive.resolve(strict=True)
    if not archive.is_file() or archive.is_symlink():
        raise ValueError("--archive must be a regular local archive")
    actual_archive_hash = sha256(archive)
    if actual_archive_hash != project["archive_sha256"]:
        raise ValueError("Archive SHA256 differs from the recorded pinned-commit archive")
    work = args.output_dir.absolute()
    if work.exists():
        raise ValueError("--output-dir must not exist; existing artifacts are never overwritten")
    work.mkdir(parents=True, exist_ok=False)
    source = work / "source"
    source.mkdir()
    prefix = args.project + "-" + project["commit"]
    entries, skipped = [], []
    seen, portable_seen = set(), set()
    term_patterns = {case["name"]: exact_term_regex(case["pattern"])
                     for case in cases(args.project) if case["kind"] == "term"}
    term_reference = {name: {} for name in term_patterns}
    started = time.perf_counter()

    def skip(member, reason):
        skipped.append({"path": member.name, "bytes": member.size, "reason": reason})

    # Streaming mode avoids a second tar-directory pass and retains no archive
    # member data beyond the one explicitly bounded regular source file.
    with tarfile.open(archive, "r|*") as bundle:
        for member in bundle:
            raw_name = member.name.rstrip("/") if member.isdir() else member.name
            parts = PurePosixPath(raw_name).parts
            if not parts or parts[0] != prefix:
                raise ValueError("Archive member has an unexpected commit root: " + member.name)
            if member.isdir():
                skip(member, "directory")
                continue
            if not member.isfile():
                skip(member, "not_regular")
                continue
            relative = raw_name[len(prefix) + 1:]
            reason = path_problem(relative)
            if reason:
                skip(member, reason)
                continue
            if PurePosixPath(relative).suffix.lower() not in (".c", ".h"):
                skip(member, "not_c_or_h")
                continue
            if relative in seen:
                raise ValueError("Duplicate selected path: " + relative)
            if relative.casefold() in portable_seen:
                # A pinned source tree may contain legitimate case-distinct
                # files. Retain the first suitable archive entry so the same
                # captured subset can be created on case-insensitive volumes.
                skip(member, "case_collision")
                continue
            if not 0 <= member.size <= args.max_file_bytes:
                skip(member, "file_size_limit")
                continue
            stream = bundle.extractfile(member)
            if stream is None:
                raise ValueError("Cannot read selected regular archive member")
            with stream:
                data = stream.read(member.size + 1)
            if len(data) != member.size:
                raise ValueError("Truncated or oversized selected archive member: " + relative)
            if b"\0" in data:
                skip(member, "binary_nul")
                continue
            try:
                data.decode("utf-8", "strict")
            except UnicodeError:
                skip(member, "invalid_utf8")
                continue
            # stream_text applies max_line_bytes before CR/BOM normalization.
            maximum = max(map(len, data.split(b"\n")), default=0)
            if maximum > args.max_line_bytes:
                skip(member, "line_size_limit")
                continue
            target = source.joinpath(*relative.split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as output:
                output.write(data)
            seen.add(relative)
            portable_seen.add(relative.casefold())
            count_lines = data.count(b"\n") + bool(data and not data.endswith(b"\n"))
            entries.append({"path": relative, "bytes": len(data), "lines": count_lines,
                            "max_line_bytes": maximum, "sha256": hashlib.sha256(data).hexdigest()})
            token_data = data[3:] if data.startswith(b"\xef\xbb\xbf") else data
            for name, pattern in term_patterns.items():
                count = sum(1 for _ in pattern.finditer(token_data))
                if count:
                    term_reference[name][relative] = count
    entries.sort(key=lambda entry: entry["path"])
    if not entries:
        raise ValueError("The selected archive contains no suitable C/H source files")
    total_bytes = sum(entry["bytes"] for entry in entries)
    if total_bytes < args.minimum_bytes:
        raise ValueError("Selected source corpus is below the requested minimum bytes")
    manifest = {
        "format": FORMAT, "project": args.project, **project,
        "archive_url": project["repository"] + "/archive/" + project["commit"] + ".tar.gz",
        "archive_path": str(archive), "archive_bytes": archive.stat().st_size,
        "archive_sha256": actual_archive_hash,
        "provenance": "GitHub pinned-commit archive with recorded SHA256; original file bytes retained",
        "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "capture_seconds_excluding_archive_hash": time.perf_counter() - started,
        "source": str(source), "extensions": [".c", ".h"], "files": entries,
        "case_collision_policy": "Retain the first suitable regular UTF-8 C/H entry in pinned archive order; record later casefold collisions as skipped",
        "content_manifest_sha256": hashlib.sha256(canonical(entries)).hexdigest(),
        "file_count": len(entries), "bytes": total_bytes,
        "lines": sum(entry["lines"] for entry in entries),
        "index_policy": {"max_file_bytes": args.max_file_bytes, "max_line_bytes": args.max_line_bytes},
        "skipped": skipped, "skip_counts": dict(sorted(Counter(item["reason"] for item in skipped).items())),
        "term_reference": term_reference,
        "term_reference_method": "One capture-time pass per retained file; byte-exact RS ASCII token boundaries, including all non-ASCII bytes as word characters; first-file BOM stripped only for this oracle",
        "cases": cases(args.project),
    }
    manifest["manifest_sha256"] = hashlib.sha256(canonical(manifest)).hexdigest()
    dump_new(work / "manifest.json", manifest)
    editable = next(entry for entry in entries if entry["bytes"] and entry["path"].endswith(".c"))
    dump_new(work / "update-fixture.json", {
        "format": "researchify-update-fixture-v1", "source": str(source),
        "edit_file": editable["path"], "edit_sha256": editable["sha256"], "files": len(entries),
        "description": f"Unmodified regular UTF-8 C/H source subset of {args.project} {project['tag']} ({project['commit']}); see adjacent manifest.json",
    })
    return manifest


def validate_fixture(work: Path, verify_files: bool = True) -> dict:
    manifest = json.loads((work / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("format") != FORMAT or manifest.get("project") not in PROJECTS:
        raise ValueError("Not a supported pinned external-source fixture")
    check = dict(manifest)
    checksum = check.pop("manifest_sha256", None)
    if hashlib.sha256(canonical(check)).hexdigest() != checksum:
        raise ValueError("Fixture manifest checksum differs")
    project = PROJECTS[manifest["project"]]
    if any(manifest.get(key) != project[key] for key in ("commit", "archive_sha256")):
        raise ValueError("Fixture does not match the pinned upstream identity")
    source = (work / "source").resolve(strict=True)
    if source != Path(manifest["source"]).resolve(strict=True):
        raise ValueError("Fixture moved after capture; recapture instead of silently changing its identity")
    if verify_files:
        paths = set()
        for path in source.rglob("*"):
            if path.is_symlink():
                raise ValueError("Symlink appeared in captured source")
            if path.is_file():
                paths.add(path.relative_to(source).as_posix())
        if paths != {entry["path"] for entry in manifest["files"]}:
            raise ValueError("Captured file set changed")
        for entry in manifest["files"]:
            if path_problem(entry["path"]):
                raise ValueError("Unsafe manifest file path")
            path = source.joinpath(*entry["path"].split("/"))
            if path.stat().st_size != entry["bytes"] or sha256(path) != entry["sha256"]:
                raise ValueError("Captured file changed: " + entry["path"])
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", choices=tuple(PROJECTS), required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--max-file-bytes", type=int, default=1024 * 1024 * 1024)
    parser.add_argument("--max-line-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--minimum-bytes", type=int, default=0)
    args = parser.parse_args()
    if args.max_file_bytes < 1 or not 1 <= args.max_line_bytes <= 16 * 1024 * 1024 or args.minimum_bytes < 0:
        parser.error("Invalid file/line/minimum byte limit")
    manifest = capture(args)
    print(json.dumps({key: manifest[key] for key in
                      ("project", "tag", "commit", "file_count", "bytes", "lines", "skip_counts", "manifest_sha256")}, indent=2))


if __name__ == "__main__":
    main()

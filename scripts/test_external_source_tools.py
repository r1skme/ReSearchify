"""Standard-library tests for archive capture and independent result oracles.

Run explicitly: python -m unittest discover -s scripts -p test_external_source_tools.py
These tests never invoke ReSearchify/ripgrep or use an external source archive.
"""
from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import prepare_external_source as fixture
import external_source_benchmark as driver


class CaptureTests(unittest.TestCase):
    def make_archive(self, root: Path) -> Path:
        archive = root / "input.tar.gz"
        prefix = "cpython-" + fixture.PROJECTS["cpython"]["commit"]
        with tarfile.open(archive, "w:gz") as bundle:
            entries = {
                "src/a.c": b"\xef\xbb\xbfreturn;\r\nreturn return_\n",
                "include/a.h": "/* π */\nreturn;\n".encode(),
                "ignored.py": b"return\n",
                "invalid.c": b"\xff\n",
                "binary.h": b"a\0b",
                "long.c": b"a" * 129,
                "../escape.c": b"return;\n",
                "CON.c": b"return;\n",
            }
            for name, data in entries.items():
                member = tarfile.TarInfo(prefix + "/" + name)
                member.size = len(data)
                bundle.addfile(member, io.BytesIO(data))
            link = tarfile.TarInfo(prefix + "/linked.c")
            link.type = tarfile.SYMTYPE
            link.linkname = "../../outside.c"
            bundle.addfile(link)
        return archive

    def test_capture_preserves_bytes_skips_links_and_records_update_interface(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = self.make_archive(root)
            output = root / "capture"
            args = argparse.Namespace(project="cpython", archive=archive, output_dir=output,
                                      max_file_bytes=1024, max_line_bytes=128, minimum_bytes=1)
            # Unit fixture only: production capture always requires the pinned
            # archive hash embedded in PROJECTS. Never alter a real archive.
            with patch.dict(fixture.PROJECTS["cpython"], {"archive_sha256": fixture.sha256(archive)}):
                manifest = fixture.capture(args)
                self.assertEqual(manifest["file_count"], 2)
                self.assertEqual(manifest["lines"], 4)
                self.assertEqual(manifest["term_reference"]["common_term"], {"src/a.c": 2, "include/a.h": 1})
                self.assertEqual((output / "source/src/a.c").read_bytes(), b"\xef\xbb\xbfreturn;\r\nreturn return_\n")
                for reason in ("not_c_or_h", "invalid_utf8", "binary_nul", "line_size_limit", "unsafe_path", "nonportable_path", "not_regular"):
                    self.assertEqual(manifest["skip_counts"][reason], 1)
                self.assertFalse((root / "escape.c").exists())
                self.assertFalse((output / "source/linked.c").exists())
                self.assertEqual(fixture.validate_fixture(output)["manifest_sha256"], manifest["manifest_sha256"])
                update = json.loads((output / "update-fixture.json").read_text())
                self.assertEqual(update["format"], "researchify-update-fixture-v1")
                self.assertEqual(update["files"], 2)
                self.assertEqual(update["edit_file"], "src/a.c")
                self.assertEqual(fixture.sha256(Path(update["source"]) / update["edit_file"]), update["edit_sha256"])
                with self.assertRaises(ValueError):
                    fixture.capture(args)
                (output / "source/src/a.c").write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "Captured file changed"):
                    fixture.validate_fixture(output)

    def test_archive_hash_failure_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = self.make_archive(root)
            output = root / "capture"
            args = argparse.Namespace(project="cpython", archive=archive, output_dir=output,
                                      max_file_bytes=1024, max_line_bytes=128, minimum_bytes=1)
            with self.assertRaisesRegex(ValueError, "SHA256 differs"):
                fixture.capture(args)
            self.assertFalse(output.exists())


class OracleTests(unittest.TestCase):
    def test_exact_ascii_term_treats_all_non_ascii_bytes_as_word(self):
        case = {"kind": "term", "pattern": "return"}
        data = "\ufeffreturn; return\r\néreturn return© return_ _return\nreturn".encode()
        count, snippets = driver.expected_snippets(data, case)
        self.assertEqual(count, 3)
        self.assertEqual(snippets, [
            {"line": 1, "column": 1, "text": "return; return"},
            {"line": 3, "column": 1, "text": "return"},
        ])

    def test_regex_counts_lines_literal_counts_occurrences(self):
        data = "π return x; return y;\r\nif\t(z) while (z)\n".encode()
        count, snippets = driver.expected_snippets(data, {"kind": "regex", "pattern": "(return|if|while)[ \\t]"})
        self.assertEqual(count, 2)
        self.assertEqual(snippets[0], {"line": 1, "column": 4, "text": "π return x; return y;"})
        self.assertEqual(snippets[1], {"line": 2, "column": 1, "text": "if\t(z) while (z)"})
        count, snippets = driver.expected_snippets(data, {"kind": "literal", "pattern": "return "})
        self.assertEqual(count, 2)
        self.assertEqual(len(snippets), 1)

    def test_utf8_snippet_window_does_not_split_scalars(self):
        data = ("π" * 100 + "return " + "é" * 200).encode()
        count, snippets = driver.expected_snippets(data, {"kind": "literal", "pattern": "return "})
        self.assertEqual(count, 1)
        self.assertEqual(snippets[0]["column"], 201)
        self.assertEqual(snippets[0]["text"], "..." + "π" * 40 + "return " + "é" * 76 + "...")

    def test_regex_and_literal_use_equivalent_rg_count_modes(self):
        command = driver.rg_command("rg", {"kind": "regex", "pattern": "return[ \\t]"}, 8)
        self.assertIn("--count", command)
        self.assertNotIn("--count-matches", command)
        command = driver.rg_command("rg", {"kind": "term", "pattern": "return"}, 8)
        for flag in ("--count-matches", "--word-regexp", "--fixed-strings"):
            self.assertIn(flag, command)

    def test_shortened_full_count_pages_follow_cursor(self):
        calls = []
        responses = [
            {"snapshot": {"generation": "1"}, "total": 2, "results": [{"path": "a.c", "matches": 2}],
             "has_more": True, "output_truncated": True, "next_cursor": "next-page"},
            {"snapshot": {"generation": "1"}, "total": 2, "results": [{"path": "b.c", "matches": 1}],
             "has_more": False, "output_truncated": False, "next_cursor": None},
        ]
        class FakeService:
            def __init__(self, argv, timeout):
                pass
            def request(self, request):
                calls.append(dict(request))
                return {"response": responses[len(calls) - 1]}
            def close(self, validate=True):
                self.validated = validate
        with patch.object(driver, "Service", FakeService):
            result = driver.all_counts("rs", Path("index"), {"name": "term", "query": "return"},
                                       {"a.c": 2, "b.c": 1}, 10)
        self.assertEqual(result["pages"], 2)
        self.assertEqual(calls[0]["memory_mb"], 1024)
        self.assertEqual(calls[0]["limit"], 10000)
        self.assertEqual(calls[1]["cursor"], "next-page")


if __name__ == "__main__":
    unittest.main()

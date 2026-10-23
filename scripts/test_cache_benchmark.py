#!/usr/bin/env python3
"""Pure guard/label checks; no cache eviction, mmap, targets or benchmarks."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_benchmark as cache


@unittest.skipUnless(sys.platform == "linux", "Linux cache benchmark guards")
class CacheBenchmarkGuards(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "file.cpp").write_bytes(b"content\n")
        (self.root / "index").mkdir()
        (self.root / "index/commit-1.bin").write_bytes(b"fake fixture index\n")
        self.marker = self.root / "cache-fixture.json"
        self.description = {"format": cache.FORMAT, "disposable": True,
                            "description": "owned unit fixture", "source": "source",
                            "indexes": {"baseline": "index", "current": "index"}}
        self.marker.write_text(json.dumps(self.description))

    def test_marker_requires_explicit_disposable_descendants(self):
        self.assertEqual(cache.load_fixture(self.marker)[1], self.source)
        for changes in ({"disposable": False}, {"source": "../outside"},
                        {"indexes": {"baseline": "source", "current": "index"}}):
            self.marker.write_text(json.dumps({**self.description, **changes}))
            with self.assertRaises(ValueError):
                cache.load_fixture(self.marker)

    def test_marker_rejects_symlink_and_inventory_hardlink(self):
        alias = self.root / "alias.json"
        alias.symlink_to(self.marker)
        with self.assertRaises(ValueError):
            cache.load_fixture(alias)
        os.link(self.source / "file.cpp", self.source / "linked.cpp")
        with self.assertRaises(ValueError):
            cache.inventory(self.source)

    def test_snapshot_rejects_mutation_instead_of_downgrading_label(self):
        files = cache.inventory(self.source)
        (self.source / "file.cpp").write_bytes(b"replaced contents\n")
        with self.assertRaisesRegex(RuntimeError, "identity changed"):
            cache.snapshot(files, None)

    def test_zero_residency_label_requires_complete_nonempty_observation(self):
        zero = {"complete": True, "pages_checked": 2, "resident_pages": 0}
        advised = {"complete": True}
        self.assertEqual(cache.cache_label("cold", advised, zero), "verified-cold-file-data-pages")
        for change in ({"complete": False}, {"pages_checked": 0}, {"resident_pages": 1}):
            self.assertEqual(cache.cache_label("cold", advised, {**zero, **change}), "advice-only")
        self.assertEqual(cache.cache_label("cold", {"complete": False}, {**zero, "complete": False}), "unverified-cache-state")
        self.assertEqual(cache.cache_label("warm", None, zero), "warm-query-replay")

    def test_probe_failure_retains_advice_only_and_does_not_fault_pages(self):
        class Unsupported:
            def pages(self, fd, length):
                raise OSError("unsupported test probe")
        files = cache.inventory(self.source)
        observed = cache.snapshot(files, Unsupported())
        self.assertFalse(observed["complete"])
        self.assertEqual(observed["files_checked"], 0)
        self.assertIn("unsupported test probe", observed["errors"][0]["error"])
        self.assertEqual(cache.cache_label("cold", {"complete": True}, observed), "advice-only")

    def test_advice_is_scoped_to_checked_files_and_failure_is_recorded(self):
        files = cache.inventory(self.source)
        calls = []
        def fake(fd, offset, length, mode):
            self.assertEqual(os.fstat(fd).st_ino, files[0]["st_ino"])
            calls.append((offset, length, mode))
        with patch.object(os, "posix_fadvise", side_effect=fake):
            self.assertTrue(cache.advise(files)["complete"])
        self.assertEqual(calls, [(0, 0, os.POSIX_FADV_DONTNEED)])
        with patch.object(os, "posix_fadvise", side_effect=OSError("unsupported advice")):
            result = cache.advise(files)
        self.assertFalse(result["complete"])
        self.assertEqual(result["advised_files"], 0)

    def test_index_coordination_excluded_but_committed_data_is_hashed(self):
        index = self.root / "index"
        (index / "pins").mkdir()
        (index / "pins/reader.pin").write_text("coordination")
        self.assertEqual([item["relative"] for item in cache.inventory(index, True)], ["commit-1.bin"])
        (index / "transaction-2").mkdir()
        with self.assertRaisesRegex(ValueError, "scratch"):
            cache.inventory(index, True)

    def test_regex_counts_lines_and_other_queries_count_occurrences(self):
        for case in cache.bench.cases(64):
            if case["name"] not in ("literal", "broad_regex", "term"):
                continue
            command = cache.rg_command("rg", case)
            self.assertEqual("--count" in command, case["name"] == "broad_regex")
            self.assertEqual("--count-matches" in command, case["name"] != "broad_regex")

    def test_successful_rg_output_is_digest_only_but_mismatch_is_retained(self):
        sample = {"returncode": 0, "stderr": "", "stdout": "./a.cpp\0" + "2\n"}
        cache.finish_sample(sample, "rg", {"a.cpp": 2}, "oracle-digest")
        self.assertNotIn("stdout", sample)
        self.assertEqual(sample["path_count_sha256"], "oracle-digest")
        failed = {"returncode": 0, "stderr": "", "stdout": "./a.cpp\0" + "3\n"}
        with self.assertRaises(RuntimeError):
            cache.finish_sample(failed, "rg", {"a.cpp": 2}, "oracle-digest")
        self.assertIn("stdout", failed)


if __name__ == "__main__":
    unittest.main()

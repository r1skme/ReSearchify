#!/usr/bin/env python3
"""Completion/artifact checks for native update orchestration; no rs target runs."""
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tiny_comparison as tiny


class TinyUpdateOrchestrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.reports = self.work / "results"
        self.reports.mkdir()
        self.edit = self.work / "fixture/source/group_0/module_42.cpp"
        self.edit.parent.mkdir(parents=True)
        self.edit.write_bytes(b"int needle_42 = 0;\n")
        self.args = SimpleNamespace(files=64, update_iterations=1, index_threads=2, timeout=10)
        self.binaries = {"baseline": "trusted-before", "current": "trusted-current"}
        self.summary = {"fixture": {"files": 64}, "binaries": {}, "reports": [], "update_reports": []}

    @staticmethod
    def raw_report(argv, value):
        work = Path(argv[argv.index("--work") + 1])
        work.mkdir()
        (work / "results.json").write_text(json.dumps(value), encoding="utf-8")

    def test_failed_child_raw_is_preserved(self):
        def fail(argv, **unused):
            self.raw_report(argv, {"format": "researchify-update-workflow-v1", "complete": False,
                                   "error": "semantic mismatch"})
            raise subprocess.CalledProcessError(1, argv)
        with patch.object(tiny.subprocess, "run", side_effect=fail):
            with self.assertRaises(subprocess.CalledProcessError):
                tiny.run_update_comparisons(self.args, self.binaries, self.work, self.reports, self.summary)
        raw = json.loads((self.reports / "compact-updates.json").read_text())
        self.assertFalse(raw["complete"])
        self.assertEqual(raw["error"], "semantic mismatch")
        self.assertEqual(len(self.summary["update_reports"]), 1)
        self.assertFalse(self.summary["update_reports"][0]["complete"])

    def test_parent_rejects_incorrect_child_restoration_claim(self):
        def changed(argv, **unused):
            self.raw_report(argv, {"format": "researchify-update-workflow-v1", "complete": True,
                                   "source_restored": True, "operations": [{}] * 10,
                                   "summaries": [{}] * 5})
            self.edit.write_bytes(b"unexpected external replacement\n")
        with patch.object(tiny.subprocess, "run", side_effect=changed):
            with self.assertRaisesRegex(RuntimeError, "unrestored"):
                tiny.run_update_comparisons(self.args, self.binaries, self.work, self.reports, self.summary)
        self.assertFalse(self.summary["update_reports"][0]["source_restored"])
        self.assertFalse(self.summary["update_reports"][0]["complete"])
        self.assertFalse((self.reports / "fast-updates.json").exists())

    def test_complete_profiles_have_separate_raw_files_and_bounded_logs(self):
        commands = []
        def complete(argv, **unused):
            commands.append(argv)
            self.raw_report(argv, {"format": "researchify-update-workflow-v1", "complete": True,
                                   "source_restored": True, "operations": [{}] * 10,
                                   "summaries": [{"tools": {"candidate": {"cpu_ms": None,
                                                  "wall_ms": {"median": 1, "samples": [1]}}}}] * 5,
                                   "checks": [], "conditions": {"cache": "not cold"}})
        output = StringIO()
        with patch.object(tiny.subprocess, "run", side_effect=complete), redirect_stdout(output):
            tiny.run_update_comparisons(self.args, self.binaries, self.work, self.reports, self.summary)
        self.assertEqual(self.summary["reports"], ["compact-updates.json", "fast-updates.json"])
        self.assertTrue(all(row["complete"] for row in self.summary["update_reports"]))
        for command, profile in zip(commands, ("compact", "fast")):
            self.assertEqual(command[command.index("--base-index") + 1], str(self.work / ("current-" + profile)))
        log = output.getvalue()
        self.assertEqual(log.count("RESEARCHIFY_TINY_UPDATE_SUMMARY "), 2)
        self.assertNotIn('"samples"', log)
        self.assertIn('"cpu_ms": null', log)
        self.assertIn('"format": "researchify-tiny-update-summary-v1"', log)


if __name__ == "__main__":
    unittest.main()

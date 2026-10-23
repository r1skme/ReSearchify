#!/usr/bin/env python3
"""Update-only job boundaries; all target execution is mocked."""
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_tiny_corpus as generator
import tiny_update_comparison as updates


class UpdateOnlyJobTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.before = self.root / "before"
        self.after = self.root / "after"
        self.before.write_bytes(b"test-only-before")
        self.after.write_bytes(b"test-only-after")
        self.args = SimpleNamespace(work=self.root / "run", baseline=self.before, current=self.after,
                                    baseline_revision="a" * 40, current_revision="b" * 40,
                                    files=64, index_threads=4, update_iterations=7, timeout=10)
        self.commands = []
        self.incomplete_build = False

    def command(self, argv, timeout):
        self.commands.append(argv)
        if "--files" in argv:
            work = Path(argv[argv.index("--work") + 1])
            source = work / "source"
            source.mkdir(parents=True)
            for i in range(self.args.files):
                path = source / f"group_{i // 1000}/module_{i}.cpp"
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(generator.contents(i))
            response = {"format": "researchify-tiny-corpus-v1", **updates.corpus_state(source, self.args.files)}
        else:
            self.assertEqual(argv[0], str(self.after.resolve()))
            self.assertEqual(argv[1], "create")
            source = self.args.work / "fixture/source"
            response = {"status": "ok", "execution_complete": not self.incomplete_build,
                        **updates.corpus_state(source, self.args.files)}
        return {"returncode": 0, "stderr": "", "stdout": json.dumps(response), "wall_seconds": 0.01}

    def complete_updates(self, args, binaries, work, reports, summary):
        self.assertEqual(args.update_iterations, 7)
        self.assertEqual(args.index_threads, 4)
        self.assertEqual(set(binaries), {"baseline", "current"})
        summary["update_reports"] = [{"profile": name, "complete": True, "source_restored": True,
                                       "operations": 70} for name in ("compact", "fast")]
        summary["reports"] = ["compact-updates.json", "fast-updates.json"]

    def execute(self, update_callback):
        with patch.object(updates, "architecture", return_value={"format": "mocked", "machine": "native"}), \
             patch.object(updates.bench, "command", side_effect=self.command), \
             patch.object(updates.bench, "index_storage", return_value={"total_bytes": 1}), \
             patch.object(updates, "run_update_comparisons", side_effect=update_callback), \
             redirect_stdout(StringIO()):
            return updates.run(self.args)

    def saved(self):
        return json.loads((self.args.work / "results/summary.json").read_text())

    def test_update_only_scope_builds_current_profiles_and_preserves_seven_pairs(self):
        result = self.execute(self.complete_updates)
        self.assertTrue(result["complete"])
        self.assertEqual(result["scope"], "updates-only")
        self.assertFalse(result["conditions"]["search_series"])
        self.assertTrue(result["source_restoration"]["restored"])
        self.assertEqual(len(self.commands), 3)  # Generator plus two current-writer builds only.
        self.assertEqual([row["profile"] for row in result["builds"]], ["compact", "fast"])
        self.assertEqual([row["operations"] for row in result["update_reports"]], [70, 70])
        self.assertIn("Independent update-only runner", result["conditions"]["window"])

    @unittest.skipIf(sys.platform == "win32", "Directory symlinks may require Windows privileges")
    def test_directory_alias_keeps_canonical_binary_identity(self):
        actual = self.root / "actual"
        actual.mkdir()
        self.before.replace(actual / "before")
        self.after.replace(actual / "after")
        alias = self.root / "alias"
        alias.symlink_to(actual, target_is_directory=True)
        self.before = alias / "before"
        self.after = alias / "after"
        self.args.baseline = self.before
        self.args.current = self.after
        self.args.work = alias / "run"
        result = self.execute(self.complete_updates)
        self.assertTrue(result["complete"])
        self.assertTrue(result["source_restoration"]["restored"])
        self.assertEqual(result["binaries"]["current"]["path"], str((actual / "after").resolve()))
        self.assertEqual(result["binaries"]["baseline"]["path"], str((actual / "before").resolve()))
        self.assertEqual(len(self.commands), 3)

    def test_interrupted_update_keeps_error_partial_artifact_and_restore_result(self):
        def interrupted(args, binaries, work, reports, summary):
            (reports / "compact-updates.json").write_text('{"complete": false, "operations": []}')
            summary["update_reports"].append({"profile": "compact", "complete": False, "source_restored": True})
            raise RuntimeError("update interrupted")
        with self.assertRaisesRegex(RuntimeError, "update interrupted"):
            self.execute(interrupted)
        result = self.saved()
        self.assertFalse(result["complete"])
        self.assertIn("update interrupted", result["error"])
        self.assertTrue(result["source_restoration"]["restored"])
        self.assertTrue((self.args.work / "results/compact-updates.json").is_file())

    def test_full_corpus_check_rejects_unexpected_file_despite_child_claim(self):
        def extra(args, binaries, work, reports, summary):
            self.complete_updates(args, binaries, work, reports, summary)
            (work / "fixture/source/unexpected.cpp").write_bytes(b"leftover\n")
        with self.assertRaisesRegex(RuntimeError, "unexpected files"):
            self.execute(extra)
        result = self.saved()
        self.assertFalse(result["complete"])
        self.assertFalse(result["source_restoration"]["restored"])

    def test_incomplete_build_preserves_sample_and_stops_before_updates(self):
        self.incomplete_build = True
        def forbidden(*args):
            self.fail("Updates must not run after an incomplete build")
        with self.assertRaisesRegex(RuntimeError, "Incomplete surrogate indexing"):
            self.execute(forbidden)
        result = self.saved()
        self.assertFalse(result["complete"])
        self.assertEqual(len(result["builds"]), 1)
        self.assertIn("sample", result["builds"][0])
        self.assertFalse(result["builds"][0]["complete"])
        self.assertTrue(result["source_restoration"]["restored"])


if __name__ == "__main__":
    unittest.main()

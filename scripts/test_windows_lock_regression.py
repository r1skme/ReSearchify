#!/usr/bin/env python3
"""Expected-failure validation and restoration, without executing native tools."""
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
import windows_lock_regression as gate


NAMES = ["stable", "concurrent_gc_and_reader_open_are_coordinated", gate.READER, gate.WRITER]


def transcript(legacy):
    lines = ["DELETE_PENDING access.lock created=0 error=system:5 (Access is denied.) legacy_wait_would_reject=1",
             "DELETE_PENDING writer.lock created=0 error=system:5 (Access is denied.) legacy_wait_would_reject=1"]
    for name in NAMES:
        lines.append("FAIL " + name + ": " + gate.OLD_MESSAGE if legacy and name == gate.READER else "PASS " + name)
    lines.append(f"{len(NAMES) - int(legacy)}/{len(NAMES)} cases; 42 assertions")
    return "\n".join(lines) + "\n"


class WindowsLockGateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.baseline = self.root / "baseline"
        self.current = self.root / "current"
        self.original = b"original baseline test bytes\n"
        self.replacement = "\n".join('{"' + name + '", [] {' for name in NAMES).encode()
        self.core_files = ["CMakeLists.txt", "src/engine.cpp", "tests/test_support.hpp"]
        for name, source in (("baseline", self.baseline), ("current", self.current)):
            for relative in self.core_files:
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"support" if relative.endswith("test_support.hpp") else name.encode())
            (source / "tests/v2_tests.cpp").write_bytes(self.original if name == "baseline" else self.replacement)
            build = source / "build"
            (build / "Release").mkdir(parents=True)
            (build / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n"
                "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\nCMAKE_GENERATOR_PLATFORM:INTERNAL=x64\n"
                "RESEARCHIFY_STATIC_RUNTIME:BOOL=ON\n", encoding="utf-8")
            for binary in ("rs.exe", "researchify_core.lib", "cs_v2_tests.exe"):
                (build / "Release" / binary).write_bytes((name + ":" + binary).encode())
        self.original_mtime = (self.baseline / "tests/v2_tests.cpp").stat().st_mtime_ns
        self.args = SimpleNamespace(baseline_source=self.baseline, current_source=self.current,
                                    current_build=self.current / "build", work=self.root / "proof", timeout=1)
        self.calls = []
        self.fail_build = False
        self.timeout_build = False
        self.bad_control = False
        self.change_cli = False
        self.wrong_baseline = False

    def subprocess(self, command, stdout, **kwargs):
        self.calls.append(command)
        code = 0
        if command[0] == "git":
            source = Path(command[2])
            operation = command[3]
            if operation == "rev-parse":
                text = gate.BASELINE if source == self.baseline and not self.wrong_baseline else "c" * 40
            elif operation == "status":
                self.assertEqual((self.baseline / "tests/v2_tests.cpp").read_bytes(), self.original)
                text = ""
            elif operation == "ls-files":
                text = "\0".join(self.core_files) + "\0"
            else:
                self.fail("Unexpected git operation: " + operation)
        elif command[0] == "cmake":
            self.assertEqual((self.baseline / "tests/v2_tests.cpp").read_bytes(), self.replacement)
            text = "mocked build/configure\n"
            if "--build" in command:
                self.assertEqual(command[command.index("--target") + 1], "cs_v2_tests")
                if self.timeout_build:
                    stdout.write(b"partial mocked build log\n")
                    raise gate.subprocess.TimeoutExpired(command, 1)
                code = 2 if self.fail_build else 0
        else:
            self.assertTrue(command[0].endswith("cs_v2_tests.exe"))
            legacy = Path(command[0]).is_relative_to(self.baseline)
            text = transcript(legacy)
            code = int(legacy)
            if legacy and self.bad_control:
                text = text.replace(gate.OLD_MESSAGE, "unexpected failure")
            if legacy and self.change_cli:
                (self.baseline / "build/Release/rs.exe").write_bytes(b"unexpected replacement")
        stdout.write(text.encode())
        return SimpleNamespace(returncode=code)

    def run_gate(self):
        with patch.object(gate.platform, "system", return_value="Windows"), \
             patch.object(gate.platform, "platform", return_value="mocked Windows"), \
             patch.object(gate.platform, "machine", return_value="AMD64"), \
             patch.object(gate.subprocess, "run", side_effect=self.subprocess), redirect_stdout(StringIO()):
            return gate.run(self.args)

    def summary(self):
        return json.loads((self.args.work / "summary.json").read_text())

    def assert_restored(self):
        path = self.baseline / "tests/v2_tests.cpp"
        self.assertEqual(path.read_bytes(), self.original)
        self.assertEqual(path.stat().st_mtime_ns, self.original_mtime)
        self.assertTrue(self.summary()["restoration"]["restored"])

    def test_parses_only_exact_expected_failure_and_complete_fixed_pass(self):
        before = gate.validate_run(transcript(True), 1, NAMES, True)
        after = gate.validate_run(transcript(False), 0, NAMES, False)
        self.assertEqual(before["failures"], {gate.READER: gate.OLD_MESSAGE})
        self.assertEqual(after["passed"], len(NAMES))
        self.assertEqual([row["value"] for row in after["delete_pending"]], [5, 5])

    def test_rejects_wrong_assertion_crash_missing_case_and_side_failures(self):
        valid = transcript(True)
        invalid = [(valid, 3), (valid.replace(gate.OLD_MESSAGE, "unrelated assertion"), 1),
                   (valid.replace("PASS stable", "FAIL stable: unrelated"), 1),
                   (valid.replace("PASS stable\n", ""), 1), (valid + "PASS stable\n", 1),
                   (valid.replace("legacy_wait_would_reject=1", "legacy_wait_would_reject=0"), 1),
                   (valid.replace("3/4 cases; 42 assertions\n", ""), 1)]
        for output, code in invalid:
            with self.subTest(output=output, code=code), self.assertRaises(RuntimeError):
                gate.validate_run(output, code, NAMES, True)

    def test_orchestrates_same_test_only_build_and_restores_original(self):
        report = self.run_gate()
        self.assertTrue(report["complete"])
        self.assertEqual(report["sources"]["baseline"]["revision"], gate.BASELINE)
        self.assertEqual(set(report["checks"]), {"original-control", "fixed-control"})
        self.assertEqual(len([call for call in self.calls if call[0].endswith("cs_v2_tests.exe")]), 2)
        self.assertTrue(all("sha256" in value for value in report["control_binaries"].values()))
        self.assert_restored()

    def test_build_failure_keeps_log_and_restores_without_running_targets(self):
        self.fail_build = True
        with self.assertRaisesRegex(RuntimeError, "build-control-test failed"):
            self.run_gate()
        report = self.summary()
        self.assertFalse(report["complete"])
        self.assertFalse(report["checks"])
        self.assertTrue(any(row.get("returncode") == 2 and row["log_sha256"] for row in report["commands"]))
        self.assertFalse(any(call[0].endswith("cs_v2_tests.exe") for call in self.calls))
        self.assert_restored()

    def test_unexpected_control_failure_is_not_accepted_and_restores(self):
        self.bad_control = True
        with self.assertRaisesRegex(RuntimeError, "Unexpected exit or test failure"):
            self.run_gate()
        self.assertFalse(self.summary()["complete"])
        self.assert_restored()

    def test_timeout_preserves_partial_log_and_restores_test_source(self):
        self.timeout_build = True
        with self.assertRaises(gate.subprocess.TimeoutExpired):
            self.run_gate()
        report = self.summary()
        self.assertFalse(report["complete"])
        build = next(row for row in report["commands"] if row["label"] == "build-control-test")
        self.assertFalse(build["complete"])
        self.assertIn("TimeoutExpired", build["error"])
        self.assertEqual((self.args.work / build["log"]).read_text(), "partial mocked build log\n")
        self.assert_restored()

    def test_changed_performance_cli_blocks_success(self):
        self.change_cli = True
        with self.assertRaisesRegex(RuntimeError, "baseline_cli executable/library changed"):
            self.run_gate()
        self.assertFalse(self.summary()["complete"])
        self.assert_restored()

    def test_wrong_original_revision_stops_before_test_copy_and_build(self):
        self.wrong_baseline = True
        with self.assertRaisesRegex(RuntimeError, "not the pinned original revision"):
            self.run_gate()
        self.assertEqual((self.baseline / "tests/v2_tests.cpp").read_bytes(), self.original)
        self.assertFalse(any(call[0] == "cmake" for call in self.calls))

    def test_wrong_cmake_source_is_rejected_before_copy(self):
        cache = self.baseline / "build/CMakeCache.txt"
        cache.write_text(cache.read_text().replace(str(self.baseline), str(self.current)))
        with self.assertRaisesRegex(RuntimeError, "Unexpected source/generator/architecture/runtime"):
            self.run_gate()
        self.assertEqual((self.baseline / "tests/v2_tests.cpp").read_bytes(), self.original)
        self.assertFalse(any(call[0] == "cmake" for call in self.calls))


if __name__ == "__main__":
    unittest.main()

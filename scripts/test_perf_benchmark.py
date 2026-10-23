#!/usr/bin/env python3
"""Standard-library tests for the development performance harness."""
import math
import hashlib
import random
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_benchmark as bench
import compare_sessions as sessions


class PerformanceHarnessTests(unittest.TestCase):
    def test_paired_session_order_is_balanced_and_reproducible(self):
        orders = sessions.paired_orders(201, random.Random(1729))
        self.assertEqual(orders, sessions.paired_orders(201, random.Random(1729)))
        self.assertTrue(all(set(order) == {"baseline", "current"} for order in orders))
        baseline_first = sum(order[0] == "baseline" for order in orders)
        self.assertIn(baseline_first, (100, 101))
        self.assertTrue(all(orders[i] == list(reversed(orders[i - 1])) for i in range(1, len(orders))))

    def test_paired_session_records_engine_and_client_durations(self):
        code = ("import json,sys\n"
                "for line in sys.stdin:\n"
                " r=json.loads(line)\n"
                " print(json.dumps({'id':r['id'],'status':'ok','execution_complete':True,"
                "'total':42,'metrics':{'seconds':0.001}}),flush=True)\n")
        service = sessions.Service([sys.executable, "-c", code], 10)
        try:
            observed = [service.request({"command": "find"}) for _ in range(3)]
            self.assertEqual([row["response"]["id"] for row in observed], [0, 1, 2])
            self.assertTrue(all(row["request_wall_seconds"] > 0 for row in observed))
            summary = sessions.summarize(observed)
            self.assertEqual(summary["engine_wall_ms"]["samples"], [1, 1, 1])
            self.assertEqual(summary["raw"], observed)
            sessions.check_same(observed[0], observed[1]["response"])
            with self.assertRaises(RuntimeError):
                sessions.check_same(observed[0], {**observed[1]["response"], "total": 43})
        finally:
            service.close()

    def test_paired_session_rejects_incomplete_search(self):
        code = ("import json,sys\n"
                "for line in sys.stdin:\n"
                " r=json.loads(line)\n"
                " print(json.dumps({'id':r['id'],'status':'ok','execution_complete':False,"
                "'metrics':{'seconds':0.001}}),flush=True)\n")
        service = sessions.Service([sys.executable, "-c", code], 10)
        try:
            with self.assertRaises(RuntimeError):
                service.request({"command": "find"})
        finally:
            service.close(validate=False)

    @staticmethod
    def malformed_shutdown_code():
        return ("import json,sys\n"
                "for line in sys.stdin:\n"
                " r=json.loads(line)\n"
                " print(json.dumps({'id':r['id'],'status':'ok','execution_complete':True,"
                "'total':42,'metrics':{'seconds':0.001}}),flush=True)\n"
                "sys.stdout.buffer.write(bytes([255]));sys.stdout.buffer.flush()\n")

    def test_paired_session_rejects_malformed_stdout_on_shutdown(self):
        service = sessions.Service([sys.executable, "-c", self.malformed_shutdown_code()], 10)
        try:
            service.request({"command": "find"})
            with self.assertRaisesRegex(RuntimeError, "reader failed"):
                service.close()
        finally:
            service.close(validate=False)

    def test_single_session_rejects_malformed_stdout_on_shutdown(self):
        with self.assertRaisesRegex(RuntimeError, "reader failed"):
            bench.measure({"kind": "jsonl", "argv": [sys.executable, "-c", self.malformed_shutdown_code()],
                           "requests": [{"id": 0}], "timeout": 10})

    def test_index_storage_separates_nested_spills(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for name, size in {"manifest": 5, "segment-1/content.bin": 7,
                               "transaction-1/run.bin": 11, "segment-1/sort/run.bin": 13,
                               "segment-1/literal-sort/run.bin": 17, "segment-1/literal.tbl": 19,
                               "pins/reader": 23}.items():
                path = base / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"x" * size)
            measured = bench.index_storage(directory)
            self.assertEqual(measured["committed_index_bytes"], 31)
            self.assertEqual(measured["transaction_scratch_bytes"], 11)
            self.assertEqual(measured["spill_scratch_bytes"], 30)
            self.assertEqual(measured["coordination_bytes"], 23)
            self.assertEqual(measured["literal_sidecar_bytes"], 19)
            self.assertEqual(measured["total_file_bytes"], 95)

    def test_executable_identity(self):
        path = str(Path(__file__).resolve())
        expected = hashlib.sha256(Path(path).read_bytes()).hexdigest()
        self.assertEqual(bench.binary_identity(path), expected)

    def test_distribution_and_raw_samples(self):
        result = bench.distribution([4, 1, 2, 3])
        self.assertEqual(result["median"], 2.5)
        self.assertEqual(result["p95"], 4)
        self.assertEqual(result["min"], 1)
        self.assertEqual(result["max"], 4)
        self.assertEqual(result["samples"], [4, 1, 2, 3])
        self.assertEqual(bench.distribution(list(range(1, 22)))["p95"], 20)
        with self.assertRaises(ValueError):
            bench.distribution([])

    def test_null_delimited_rg_paths(self):
        self.assertEqual(bench.parse_rg_counts("./space name.cpp\0" + "2\n./line\nname.cpp\0" + "7\n"),
                         {"space name.cpp": 2, "line\nname.cpp": 7})
        self.assertEqual(bench.parse_rg_counts(".\\dir\\file.cpp\0" + "9\n"), {"dir/file.cpp": 9})
        self.assertEqual(bench.parse_rg_counts(""), {})
        with self.assertRaises(ValueError):
            bench.parse_rg_counts("file.cpp:1\n")
        with self.assertRaises(ValueError):
            bench.parse_rg_counts("a\0" + "1\na\0" + "2\n")

    def test_suite_edge_size(self):
        suite = bench.cases(1)
        self.assertEqual(next(c for c in suite if c["name"] == "or")["query"], "needle_0")
        self.assertIn("literal", {c["name"] for c in suite})
        self.assertIn("regex_no_match", {c["name"] for c in suite})

    def test_large_count_output(self):
        output = "".join(f"./group/file_{i}.cpp\0{i + 1}\n" for i in range(10000))
        result = bench.parse_rg_counts(output)
        self.assertEqual(len(result), 10000)
        self.assertEqual(result["group/file_0.cpp"], 1)
        self.assertEqual(result["group/file_9999.cpp"], 10000)

    def test_pagination_fits_current_result_budget(self):
        page = bench.correctness_page_limit(64)
        self.assertGreater(page, 0)
        self.assertLessEqual(page * (32768 + 512) + 8 * 1048576, 64 * 1048576 // 3)
        self.assertEqual(bench.correctness_page_limit(16), 1)
        self.assertEqual(bench.correctness_page_limit(256), 1000)

    def test_only_non_timing_values_are_compared(self):
        a = {"total": 3, "returned": 0, "results": [], "metrics": {"seconds": 1}}
        b = {**a, "metrics": {"seconds": 2}}
        self.assertEqual(bench.normalized(a), bench.normalized(b))
        self.assertNotEqual(bench.normalized(a), bench.normalized({**b, "total": 4}))

    def test_noop_session_rejects_real_updates(self):
        find = {"total": 1, "returned": 0, "results": []}
        update = {"added": 0, "changed": 0, "removed": 0, "tokenized": 0}
        bench.validate_noop_session([find, update, find], find)
        for field in update:
            with self.assertRaises(RuntimeError):
                bench.validate_noop_session([find, {**update, field: 1}, find], find)
        with self.assertRaises(RuntimeError):
            bench.validate_noop_session([{**find, "total": 2}, update, find], find)
        with self.assertRaises(RuntimeError):
            bench.validate_noop_session([find, update], find)

    def test_isolated_child_measurement(self):
        result = bench.command([sys.executable, "-c", "print('measured target')"], 10)
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["stdout"].splitlines(), ["measured target"])
        self.assertEqual(result["stderr"], "")
        self.assertGreater(result["wall_seconds"], 0)
        if result["cpu_seconds"] is not None:
            self.assertGreaterEqual(result["cpu_seconds"], 0)
        if result["peak_rss_bytes"] is not None:
            self.assertGreater(result["peak_rss_bytes"], 0)

    def test_persistent_session_measures_each_exchange(self):
        code = ("import json,sys\n"
                "for line in sys.stdin:\n"
                " r=json.loads(line)\n"
                " print(json.dumps({'id':r['id'],'status':'ok','total':42}),flush=True)\n")
        result = bench.measure({"kind": "jsonl", "argv": [sys.executable, "-c", code],
                                "requests": [{"id": i} for i in range(4)], "timeout": 10})
        self.assertEqual([r["id"] for r in result["responses"]], [0, 1, 2, 3])
        self.assertEqual(len(result["request_seconds"]), 4)
        self.assertTrue(all(math.isfinite(x) and x > 0 for x in result["request_seconds"]))


if __name__ == "__main__":
    unittest.main()

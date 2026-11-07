"""Failure and accounting checks for the Q03 installed harness."""

import unittest
import tempfile
from pathlib import Path
from types import SimpleNamespace

from q03_installed import (IsosHTTPError, correlated_bridge_events,
                           readiness_failure_evidence, readiness_process_status,
                           summarize, summarize_results, wait_for)


class HarnessTests(unittest.TestCase):
    def test_readiness_retries_only_process_status_503_and_keeps_raw_replies(self):
        class FakeHome:
            def __init__(self, output):
                self.args = SimpleNamespace(output=output)
                self.scene, self.repetition = "pick-place", 0
                self.readiness_status_503_attempts = []
                self.replies = [IsosHTTPError("process-status", 503, '{"error":"busy"}'),
                                {"state": "running"}]

            def api(self, path):
                assert path == "process-status"
                reply = self.replies.pop(0)
                if isinstance(reply, Exception):
                    raise reply
                return reply

            def readiness_503_summary(self):
                attempts = self.readiness_status_503_attempts
                return {"count": len(attempts), "attempts": attempts,
                        "last_error": attempts[-1]["error"] if attempts else None}

        with tempfile.TemporaryDirectory() as directory:
            home = FakeHome(Path(directory))
            self.assertIsNone(readiness_process_status(home))
            self.assertEqual(readiness_process_status(home), {"state": "running"})
            summary = home.readiness_503_summary()
            self.assertEqual(summary["count"], 1)
            self.assertEqual(summary["attempts"][0]["body"], '{"error":"busy"}')
            self.assertIn("HTTP 503", summary["last_error"])
            self.assertTrue((Path(directory) / "pick-place-0-readiness-status-503.json").is_file())
            home.replies = [IsosHTTPError("process-status", 422, "bad graph")]
            with self.assertRaises(IsosHTTPError):
                readiness_process_status(home)
            home.replies = [IsosHTTPError("node-graph", 503, "wrong endpoint")]
            with self.assertRaises(IsosHTTPError):
                readiness_process_status(home)
            self.assertEqual(home.readiness_503_summary()["count"], 1)

    def test_nearest_rank_and_even_median(self):
        result = summarize([1.0, 4.0, 3.0, 2.0], 2.0)
        self.assertEqual(result["median_ms"], 2.5)
        self.assertEqual(result["p99_ms"], 4.0)
        self.assertEqual(result["throughput_per_s"], 2.0)

    def test_missing_samples_or_bad_elapsed_fails(self):
        for samples, elapsed in (([], 1), ([1], 0), ([1], -1)):
            with self.assertRaises(ValueError):
                summarize(samples, elapsed)

    def test_timeout_is_not_reported_as_ready(self):
        with self.assertRaisesRegex(TimeoutError, "owner did not become ready"):
            wait_for(lambda: None, 0.01, "owner")

    def test_success_after_deadline_is_rejected(self):
        def late_success():
            import time
            time.sleep(0.025)
            return {"state": "running"}

        with self.assertRaisesRegex(TimeoutError, "owner did not become ready"):
            wait_for(late_success, 0.01, "owner")

    def test_readiness_failure_file_retains_exception_even_without_503(self):
        with tempfile.TemporaryDirectory() as directory:
            home = SimpleNamespace(args=SimpleNamespace(output=Path(directory)),
                                   scene="navigation", repetition=0,
                                   readiness_503_summary=lambda: {"count": 0,
                                                                  "attempts": [],
                                                                  "last_error": None})
            record = readiness_failure_evidence(home, 1, RuntimeError("EOS health HTTP 422"))
            self.assertEqual(record["exception"], {"type": "RuntimeError",
                                                    "message": "EOS health HTTP 422"})
            self.assertEqual(record["process_status_503"]["count"], 0)
            self.assertGreater(record["elapsed_ms"], 0)
            path = Path(directory) / "navigation-0-readiness-failure.json"
            self.assertTrue(path.is_file())
            self.assertIn('"last_error": null', path.read_text())

    def test_scene_capture_workloads_remain_separate(self):
        rows = [{"scene": "pick-place", "startup_ms": 1000, "captures_ms": [2, 4],
                 "capture_elapsed_s": 1},
                {"scene": "navigation", "startup_ms": 2000, "captures_ms": [6, 8],
                 "capture_elapsed_s": 1}]
        pooled = summarize_results(rows)
        self.assertEqual(pooled["pick_place_start_summary"]["count"], 1)
        self.assertEqual(pooled["pick_place_capture_summary"]["count"], 2)
        self.assertEqual(pooled["pick_place_capture_summary"]["throughput_per_s"], 2)
        self.assertEqual(pooled["navigation_capture_summary"]["count"], 2)
        self.assertNotIn("pooled_capture_summary", pooled)

    def test_bridge_events_require_same_run_id_phase_and_completed_status(self):
        def event(run, command, phase, status):
            return {"run_id": run, "correlation_id": command,
                    "event_code": "bridge_primitive_result",
                    "fields": {"phase": phase, "status": status}}

        other = [event("run-1", "other", "submit", "InProgress"),
                 event("run-1", "other", "followup", "Completed"),
                 event("old-run", "wanted", "followup", "Completed")]
        with self.assertRaises(AssertionError):
            correlated_bridge_events(other, "run-1", "wanted")
        submitted = event("run-1", "wanted", "submit", "InProgress")
        with self.assertRaises(AssertionError):
            correlated_bridge_events(other + [submitted], "run-1", "wanted")
        complete = event("run-1", "wanted", "followup", "Completed")
        self.assertEqual(correlated_bridge_events(other + [submitted, complete],
                                                  "run-1", "wanted"), [submitted, complete])


if __name__ == "__main__":
    unittest.main()

"""A manager socket can precede its HTTP bind during restart."""

import json
import tempfile
from pathlib import Path
from unittest import TestCase
from unittest.mock import patch
from urllib.error import URLError

from q01_r06_a03_replay import wait_manager_restart_http


class Server:
    def __init__(self, exit_code=None, exits_after=None):
        self.exit_code = exit_code
        self.exits_after = exits_after
        self.polls = 0

    def poll(self):
        self.polls += 1
        if self.exits_after is not None and self.polls > self.exits_after:
            return 1
        return self.exit_code


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


GOOD = {"http_status": 200, "reply": {"ok": True,
        "data": {"state": "degraded", "terminal": {"outcome": "unknown"}}}}


class RestartHTTP(TestCase):
    def invoke(self, responses, *, server=None, seconds=1):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        clock = Clock()
        with (patch("q01_r06_a03_replay.raw_api", side_effect=responses) as get,
              patch("q01_r06_a03_replay.time.monotonic", clock.monotonic),
              patch("q01_r06_a03_replay.time.sleep", clock.sleep)):
            try:
                return wait_manager_restart_http(16000, server or Server(), root,
                                                 seconds=seconds)
            finally:
                self.summary = json.loads((root / "manager-restart-http-probes-summary.json").read_text())
                self.probes = [json.loads(line) for line in
                    (root / "manager-restart-http-probes.jsonl").read_text().splitlines()]
                self.calls = get.call_args_list

    def test_delayed_listener_then_first_success(self):
        refused = URLError(ConnectionRefusedError(111, "Connection refused"))
        self.assertEqual(self.invoke([refused, refused, GOOD]), GOOD["reply"]["data"])
        self.assertEqual(self.summary["attempts"], 3)
        self.assertEqual(self.summary["connection_refused"], 2)
        self.assertEqual([row.get("connection_refused", False) for row in self.probes],
                         [True, True, False])
        self.assertTrue(all("elapsed_seconds" in row and "observed_unix_ns" in row
                            for row in self.probes))
        self.assertTrue(all(call.kwargs["timeout"] <= 3 for call in self.calls))

    def test_exited_server_fails_before_http(self):
        with self.assertRaisesRegex(AssertionError, "isos serve exited"):
            self.invoke([GOOD], server=Server(1))
        self.assertEqual(self.summary["attempts"], 0)
        self.assertEqual(self.calls, [])

    def test_server_exits_after_one_refusal(self):
        refused = URLError(ConnectionRefusedError(111, "Connection refused"))
        with self.assertRaisesRegex(AssertionError, "isos serve exited"):
            self.invoke([refused, GOOD], server=Server(exits_after=1))
        self.assertEqual(self.summary["attempts"], 1)
        self.assertEqual(len(self.calls), 1)

    def test_first_http_503_fails_without_retry(self):
        refused = URLError(ConnectionRefusedError(111, "Connection refused"))
        with self.assertRaises(AssertionError):
            self.invoke([refused, {"http_status": 503, "reply": {"ok": False}}, GOOD])
        self.assertEqual(self.summary["attempts"], 2)
        self.assertEqual(len(self.calls), 2)

    def test_non_refused_transport_error_fails(self):
        with self.assertRaises(URLError):
            self.invoke([URLError("unexpected transport failure"), GOOD])
        self.assertEqual(len(self.calls), 1)

    def test_first_http_malformed_200_fails(self):
        with self.assertRaises(AssertionError):
            self.invoke([{"http_status": 200, "reply": {"ok": True}}, GOOD])
        self.assertEqual(len(self.calls), 1)

    def test_refusal_deadline_is_bounded(self):
        refused = URLError(ConnectionRefusedError(111, "Connection refused"))
        with self.assertRaisesRegex(AssertionError, "did not bind"):
            self.invoke([refused] * 100, seconds=.12)
        self.assertEqual(self.summary["connection_refused"], 3)

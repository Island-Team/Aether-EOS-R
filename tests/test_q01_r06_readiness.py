"""Initial R06 status polling admits only the documented IsOS busy reply."""

import json
import tempfile
from pathlib import Path
from unittest import TestCase, main
from unittest.mock import patch

from q01_r06_a03_replay import wait_initial_ready


REVISION = "a" * 64


class Run:
    def __init__(self, exit_code=None):
        self.exit_code = exit_code

    def poll(self):
        return self.exit_code


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, duration):
        self.now += duration


def reply(status, body):
    return {"http_status": status, "reply": body}


BUSY = reply(503, {"schema_version": 1, "ok": False, "data": None,
    "error": "process status is unavailable"})
READY = reply(200, {"ok": True, "data": {"schema_version": "1.0.0",
    "state": "running",
    "active_revision": REVISION,
    "processes": [{"id": name, "pid": number, "state": "running"}
                  for number, name in enumerate(("eos", "bridge", "egia"), 1)]}})


class Readiness(TestCase):
    def invoke(self, responses, *, seconds=1, run=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        clock = Clock()
        self.responses = responses
        with (patch("q01_r06_a03_replay.raw_api", side_effect=responses) as get,
              patch("q01_r06_a03_replay.time.monotonic", clock.monotonic),
              patch("q01_r06_a03_replay.time.sleep", clock.sleep)):
            try:
                result = wait_initial_ready(16000, REVISION, run or Run(), root,
                                            seconds=seconds)
            finally:
                self.summary = json.loads((root / "readiness-probes-summary.json").read_text())
                self.probes = [json.loads(line) for line in
                    (root / "readiness-probes.jsonl").read_text().splitlines()]
                self.calls = get.call_args_list
        return result

    def test_documented_busy_then_ready_records_both(self):
        self.assertEqual(self.invoke([BUSY, READY]), READY["reply"]["data"])
        self.assertEqual(self.summary["counts"], {
            "transient_503": 1, "valid_not_ready": 0, "ready": 1})
        self.assertEqual([row["http_status"] for row in self.probes], [503, 200])
        self.assertTrue(all("observed_unix_ns" in row for row in self.probes))
        self.assertTrue(all(call.kwargs["timeout"] <= 3 for call in self.calls))

    def test_client_timeout_exceeds_owner_socket_deadline(self):
        self.assertEqual(self.invoke([BUSY, READY], seconds=4), READY["reply"]["data"])
        self.assertEqual(self.calls[0].kwargs["timeout"], 3)
        self.assertGreater(self.calls[1].kwargs["timeout"], 2)

    def test_client_timeout_is_capped_by_overall_deadline(self):
        self.assertEqual(self.invoke([READY], seconds=1), READY["reply"]["data"])
        self.assertEqual(self.calls[0].kwargs["timeout"], 1)

    def test_unrelated_503_fails_immediately(self):
        with self.assertRaises(AssertionError):
            self.invoke([reply(503, {"ok": False, "error": "different"}), READY])
        self.assertEqual(len(self.calls), 1)

    def test_incorrectly_shaped_busy_503_fails_immediately(self):
        with self.assertRaises(AssertionError):
            self.invoke([reply(503, {"ok": False,
                "error": "process status is unavailable"}), READY])
        self.assertEqual(len(self.calls), 1)

    def test_other_http_error_fails_immediately(self):
        with self.assertRaises(AssertionError):
            self.invoke([reply(422, {"ok": False, "error": "invalid"}), READY])
        self.assertEqual(len(self.calls), 1)

    def test_http_200_non_ok_fails_immediately(self):
        with self.assertRaises(AssertionError):
            self.invoke([reply(200, {"ok": False, "error": "bad"}), READY])
        self.assertEqual(len(self.calls), 1)

    def test_busy_never_extends_deadline(self):
        with self.assertRaisesRegex(AssertionError, "did not converge"):
            self.invoke([BUSY] * 100, seconds=.12)
        self.assertEqual(self.summary["counts"]["transient_503"], 3)
        self.assertEqual(self.summary["counts"]["ready"], 0)

    def test_valid_nonready_never_becomes_ready(self):
        pending = reply(200, {"ok": True, "data": {"schema_version": "1.0.0",
            "state": "stopped", "active_revision": None, "processes": []}})
        with self.assertRaisesRegex(AssertionError, "did not converge"):
            self.invoke([pending] * 100, seconds=.12)
        self.assertEqual(self.summary["counts"]["valid_not_ready"], 3)

    def test_malformed_success_status_fails_immediately(self):
        for data in ({},
                     {"schema_version": "1.0.0", "state": "starting",
                      "active_revision": None, "processes": []},
                     {"schema_version": "1.0.0", "state": "nonsense",
                      "active_revision": None, "processes": []},
                     {"schema_version": "1.0.0", "state": "stopped",
                      "active_revision": None, "processes": {}}):
            with self.subTest(data=data), self.assertRaises(AssertionError):
                self.invoke([reply(200, {"ok": True, "data": data}), READY])
            self.assertEqual(len(self.calls), 1)

    def test_invalid_process_id_and_revision_fail_immediately(self):
        for change in ({"active_revision": "short"},
                       {"processes": [{"id": "", "pid": 1, "state": "running"}]},
                       {"processes": [{"id": "eos", "pid": True,
                                       "state": "running"}]}):
            with self.subTest(change=change), self.assertRaises(AssertionError):
                data = {"schema_version": "1.0.0", "state": "stopped",
                        "active_revision": None, "processes": []}
                data.update(change)
                self.invoke([reply(200, {"ok": True, "data": data}), READY])

    def test_run_exit_is_diagnostic_and_prevents_poll(self):
        with self.assertRaisesRegex(AssertionError, "isos run exited before readiness: 17"):
            self.invoke([], run=Run(17))
        self.assertEqual(self.calls, [])


if __name__ == "__main__":
    main()

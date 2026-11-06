#!/usr/bin/env python3
"""Focused Q03 runner accounting and failure tests (stdlib only)."""
import importlib.util
import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / "scripts/q03_browser_benchmark.py"
SPEC = importlib.util.spec_from_file_location("q03_browser_benchmark", PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class FakeDriver:
    def __init__(self, reply):
        self.reply = reply

    def execute_async_script(self, *_args):
        return self.reply


class BenchmarkTests(unittest.TestCase):
    def test_midpoint_median_nearest_rank_p99_and_failures(self):
        samples = [{"ms": 1}, {"ms": 2}, {"ms": 3}, {"ms": 100}, {"error": "rejected"}]
        result = runner.stats(samples, 3.0)
        self.assertEqual((result["completed"], result["failures"]), (4, 1))
        self.assertEqual(result["median_ms"], 2.5)
        self.assertEqual(result["p99_ms"], 100)
        self.assertEqual(result["measured_duration_s"], 0.106)
        self.assertEqual(result["wall_duration_s"], 3.0)

    def test_one_unique_synthetic_post_per_interaction(self):
        first = {"op": "primitive_typed", "command_id": "first"}
        second = {"op": "primitive_typed", "command_id": "second"}
        self.assertEqual(len(runner.validate_posts([first, second], 2)), 2)
        with self.assertRaisesRegex(RuntimeError, "unique synthetic POSTs"):
            runner.validate_posts([first, first], 2)
        with self.assertRaisesRegex(RuntimeError, "unique synthetic POSTs"):
            runner.validate_posts([first, second], 1)

    def test_rejected_and_timed_out_dom_are_not_samples(self):
        for reason in ("owner rejected", "graph timeout nodes=0 edges=0"):
            with self.assertRaisesRegex(RuntimeError, reason):
                runner.measure(FakeDriver({"error": reason}), "script")
        self.assertEqual(runner.stats([{"error": "timeout"}], 2)["completed"], 0)

    def test_control_script_ignores_previous_completed_result_until_new_invoke(self):
        harness = r"""
const script=process.argv[1];
let command='prior', status='Completed', clicks=0, posts=0;
const invoke={disabled:false,textContent:'Invoke',click(){
  clicks++;
  window.fetch('http://127.0.0.1:16020/api/eos/v2/control',
    {body:JSON.stringify({op:'primitive_typed',header:{command_id:'current'}})});
  command='current';status='Completed';
}};
const code=()=>({textContent:command});
const result={querySelector(selector){return selector==='code'?code():{textContent:status}},
  querySelectorAll(){return [code(),code(),{textContent:'revision'}]},
  querySelectorAllButtons(){return []}};
const card={querySelector(selector){
  if(selector==='.eos-control__result')return result;
  if(selector==='.eos-control__result code')return code();
  if(selector==='.eos-control__result strong')return {textContent:status};
  return null;
 },
  querySelectorAll(selector){return selector==='button'?[invoke]:[code(),code(),{textContent:'revision'}]}};
const nav={textContent:'EOS',click(){}};
global.window={fetch(){if(this!==window)throw Error('fetch lost Window receiver');posts++;return Promise.resolve({})}};
global.document={querySelector(selector){
  if(selector==='.eos-control__card--typed')return card;
  if(selector==='.isos-workbench__safety code')return {textContent:'revision'};
  return null;
 },querySelectorAll(){return [nav]}};
global.performance={now:()=>Date.now()};
global.requestAnimationFrame=fn=>setTimeout(fn,0);
new Function(script)(value=>{
  if(value.command!=='current'||value.status!=='Completed'||clicks!==1||posts!==1)
    throw Error(JSON.stringify({value,clicks,posts}));
  process.stdout.write(JSON.stringify({value,clicks,posts}));
});
"""
        completed = subprocess.run(["node", "-e", harness, runner.CONTROL_JS],
                                   capture_output=True, text=True, timeout=3)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["clicks"], 1)

    def test_failure_checkpoint_retains_attempt_and_reason(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "failed.json"
            attempts = [{"index": 0, "warmup": True,
                         "outcome": {"error": "old result"}, "requests": []}]
            prior = [{"index": 1, "cases": {"graph": {"samples": [{"ms": 20}]}}},
                     {"index": 2, "cases": {"graph": {"samples": [{"ms": 21}]}}}]
            graph = {"summary": {"completed": 1000, "failures": 0},
                     "samples": [{"ms": 22}], "request_bytes": {"512": 1}}
            runner.write_failure_checkpoint(path,
                                            {"source_commit": "selected", "repetitions": prior},
                                            2, "control", 0, "no POST", attempts,
                                            {"graph": graph}, {"session_id": "third"},
                                            {"/assets/main.js": "digest"})
            checkpoint = json.loads(path.read_text())
            self.assertEqual(checkpoint["repetitions"], prior)
            self.assertEqual(checkpoint["failure_checkpoint"]["repetition"], 3)
            self.assertEqual(checkpoint["failure_checkpoint"]["reason"], "no POST")
            self.assertEqual(checkpoint["failure_checkpoint"]["attempts"], attempts)
            self.assertEqual(checkpoint["failure_checkpoint"]["current_repetition"],
                             {"index": 3, "browser": {"session_id": "third"},
                              "served_asset_sha256": {"/assets/main.js": "digest"},
                              "completed_cases": {"graph": graph}})

    def test_each_repetition_opens_fresh_verified_browser(self):
        class Browser:
            def __init__(self, index):
                self.session_id = f"session-{index}"
                self.capabilities = {"browserVersion": "145.0"}
                self.service = type("Service", (), {"process": type("Process", (), {"pid": index})()})()
                self.urls = []
                self.timeout = None
            def set_window_size(self, *_args):
                pass
            def execute_script(self, script):
                return {"w": 1280, "h": 800, "iw": 1280, "ih": 800} if "outerWidth" in script else runner.MANIFEST["viewport"]
            def set_script_timeout(self, value):
                self.timeout = value
            def get(self, url):
                self.urls.append(url)
            def quit(self):
                pass
        class Factory:
            def __init__(self):
                self.created = []
            def Firefox(self, **_kwargs):
                browser = Browser(len(self.created) + 1)
                self.created.append(browser)
                return browser
        factory = Factory()
        with patch.object(runner, "verify_served_bundle") as verify:
            browsers = [runner.open_browser(factory, lambda: type("Options", (), {"add_argument": lambda *_: None})(),
                                            lambda *_: object(), "hosted")[0] for _ in range(3)]
        self.assertEqual(verify.call_count, 3)
        self.assertEqual([runner.browser_identity(browser)["session_id"] for browser in browsers],
                         ["session-1", "session-2", "session-3"])
        self.assertEqual([browser.timeout for browser in browsers], [5, 5, 5])
        self.assertTrue(all(browser.urls == ["http://127.0.0.1:16001/en/isos?page=eos"] for browser in browsers))

    def test_asset_mismatch_checkpoints_completed_cases_without_publishing_run(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.json"
            result = {"repetitions": [{"index": 1, "cases": {}}]}
            cases = {"graph": {"samples": [{"ms": 24}]},
                     "control": {"samples": [{"ms": 180}]}}
            with patch.object(runner, "observed_assets", side_effect=[
                    RuntimeError("served asset hash mismatch"),
                    {"verification_error": "served asset hash mismatch"}]):
                with self.assertRaisesRegex(RuntimeError, "served asset hash mismatch"):
                    runner.finish_repetition(output, result, 1, cases,
                                             {"session_id": "second"},
                                             runner.MANIFEST["viewport"], "hosted", 0)
            self.assertEqual(len(result["repetitions"]), 1)
            checkpoint = json.loads(output.read_text())
            self.assertEqual(checkpoint["failure_checkpoint"]["case"], "asset_verification")
            self.assertEqual(checkpoint["failure_checkpoint"]["current_repetition"]["completed_cases"], cases)
            self.assertEqual(checkpoint["failure_checkpoint"]["current_repetition"]["browser"],
                             {"session_id": "second"})

    def test_previous_browser_quit_failure_checkpoints_next_run(self):
        class RefusingBrowser:
            def quit(self):
                raise RuntimeError("browser teardown failed")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.json"
            prior = [{"index": 1, "cases": {"graph": {"samples": [{"ms": 21}]}}}]
            with patch.object(runner, "observed_assets", return_value={}), \
                    patch.object(runner, "open_browser") as start:
                with self.assertRaisesRegex(RuntimeError, "browser teardown failed"):
                    runner.begin_repetition(RefusingBrowser(), None, None, None,
                                            output, {"repetitions": prior}, 1, "local", 0)
            start.assert_not_called()
            checkpoint = json.loads(output.read_text())
            self.assertEqual(checkpoint["repetitions"], prior)
            self.assertEqual(checkpoint["failure_checkpoint"]["case"], "browser_start")
            self.assertEqual(checkpoint["failure_checkpoint"]["reason"], "browser teardown failed")

    def test_result_read_and_visible_id_must_match_this_submit_and_run(self):
        requests = [
            {"op": "primitive_typed", "command_id": "new", "path": "/api/eos/v2/control"},
            {"op": "result", "command_id": "new", "path": "/api/eos/v2/control"},
            {"path": "/api/isos/v1/process-status", "run_id": runner.RUN_ID, "active_revision": runner.REVISION},
        ]
        visible = {"command": "new", "status": "Completed", "revision": runner.REVISION, "activeRevision": runner.REVISION}
        self.assertEqual(runner.validate_correlation(visible, requests), "new")
        with self.assertRaisesRegex(RuntimeError, "visible terminal ID"):
            runner.validate_correlation({**visible, "command": "old"}, requests)
        with self.assertRaisesRegex(RuntimeError, "result read"):
            runner.validate_correlation(visible, [{**requests[0]}, {**requests[1], "command_id": "old"}, requests[2]])
        with self.assertRaisesRegex(RuntimeError, "IsOS run"):
            runner.validate_correlation(visible, [requests[0], requests[1], {**requests[2], "run_id": "old"}])
        with self.assertRaisesRegex(RuntimeError, "visible result revision"):
            runner.validate_correlation({**visible, "revision": "old"}, requests)

    def test_late_hosted_chunk_digest_is_checked_after_interactions(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "assets").mkdir()
            early = base / "assets/main.js"
            late = base / "assets/graph.js"
            early.write_bytes(b"initial bundle")
            late.write_bytes(b"lazy graph chunk")
            request = lambda name, digest: {"port": 16001, "path": f"/isosui/assets/{name}", "sha256": digest}
            early_request = request("main.js", hashlib.sha256(early.read_bytes()).hexdigest())
            late_request = request("graph.js", hashlib.sha256(late.read_bytes()).hexdigest())
            self.assertEqual(len(runner.verify_served_assets("hosted", [early_request, late_request], base)), 2)
            late.write_bytes(b"substituted lazy chunk")
            with self.assertRaisesRegex(RuntimeError, "served asset hash mismatch: /isosui/assets/graph.js"):
                runner.verify_served_assets("hosted", [early_request, late_request], base)


if __name__ == "__main__":
    unittest.main()

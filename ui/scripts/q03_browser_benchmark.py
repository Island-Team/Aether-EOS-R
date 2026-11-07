#!/usr/bin/env python3
"""Fixed Q03 browser baseline against isolated IsOS/EOS loopback stubs.

Install the optional runner in a temporary directory with
`uv pip install --target /tmp/q03-ui-py selenium==4.38.0` and set PYTHONPATH.
The production bundles are served unchanged. No request can reach a live owner:
the runner must acquire all three fixed loopback ports before launching Firefox.
"""
import argparse
import hashlib
import json
import platform
import shutil
import statistics
import subprocess
import sys
import threading
import time
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/q03_browser_benchmark/v1"
MANIFEST = json.loads((FIXTURE / "manifest.json").read_text())
REVISION = json.loads((FIXTURE / "graph.json").read_text())["revision"]
RUN_ID = json.loads((FIXTURE / "status.json").read_text())["run_id"]
REQUESTS = []
LOCK = threading.Lock()
STUB_DELAY = MANIFEST["stub_delay_ms"] / 1000


def body(name):
    return (FIXTURE / name).read_bytes()


class Server(ThreadingHTTPServer):
    allow_reuse_address = False


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def send(self, code, data, mime="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "http://127.0.0.1:16001")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self):
        self.send(204, b"")

    def do_GET(self):
        self.route(None)

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        if size > 100_000:
            self.send(413, b"{}")
            return
        try:
            payload = json.loads(self.rfile.read(size))
        except (ValueError, UnicodeDecodeError):
            self.send(400, b"{}")
            return
        self.route(payload)

    def route(self, payload):
        path = urlsplit(self.path).path
        port = self.server.server_port
        if port == 16001:
            self.static(path)
            return
        if self.headers.get("Authorization") or self.headers.get("X-EOS-Grant"):
            self.send(400, b'{"ok":false,"error":"private credentials are forbidden"}')
            return
        time.sleep(STUB_DELAY)
        if port == 16000:
            names = {
                "/api/isos/v1/node-graph": "graph.json",
                "/api/isos/v1/node-types": "catalog.json",
                "/api/isos/v1/node-types/selected": "selected-catalog.json",
                "/api/isos/v1/process-status": "status.json",
                "/api/isos/v1/node-options": "options.json",
                "/api/isos/v1/node-sets": "node-sets.json",
                "/api/isos/v1/backends": "backends.json",
                "/api/isos/v1/core-readiness": "readiness.json",
            }
            name = names.get(path)
            if name:
                data = b'{"schema_version":1,"ok":true,"data":' + body(name).strip() + b',"error":null}'
                with LOCK:
                    REQUESTS.append({"port": port, "method": self.command, "path": path, "bytes": len(data),
                                     "sha256": hashlib.sha256(data).hexdigest(),
                                     "run_id": RUN_ID if name == "status.json" else None,
                                     "active_revision": REVISION if name == "status.json" else None})
                self.send(200, data)
            else:
                self.send(404, b'{"schema_version":1,"ok":false,"data":null,"error":"fixture route unavailable"}')
            return
        if path != "/api/eos/v2/control" or not isinstance(payload, dict):
            self.send(404, b'{"ok":false,"error":"fixture route unavailable"}')
            return
        op = payload.get("op")
        if op == "primitive_typed":
            header = payload.get("header", {})
            assert header.get("active_revision") == REVISION
            assert header.get("source_path") == MANIFEST["binding"]
            assert header.get("target_path") == "eos/hardware/base/drive"
            assert payload.get("payload", {}).get("type") == "body_velocity"
            command = header["command_id"]
            data = body("synthetic-result-in-progress.json").replace(b"__COMMAND_ID__", command.encode()).strip()
            reply = json.loads(data)
        elif op == "result":
            command = payload["command_id"]
            assert payload.get("active_revision") == REVISION
            data = body("synthetic-result-completed.json").replace(b"__COMMAND_ID__", command.encode()).strip()
            reply = json.loads(data)
        elif op == "time":
            reply = {"ok": True, "monotonic_ns": 1_000_000_000}
        elif op == "discover":
            reply = {"ok": True, "nodes": [{"path": "eos/hardware/base/drive", "node_id": "drive", "kind": "base", "label": "Planar base", "facets": [], "capabilities": ["body_velocity"]}]}
        elif op == "status":
            reply = {"ok": True, "status": {"state": "ready", "active_command_id": None}}
        elif op == "health":
            reply = {"ok": True, "health": {"state": "healthy"}}
        elif op == "observe":
            reply = {"ok": True, "observation": {"x_m": 0, "y_m": 0, "yaw_rad": 0}}
        else:
            reply = {"ok": False, "error": "unsupported fixture operation"}
        if op not in ("primitive_typed", "result"):
            data = json.dumps(reply, separators=(",", ":")).encode()
        with LOCK:
            REQUESTS.append({"port": port, "method": self.command, "path": path, "op": op, "bytes": len(data),
                             "sha256": hashlib.sha256(data).hexdigest(),
                             "command_id": command if op in ("primitive_typed", "result") else None})
        self.send(200 if reply["ok"] else 400, data)

    def static(self, path):
        target = self.server.target
        prefix = "/isosui/" if target == "hosted" else "/"
        if target == "hosted" and path.startswith(prefix):
            relative = path[len(prefix):]
        elif path in ("/en/isos", "/cn/isos", "/"):
            relative = "index.html"
        elif target == "local" and path.startswith("/"):
            relative = path[1:]
        else:
            self.send(404, b"not found", "text/plain")
            return
        file = (ROOT / "dist" / target / relative).resolve()
        base = (ROOT / "dist" / target).resolve()
        if not file.is_relative_to(base) or not file.is_file():
            self.send(404, b"not found", "text/plain")
            return
        mime = "text/html" if file.suffix == ".html" else "application/javascript" if file.suffix == ".js" else "text/css" if file.suffix == ".css" else "application/json" if file.suffix == ".json" else "application/octet-stream"
        data = file.read_bytes()
        with LOCK:
            REQUESTS.append({"port": 16001, "method": self.command, "path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        self.send(200, data, mime)


GRAPH_JS = """
const expected=arguments[0], done=arguments[arguments.length-1], start=performance.now();
const button=[...document.querySelectorAll('nav button')].find(x=>x.textContent.trim()==='Graph');
if(!button){done({error:'Graph nav missing'});return;}button.click();
const deadline=start+2000;function poll(){
 const nodes=document.querySelectorAll('.react-flow__node').length;
 const edges=document.querySelectorAll('.react-flow__edge').length;
 const revision=document.querySelector('.isos-workbench__safety code')?.textContent;
 if(nodes===19&&edges===10&&revision===expected){
  requestAnimationFrame(()=>requestAnimationFrame(()=>{
   const stable=document.querySelectorAll('.react-flow__node').length===19&&document.querySelectorAll('.react-flow__edge').length===10&&document.querySelector('.isos-workbench__safety code')?.textContent===expected;
   if(stable)done({ms:performance.now()-start,nodes,edges,revision});else poll();
  }));
 }else if(performance.now()>deadline)done({error:`graph timeout nodes=${nodes} edges=${edges} revision=${revision}`});
 else setTimeout(poll,10);
}poll();
"""

RESET_JS = """
const target=arguments[0], expected=arguments[1], done=arguments[arguments.length-1];
const button=[...document.querySelectorAll('nav button')].find(x=>x.textContent.trim()===target);
if(!button){done({error:`${target} reset nav missing`});return;}
button.click();
const deadline=performance.now()+2000;
function ready(){
 const selected=button.getAttribute('aria-current')==='page';
 const page=new URL(window.location.href).searchParams.get('page');
 const revision=document.querySelector('.isos-workbench__safety code')?.textContent;
 const graph=target==='Graph'&&(page===null||page==='graph')&&
   document.querySelectorAll('.react-flow__node').length===19&&
   document.querySelectorAll('.react-flow__edge').length===10;
 const control=target==='EOS'&&page==='eos'&&
   Boolean(document.querySelector('.eos-control__card--typed'));
 if(selected&&revision===expected&&(graph||control)){
  requestAnimationFrame(()=>requestAnimationFrame(()=>done({ready:true})));
 }else if(performance.now()>deadline)done({error:`${target} reset did not settle: selected=${selected} page=${page} revision=${revision}`});
 else setTimeout(ready,10);
}ready();
"""

CONTROL_JS = """
const done=arguments[arguments.length-1];
const originalFetch=window.fetch;
let submitted=null, invocations=0;
window.fetch=function(input, options){
 const url=typeof input==='string'?input:input?.url;
 if(url==='http://127.0.0.1:16020/api/eos/v2/control'&&options?.body){
  try { const payload=JSON.parse(options.body);
   if(payload.op==='primitive_typed') { submitted=payload.header?.command_id; invocations++; }
  } catch (_) { /* Stub request validation reports malformed payloads. */ }
 }
 return originalFetch.call(window,input,options);
};
const finish=value=>{window.fetch=originalFetch;done(value)};
const start=performance.now();
const button=[...document.querySelectorAll('nav button')].find(x=>x.textContent.trim()==='EOS');
if(!button){finish({error:'EOS nav missing'});return;}button.click();
const deadline=start+2000;
let refreshed=false;
function poll(){
 const card=document.querySelector('.eos-control__card--typed');
 const invoke=[...card?.querySelectorAll('button')||[]].find(x=>x.textContent.trim()==='Invoke');
 const result=card?.querySelector('.eos-control__result');
 const command=result?.querySelector('code')?.textContent;
 const status=result?.querySelector('strong')?.textContent;
 const revision=result?.querySelectorAll('code')[2]?.textContent;
 const activeRevision=document.querySelector('.isos-workbench__safety code')?.textContent;
 if(invocations>1){finish({error:`more than one Invoke: ${invocations}`});return;}
 if(submitted&&command===submitted&&status==='Completed'){
  requestAnimationFrame(()=>requestAnimationFrame(()=>{
   const stable=card?.querySelector('.eos-control__result code')?.textContent===submitted&&card?.querySelector('.eos-control__result strong')?.textContent==='Completed'&&card?.querySelectorAll('.eos-control__result code')[2]?.textContent===revision;
   if(stable&&invocations===1)finish({ms:performance.now()-start,command,status,revision,activeRevision});
   else if(performance.now()>deadline)finish({error:'control result did not stabilize'});
   else poll();
  }));
  return;
 }
 if(submitted&&command===submitted&&status==='InProgress'&&!refreshed){
  const refresh=[...result.querySelectorAll('button')].find(x=>x.textContent.trim()==='Refresh result');
  if(refresh){refreshed=true;refresh.click();}
 }
 if(!window.q03Invoked&&invoke&&!invoke.disabled){window.q03Invoked=true;invoke.click();}
 if(performance.now()>deadline){finish({error:`control timeout submitted=${submitted} command=${command} status=${status} enabled=${!invoke?.disabled}`} );return;}
 setTimeout(poll,10);
}window.q03Invoked=false;poll();
"""


def measure(driver, js, revision=None):
    result = driver.execute_async_script(js, revision) if revision else driver.execute_async_script(js)
    if "error" in result:
        raise RuntimeError(result["error"])
    return result


def reset_page(driver, target):
    result = driver.execute_async_script(RESET_JS, target, REVISION)
    if result != {"ready": True}:
        raise RuntimeError(result.get("error", f"{target} reset failed"))


def write_failure_checkpoint(output, result, run, case, index, reason, attempts,
                             completed_cases, browser=None, assets=None):
    """Retain prior runs, completed cases in this run, and failed attempts."""
    failure = {**result, "failure_checkpoint": {
        "repetition": run + 1, "case": case, "interaction_index": index,
        "reason": reason, "attempts": attempts,
        "current_repetition": {"index": run + 1,
                               "browser": browser,
                               "served_asset_sha256": assets,
                               "completed_cases": completed_cases}}}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(failure, indent=2, default=dict) + "\n")


def stats(samples, wall_duration):
    values = sorted(item["ms"] for item in samples if "ms" in item)
    measured_duration = sum(values) / 1000
    return {"completed": len(values), "failures": len(samples) - len(values), "throughput_per_s": len(values) / measured_duration if measured_duration else 0,
            "median_ms": statistics.median(values) if values else None,
            "p99_ms": values[max(0, (99 * len(values) + 99) // 100 - 1)] if values else None,
            "measured_duration_s": measured_duration, "wall_duration_s": wall_duration}


def validate_posts(requests, expected):
    posts = [item for item in requests if item.get("op") == "primitive_typed"]
    ids = [item["command_id"] for item in posts]
    if len(posts) != expected or len(set(ids)) != expected:
        raise RuntimeError(f"expected {expected} unique synthetic POSTs, got {len(posts)} total and {len(set(ids))} unique")
    return posts


def validate_correlation(measured, requests):
    posts = validate_posts(requests, 1)
    command_id = posts[0]["command_id"]
    reads = [item for item in requests if item.get("op") == "result"]
    statuses = [item for item in requests if item["path"] == "/api/isos/v1/process-status"]
    if not reads or any(item["command_id"] != command_id for item in reads):
        raise RuntimeError("result read does not match submitted command ID")
    if measured.get("command") != command_id or measured.get("status") != "Completed":
        raise RuntimeError("visible terminal ID does not match submitted command ID")
    if measured.get("revision") != REVISION or measured.get("activeRevision") != REVISION:
        raise RuntimeError("visible result revision does not match active revision")
    if not statuses or any(item.get("run_id") != RUN_ID or item.get("active_revision") != REVISION for item in statuses):
        raise RuntimeError("result belongs to a different or unknown IsOS run")
    return command_id


def verify_served_bundle(driver, target):
    for language, html_lang in (("en", "en"), ("cn", "zh-CN")):
        driver.get(f"http://127.0.0.1:16001/{language}/isos?page=eos")
        actual = driver.execute_script("return {lang:document.documentElement.lang,scripts:[...document.scripts].map(x=>x.getAttribute('src')).filter(Boolean)}")
        if actual["lang"] != html_lang:
            raise RuntimeError(f"{language} route did not load editor language")
        prefix = "/isosui/assets/" if target == "hosted" else "/assets/"
        if not actual["scripts"] or any(not path.startswith(prefix) for path in actual["scripts"]):
            raise RuntimeError(f"{language} route loaded wrong asset base: {actual['scripts']}")
    manifest_url = "/isosui/manifest.json" if target == "hosted" else "/manifest.json"
    loaded_manifest = driver.execute_async_script("const done=arguments[arguments.length-1];fetch(arguments[0]).then(x=>x.json()).then(done).catch(e=>done({error:String(e)}))", manifest_url)
    if loaded_manifest != json.loads((ROOT / "dist" / target / "manifest.json").read_text()):
        raise RuntimeError("browser manifest differs from selected bundle")
    return loaded_manifest


def verify_served_assets(target, requests, bundle_dir=None):
    """Verify every observed JS/CSS response, including late lazy chunks."""
    base = (bundle_dir or ROOT / "dist" / target).resolve()
    assets = [item for item in requests if item["port"] == 16001 and item["path"].endswith((".js", ".css"))]
    if not any(item["path"].endswith(".js") for item in assets):
        raise RuntimeError("browser loaded no JS assets")
    prefix = "/isosui/" if target == "hosted" else "/"
    digests = {}
    for item in assets:
        if not item["path"].startswith(prefix):
            raise RuntimeError(f"asset escaped {target} prefix")
        file = (base / item["path"][len(prefix):]).resolve()
        if not file.is_relative_to(base) or not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest() != item["sha256"]:
            raise RuntimeError(f"served asset hash mismatch: {item['path']}")
        if item["path"] in digests and digests[item["path"]] != item["sha256"]:
            raise RuntimeError(f"served asset changed during run: {item['path']}")
        digests[item["path"]] = item["sha256"]
    return digests


def open_browser(webdriver, Options, Service, target):
    """Start an independent browser for one repetition and verify its bundle."""
    opts = Options(); opts.add_argument("-headless")
    driver = webdriver.Firefox(options=opts, service=Service("/snap/bin/geckodriver"))
    try:
        driver.set_window_size(1280, 800)
        outer = driver.execute_script("return {w:outerWidth,h:outerHeight,iw:innerWidth,ih:innerHeight}")
        driver.set_window_size(1280 + outer["w"] - outer["iw"], 800 + outer["h"] - outer["ih"])
        viewport = driver.execute_script("return {width:innerWidth,height:innerHeight}")
        if viewport != MANIFEST["viewport"]:
            raise RuntimeError(f"viewport mismatch: {viewport}")
        driver.set_script_timeout(5)
        verify_served_bundle(driver, target)
        driver.get("http://127.0.0.1:16001/en/isos?page=eos")
        return driver, viewport
    except BaseException:
        driver.quit()
        raise


def browser_identity(driver):
    """Record the separate WebDriver and geckodriver process for this run."""
    return {"session_id": driver.session_id,
            "version": driver.capabilities.get("browserVersion"),
            "geckodriver_pid": driver.service.process.pid}


def observed_assets(target, start, strict=True):
    """Verify and record only assets requested during this repetition."""
    with LOCK:
        observed = list(REQUESTS[start:])
    try:
        return verify_served_assets(target, observed)
    except RuntimeError as error:
        if strict:
            raise
        return {"verification_error": str(error)}


def finish_repetition(output, result, run, cases, identity, viewport, target, request_start):
    """Verify assets before publishing this repetition as complete."""
    try:
        assets = observed_assets(target, request_start)
        with LOCK:
            all_requests = list(REQUESTS)
        all_assets = verify_served_assets(target, all_requests)
    except RuntimeError as error:
        write_failure_checkpoint(output, result, run, "asset_verification", -1,
                                 str(error), [], cases, identity,
                                 observed_assets(target, request_start, False))
        raise
    result["repetitions"].append({"index": run + 1, "browser": identity,
                                  "viewport": viewport, "served_asset_sha256": assets,
                                  "cases": cases})
    result["served_asset_sha256"] = all_assets
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, default=dict) + "\n")


def begin_repetition(driver, webdriver, Options, Service, output, result, run, target,
                     request_start):
    """Close the previous session and checkpoint any startup failure."""
    try:
        if driver is not None:
            driver.quit()
        fresh, viewport = open_browser(webdriver, Options, Service, target)
        return fresh, viewport, browser_identity(fresh)
    except Exception as error:
        write_failure_checkpoint(output, result, run, "browser_start", -1,
                                 str(error), [], {}, assets=observed_assets(target, request_start, False))
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=("local", "hosted"), required=True)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not (ROOT / "dist" / args.target / "manifest.json").is_file():
        parser.error("build the selected production bundle first")
    from selenium import webdriver
    from selenium.webdriver.firefox.options import Options
    from selenium.webdriver.firefox.service import Service
    servers = []
    started_servers = []
    try:
        for port in (16000, 16001, 16020):
            server = Server(("127.0.0.1", port), Handler)
            server.target = args.target
            servers.append(server)
        for server in servers:
            threading.Thread(target=server.serve_forever, daemon=True).start()
            started_servers.append(server)
        driver = None
        try:
            node = shutil.which("node")
            npm = shutil.which("npm")
            result = {"schema_version": 1, "target": args.target, "source_commit": json.loads((ROOT / "dist" / args.target / "manifest.json").read_text())["source_commit"],
                      "fixture_manifest_sha256": hashlib.sha256((FIXTURE / "manifest.json").read_bytes()).hexdigest(),
                      "served_asset_sha256": {},
                      "browser": None, "viewport": MANIFEST["viewport"], "os": platform.platform(),
                      "cpu": next((line.partition(":")[2].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")), platform.processor()),
                      "node": subprocess.check_output([node, "--version"], text=True).strip() if node else None,
                      "npm": subprocess.check_output([npm, "--version"], text=True).strip() if npm else None,
                      "samples_per_case": args.samples, "warmup_per_case": args.warmup, "repetitions": []}
            for run in range(args.repetitions):
                with LOCK:
                    run_request_start = len(REQUESTS)
                driver, viewport, identity = begin_repetition(
                    driver, webdriver, Options, Service, args.output, result, run,
                    args.target, run_request_start)
                if result["browser"] is None:
                    result["browser"] = identity["version"]
                cases = {}
                for case, js in (("graph", GRAPH_JS), ("control", CONTROL_JS)):
                    samples = []
                    attempts = []
                    case_started = time.monotonic()
                    before = len(REQUESTS)
                    for index in range(args.warmup + args.samples):
                        # Reset is outside the browser monotonic measured interval.
                        reset = "EOS" if case == "graph" else "Graph"
                        try:
                            reset_page(driver, reset)
                        except Exception as exc:
                            write_failure_checkpoint(args.output, result, run, case,
                                                     index, str(exc), attempts, cases, identity,
                                                     observed_assets(args.target, run_request_start, False))
                            raise
                        with LOCK:
                            sample_start = len(REQUESTS)
                        try:
                            measured = measure(driver, js, REVISION if case == "graph" else None)
                        except Exception as exc:
                            measured = {"error": str(exc)}
                        with LOCK:
                            sample_requests = REQUESTS[sample_start:]
                        measured["response_bytes"] = [item["bytes"] for item in sample_requests]
                        measured["response_sha256"] = [item["sha256"] for item in sample_requests]
                        attempts.append({"index": index, "warmup": index < args.warmup,
                                         "outcome": measured, "requests": sample_requests})
                        if case == "graph" and "error" in measured:
                            write_failure_checkpoint(args.output, result, run, case,
                                                     index, measured["error"], attempts, cases, identity,
                                                     observed_assets(args.target, run_request_start, False))
                            raise RuntimeError(measured["error"])
                        if case == "control":
                            try:
                                validate_correlation(measured, sample_requests)
                            except RuntimeError as exc:
                                attempts[-1]["error"] = str(exc)
                                attempts[-1]["control_dom"] = driver.execute_script(
                                    "return document.querySelector('.eos-control__card--typed')?.outerHTML || null")
                                write_failure_checkpoint(args.output, result, run, case,
                                                         index, str(exc), attempts, cases, identity,
                                                         observed_assets(args.target, run_request_start, False))
                                raise
                        if index >= args.warmup:
                            samples.append(measured)
                    elapsed = time.monotonic() - case_started
                    with LOCK:
                        requests = REQUESTS[before:]
                    try:
                        posts = validate_posts(requests, args.warmup + args.samples) if case == "control" else []
                    except RuntimeError as exc:
                        write_failure_checkpoint(args.output, result, run, case,
                                                 args.warmup + args.samples - 1,
                                                 str(exc), attempts, cases, identity,
                                                 observed_assets(args.target, run_request_start, False))
                        raise
                    cases[case] = {"summary": stats(samples, elapsed), "samples": samples,
                                   "request_bytes": Counter(str(r["bytes"]) for r in requests),
                                   "synthetic_posts": len(posts), "unique_command_ids": len({r["command_id"] for r in posts})}
                finish_repetition(args.output, result, run, cases, identity, viewport,
                                  args.target, run_request_start)
            result["pooled"] = {case: stats([sample for run in result["repetitions"] for sample in run["cases"][case]["samples"]],
                                             sum(run["cases"][case]["summary"]["wall_duration_s"] for run in result["repetitions"]))
                                for case in ("graph", "control")}
            args.output.write_text(json.dumps(result, indent=2, default=dict) + "\n")
            print(json.dumps({"target": args.target, "runs": [{c: v["summary"] for c, v in run["cases"].items()} for run in result["repetitions"]]}, indent=2))
        finally:
            if driver is not None:
                driver.quit()
    finally:
        for server in started_servers:
            server.shutdown()
        for server in servers:
            server.server_close()


if __name__ == "__main__":
    main()

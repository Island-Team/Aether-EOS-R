"""Backend link status: tracker, /backend endpoint, context section."""

from __future__ import annotations

import asyncio
import json
from types import SimpleNamespace

import pytest

from egia.api.status import BackendConnection
from egia.core.context import ContextBuilder
from egia.core.link import BackendStatusTracker, backend_section_renderer
from egia.core.monitor import monitor_entry
from egia.core.perception.admission import Admission
from egia.lib.config import MonitorConfig


def test_tracker_lifecycle() -> None:
  tracker = BackendStatusTracker("127.0.0.1:18010", failure_threshold=3)
  assert tracker.snapshot().connection is BackendConnection.UNKNOWN
  tracker.record_success()
  snap = tracker.snapshot()
  assert snap.connection is BackendConnection.UP
  assert snap.last_heartbeat_at is not None
  tracker.record_failure()
  tracker.record_failure()
  assert tracker.snapshot().connection is BackendConnection.DEGRADED
  tracker.record_failure()
  assert tracker.snapshot().connection is BackendConnection.DOWN
  assert tracker.snapshot().consecutive_failures == 3
  tracker.record_success()
  assert tracker.snapshot().connection is BackendConnection.UP
  assert tracker.snapshot().consecutive_failures == 0


def test_render_backend_status_line() -> None:
  tracker = BackendStatusTracker("x:18010")
  line = backend_section_renderer(tracker)()
  assert "unknown" in line
  tracker.record_failure()
  assert "degraded" in backend_section_renderer(tracker)()


def test_context_builder_appends_backend_section(etc_root) -> None:
  from egia.lib.config import load_prompt_config

  tracker = BackendStatusTracker("x:18010")
  tracker.record_success()
  tracker.update_runtime(
    SimpleNamespace(
      robot_id="00000000-0000-7000-8000-000000000000",
      robot_name="test-robot",
      robot_model="test-model",
      robot_description="test robot",
      state="idle",
      health="ready",
      estop=False,
      power_pct=80.0,
      version="3.0.0",
      started_at="2026-09-23T00:00:00Z",
      devices=(),
    )
  )
  builder = ContextBuilder(
    Admission(4),
    load_prompt_config(etc_root),
    backend_render=backend_section_renderer(tracker),
  )
  snapshot = builder.build()
  kinds = [s.kind for s in snapshot.sections]
  assert "backend" in kinds
  backend = next(s for s in snapshot.sections if s.kind == "backend")
  assert backend.has_data and "up" in backend.rendered
  assert "Backend" in snapshot.prompt


class _FakeClient:
  def __init__(self, healthy: bool) -> None:
    self._healthy = healthy

  async def health_check(self) -> bool:
    return self._healthy


async def _drive_monitor(healthy: bool) -> BackendStatusTracker:
  tracker = BackendStatusTracker("x:18010", failure_threshold=2)
  ready, stop = asyncio.Event(), asyncio.Event()
  task = asyncio.create_task(
    monitor_entry(
      _FakeClient(healthy),  # type: ignore[arg-type]
      Admission(4),
      MonitorConfig(heartbeat_interval_s=0.01),
      ready,
      stop,
      backend=tracker,
    )
  )
  await ready.wait()
  await asyncio.sleep(0.05)
  stop.set()
  await asyncio.wait_for(task, 1.0)
  return tracker


@pytest.mark.asyncio
async def test_monitor_records_heartbeats() -> None:
  up = await _drive_monitor(True)
  assert up.snapshot().connection is BackendConnection.UP
  down = await _drive_monitor(False)
  assert down.snapshot().connection is BackendConnection.DOWN


@pytest.mark.asyncio
async def test_backend_ws_stream() -> None:
  import websockets

  from egia.io.northbound import BACKEND_PATH, NorthboundServer
  from egia.io.state import ServerState

  state = ServerState("test")
  tracker = BackendStatusTracker("x:18010")
  tracker.record_success()
  tracker.update_runtime(
    SimpleNamespace(
      robot_id="00000000-0000-7000-8000-000000000000",
      robot_name="test-robot",
      robot_model="test-model",
      robot_description="test robot",
      state="idle",
      health="ready",
      estop=False,
      power_pct=80.0,
      version="3.0.0",
      started_at="2026-09-23T00:00:00Z",
      devices=(),
    )
  )
  server = NorthboundServer(
    state,
    Admission(4),
    host="127.0.0.1",
    port=0,
    status_frequency_hz=50.0,
    backend=lambda: [tracker.snapshot()],
  )
  await server.start()
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{BACKEND_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), 2.0))
      # One frame, one BackendStatus per wired endpoint (northbound.md).
      assert frame["backends"][0]["connection"] == "up"
      assert frame["backends"][0]["endpoint"] == "x:18010"
      assert frame["backends"][0]["backend"] == "default"
      assert frame["backends"][0]["robot_name"] == "test-robot"
      assert frame["backends"][0]["robot_state"] == "idle"
      assert frame["backends"][0]["robot_health"] == "ready"
      assert frame["backends"][0]["eos_started_at"] == "2026-09-23T00:00:00Z"
      assert "runtime_health" not in frame["backends"][0]
      assert "hardware_model" not in frame["backends"][0]
  finally:
    await server.stop()


def test_tracker_carries_robot_profile() -> None:
  tracker = BackendStatusTracker(
    "x:18010", backend="station-1", name="station-1 robot", description="arm"
  )
  snap = tracker.snapshot()
  assert snap.name == "station-1 robot"
  assert snap.description == "arm"


def test_render_backend_status_names_the_robot() -> None:
  from egia.core.link import render_backend_status

  tracker = BackendStatusTracker("x:18010", backend="station-1", name="station-1 robot")
  tracker.record_success()
  line = render_backend_status(tracker.snapshot())
  assert "station-1 robot" in line and "station-1" in line
  # Unprofiled named backend keeps the plain attribution label.
  plain = BackendStatusTracker("x:18010", backend="b1")
  plain.record_success()
  assert render_backend_status(plain.snapshot()).startswith("Backend b1 (x:18010): ")
  # Default backend stays byte-identical to the pre-profile shape.
  default = BackendStatusTracker("x:18010")
  default.record_success()
  assert render_backend_status(default.snapshot()).startswith("Backend (x:18010):")


def test_tracker_applies_live_device_status_to_agent_context() -> None:
  from egia.core.link import render_backend_status

  tracker = BackendStatusTracker("x:18010", backend="sandbox")
  tracker.update_runtime(
    SimpleNamespace(
      robot_id="00000000-0000-7000-8000-000000000000",
      robot_name="sandbox",
      robot_model="local-system-devices",
      robot_description="playground",
      state="idle",
      health="ready",
      estop=False,
      power_pct=75.0,
      version="1",
      started_at="2026-09-23T00:00:00Z",
      devices=(
        SimpleNamespace(
          id="mic-0",
          device_class="mic",
          part_id="default",
          description="microphone",
          state="idle",
          health="ready",
          supported_primitives=(),
        ),
      ),
    )
  )
  tracker.update_status({"status": "degraded"})

  snapshot = tracker.snapshot()
  assert snapshot.robot_name == "sandbox"
  assert snapshot.eos_started_at == "2026-09-23T00:00:00Z"
  assert snapshot.robot_health == "degraded"
  assert snapshot.devices[0].health == "ready"
  assert "mic mic-0: ready" in render_backend_status(snapshot)


def test_tracker_applies_current_gateway_aggregate_health() -> None:
  tracker = BackendStatusTracker("x:18010", backend="sandbox")
  tracker.update_status(
    {
      "robot_id": "00000000-0000-7000-8000-000000000000",
      "status": "degraded",
      "devices_total": 2,
      "devices_alive": 1,
      "robots": [],
    }
  )

  assert tracker.snapshot().robot_health == "degraded"

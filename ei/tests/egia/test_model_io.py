"""Model I/O exchange: store, /model_io endpoint, planner publishing."""

from __future__ import annotations

import asyncio
import json
from datetime import UTC, datetime

import pytest

from egia.api.model_io import ModelActivity, ModelExchange, ModelMessage
from egia.core.decision.model_io import ModelIOStore


def _exchange(
  eid: int, reply: str = "ok", thinking: str | None = None
) -> ModelExchange:
  return ModelExchange(
    exchange_id=eid,
    mission_id="m-1",
    step=eid + 1,
    messages=[ModelMessage(role="user", content="dynamic context")],
    reply=reply,
    thinking=thinking,
    timestamp=datetime.now(UTC),
  )


def test_store_counts_and_retains_latest() -> None:
  store = ModelIOStore()
  assert store.frame().total == 0 and store.frame().exchange is None
  store.publish(_exchange(0))
  store.publish(_exchange(1, reply="later"))
  frame = store.frame()
  assert frame.total == 2
  assert frame.exchange is not None and frame.exchange.reply == "later"


def test_store_stream_updates_count_as_one_request() -> None:
  store = ModelIOStore()
  first = _exchange(0, reply="")
  first.status = "streaming"
  store.publish(first)
  update = _exchange(0, reply="partial")
  update.status = "streaming"
  store.publish(update)
  store.publish(_exchange(0, reply="complete"))

  assert store.total == 1
  assert store.version == 3
  assert store.latest() is not None
  assert store.latest().reply == "complete"


def test_store_distinguishes_publishers_with_overlapping_counters() -> None:
  store = ModelIOStore()
  store.publish(_exchange(0))
  proposer = _exchange(0)
  proposer.mission_id = "proposer:idle"
  store.publish(proposer)
  assert store.total == 2


def test_store_reports_every_agent_and_thinking_state() -> None:
  store = ModelIOStore()
  planner = _exchange(0)
  planner.agent_id = "planner"
  planner.status = "streaming"
  proposer = _exchange(0)
  proposer.agent_id = "proposer"
  proposer.mission_id = "proposer:idle"
  store.publish(planner)
  store.publish(proposer)

  frame = store.frame()

  assert [(agent.agent_id, agent.thinking) for agent in frame.agents] == [
    ("planner", True),
    ("proposer", False),
  ]
  assert frame.agents[0].status == "thinking"


def test_store_reports_primitive_activity_on_the_same_frame() -> None:
  store = ModelIOStore()
  store.publish_activity(
    ModelActivity(
      activity_id=0,
      agent_id="planner",
      mission_id="m-1",
      step=1,
      name="base.velocity",
      arguments={"linear_x_m_s": 0.1},
      started_at=datetime.now(UTC),
    )
  )

  frame = store.frame()

  assert frame.agents[0].status == "working"
  assert frame.agents[0].thinking is False
  assert frame.agents[0].activity is not None
  assert frame.agents[0].activity.name == "base.velocity"


@pytest.mark.asyncio
async def test_store_wait_next() -> None:
  store = ModelIOStore()
  waiter = asyncio.create_task(store.wait_next(store.version))
  await asyncio.sleep(0.01)
  store.publish(_exchange(0))
  frame = await asyncio.wait_for(waiter, 1.0)
  assert frame.total == 1


@pytest.mark.asyncio
async def test_model_io_ws_stream() -> None:
  import websockets

  from egia.api.status import HealthState
  from egia.core.perception.admission import Admission
  from egia.io.northbound import MODEL_IO_PATH, NorthboundServer
  from egia.io.state import ServerState

  state = ServerState("test")
  state.status = HealthState.READY
  store = ModelIOStore()
  store.publish(_exchange(0, reply="step reply", thinking="considering the route"))
  server = NorthboundServer(
    state,
    Admission(4),
    host="127.0.0.1",
    port=0,
    model_io_store=store,
  )
  await server.start()
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{MODEL_IO_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), 2.0))
      assert frame["total"] == 1
      assert frame["exchange"]["reply"] == "step reply"
      assert frame["exchange"]["thinking"] == "considering the route"
      assert frame["exchange"]["messages"][0]["role"] == "user"
      assert frame["agents"][0]["agent_id"] == "planner"
      assert frame["agents"][0]["thinking"] is False
      assert frame["agents"][0]["status"] == "idle"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_model_io_ws_gated_before_ready() -> None:
  import websockets

  from egia.core.perception.admission import Admission
  from egia.io.northbound import MODEL_IO_PATH, NorthboundServer
  from egia.io.state import ServerState

  state = ServerState("test")
  server = NorthboundServer(
    state,
    Admission(4),
    host="127.0.0.1",
    port=0,
    model_io_store=ModelIOStore(),
  )
  await server.start()
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{MODEL_IO_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), 2.0))
      assert frame == {"error": "unavailable"}
  finally:
    await server.stop()


def test_planner_publishes_exchanges() -> None:
  """The planner forwards each request/response pair to the sink."""
  from egia.core.decision.planner import DecisionPlanner as P
  from egia.lib.vlm import ModelResponse, Usage

  planner = P.__new__(P)
  planner._exchange_sink = None
  planner._exchange_counter = 0

  captured: list[ModelExchange] = []
  planner._exchange_sink = captured.append
  planner._publish_exchange(
    "m-1",
    1,
    [
      __import__("egia.lib.vlm", fromlist=["ChatMessage"]).ChatMessage(
        role="user", content="hi"
      )
    ],
    ModelResponse(
      text="ok",
      thinking="reasoning block",
      model="test",
      usage=Usage(prompt_tokens=1, completion_tokens=2),
    ),
    0.0,
    reasoning_effort="high",
  )
  assert len(captured) == 1
  ex = captured[0]
  assert ex.mission_id == "m-1" and ex.step == 1
  assert ex.messages[0].content == "hi" and ex.reply == "ok"
  assert ex.thinking == "reasoning block"
  assert ex.reasoning_effort == "high"
  assert planner._exchange_counter == 1

"""Discovery-driven EOS status and world perception."""

from __future__ import annotations

import asyncio
import contextlib
import json
from collections.abc import Awaitable, Callable, Mapping, Sequence
from typing import Protocol

from pydantic import BaseModel

from egia.contracts.endpoints import ResourceKind
from egia.contracts.perception.world.asr import AsrSnapshot
from egia.contracts.perception.world.detection import DetectionSnapshot
from egia.contracts.perception.world.state import WorldStateSnapshot
from egia.contracts.registry import RESOURCE_CONTRACTS
from egia.core.link import EosClient, ResourceBinding
from egia.core.perception.admission import Admission
from egia.core.perception.body.render import render_detection
from egia.core.perception.body.source import EosResourceSource
from egia.lib.logger import get_logger

DETECTION_KIND = "detection"
BODY_KIND = "body"


class PartPublisher(Protocol):
  async def publish(self, state: BaseModel, *, key: str | None = None) -> None: ...


PART_MODELS = {kind: contract.model for kind, contract in RESOURCE_CONTRACTS.items()}

__all__ = [
  "BODY_KIND",
  "DETECTION_KIND",
  "PART_MODELS",
  "EosResourceSource",
  "PartPublisher",
  "parts_perception_entry",
  "render_detection",
]


def _render_detection_payload(snapshot: DetectionSnapshot) -> str:
  return json.dumps(
    [render_detection(item) for item in snapshot.detections],
    ensure_ascii=False,
    separators=(",", ":"),
  )


async def _admit_detections(
  queue: Admission, snapshot: DetectionSnapshot, robot: str, device_id: str
) -> bool:
  detections = snapshot.detections
  record = await queue.enqueue_latest(
    kind=DETECTION_KIND,
    source=f"detection.{robot}.{device_id}",
    text=_render_detection_payload(snapshot),
    metadata={
      "robot_id": robot,
      "device_id": device_id,
      "detection_count": str(len(detections)),
      "source_sequence": str(snapshot.source_sequence or snapshot.sequence),
      "source_timestamp": snapshot.timestamp.isoformat(),
    },
  )
  return record is not None


async def parts_perception_entry(
  client: EosClient,
  resources: Sequence[ResourceBinding],
  stores: Mapping[ResourceKind, PartPublisher],
  admission: Admission,
  ready: asyncio.Event,
  stop: asyncio.Event,
  *,
  frame_monitor: Callable[[ResourceKind, BaseModel], Awaitable[None]] | None = None,
  backend: str = "default",
) -> None:
  """Watch every discovered status/world resource and publish typed snapshots."""
  log = get_logger("egia.core.perception")
  signatures: dict[str, tuple[str, str]] = {}
  producers: list[asyncio.Task[None]] = []
  wired: list[str] = []
  for binding in resources:
    kind = binding.kind
    model = PART_MODELS.get(kind)
    store = stores.get(kind)
    if model is None or store is None:
      continue
    device_id = binding.device_id or "default"
    target = f"{backend}/{device_id}"

    async def _publish(
      snapshot: BaseModel,
      *,
      kind=kind,
      store=store,
      device_id=device_id,
      target=target,
      uri=binding.uri,
    ) -> None:
      await store.publish(snapshot, key=target)
      if frame_monitor is not None:
        await frame_monitor(kind, snapshot)
      if kind is ResourceKind.WORLD and isinstance(snapshot, WorldStateSnapshot):
        await admission.enqueue_latest(
          kind="objects",
          source=f"world.{backend}",
          text=json.dumps(
            {
              "objects": [item.model_dump(mode="json") for item in snapshot.entities],
              "stock": [],
            },
            ensure_ascii=False,
            separators=(",", ":"),
          ),
          metadata={
            "robot_id": backend,
            "resource_uri": uri,
            "source_sequence": str(snapshot.sequence),
          },
        )
      if kind is ResourceKind.DETECTION and isinstance(snapshot, DetectionSnapshot):
        signature = (_render_detection_payload(snapshot), "")
        if signature != signatures.get(target) and await _admit_detections(
          admission, snapshot, backend, device_id
        ):
          signatures[target] = signature
      if kind is ResourceKind.ASR and isinstance(snapshot, AsrSnapshot):
        transcript = snapshot.text.strip()
        signature = (transcript, "")
        if transcript and signature != signatures.get(target):
          record = await admission.enqueue(
            kind="hearing",
            source=f"microphone.{backend}.{device_id}",
            text=transcript,
            metadata={
              "robot_id": backend,
              "device_id": device_id,
              "source_sequence": str(snapshot.sequence),
            },
          )
          if record is not None:
            signatures[target] = signature

    source = EosResourceSource(client, kind, binding.uri, model)
    producers.append(
      asyncio.create_task(
        source.run(_publish, stop), name=f"egia-eos-{backend}-{device_id}"
      )
    )
    wired.append(binding.uri)
  ready.set()
  log.info("parts_perception_ready", robot=backend, resources=sorted(wired))
  await stop.wait()
  for producer in producers:
    producer.cancel()
  for producer in producers:
    with contextlib.suppress(asyncio.CancelledError):
      await producer
  log.info("parts_perception_stopped", robot=backend)

"""Durable pre-egress operation identities for receipt-only recovery."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from egia.lib.safety_state import SafetyStateError, _directory, _read, _write

_ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")


@dataclass(frozen=True)
class PendingOperation:
  operation_id: str
  alias: str
  primitive: str
  device_id: str
  mission_id: str | None
  eos_robot_id: str
  graph_revision: str


class OperationLedger:
  """One atomic file per operation; startup never resubmits these IDs."""

  def __init__(self, root: Path) -> None:
    self.root = root / "operations"
    self._presence = root / "operations-history.json"
    if self._presence.exists() and not self.root.exists():
      raise SafetyStateError("operation directory is missing after prior reservation")
    _directory(self.root)

  def reserve(self, operation: PendingOperation) -> None:
    self.pending()
    if not _ID.fullmatch(operation.operation_id) or not _ID.fullmatch(operation.alias):
      raise SafetyStateError("invalid operation identity")
    if any(
      not isinstance(value, str) or len(value) > 128
      for value in (operation.primitive, operation.device_id)
    ):
      raise SafetyStateError("invalid operation target")
    if operation.mission_id is not None and not _ID.fullmatch(operation.mission_id):
      raise SafetyStateError("invalid mission identity")
    if (
      not operation.eos_robot_id
      or len(operation.eos_robot_id.encode()) > 128
      or not re.fullmatch(r"[0-9a-f]{64}|standalone", operation.graph_revision)
    ):
      raise SafetyStateError("invalid EOS operation generation")
    path = self.root / f"{operation.operation_id}.json"
    index = self.root / f"{operation.operation_id}.index"
    if path.exists() or index.exists():
      raise SafetyStateError("operation ID already reserved")
    presence = _read(self._presence)
    count = 0 if presence is None else presence["reserved_count"]
    if not isinstance(count, int) or count >= 2**63 - 1:
      raise SafetyStateError("operation history is malformed")
    sequence = count + 1
    # Monotonic count plus per-ID sequence detects a lost index/payload pair
    # without growing the history file on every operation.
    _write(
      self._presence,
      {"schema": "egia-operations-history-v1", "reserved_count": sequence},
    )
    # Index first: a crash before the pending payload is written leaves a
    # detectable missing record, never an invisible physical attempt.
    _write(
      index,
      {
        "schema": "egia-operation-index-v1",
        "operation_id": operation.operation_id,
        "sequence": sequence,
      },
    )
    _write(
      path, {"schema": "egia-operation-v1", "state": "pending", **operation.__dict__}
    )

  def settle(self, operation_id: str, outcome: str) -> None:
    operation = self.get(operation_id)
    value = _read(self.root / f"{operation_id}.json")
    if (
      operation is None
      or value is None
      or value.get("state") != "pending"
      or outcome not in {"succeeded", "failed", "cancelled", "rejected"}
    ):
      raise SafetyStateError("operation cannot be settled")
    _write(
      self.root / f"{operation_id}.json",
      {
        "schema": "egia-operation-v1",
        "state": "terminal",
        "outcome": outcome,
        **operation.__dict__,
      },
    )

  def get(self, operation_id: str) -> PendingOperation | None:
    if not _ID.fullmatch(operation_id):
      raise SafetyStateError("invalid operation identity")
    index = _read(self.root / f"{operation_id}.index")
    value = _read(self.root / f"{operation_id}.json")
    if index is None and value is None:
      return None
    index_sequence = index.get("sequence") if index is not None else None
    if (
      index is None
      or index.get("schema") != "egia-operation-index-v1"
      or index.get("operation_id") != operation_id
      or type(index_sequence) is not int
      or not 1 <= index_sequence < 2**63
      or value is None
    ):
      raise SafetyStateError("operation index or checkpoint is missing")
    if value.get("schema") != "egia-operation-v1" or value.get("state") not in {
      "pending",
      "terminal",
    }:
      raise SafetyStateError("operation checkpoint is malformed")
    if value["state"] == "terminal" and value.get("outcome") not in {
      "succeeded",
      "failed",
      "cancelled",
      "rejected",
    }:
      raise SafetyStateError("operation terminal checkpoint is malformed")
    if value["state"] == "pending" and "outcome" in value:
      raise SafetyStateError("pending operation has a terminal outcome")
    stored_id = value.get("operation_id")
    alias = value.get("alias")
    primitive = value.get("primitive")
    device_id = value.get("device_id")
    mission_id = value.get("mission_id")
    eos_robot_id = value.get("eos_robot_id")
    graph_revision = value.get("graph_revision")
    if (
      type(stored_id) is not str
      or stored_id != operation_id
      or type(alias) is not str
      or _ID.fullmatch(alias) is None
      or type(primitive) is not str
      or len(primitive.encode()) > 128
      or type(device_id) is not str
      or len(device_id.encode()) > 128
      or (
        mission_id is not None
        and (type(mission_id) is not str or _ID.fullmatch(mission_id) is None)
      )
      or type(eos_robot_id) is not str
      or not eos_robot_id
      or len(eos_robot_id.encode()) > 128
      or type(graph_revision) is not str
      or re.fullmatch(r"[0-9a-f]{64}|standalone", graph_revision) is None
    ):
      raise SafetyStateError("operation checkpoint is malformed")
    return PendingOperation(
      stored_id, alias, primitive, device_id, mission_id, eos_robot_id, graph_revision
    )

  def pending(self) -> list[PendingOperation]:
    result: list[PendingOperation] = []
    presence = _read(self._presence)
    reserved_count = presence.get("reserved_count") if presence is not None else None
    if presence is not None and (
      presence.get("schema") != "egia-operations-history-v1"
      or type(reserved_count) is not int
      or not 1 <= reserved_count < 2**63
    ):
      raise SafetyStateError("operation history is malformed")
    indexes: set[str] = set()
    records: set[str] = set()
    for path in sorted(self.root.iterdir()):
      if path.name.startswith("."):
        continue
      if path.suffix == ".index" and _ID.fullmatch(path.stem):
        indexes.add(path.stem)
      elif path.suffix == ".json" and _ID.fullmatch(path.stem):
        records.add(path.stem)
      else:
        raise SafetyStateError("unexpected operation checkpoint filename")
    if indexes != records:
      raise SafetyStateError("operation index and checkpoint set differ")
    if (presence is None and indexes) or (
      presence is not None and len(indexes) != reserved_count
    ):
      raise SafetyStateError("operation history and index set differ")
    sequences: set[int] = set()
    for operation_id in sorted(indexes):
      operation = self.get(operation_id)
      index = _read(self.root / f"{operation_id}.index")
      if index is None:
        raise SafetyStateError("operation index is missing")
      sequence = index["sequence"]
      if not isinstance(sequence, int) or sequence in sequences:
        raise SafetyStateError("operation sequence is malformed")
      sequences.add(sequence)
      value = _read(self.root / f"{operation_id}.json")
      if (
        operation is not None and value is not None and value.get("state") == "pending"
      ):
        result.append(operation)
    if presence is not None and (
      min(sequences) != 1 or max(sequences) != reserved_count
    ):
      raise SafetyStateError("operation sequence set differs from history")
    return result

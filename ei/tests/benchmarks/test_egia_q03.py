"""Q03 benchmark accounting and failure checks, not throughput targets."""

from __future__ import annotations

from unittest.mock import Mock

import pytest

from tests.benchmarks.egia_q03 import accounting, drain_barrier, percentile, stage_run


def test_nearest_rank_and_accounting() -> None:
  assert percentile([3_000_000, 1_000_000, 2_000_000], 0.5) == 2
  assert percentile([3_000_000, 1_000_000, 2_000_000], 0.99) == 3
  accounting(4, 2, 2, 0)
  with pytest.raises(RuntimeError, match="accounting failed"):
    accounting(4, 2, 1, 0)
  with pytest.raises(RuntimeError, match="accounting failed"):
    accounting(4, 2, 2, 1)


def test_owner_failure_and_barrier_timeout() -> None:
  with pytest.raises(RuntimeError, match="injected owner failure"):
    stage_run(1, contended=False, fail_owner=True)
  edge = object()
  runtime = Mock()
  runtime.edge_counters = {edge: Mock(delivered=0, overflow=0)}
  runtime._pending = {edge: object()}
  with pytest.raises(TimeoutError, match="barrier"):
    drain_barrier(
      runtime, {"resource": edge}, {"resource": 1}, interval_ns=1, timeout_ns=0
    )
  runtime.drain_ready.assert_not_called()


def test_small_stage_fixture_accounts_both_routes() -> None:
  result = stage_run(10, contended=True)
  for route in result["routes"].values():
    accounting(route["offered"], route["delivered"], route["overflow"], 0)
    assert route["throughput_delivered_per_second"] == pytest.approx(
      route["delivered"] / result["elapsed_seconds"]
    )
  assert (
    result["payloads"]["resource"]["bytes"]
    > result["payloads"]["observations"]["bytes"]
  )
  assert all(len(payload["sha256"]) == 64 for payload in result["payloads"].values())
  assert result["errors"] == []

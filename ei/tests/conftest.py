"""Shared fixtures for EGIA tests."""

from __future__ import annotations

from pathlib import Path

import pytest

from egia.lib.config import GatewayConfig, LaunchConfig, load_launch_config
from tests.egia.stub.gateway import StubGateway, start_stub_gateway, stop_stub_gateway

PROJECT_ROOT = Path(__file__).resolve().parents[1]
ETC_ROOT = PROJECT_ROOT / "etc/egia"


@pytest.fixture
def etc_root() -> Path:
  return ETC_ROOT


@pytest.fixture
def launch_config(etc_root: Path) -> LaunchConfig:
  return load_launch_config(etc=str(etc_root), mode="sandbox")


@pytest.fixture
async def stub_gateway() -> StubGateway:
  stub = await start_stub_gateway()
  yield stub
  await stop_stub_gateway(stub)


@pytest.fixture
def stub_launch_config(
  stub_gateway: StubGateway, launch_config: LaunchConfig
) -> LaunchConfig:
  return launch_config.model_copy(
    update={
      "gateway": GatewayConfig(
        host=stub_gateway.host,
        grpc_port=stub_gateway.port,
      ),
      "extension": None,
    }
  )

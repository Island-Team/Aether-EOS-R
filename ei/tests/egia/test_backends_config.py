"""EOS endpoint configuration and stable robot aliases."""

from __future__ import annotations

import pytest

from egia.lib.config import (
  ConfigError,
  DecisionConfig,
  GatewayConfig,
  PerceptionConfig,
  SandboxRuntimeConfig,
  ServerConfig,
  load_launch_config,
  load_mode_config,
)


def test_default_listener_ports_use_product_registry() -> None:
  assert ServerConfig().port == 16060
  assert GatewayConfig(host="127.0.0.1").grpc_port == 16200
  assert SandboxRuntimeConfig().gateway.grpc_port == 16022
  assert SandboxRuntimeConfig().websocket.port is None


_TWO_ENDPOINTS = """
[modes.sim.gateway]
host = "127.0.0.1"
grpc_port = 18010

[[modes.sim.backends]]
id = "robot-a"
gateway = { host = "127.0.0.1", grpc_port = 18011 }

[[modes.sim.backends]]
id = "robot-b"
gateway = { host = "127.0.0.1", grpc_port = 18012 }
type = "live"
"""


def _mode_root(tmp_path, text: str):
  root = tmp_path / "etc"
  root.mkdir(exist_ok=True)
  (root / "modes.toml").write_text(
    'default = "sim"\n[modes.sim]\n' + text, encoding="utf-8"
  )
  return root


def test_endpoint_list_preserves_alias_and_grpc_address(tmp_path) -> None:
  preset = load_mode_config(str(_mode_root(tmp_path, _TWO_ENDPOINTS)))
  assert [ep.id for ep in preset.backends] == ["robot-a", "robot-b"]
  assert [ep.gateway.grpc_port for ep in preset.backends] == [18011, 18012]
  assert preset.backends[1].type == "live"


def test_duplicate_aliases_rejected(tmp_path) -> None:
  text = _TWO_ENDPOINTS.replace('id = "robot-b"', 'id = "robot-a"')
  with pytest.raises(ConfigError, match="duplicate backend id"):
    load_mode_config(str(_mode_root(tmp_path, text)))


def test_empty_alias_rejected(tmp_path) -> None:
  text = _TWO_ENDPOINTS.replace('id = "robot-a"', 'id = ""')
  with pytest.raises(ConfigError, match="invalid backends entry"):
    load_mode_config(str(_mode_root(tmp_path, text)))


def test_whitespace_gateway_values_and_unknown_keys_are_rejected(tmp_path) -> None:
  whitespace = _TWO_ENDPOINTS.replace(
    'gateway = { host = "127.0.0.1", grpc_port = 18011 }',
    'gateway = { host = "   ", grpc_port = 18011 }',
  )
  with pytest.raises(ConfigError, match="invalid backends entry"):
    load_mode_config(str(_mode_root(tmp_path, whitespace)))

  misspelled = _TWO_ENDPOINTS.replace(
    "grpc_port = 18011", "grpc_port = 18011, grpc_prt = 18011"
  )
  with pytest.raises(ConfigError, match="invalid backends entry"):
    load_mode_config(str(_mode_root(tmp_path, misspelled)))


def test_duplicate_gateway_endpoints_are_rejected(tmp_path) -> None:
  text = _TWO_ENDPOINTS.replace("grpc_port = 18012", "grpc_port = 18011")
  with pytest.raises(ConfigError, match="duplicate backend gateway"):
    load_mode_config(str(_mode_root(tmp_path, text)))


@pytest.mark.parametrize("removed", ["tools", "capabilities"])
def test_removed_mode_configuration_is_rejected(tmp_path, removed: str) -> None:
  text = (
    '[modes.sim.gateway]\nhost = "127.0.0.1"\ngrpc_port = 18010\n\n'
    f"[modes.sim.{removed}]\nexpose_operator = true\n"
  )
  with pytest.raises(ConfigError, match="removed configuration"):
    load_mode_config(str(_mode_root(tmp_path, text)))


def test_gateway_accepts_port_as_transition_alias() -> None:
  gateway = GatewayConfig(host="127.0.0.1", port=18010)
  assert gateway.grpc_port == 18010
  assert gateway.endpoint == "127.0.0.1:18010"


def test_endpoints_empty_list_synthesizes_default(launch_config) -> None:
  endpoints = launch_config.endpoints()
  assert [(ep.id, ep.gateway) for ep in endpoints] == [
    ("default", launch_config.gateway)
  ]


def test_launch_config_carries_endpoint_list(tmp_path, etc_root) -> None:
  import shutil

  for name in ("models.toml", "prompts.toml", "safety", "egia.toml"):
    source = etc_root / name
    target = tmp_path / name
    if source.is_dir():
      shutil.copytree(source, target)
    else:
      shutil.copy(source, target)
  (tmp_path / "modes.toml").write_text(
    'default = "sim"\n[modes.sim]\n' + _TWO_ENDPOINTS, encoding="utf-8"
  )
  config = load_launch_config(
    etc=str(tmp_path),
  )
  assert [ep.id for ep in config.backends] == ["robot-a", "robot-b"]


@pytest.mark.parametrize("removed", ["tools", "capabilities"])
def test_launch_rejects_removed_top_level_configuration(
  tmp_path, etc_root, removed: str
) -> None:
  import shutil

  root = tmp_path / "etc"
  shutil.copytree(etc_root, root)
  config_file = root / "egia.toml"
  config_file.write_text(
    config_file.read_text(encoding="utf-8")
    + f"\n[{removed}]\nexpose_operator = true\n",
    encoding="utf-8",
  )
  with pytest.raises(ConfigError, match="removed configuration"):
    load_launch_config(etc=str(root))


@pytest.mark.parametrize("filename", ["egia.toml", "models.toml"])
def test_launch_rejects_removed_speech_configuration(
  tmp_path, etc_root, filename: str
) -> None:
  import shutil

  root = tmp_path / "etc"
  shutil.copytree(etc_root, root)
  config_file = root / filename
  config_file.write_text(
    config_file.read_text(encoding="utf-8") + "\n[speech]\n",
    encoding="utf-8",
  )
  with pytest.raises(ConfigError, match="configure ASR and TTS in EOS"):
    load_launch_config(etc=str(root))


def test_removed_decision_and_perception_fields_are_rejected() -> None:
  with pytest.raises(ValueError, match="plan_execute_mode"):
    DecisionConfig.model_validate({"plan_execute_mode": False})
  with pytest.raises(ValueError, match="theta_frame"):
    PerceptionConfig.model_validate({"theta_frame": 3.0})
  with pytest.raises(ValueError, match="tau"):
    PerceptionConfig.model_validate({"tau": {"fault": 300.0}})


def test_mission_preload_defaults_off_and_can_be_enabled() -> None:
  defaults = DecisionConfig()
  assert defaults.mission_preload is False
  assert defaults.load_mission_templates is False
  assert defaults.max_invalid_replies == 3
  configured = DecisionConfig.model_validate(
    {"mission_preload": True, "load_mission_templates": True}
  )
  assert configured.mission_preload is True
  assert configured.load_mission_templates is True

from egia.core.context import ContextBuilder, ModeState
from egia.core.perception.admission import Admission
from egia.lib.config import load_prompt_config


def test_mode_is_rendered_and_switch_rebuilds_prompt(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  state = ModeState()
  builder = ContextBuilder(Admission(capacity=4), prompts, mode_state=state)

  assert "SANDBOX" in builder.build().prompt
  assert "LIVE" in builder.switch_mode("live").prompt


def test_mode_switch_rejects_unknown_mode(etc_root) -> None:
  state = ModeState()
  prompts = load_prompt_config(etc_root)
  builder = ContextBuilder(Admission(capacity=4), prompts, mode_state=state)

  try:
    builder.switch_mode("other")
  except ValueError as exc:
    assert "unsupported backend mode" in str(exc)
  else:
    raise AssertionError("unknown mode was accepted")

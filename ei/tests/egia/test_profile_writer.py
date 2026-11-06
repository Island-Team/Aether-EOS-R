"""Profile-writer prompt, validation, retry, and persistence behavior."""

from __future__ import annotations

from pathlib import Path

import pytest

from egia.core.decision.profile_writer import ProfileWriter, ProfileWriterError
from egia.core.decision.profiles import Profile, ProfileStore
from egia.core.registry import PrimitiveRegistry
from egia.lib.config import load_prompt_config
from egia.lib.vlm import ChatMessage, ModelResponse

_PROFILE_V1 = """```toml
name = "cabinet-a2"
version = 1
order = "assemble breaker into cabinet A2"
intro = "Pick from the tray, then place at the fixture."

[[steps]]
id = "fetch"
primitive = "gripper.motion"
tags = ["pick-place"]
arguments = {action = "close"}

[[steps]]
id = "place"
primitive = "gripper.motion"
after = ["fetch"]
est_duration_s = 2.5
arguments = {action = "open"}
```"""

_PROFILE_V2 = _PROFILE_V1.replace("version = 1", "version = 2").replace(
  'intro = "Pick from the tray, then place at the fixture."',
  'intro = "Updated fixture sequence."',
)

_BAD_DEPENDENCY = """name = "cabinet-a2"
version = 1
order = "assemble breaker into cabinet A2"

[[steps]]
id = "place"
primitive = "gripper.motion"
after = ["ghost"]
"""

_UNKNOWN_PRIMITIVE = """name = "cabinet-a2"
version = 1
order = "assemble breaker into cabinet A2"

[[steps]]
id = "fetch"
primitive = "magic.grab"
"""


class ScriptedModel:
  reasoning_options = ("none", "high")

  def __init__(self, replies: list[str]) -> None:
    self._replies = list(replies)
    self.requests: list[list[ChatMessage]] = []
    self.kwargs: list[dict[str, object]] = []

  async def complete(
    self, messages: list[ChatMessage], **kwargs: object
  ) -> ModelResponse:
    self.requests.append([message.model_copy(deep=True) for message in messages])
    self.kwargs.append(kwargs)
    return ModelResponse(text=self._replies.pop(0))


def _writer(
  tmp_path: Path,
  etc_root: Path,
  replies: list[str],
  *,
  store: ProfileStore | None = None,
  max_attempts: int = 3,
  reasoning_effort: str | None = None,
) -> tuple[ProfileWriter, ScriptedModel, ProfileStore]:
  model = ScriptedModel(replies)
  profile_store = store or ProfileStore(tmp_path / "profiles")
  writer = ProfileWriter(
    model,
    PrimitiveRegistry.from_contracts(["robot-a"]),
    profile_store,
    prompts=load_prompt_config(etc_root).profile_writer,
    max_attempts=max_attempts,
    reasoning_effort=reasoning_effort,  # type: ignore[arg-type]
  )
  return writer, model, profile_store


@pytest.mark.asyncio
async def test_profile_writer_keeps_static_and_dynamic_messages_separate(
  tmp_path: Path, etc_root: Path
) -> None:
  writer, model, store = _writer(tmp_path, etc_root, [_PROFILE_V1])

  profile = await writer.write("assemble breaker into cabinet A2")

  assert store.load("cabinet-a2") == profile
  assert len(model.requests) == 1
  system, user = model.requests[0]
  assert system.role == "system"
  assert user.role == "user"
  assert isinstance(system.content, str)
  assert isinstance(user.content, str)
  assert system.content == load_prompt_config(etc_root).profile_writer.instruction
  assert "assemble breaker into cabinet A2" not in system.content
  assert "gripper.motion" not in system.content
  assert "Primitives:\n" in user.content
  assert "gripper.motion@robot-a/gripper-0" in user.content
  assert "Existing profiles: unavailable" in user.content
  assert "Mission: assemble breaker into cabinet A2" in user.content
  assert user.content.endswith("Previous profile validation: none")
  assert user.content.index("Primitives:") < user.content.index("Mission:")


@pytest.mark.asyncio
async def test_profile_writer_retries_with_validation_feedback_at_the_end(
  tmp_path: Path, etc_root: Path
) -> None:
  writer, model, _store = _writer(tmp_path, etc_root, [_BAD_DEPENDENCY, _PROFILE_V1])

  profile = await writer.write("assemble breaker into cabinet A2")

  assert profile.name == "cabinet-a2"
  assert len(model.requests) == 2
  first_user = model.requests[0][1].content
  second_user = model.requests[1][1].content
  assert isinstance(first_user, str)
  assert isinstance(second_user, str)
  assert first_user.endswith("Previous profile validation: none")
  assert "depends on unknown steps" in second_user
  assert "ghost" in second_user
  assert second_user.rsplit("\n\n", 1)[-1].startswith(
    "Previous profile validation failed:"
  )
  assert model.requests[0][0] == model.requests[1][0]


@pytest.mark.asyncio
async def test_profile_writer_exhaustion_does_not_update_store(
  tmp_path: Path, etc_root: Path
) -> None:
  writer, model, store = _writer(
    tmp_path,
    etc_root,
    ["not = valid = toml", _UNKNOWN_PRIMITIVE],
    max_attempts=2,
  )

  with pytest.raises(ProfileWriterError, match="magic.grab"):
    await writer.write("assemble breaker into cabinet A2")

  assert len(model.requests) == 2
  assert "TOML parse error" in model.requests[1][1].content
  assert store.load_all() == []


@pytest.mark.asyncio
async def test_profile_writer_renders_existing_profile_and_saves_new_version(
  tmp_path: Path, etc_root: Path
) -> None:
  store = ProfileStore(tmp_path / "profiles")
  existing = Profile.model_validate(
    {
      "name": "cabinet-a2",
      "version": 1,
      "order": "assemble breaker into cabinet A2",
      "steps": [{"id": "fetch", "primitive": "gripper.motion"}],
    }
  )
  store.save(existing)
  writer, model, _store = _writer(
    tmp_path, etc_root, [_PROFILE_V2], store=store, reasoning_effort="high"
  )

  updated = await writer.write("assemble breaker into cabinet A2")

  assert updated.version == 2
  assert store.load("cabinet-a2") == updated
  assert store.load("cabinet-a2", 1) == existing
  user = model.requests[0][1].content
  assert isinstance(user, str)
  assert "Existing profiles:\n- cabinet-a2 v1" in user
  assert user.index("Existing profiles:") < user.index("Mission:")
  assert model.kwargs == [{"reasoning_effort": "high"}]

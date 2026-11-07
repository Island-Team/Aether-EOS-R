"""Model-authored job profiles — persisted plan notes with an enforced skeleton.

A profile is the planner's own note for one kind of job, written as a TOML
file under the injected durable-state root at decomposition time and re-read on later
requests: long-term file-based memory in the "plans as memory" loop (§3.3),
promoted to a first-class artifact. A changed job means a new profile file —
changeover is one write, never re-programming.

The model authors the *whole* file; the skeleton inside is the one enforced
part. Free text (``intro`` — ordering rationale, scene specifics) is never
parsed; the ``[[steps]]`` core is validated on write because downstream
consumers are machines: the coordination board computes ready sets from
``after`` dependencies, bidders match on ``tags``, and the executor needs
primitive names it can resolve. Step ``primitive`` values are plain strings
and are cross-checked against the live
registry at decomposition time, not inside the model, so the model stays
registry-agnostic.
"""

from __future__ import annotations

import re
import shutil
import tomllib
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from egia.core.registry import PrimitiveRegistry
from egia.lib.logger import get_logger

__all__ = [
  "Profile",
  "ProfileError",
  "ProfileStep",
  "ProfileStore",
  "ready_steps",
  "render_active",
  "render_catalog",
  "unknown_primitives",
]

_NAME_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")
_STEP_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]*$")


class ProfileError(Exception):
  """Raised when a profile fails skeleton validation.

  The message names the profile and the offending step so the writer seam
  can feed it straight back to the model for a retry.
  """


class ProfileStep(BaseModel):
  """One enforced skeleton step: what runs, in what order, on what tags."""

  model_config = ConfigDict(extra="forbid")

  id: str = Field(description="step id unique within the profile")
  primitive: str = Field(min_length=1, description="discovered primitive name")
  after: list[str] = Field(
    default_factory=list, description="step ids that must finish before this one"
  )
  tags: list[str] = Field(
    default_factory=list, description="primitive tags; bidders match on these"
  )
  inputs: list[str] = Field(
    default_factory=list, description="object ids the step consumes"
  )
  outputs: list[str] = Field(
    default_factory=list, description="object ids the step produces"
  )
  zone: str | None = Field(
    default=None, description="shared-workspace zone the step occupies, if any"
  )
  station: str | None = Field(
    default=None,
    description="station the step serves, when it names one; bidders that do "
    "not serve it abstain (reachability is the bidder's feasibility)",
  )
  est_duration_s: float = Field(
    default=1.0, gt=0, description="estimated execution duration in seconds"
  )
  arguments: dict[str, Any] = Field(
    default_factory=dict,
    description=(
      "primitive request body the executor sends verbatim; the coordinator adds "
      "part_id and the gate validates the contract"
    ),
  )

  @field_validator("id")
  @classmethod
  def _check_id(cls, value: str) -> str:
    if not _STEP_ID_PATTERN.match(value):
      raise ValueError(
        f"step id {value!r} must be lowercase letters/digits/dashes/underscores"
      )
    return value


class Profile(BaseModel):
  """One validated profile: free-form note plus the enforced step skeleton."""

  model_config = ConfigDict(extra="forbid")

  name: str = Field(description="stable profile slug; also the file stem")
  version: int = Field(default=1, ge=1, description="bumped when semantics change")
  order: str = Field(
    min_length=1, description="source order text this profile was written for"
  )
  intro: str = Field(
    default="", description="the model's semantic note to itself; never parsed"
  )
  steps: list[ProfileStep] = Field(min_length=1)

  @field_validator("name")
  @classmethod
  def _check_name(cls, value: str) -> str:
    if not _NAME_PATTERN.match(value):
      raise ValueError(
        f"profile name {value!r} must be lowercase letters/digits/dashes"
      )
    return value

  @model_validator(mode="after")
  def _check_skeleton(self) -> Profile:
    ids = [step.id for step in self.steps]
    duplicates = {step_id for step_id in ids if ids.count(step_id) > 1}
    if duplicates:
      raise ValueError(f"duplicate step ids: {sorted(duplicates)}")
    known = set(ids)
    for step in self.steps:
      missing = [dep for dep in step.after if dep not in known]
      if missing:
        raise ValueError(f"step {step.id!r} depends on unknown steps: {missing}")
    _reject_cycles(self.steps)
    return self

  def step(self, step_id: str) -> ProfileStep | None:
    """The step with this id, or ``None``."""
    return next((step for step in self.steps if step.id == step_id), None)


def _reject_cycles(steps: Sequence[ProfileStep]) -> None:
  """Raise on dependency cycles; DFS with tri-color marking."""
  by_id = {step.id: step for step in steps}
  state: dict[str, int] = {}  # 0 unvisited, 1 in-stack, 2 done

  def visit(step_id: str, stack: list[str]) -> None:
    mark = state.get(step_id, 0)
    if mark == 2:
      return
    if mark == 1:
      cycle = stack[stack.index(step_id) :] + [step_id]
      raise ValueError(f"dependency cycle: {' -> '.join(cycle)}")
    state[step_id] = 1
    for dep in by_id[step_id].after:
      visit(dep, stack + [step_id])
    state[step_id] = 2

  for step in steps:
    visit(step.id, [])


def ready_steps(profile: Profile, done: set[str]) -> list[ProfileStep]:
  """Steps whose dependencies are all in ``done``, in profile order.

  The board's precondition filter: this is the ready set the auction
  publishes. Order is the file's declaration order, so results are
  deterministic for the same profile and done-set.
  """
  return [
    step
    for step in profile.steps
    if step.id not in done and all(dep in done for dep in step.after)
  ]


def unknown_primitives(profile: Profile, registry: PrimitiveRegistry) -> list[str]:
  """Step primitive names the registry cannot resolve on any backend.

  The decomposition seam calls this before storing a profile: a stored
  profile is executable by construction, so the executor never meets a
  primitive it cannot invoke. The model writes names; this seam checks them.
  """
  unknown: list[str] = []
  for step in profile.steps:
    try:
      if registry.resolve(step.primitive) is None and step.primitive not in unknown:
        unknown.append(step.primitive)
    except LookupError:
      # Present on several backends: resolvable, just needs a backend choice
      # at award time — the auction makes that choice.
      continue
  return unknown


# ------------------------------------------------------------------ store


def _dump_profile(profile: Profile) -> str:
  """Serialize one profile to TOML (tomllib is read-only; the shape is fixed)."""
  lines = [
    "# Written by the planner; skeleton validated on write. Free text stays free.",
    f'name = "{profile.name}"',
    f"version = {profile.version}",
    f"order = {_toml_string(profile.order)}",
    f"intro = {_toml_string(profile.intro)}",
    "",
  ]
  for step in profile.steps:
    lines.append("[[steps]]")
    lines.append(f'id = "{step.id}"')
    lines.append(f'primitive = "{step.primitive}"')
    for key, values in (
      ("after", step.after),
      ("tags", step.tags),
      ("inputs", step.inputs),
      ("outputs", step.outputs),
    ):
      if values:
        lines.append(f"{key} = [{', '.join(_toml_string(v) for v in values)}]")
    if step.zone is not None:
      lines.append(f'zone = "{step.zone}"')
    if step.station is not None:
      lines.append(f'station = "{step.station}"')
    lines.append(f"est_duration_s = {step.est_duration_s}")
    if step.arguments:
      if len(step.arguments) == 1:
        # A single pair stays inline; several become a sub-table.
        key, value = next(iter(step.arguments.items()))
        lines.append(f"arguments = {{{key} = {_toml_value(value)}}}")
      else:
        lines.append("[steps.arguments]")
        lines.extend(
          f"{key} = {_toml_value(value)}" for key, value in step.arguments.items()
        )
    lines.append("")
  return "\n".join(lines)


def _toml_value(value: Any) -> str:
  """One TOML scalar or array for a step-argument value."""
  if isinstance(value, bool):
    return "true" if value else "false"
  if isinstance(value, (int, float)):
    return repr(value)
  if isinstance(value, list):
    return f"[{', '.join(_toml_value(item) for item in value)}]"
  return _toml_string(str(value))


def _toml_string(value: str) -> str:
  """One TOML basic string with minimal escaping."""
  escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
  return f'"{escaped}"'


class ProfileStore:
  """File-backed profile store under the injected durable-state root.

  Writes only validate profiles (the seam retries invalid ones); loads
  quarantine files that fail validation instead of crashing the pipeline —
  a corrupted or hand-edited profile is one job's note, never a launch
  failure. Quarantined files move to ``quarantine/`` with a ``.invalid``
  suffix so they are visible but never re-loaded.
  """

  def __init__(self, root: Path) -> None:
    self._root = root
    self._log = get_logger("egia.core.decision.profiles")

  @property
  def root(self) -> Path:
    return self._root

  def path_for(self, profile: Profile) -> Path:
    """The file a profile with this name/version occupies."""
    return self._root / f"{profile.name}.v{profile.version}.toml"

  def save(self, profile: Profile) -> Path:
    """Persist one validated profile; overwrites the same name+version."""
    self._root.mkdir(parents=True, exist_ok=True)
    path = self.path_for(profile)
    path.write_text(_dump_profile(profile), encoding="utf-8")
    self._log.info(
      "profile_saved",
      profile=profile.name,
      version=profile.version,
      steps=len(profile.steps),
    )
    return path

  def load(self, name: str, version: int | None = None) -> Profile | None:
    """The named profile (latest version when ``version`` is None), or ``None``."""
    if version is not None:
      return self._load_file(self._root / f"{name}.v{version}.toml")
    # Names are [a-z0-9-] by validation, so they carry no glob metacharacters.
    candidates = sorted(self._root.glob(f"{name}.v*.toml"))
    for path in reversed(candidates):
      profile = self._load_file(path)
      if profile is not None:
        return profile
    return None

  def load_all(self) -> list[Profile]:
    """Every valid profile, newest version per name, sorted by name."""
    latest: dict[str, Profile] = {}
    for path in sorted(self._root.glob("*.v*.toml")):
      profile = self._load_file(path)
      if profile is None:
        continue
      current = latest.get(profile.name)
      if current is None or profile.version > current.version:
        latest[profile.name] = profile
    return [latest[name] for name in sorted(latest)]

  def _load_file(self, path: Path) -> Profile | None:
    if not path.is_file():
      return None
    try:
      document = tomllib.loads(path.read_text(encoding="utf-8"))
      return Profile.model_validate(document)
    except (OSError, ValueError, tomllib.TOMLDecodeError) as exc:
      self._quarantine(path, str(exc))
      return None

  def _quarantine(self, path: Path, reason: str) -> None:
    quarantine = self._root / "quarantine"
    quarantine.mkdir(parents=True, exist_ok=True)
    target = quarantine / f"{path.name}.invalid"
    try:
      shutil.move(str(path), target)
    except OSError:
      self._log.error("profile_quarantine_failed", file=str(path))
      return
    self._log.warning(
      "profile_quarantined", file=path.name, target=str(target), reason=reason
    )


# -------------------------------------------------------------- rendering


def render_active(profile: Profile) -> str:
  """The active profile's prompt block: full skeleton plus the intro."""
  lines = [
    f"profile {profile.name} v{profile.version} ({len(profile.steps)} steps)",
    f"order: {profile.order}",
  ]
  if profile.intro:
    lines.append(f"intro: {profile.intro}")
  for step in profile.steps:
    deps = f" after [{', '.join(step.after)}]" if step.after else ""
    tags = f" tags [{', '.join(step.tags)}]" if step.tags else ""
    lines.append(f"- {step.id}: {step.primitive}{deps}{tags} ~{step.est_duration_s:g}s")
  return "\n".join(lines)


def render_catalog(profiles: Sequence[Profile]) -> str:
  """One line per profile — the catalog slot for non-active profiles."""
  return "\n".join(
    f"- {profile.name} v{profile.version}: {len(profile.steps)} steps, {profile.order}"
    for profile in profiles
  )

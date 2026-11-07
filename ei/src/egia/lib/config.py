"""Configuration loading — TOML via stdlib tomllib into Pydantic models.

Implements the ``etc/`` contract from ``docs/egia/egia-2607.md`` §1.3: the config
root is resolved from ``--etc`` or ``EGIA_ETC`` at startup; a missing root
aborts launch before any module initializes. Launch-mode presets name external
EOS gRPC endpoints; resource and primitive routes are discovered at runtime.

Sits in ``egia/lib/``: imports no other ``egia`` module.
"""

from __future__ import annotations

import os
import tomllib
from pathlib import Path
from typing import Any, Literal

from pydantic import AliasChoices, BaseModel, ConfigDict, Field, model_validator

from island.path import Layout

__all__ = [
  "AgentPromptConfig",
  "BackendEndpointConfig",
  "CoordinationConfig",
  "DecisionConfig",
  "DecisionSentinelConfig",
  "GatewayConfig",
  "LaunchConfig",
  "LaunchPolicyConfig",
  "LogSinkConfig",
  "LoggingConfig",
  "MemoryConfig",
  "MissionTemplateCatalog",
  "MissionTemplateConfig",
  "MissionTemplateStepConfig",
  "ModelConfig",
  "ModelSelection",
  "MonitorConfig",
  "PerceptionConfig",
  "PrimitivesConfig",
  "PriorityConfig",
  "PromptConfig",
  "PromptInstructionSection",
  "PromptSectionConfig",
  "ProposerConfig",
  "SafetyConfig",
  "SafetyRateConfig",
  "SandboxRuntimeConfig",
  "ServerConfig",
  "load_launch_config",
  "load_logging_config",
  "load_mission_templates",
  "load_mode_config",
  "load_model_bindings",
  "load_model_config",
  "load_model_selection",
  "load_prompt_config",
  "load_safety_config",
  "load_sandbox_config",
]


class ServerConfig(BaseModel):
  """Northbound bind address for the EGIA server (``etc/egia/egia.toml``)."""

  host: str = "127.0.0.1"
  port: int = Field(default=16060, ge=1, le=65535)
  status_frequency_hz: float = Field(
    default=0.5,
    gt=0,
    description="health endpoint push rate in Hz (0.5 = one frame every 2s)",
  )


class GatewayConfig(BaseModel):
  """One native EOS gRPC listener, reached over TCP or a private UDS."""

  model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

  transport: Literal["tcp", "uds"] = "tcp"
  host: str | None = None
  grpc_port: int = Field(
    default=16200,
    ge=1,
    le=65535,
    validation_alias=AliasChoices("grpc_port", "port"),
  )
  path: str | None = None

  @model_validator(mode="before")
  @classmethod
  def _promote_legacy_port(cls, value: Any) -> Any:
    if isinstance(value, dict) and "port" in value:
      promoted = dict(value)
      promoted.setdefault("grpc_port", promoted.pop("port"))
      return promoted
    return value

  @model_validator(mode="after")
  def _valid_endpoint(self) -> GatewayConfig:
    if self.transport == "tcp":
      if not self.host:
        raise ValueError("TCP gateway requires a non-empty host")
      if self.path is not None:
        raise ValueError("TCP gateway cannot set a UDS path")
    else:
      if self.host is not None:
        raise ValueError("UDS gateway cannot set a TCP host")
      if (
        self.path is None
        or not Path(self.path).is_absolute()
        or ".." in Path(self.path).parts
      ):
        raise ValueError("UDS gateway path must be an absolute path without '..'")
    return self

  @property
  def port(self) -> int:
    """Compatibility accessor for internal callers migrating to grpc_port."""
    return self.grpc_port

  @property
  def endpoint(self) -> str:
    if self.transport == "uds":
      return f"unix://{self.path}"
    return f"{self.host}:{self.grpc_port}"


class AdmissionConfig(BaseModel):
  """EGIA-side ingest and queue admission rates."""

  parts_poll_frequency_hz: float = Field(
    default=10.0, gt=0, le=120, description="per-part GET poll fallback rate in Hz"
  )


class PrimitivesConfig(BaseModel):
  """Planner visibility policy for discovered EOS primitives."""

  model_config = ConfigDict(extra="forbid")

  expose_operator: bool = False


class LogSinkConfig(BaseModel):
  """One logging sink (``[logging.<sink>]`` in ``etc/egia/egia.toml``)."""

  model_config = ConfigDict(extra="forbid")

  enabled: bool = True
  level: Literal["DEBUG", "INFO", "WARNING", "ERROR"] = "INFO"
  overwrite: bool = False


class LoggingConfig(BaseModel):
  """Logging sinks and verbosity (``[logging]`` in ``etc/egia/egia.toml``)."""

  model_config = ConfigDict(extra="forbid")

  level: Literal["DEBUG", "INFO", "WARNING", "ERROR"] = "INFO"
  console: LogSinkConfig = Field(default_factory=LogSinkConfig)
  replay: LogSinkConfig = Field(default_factory=lambda: LogSinkConfig(level="DEBUG"))
  files: LogSinkConfig = Field(default_factory=LogSinkConfig)
  dedup: bool = Field(
    default=True,
    description="collapse identical repeated events in file sinks into counted lines",
  )
  dedup_window_s: float = Field(
    default=30.0,
    gt=0,
    description="identical event payloads inside this window are suppressed; "
    "the first occurrence after the window carries repeats=N",
  )
  dedup_capacity: int = Field(
    default=4096,
    ge=1,
    le=65_536,
    description="maximum distinct event identities retained by each sink",
  )
  replay_max_bytes: int = Field(
    default=32 * 1024 * 1024,
    ge=1024,
    description="JSONL replay rotation threshold in bytes",
  )
  replay_backup_count: int = Field(
    default=5,
    ge=1,
    le=100,
    description="number of rotated JSONL replay files retained",
  )

  @model_validator(mode="after")
  def _diagnostic_files_are_required(self) -> LoggingConfig:
    if not self.files.enabled:
      raise ValueError(
        "logging.files.enabled must be true so terminal events and errors "
        "also have a diagnostic log"
      )
    levels = {"DEBUG": 10, "INFO": 20, "WARNING": 30, "ERROR": 40}
    if self.console.enabled and levels[self.files.level] > levels[self.console.level]:
      raise ValueError(
        "logging.files.level must be at least as verbose as logging.console.level"
      )
    return self


class ModelSelection(BaseModel):
  """Which ``models.toml`` binding to use (``[model]`` in ``etc/egia/egia.toml``)."""

  name: str = "local"


class PerceptionConfig(BaseModel):
  """EGIA-side perception settings (``[perception]`` in ``etc/egia/egia.toml``).

  The table is flat on the wire: ``capacity``/``history_per_kind`` are the
  admission bounds (the former ``[queue]`` section).
  """

  model_config = ConfigDict(extra="forbid")

  capacity: int = Field(default=64, ge=1, le=10_000, description="admission FIFO bound")
  history_per_kind: int = Field(
    default=8,
    ge=1,
    le=10_000,
    description="per-kind history depth for the perception-history prompt",
  )


class PriorityConfig(BaseModel):
  """Mission-table scoring weights (``[decision.priority]``, §3.9).

  ``score = w_urgency·urgency + w_value·value − w_cost·est_cost_s
  + w_age·min(age_s/age_tau_s, 1)``. Origin is a class, not a term —
  operator missions always outrank self-proposed ones regardless of score.
  """

  w_urgency: float = Field(default=1.0, ge=0)
  w_value: float = Field(default=1.0, ge=0)
  w_cost: float = Field(default=0.001, ge=0, description="per estimated second")
  w_age: float = Field(default=0.25, ge=0)
  age_tau_s: float = Field(default=300.0, gt=0)
  default_cost_s: float = Field(
    default=60.0, gt=0, description="cost fallback when no similar plan is recalled"
  )
  operator_urgency: float = Field(default=0.8, ge=0, le=1)
  operator_value: float = Field(default=0.8, ge=0, le=1)
  preempt_margin: float = Field(
    default=0.5,
    ge=0,
    description="score margin a same-class pending mission needs to preempt at a step boundary",
  )
  max_preemptions: int = Field(
    default=2,
    ge=0,
    description="per-mission cap; beyond it the mission fails rather than ping-pong",
  )


class ProposerConfig(BaseModel):
  """Autonomous mission origination (``[decision.proposer]``, §3.9)."""

  enabled: bool = Field(
    default=True, description="register the perception-driven mission proposer"
  )
  idle_reflect_interval_s: float = Field(
    default=30.0,
    gt=0,
    description="idle-time reflection period when no mission runs and the table is empty",
  )
  min_proposal_interval_s: float = Field(
    default=5.0, ge=0, description="debounce between proposal passes; 0 = none"
  )
  max_self_pending: int = Field(
    default=4, ge=0, description="pending self-originated missions allowed at once"
  )
  proposal_token_budget: int = Field(
    default=0,
    ge=0,
    description="invocation-policy budget for proposer model calls; 0 = unlimited (§3.8)",
  )
  proposal_max_tokens: int = Field(
    default=4096,
    ge=64,
    description=(
      "completion bound for one proposal reply; reasoning models spend tokens"
      " on chain-of-thought before the JSON, so keep this generous"
    ),
  )


class CoordinationConfig(BaseModel):
  """Primitive-market coordination (``[decision.coordination]`` in egia.toml).

  Off by default: the serial dual-model planner path is the mission contract.
  Enabled, the coordinator replaces the decision subsystem — missions
  decompose into stored profiles (§3.3), ready steps are auctioned to
  (backend, part) bidders (§3.5), and each executor serializes its own
  calls through the safety gate — one executing step per executor, many
  executors in parallel.
  """

  enabled: bool = Field(
    default=False, description="replace the serial planner with the §3.5 market"
  )
  epsilon: float = Field(
    default=0.25,
    gt=0,
    description="auction epsilon — the optimality/latency dial (§3.5)",
  )
  congestion: float = Field(
    default=0.1,
    ge=0,
    description="lambda: price per second of a bidder's committed load",
  )
  board_capacity: int = Field(
    default=8, ge=1, le=1024, description="concurrent job bound on the task board"
  )
  rediscovery_interval_s: float = Field(
    default=30.0,
    ge=0,
    description=(
      "fleet rediscovery cadence: refresh each backend's primitive catalog "
      "and fold the result into the live market — a robot that "
      "was down at launch, rebooted with new parts, or disappeared is "
      "reflected without a restart; 0 keeps launch-time discovery only"
    ),
  )


class DecisionSentinelConfig(BaseModel):
  """Lightweight model watching perception while the planner is thinking."""

  enabled: bool = True
  model: str | None = Field(
    default=None,
    description="model binding; null reuses the selected decision binding",
  )
  max_tokens: int = Field(default=64, ge=8, le=512)
  retry_s: float = Field(default=1.0, gt=0)


class DecisionControllerConfig(BaseModel):
  """Fast non-reasoning controller lane."""

  model_config = ConfigDict(extra="forbid")

  model: str = Field(
    default="gemma4-fast", min_length=1, description="fast model binding name"
  )
  max_tokens: int = Field(default=512, ge=32, le=8192)
  reasoning_effort: Literal["none"] = "none"


class DecisionReasonerConfig(BaseModel):
  """Slow semantic-planning lane invoked only by the fast controller."""

  model_config = ConfigDict(extra="forbid")

  model: str = Field(
    default="gemma4-reasoning", min_length=1, description="reasoning model binding name"
  )
  max_tokens: int = Field(default=8192, ge=128)
  reasoning_effort: Literal["low", "medium", "high"] = "high"


class DecisionConfig(BaseModel):
  """Mission planner bounds (``[decision]`` in ``etc/egia/egia.toml``).

  The shipped runtime is continuous: step and token bounds default to
  unlimited, while consecutive invalid model replies are bounded and transport
  failures retry with capped backoff. Optional positive execution bounds are
  operator policy, not safety-gate checks; gate thresholds live in
  ``etc/egia/safety/``.
  """

  model_config = ConfigDict(extra="forbid")

  max_steps: int = Field(
    default=0,
    ge=0,
    description="hard mission-step bound; 0 = unlimited",
  )
  max_invalid_replies: int = Field(
    default=3,
    ge=1,
    description="consecutive invalid controller completions before mission failure",
  )
  model_retry_initial_s: float = Field(
    default=1.0,
    gt=0,
    description=(
      "first delay after a model transport failure; shutdown and priority "
      "interrupts cancel the retry wait"
    ),
  )
  model_retry_max_s: float = Field(
    default=30.0,
    gt=0,
    description="maximum exponential-backoff delay for model transport retries",
  )
  temperature: float = Field(default=0.2, ge=0, le=2)
  step_timeout_s: float = Field(
    default=30.0, gt=0, description="per-step egress timeout enforced by the planner"
  )
  mission_capacity: int = Field(
    default=8, ge=1, le=1024, description="pending mission queue bound"
  )
  mission_preload: bool = Field(
    default=False,
    description="restore persisted non-terminal missions when a fresh process launches",
  )
  load_mission_templates: bool = Field(
    default=False,
    description="load model-selectable canonical Missions from missions.toml",
  )
  recall_limit: int = Field(
    default=3,
    ge=0,
    le=16,
    description="prior plans recalled into the prompt before decomposition; 0 disables",
  )
  mission_token_budget: int = Field(
    default=0,
    ge=0,
    description=(
      "invocation-policy budget: total model tokens per mission before further"
      " calls are denied; 0 = unlimited (docs/egia/egia-2607.md §3.8)"
    ),
  )
  min_call_interval_s: float = Field(
    default=0.0,
    ge=0,
    description=(
      "invocation-policy cooldown between ordinary model calls; fault interrupts"
      " bypass it; 0 = no cooldown (docs/egia/egia-2607.md §3.8)"
    ),
  )
  controller: DecisionControllerConfig = Field(default_factory=DecisionControllerConfig)
  reasoner: DecisionReasonerConfig = Field(default_factory=DecisionReasonerConfig)
  sentinel: DecisionSentinelConfig = Field(default_factory=DecisionSentinelConfig)
  priority: PriorityConfig = Field(default_factory=PriorityConfig)
  proposer: ProposerConfig = Field(default_factory=ProposerConfig)
  coordination: CoordinationConfig = Field(default_factory=CoordinationConfig)

  @model_validator(mode="after")
  def _distinct_cognition_bindings(self) -> DecisionConfig:
    if self.controller.model == self.reasoner.model:
      raise ValueError("controller and reasoner must use distinct model bindings")
    return self


class SafetyRateConfig(BaseModel):
  """Egress rate budget (``[rate]`` in ``etc/egia/safety/gate.toml``).

  The budget itself is owned and maintained by the monitor (its
  ``RateWindow``); the composition root passes this value in.
  """

  max_calls_per_minute: int = Field(default=60, ge=1)


class SafetyConfig(BaseModel):
  """Mission-level gate and audit thresholds (``etc/egia/safety/gate.toml``)."""

  rate: SafetyRateConfig = Field(default_factory=SafetyRateConfig)
  audit_interval_s: float = Field(
    default=5.0, gt=0, description="background audit-loop tick period"
  )
  fault_estop_ticks: int = Field(
    default=3,
    ge=1,
    description="consecutive audit ticks holding an unrecovered fault before estop",
  )


class MemoryConfig(BaseModel):
  """Vector recall and docs-corpus settings (``[memory]`` in ``etc/egia/egia.toml``).

  Both recall paths ride the sqlite-vec index in ``opt/memory``; when the
  package is absent the composition root passes no index and recall degrades
  to keyword search without breaking startup.
  """

  vector_recall: bool = Field(
    default=True,
    description="build the sqlite-vec recall index at launch when available",
  )
  docs_corpus: str = Field(
    default="",
    description=(
      "directory of plain .md/.txt files indexed for docs RAG "
      "(relative to the resolved docs/egia root, so '.' selects all EGIA "
      "documentation, or absolute); empty disables docs retrieval"
    ),
  )


class LaunchPolicyConfig(BaseModel):
  """Top-level process and backend activation switches (``[launch]``)."""

  backend: bool = True
  memory: bool = True
  sandbox: bool = True


class SandboxConfig(BaseModel):
  """Reserved EGIA-side sandbox settings (``[sandbox]``)."""


class SandboxDeviceConfig(BaseModel):
  enabled: bool = True
  device: str = "default"


class SandboxMicConfig(SandboxDeviceConfig):
  backend: Literal["auto", "pipewire", "pulseaudio", "alsa"] = "auto"
  sample_rate_hz: int = Field(default=16000, gt=0, le=192000)
  channels: Literal[1] = 1
  capture_duration_s: float = Field(default=5.0, gt=0, le=3600)
  snapshot_window_ms: int = Field(default=1000, ge=100, le=10000)


class SandboxCameraConfig(SandboxDeviceConfig):
  backend: Literal["auto", "gstreamer"] = "auto"
  width_px: int = Field(default=640, gt=0, le=7680)
  height_px: int = Field(default=480, gt=0, le=4320)
  jpeg_quality: int = Field(default=85, ge=1, le=100)
  max_frames: int = Field(default=8, ge=1, le=64)
  min_interval_ms: int = Field(default=100, ge=0, le=60000)


class SandboxSpeakerConfig(SandboxDeviceConfig):
  backend: Literal["auto", "pipewire", "pulseaudio", "alsa"] = "auto"
  max_wav_bytes: int = Field(default=32 * 1024 * 1024, gt=0)
  max_play_s: float = Field(default=300.0, gt=0, le=3600)


class SandboxGatewayConfig(BaseModel):
  host: str = "127.0.0.1"
  grpc_port: int = Field(default=16022, ge=1, le=65535)


class SandboxWebSocketConfig(BaseModel):
  """Sandbox media bounds for the route served by EGIA northbound."""

  host: str | None = "127.0.0.1"
  port: int | None = Field(default=None, ge=1, le=65535)
  max_message_bytes: int = Field(default=2 * 1024 * 1024, gt=0)


class SandboxRuntimeConfig(BaseModel):
  """Out-of-process playground settings from ``opt/sandbox.toml``."""

  gateway: SandboxGatewayConfig = Field(default_factory=SandboxGatewayConfig)
  websocket: SandboxWebSocketConfig = Field(default_factory=SandboxWebSocketConfig)
  fallback_timeout_s: float = Field(default=5.0, ge=0)
  backend_retry_interval_s: float = Field(default=5.0, gt=0)
  mic: SandboxMicConfig = Field(default_factory=SandboxMicConfig)
  camera: SandboxCameraConfig = Field(default_factory=SandboxCameraConfig)
  speaker: SandboxSpeakerConfig = Field(default_factory=SandboxSpeakerConfig)

  @model_validator(mode="after")
  def _loopback_only(self) -> SandboxRuntimeConfig:
    loopback = {"127.0.0.1", "localhost"}
    if self.gateway.host not in loopback:
      raise ValueError("sandbox listener must bind to a loopback host")
    return self


class MonitorConfig(BaseModel):
  """Execution-monitoring bounds (``[monitor]`` in ``etc/egia/egia.toml``)."""

  heartbeat_interval_s: float = Field(default=5.0, gt=0)
  failure_threshold: int = Field(
    default=3, ge=1, description="consecutive heartbeat failures before escalation"
  )
  stall_timeout_s: float = Field(
    default=60.0,
    gt=0,
    description="running mission with no fresh primitive result this long is a deviation",
  )
  v_max_m_s: float = Field(
    default=2.0,
    gt=0,
    description="hardcoded base planar-velocity bound, checked every frame",
  )
  omega_max_rad_s: float = Field(
    default=3.14159,
    gt=0,
    description="hardcoded base yaw-rate bound, checked every frame",
  )
  bound_escalation_ticks: int = Field(
    default=5,
    ge=1,
    description="consecutive bound-violation frames before the estop latch engages",
  )


class PromptSectionConfig(BaseModel):
  """One prompt slot mapped to an observation kind.

  ``priority`` orders survival under token-budget pressure: higher values are
  kept longer; equal values drop in config order (earliest first). Runtime
  change frequency independently orders stable dynamic content before volatile
  content so priority does not destabilize the cacheable prefix.
  """

  kind: str
  dynamic: Literal[True] = True
  format: str = Field(description="render template; must contain {data}")
  empty: str = "no data received"
  omitted: str = Field(
    default="[{kind} omitted: token budget]",
    description="fallback line rendered when budget truncation drops this section",
  )
  priority: int = Field(
    default=0,
    description="higher values survive token-budget truncation longer",
  )
  enabled: bool = True

  @model_validator(mode="after")
  def _format_contains_data(self) -> PromptSectionConfig:
    if "{data}" not in self.format:
      raise ValueError(f"prompt section {self.kind!r}: format must contain {{data}}")
    return self

  @model_validator(mode="after")
  def _omitted_renders(self) -> PromptSectionConfig:
    try:
      self.omitted.format(kind=self.kind)
    except (KeyError, IndexError, ValueError) as exc:
      raise ValueError(
        f"prompt section {self.kind!r}: omitted template is invalid: {exc}"
      ) from exc
    return self


class PromptInstructionSection(BaseModel):
  """Named section of the system prompt source file."""

  name: str = Field(min_length=1)
  text: str = Field(min_length=1)
  dynamic: Literal[False] = False
  priority: int = 0


class MissionTemplateStepConfig(BaseModel):
  """One protected semantic outcome in a configured Mission template."""

  model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

  id: str = Field(min_length=1, pattern=r"^[a-z][a-z0-9-]*$")
  outcome: str = Field(min_length=1)
  required: bool = True
  after: list[str] = Field(default_factory=list)
  terminal: bool = False

  @model_validator(mode="after")
  def _valid_dependencies(self) -> MissionTemplateStepConfig:
    if self.id in self.after:
      raise ValueError(f"template step {self.id!r} cannot depend on itself")
    if len(self.after) != len(set(self.after)):
      raise ValueError(f"template step {self.id!r} has duplicate dependencies")
    if self.terminal and not self.required:
      raise ValueError(f"terminal template step {self.id!r} must be required")
    return self


class MissionTemplateConfig(BaseModel):
  """One named canonical Mission and its protected initial semantic plan."""

  model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

  id: str = Field(min_length=1, pattern=r"^[a-z][a-z0-9_]*$")
  version: int = Field(ge=1)
  description: str = Field(min_length=1)
  mission: str = Field(min_length=1)
  steps: list[MissionTemplateStepConfig] = Field(min_length=1)

  @model_validator(mode="after")
  def _valid_plan(self) -> MissionTemplateConfig:
    step_ids = [step.id for step in self.steps]
    if len(step_ids) != len(set(step_ids)):
      raise ValueError(f"Mission template {self.id!r} has duplicate step IDs")
    known = set(step_ids)
    for step in self.steps:
      unknown = sorted(set(step.after) - known)
      if unknown:
        raise ValueError(
          f"Mission template {self.id!r} step {step.id!r} has unknown "
          f"dependencies: {', '.join(unknown)}"
        )

    by_id = {step.id: step for step in self.steps}
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(step_id: str) -> None:
      if step_id in visiting:
        raise ValueError(f"Mission template {self.id!r} contains a dependency cycle")
      if step_id in visited:
        return
      visiting.add(step_id)
      for dependency in by_id[step_id].after:
        visit(dependency)
      visiting.remove(step_id)
      visited.add(step_id)

    for step_id in step_ids:
      visit(step_id)
    return self


class MissionTemplateCatalog(BaseModel):
  """Strict configured whitelist of model-selectable Mission templates."""

  model_config = ConfigDict(extra="forbid")

  templates: list[MissionTemplateConfig] = Field(default_factory=list)

  @model_validator(mode="after")
  def _unique_templates(self) -> MissionTemplateCatalog:
    ids = [template.id for template in self.templates]
    if len(ids) != len(set(ids)):
      raise ValueError("Mission template IDs must be unique")
    return self

  def resolve(self, template_id: str) -> MissionTemplateConfig | None:
    """Return one configured template by exact ID."""
    return next(
      (template for template in self.templates if template.id == template_id), None
    )


class AgentPromptConfig(BaseModel):
  """Static instructions and lane-specific dynamic sections for one model lane."""

  sections: list[PromptInstructionSection] = Field(default_factory=list)
  dynamic_sections: list[PromptSectionConfig] = Field(default_factory=list)

  @model_validator(mode="after")
  def _valid_sections(self) -> AgentPromptConfig:
    names = [section.name for section in self.sections]
    if not names or len(names) != len(set(names)):
      raise ValueError("agent prompt sections must be non-empty and unique")
    dynamic_names = [section.kind for section in self.dynamic_sections]
    if len(dynamic_names) != len(set(dynamic_names)):
      raise ValueError("agent dynamic prompt sections must be unique")
    return self

  @property
  def instruction(self) -> str:
    """Render static sections from low to high priority."""
    ordered = sorted(
      enumerate(self.sections), key=lambda item: (item[1].priority, item[0])
    )
    return "\n\n".join(
      f"{section.name.replace('_', ' ').title()}:\n{section.text.strip()}"
      for _, section in ordered
    )


class PromptConfig(BaseModel):
  """All Agent-owned model prompts from ``etc/egia/prompts.toml``."""

  version: str = "4"
  token_budget: int = Field(default=32768, ge=256)
  context: list[PromptSectionConfig]
  planner: AgentPromptConfig
  reasoner: AgentPromptConfig
  summarizer: AgentPromptConfig
  proposer: AgentPromptConfig
  sentinel: AgentPromptConfig
  profile_writer: AgentPromptConfig

  @model_validator(mode="after")
  def _unique_sections(self) -> PromptConfig:
    dynamic = [section.kind for section in self.context] + [
      section.kind
      for lane in (
        self.planner,
        self.reasoner,
        self.summarizer,
        self.proposer,
        self.sentinel,
        self.profile_writer,
      )
      for section in lane.dynamic_sections
    ]
    if not dynamic or len(dynamic) != len(set(dynamic)):
      raise ValueError("dynamic prompt section names must be globally unique")
    static = [
      f"{lane}.{section.name}"
      for lane in (
        "planner",
        "reasoner",
        "summarizer",
        "proposer",
        "sentinel",
        "profile_writer",
      )
      for section in getattr(self, lane).sections
    ]
    if len(static) != len(set(static)):
      raise ValueError("static prompt section names must be unique per lane")
    return self


class ModelConfig(BaseModel):
  """Decision-model binding (``[models.<name>]`` in ``etc/egia/models.toml``)."""

  endpoint: str
  model: str
  api_key_env: str = "EGIA_MODEL_API_KEY"
  timeout_seconds: float = Field(default=60.0, gt=0)
  context_window_tokens: int = Field(
    default=262144,
    ge=4096,
    description="maximum input plus output tokens accepted by the model",
  )
  max_tokens: int = Field(default=2048, ge=1)
  send_max_tokens: bool = Field(
    default=True,
    description="include max_tokens in completion requests",
  )
  temperature: float | None = Field(
    default=None,
    ge=0,
    le=2,
    description=(
      "pin the request temperature when the endpoint only accepts one value"
      " (reasoning models such as kimi-k2.6 allow only 1); overrides caller"
    ),
  )
  send_temperature: bool = Field(
    default=True,
    description="include temperature in completion requests",
  )
  reasoning_effort: Literal["none", "low", "medium", "high"] | None = Field(
    default=None,
    description=(
      "provider-neutral reasoning mode; clients map it to the endpoint's wire format"
    ),
  )
  reasoning_options: list[Literal["none", "low", "medium", "high"]] = Field(
    default_factory=list,
    description="reasoning modes this binding permits the planner to select",
  )

  @model_validator(mode="after")
  def _completion_fits_context_window(self) -> ModelConfig:
    if self.max_tokens >= self.context_window_tokens:
      raise ValueError("model max_tokens must be smaller than context_window_tokens")
    return self


class BackendEndpointConfig(BaseModel):
  """One configured EOS endpoint with a stable EGIA robot alias."""

  model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

  id: str = Field(min_length=1)
  gateway: GatewayConfig
  type: str | None = None
  name: str | None = None
  intro: str | None = None


class ModeConfig(BaseModel):
  """One resolved ``[modes.<name>]`` preset from ``etc/egia/modes.toml``.

  The backend-facing half of the configuration: external EOS gRPC endpoint,
  ingest rates, operator policy, and optional fleet endpoint aliases.
  """

  mode: str
  gateway: GatewayConfig
  admission: AdmissionConfig
  primitives: PrimitivesConfig = Field(default_factory=PrimitivesConfig)
  backends: list[BackendEndpointConfig] = Field(default_factory=list)


class LaunchConfig(BaseModel):
  """Resolved launch configuration for one mode preset."""

  mode: str
  launch: LaunchPolicyConfig = Field(default_factory=LaunchPolicyConfig)
  server: ServerConfig
  gateway: GatewayConfig
  admission: AdmissionConfig = Field(default_factory=AdmissionConfig)
  primitives: PrimitivesConfig = Field(default_factory=PrimitivesConfig)
  logging: LoggingConfig = LoggingConfig()
  decision: DecisionConfig = Field(default_factory=DecisionConfig)
  memory: MemoryConfig = Field(default_factory=MemoryConfig)
  sandbox: SandboxConfig = Field(default_factory=SandboxConfig)
  monitor: MonitorConfig = Field(default_factory=MonitorConfig)
  perception: PerceptionConfig = Field(default_factory=PerceptionConfig)
  prompts: PromptConfig
  missions: MissionTemplateCatalog = Field(default_factory=MissionTemplateCatalog)
  backends: list[BackendEndpointConfig] = Field(default_factory=list)

  def endpoints(self) -> list[BackendEndpointConfig]:
    """Resolve the backend endpoint list the pipeline wires one client for.

    An empty list synthesizes the default alias for the mode-level endpoint.
    """
    if not self.backends:
      return [BackendEndpointConfig(id="default", gateway=self.gateway)]
    return list(self.backends)


class ConfigError(Exception):
  """Raised when the config root or a required binding cannot be resolved."""


def _load_toml(path: Path) -> dict[str, object]:
  with path.open("rb") as fh:
    return tomllib.load(fh)


def _deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
  """Recursively overlay dictionaries while replacing non-dictionary values."""
  merged = dict(base)
  for key, value in override.items():
    if isinstance(value, dict) and isinstance(merged.get(key), dict):
      merged[key] = _deep_merge(merged[key], value)
    else:
      merged[key] = value
  return merged


def _table(entry: dict[str, object], key: str) -> dict[str, object]:
  raw = entry.get(key, {})
  return raw if isinstance(raw, dict) else {}


def resolve_etc_root(etc: str | None, *, layout: Layout | None = None) -> Path:
  """Resolve EGIA config without depending on the current working directory.

  An explicit component root wins, followed by ``EI_ETC`` and the legacy
  ``EGIA_ETC`` override. A relative value is interpreted below the checkout
  prefix; otherwise the shared Island layout selects ``etc/egia``.

  Args:
    etc: Explicit EGIA component-config root, if provided.
    layout: Already-resolved product layout from the composition root. Absolute
      component roots do not require it; compatibility defaults do.
  """
  selected = etc or os.environ.get("EI_ETC") or os.environ.get("EGIA_ETC")
  if selected is None:
    resolved_layout = layout or Layout.resolve()
    root = resolved_layout.component_etc("egia")
  else:
    root = Path(selected)
    if not root.is_absolute():
      resolved_layout = layout or Layout.resolve()
      root = resolved_layout.prefix / root
  root = root.expanduser().resolve()
  if not root.is_dir():
    raise ConfigError(f"config root not found: {root}")
  return root


def _defaults_root(defaults: Path | str | None) -> Path:
  """Resolve the immutable EGIA factory-config root."""
  if defaults is None:
    root = Layout.resolve().egia_defaults
  else:
    root = Path(defaults).expanduser()
    if not root.is_absolute():
      raise ConfigError(f"factory config root must be absolute: {defaults}")
  root = root.resolve()
  if not root.is_dir():
    raise ConfigError(f"factory config root not found: {root}")
  return root


def _config_doc(
  root: Path, relative: str | Path, defaults: Path | str | None
) -> dict[str, Any]:
  """Merge one factory TOML document with its deployment overlay."""
  factory_file = _defaults_root(defaults) / relative
  overlay_file = root / relative
  base = _load_toml(factory_file) if factory_file.is_file() else {}
  overlay = _load_toml(overlay_file) if overlay_file.is_file() else {}
  return _deep_merge(base, overlay)


def _profile_file(root: Path, mode: str) -> Path | None:
  """Return a mode profile, preferring an un-suffixed deployment file."""
  for name in (f"{mode}.toml", f"{mode}.example.toml"):
    candidate = root / "profiles" / name
    if candidate.is_file():
      return candidate
  return None


def load_prompt_config(
  etc: str | Path | None = None,
) -> PromptConfig:
  """Load the single Agent-owned prompt document from ``etc/egia``."""
  root = resolve_etc_root(str(etc) if isinstance(etc, Path) else etc)
  prompt_file = root / "prompts.toml"
  if not prompt_file.is_file():
    raise ConfigError(f"prompt config not found: {prompt_file}")
  return PromptConfig.model_validate(_load_toml(prompt_file))


def load_mission_templates(
  etc: str | Path | None = None,
) -> MissionTemplateCatalog:
  """Load the optional strict Mission-template catalog from ``etc/egia``."""
  root = resolve_etc_root(str(etc) if isinstance(etc, Path) else etc)
  template_file = root / "missions.toml"
  if not template_file.is_file():
    return MissionTemplateCatalog()
  try:
    return MissionTemplateCatalog.model_validate(_load_toml(template_file))
  except ValueError as exc:
    raise ConfigError(
      f"invalid Mission template config {template_file}: {exc}"
    ) from exc


def load_mode_config(
  etc: str | Path | None = None,
  mode: str | None = None,
  *,
  defaults: Path | str | None = None,
) -> ModeConfig:
  """Resolve one launch-mode preset.

  ``etc/egia/modes.toml`` holds only the ``default`` pointer; the mode tables live
  one per file under ``etc/egia/profiles/`` (``<mode>.toml``, falling back to the
  committed ``<mode>.example.toml`` template when the real file is absent —
  deployment overrides are the un-suffixed names). An inline ``[modes]``
  table in ``modes.toml`` is still honored when no profile file exists.

  Selection order is ``mode``, then ``EGIA_MODE``, then ``default``.

  Raises:
    ConfigError: If the config root, ``modes.toml``, the named mode, or its
      ``[gateway]`` table is missing.
  """
  root = resolve_etc_root(str(etc) if isinstance(etc, Path) else etc)
  factory_root = _defaults_root(defaults)
  modes_doc = _config_doc(root, "modes.toml", factory_root)
  if not modes_doc:
    raise ConfigError(
      f"modes.toml not found in {root} or factory config {factory_root}"
    )
  selected = mode or os.environ.get("EGIA_MODE") or str(modes_doc.get("default", "sim"))
  factory_profile = _profile_file(factory_root, selected)
  overlay_profile = _profile_file(root, selected)
  if factory_profile is not None or overlay_profile is not None:
    profile_doc = _deep_merge(
      _load_toml(factory_profile) if factory_profile is not None else {},
      _load_toml(overlay_profile) if overlay_profile is not None else {},
    )
    modes = profile_doc.get("modes", {})
    if not isinstance(modes, dict):
      raise ConfigError(f"profile for {selected!r} [modes] must be a table")
    mode_entry = modes.get(selected)
    if not isinstance(mode_entry, dict):
      raise ConfigError(f"profile for {selected!r} has no [modes.{selected}] table")
  else:
    modes = modes_doc.get("modes", {})
    if not isinstance(modes, dict):
      raise ConfigError("modes.toml [modes] must be a table")
    mode_entry = modes.get(selected)
    if not isinstance(mode_entry, dict):
      raise ConfigError(
        f"unknown launch mode {selected!r}: no profiles/{selected}.toml, "
        f"no {selected}.example.toml, no [modes.{selected}] in modes.toml"
      )

  removed = sorted({"tools", "capabilities"}.intersection(mode_entry))
  if removed:
    raise ConfigError(
      f"mode {selected!r} uses removed configuration: {', '.join(removed)}; "
      "configure [primitives] instead"
    )

  gateway_table = _table(mode_entry, "gateway")
  if not gateway_table:
    raise ConfigError(f"mode {selected!r} missing [gateway] table")

  backends = _parse_backends(mode_entry, selected)

  return ModeConfig(
    mode=selected,
    gateway=GatewayConfig.model_validate(gateway_table),
    admission=AdmissionConfig.model_validate(_table(mode_entry, "admission")),
    primitives=PrimitivesConfig.model_validate(_table(mode_entry, "primitives")),
    backends=backends,
  )


def _parse_backends(
  mode_entry: dict[str, object],
  mode: str,
) -> list[BackendEndpointConfig]:
  """Validate the optional external EOS endpoint list."""
  raw = mode_entry.get("backends", [])
  if not isinstance(raw, list):
    raise ConfigError(
      f"mode {mode!r}: [[modes.{mode}.backends]] must be a list of tables"
    )
  endpoints: list[BackendEndpointConfig] = []
  seen: set[str] = set()
  seen_gateways: set[str] = set()
  for entry in raw:
    if not isinstance(entry, dict):
      raise ConfigError(
        f"mode {mode!r}: each [[modes.{mode}.backends]] entry must be a table"
      )
    try:
      endpoint = BackendEndpointConfig.model_validate(entry)
    except ValueError as exc:
      raise ConfigError(f"mode {mode!r}: invalid backends entry: {exc}") from exc
    if endpoint.id in seen:
      raise ConfigError(f"mode {mode!r}: duplicate backend id {endpoint.id!r}")
    if endpoint.gateway.endpoint in seen_gateways:
      raise ConfigError(
        f"mode {mode!r}: duplicate backend gateway {endpoint.gateway.endpoint!r}"
      )
    seen.add(endpoint.id)
    seen_gateways.add(endpoint.gateway.endpoint)
    endpoints.append(endpoint)
  return endpoints


def load_launch_config(
  etc: str | None = None,
  mode: str | None = None,
  *,
  defaults: Path | str | None = None,
) -> LaunchConfig:
  """Load factory launch config with the resolved ``etc`` overlay applied."""
  root = resolve_etc_root(etc)

  egia_doc = _config_doc(root, "egia.toml", defaults)
  removed = sorted({"tools", "capabilities"}.intersection(egia_doc))
  if removed:
    raise ConfigError(
      f"egia.toml uses removed configuration: {', '.join(removed)}; "
      "configure primitives in the selected mode profile"
    )
  for section in ("memory", "sandbox"):
    section_doc = egia_doc.get(section, {})
    if isinstance(section_doc, dict) and "enabled" in section_doc:
      raise ConfigError(
        f"[{section}].enabled was removed; use [launch] {section} = true|false"
      )
  launch_policy = LaunchPolicyConfig.model_validate(egia_doc.get("launch", {}))
  server = ServerConfig.model_validate(egia_doc.get("server", {}))
  log_cfg = LoggingConfig.model_validate(egia_doc.get("logging", {}))
  decision = DecisionConfig.model_validate(egia_doc.get("decision", {}))
  memory = MemoryConfig.model_validate(egia_doc.get("memory", {}))
  sandbox = SandboxConfig.model_validate(egia_doc.get("sandbox", {}))
  monitor = MonitorConfig.model_validate(egia_doc.get("monitor", {}))
  # The [perception] table is flat: capacity/history_per_kind (the former
  # [queue] section) land directly on PerceptionConfig.
  perception_table = egia_doc.get("perception", {})
  perception_doc: dict[str, object] = (
    dict(perception_table) if isinstance(perception_table, dict) else {}
  )
  perception = PerceptionConfig.model_validate(perception_doc)
  models_doc = _config_doc(root, "models.toml", defaults)
  if "speech" in egia_doc or "speech" in models_doc:
    raise ConfigError("[speech] was removed from EGIA; configure ASR and TTS in EOS")
  raw_bindings = models_doc.get("models", {})
  bindings = (
    {
      name: ModelConfig.model_validate(entry)
      for name, entry in raw_bindings.items()
      if isinstance(entry, dict)
    }
    if isinstance(raw_bindings, dict)
    else {}
  )
  for lane, name, effort in (
    ("controller", decision.controller.model, decision.controller.reasoning_effort),
    ("reasoner", decision.reasoner.model, decision.reasoner.reasoning_effort),
  ):
    binding = bindings.get(name)
    if binding is None:
      raise ConfigError(f"decision {lane} model binding {name!r} is not configured")
    if effort not in binding.reasoning_options:
      raise ConfigError(
        f"decision {lane} binding {name!r} does not advertise reasoning mode {effort!r}"
      )
  prompts = load_prompt_config(root)
  missions = (
    load_mission_templates(root)
    if decision.load_mission_templates
    else MissionTemplateCatalog()
  )

  preset = load_mode_config(root, mode, defaults=defaults)

  return LaunchConfig(
    mode=preset.mode,
    launch=launch_policy,
    server=server,
    gateway=preset.gateway,
    admission=preset.admission,
    primitives=preset.primitives,
    logging=log_cfg,
    decision=decision,
    memory=memory,
    sandbox=sandbox,
    monitor=monitor,
    perception=perception,
    prompts=prompts,
    missions=missions,
    backends=preset.backends,
  )


def load_logging_config(
  etc: str | Path | None = None, *, defaults: Path | str | None = None
) -> LoggingConfig:
  """Load the repository-wide logging policy without composing the pipeline."""
  root = resolve_etc_root(str(etc) if isinstance(etc, Path) else etc)
  document = _config_doc(root, "egia.toml", defaults)
  return LoggingConfig.model_validate(document.get("logging", {}))


def load_sandbox_config(
  etc: str | Path | None = None, *, defaults: Path | str | None = None
) -> SandboxRuntimeConfig:
  """Load the optional playground's independent listener/device settings."""
  root = resolve_etc_root(str(etc) if isinstance(etc, Path) else etc)
  document = _config_doc(root, "opt/sandbox.toml", defaults)
  devices = document.get("devices", {})
  device_doc = dict(devices) if isinstance(devices, dict) else {}
  payload = {
    "gateway": document.get("gateway", {}),
    "websocket": document.get("websocket", {}),
    "fallback_timeout_s": document.get("fallback_timeout_s", 5.0),
    "backend_retry_interval_s": document.get("backend_retry_interval_s", 5.0),
    "mic": device_doc.get("mic", {}),
    "camera": device_doc.get("camera", {}),
    "speaker": device_doc.get("speaker", {}),
  }
  try:
    return SandboxRuntimeConfig.model_validate(payload)
  except ValueError as exc:
    raise ConfigError(f"invalid opt/sandbox.toml: {exc}") from exc


def load_safety_config(
  etc: str | Path | None = None, *, defaults: Path | str | None = None
) -> SafetyConfig:
  """Load cognitive-gate thresholds from ``etc/egia/safety/gate.toml``.

  A missing file yields the defaults rather than aborting launch: the gate
  stays fail-closed on unknown primitives and schema violations regardless.
  """
  root = resolve_etc_root(str(etc) if isinstance(etc, Path) else etc)
  doc = _config_doc(root, "safety/gate.toml", defaults)
  return SafetyConfig.model_validate(doc)


def load_model_bindings(
  etc: str | None = None, *, defaults: Path | str | None = None
) -> dict[str, ModelConfig]:
  """All model bindings in ``etc/egia/models.toml`` keyed by name.

  Model bindings are independent of EOS primitive and skill discovery.
  """
  root = resolve_etc_root(etc)
  factory_root = _defaults_root(defaults)
  doc = _config_doc(root, "models.toml", factory_root)
  models = doc.get("models", {})
  if not isinstance(models, dict):
    raise ConfigError("malformed model bindings in layered models.toml")
  return {
    name: ModelConfig.model_validate(entry)
    for name, entry in models.items()
    if isinstance(entry, dict)
  }


def load_model_selection(
  etc: str | None = None, *, defaults: Path | str | None = None
) -> str | None:
  """The ``[model] name`` default selection from ``etc/egia/egia.toml``, if set."""
  root = resolve_etc_root(etc)
  egia_doc = _config_doc(root, "egia.toml", defaults)
  return ModelSelection.model_validate(egia_doc.get("model", {})).name


def load_model_config(
  etc: str | None = None,
  name: str | None = None,
  *,
  defaults: Path | str | None = None,
) -> ModelConfig:
  """Load the decision-model binding named by ``name`` from ``etc/egia/models.toml``."""
  root = resolve_etc_root(etc)
  selected = (
    name or os.environ.get("EGIA_MODEL") or load_model_selection(etc, defaults=defaults)
  )
  bindings = load_model_bindings(etc, defaults=defaults)
  entry = bindings.get(selected) if selected is not None else None
  if entry is None:
    models_file = root / "models.toml"
    raise ConfigError(f"no model binding named {selected!r} in {models_file}")
  return entry

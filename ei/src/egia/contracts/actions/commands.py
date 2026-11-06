"""Typed argument contracts for the canonical EOS primitive catalog."""

from __future__ import annotations

from typing import ClassVar, Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator

from egia.contracts.source import load_json


class PrimitiveCommand(BaseModel):
  """Base for caller-supplied primitive arguments.

  Command identity belongs to ``CapabilityService.SubmitOperation.operation_id``.
  Supplying it inside ``arguments`` is therefore rejected by the common
  extra-field policy.
  """

  model_config = ConfigDict(extra="forbid")
  schema_path: ClassVar[str]

  @classmethod
  def model_json_schema(cls, *args, **kwargs) -> dict:
    """Return the canonical EOS schema selected explicitly by this model."""
    del args, kwargs
    return load_json(cls.schema_path)


class ArmInitCommand(PrimitiveCommand):
  """Request the deployment-configured initial joint pose for one arm."""

  schema_path = "eos/schemas/eos/actions/arm_init.v1.json"
  type: Literal["arm.init"] = "arm.init"


class BaseVelocityCommand(PrimitiveCommand):
  """Command body-frame base velocity for a bounded lease."""

  schema_path = "eos/schemas/eos/actions/base_velocity.v1.json"
  type: Literal["base.velocity"] = "base.velocity"
  linear_x_m_s: float
  linear_y_m_s: float
  angular_z_rad_s: float
  valid_for_ms: int


class BaseNavigateCommand(PrimitiveCommand):
  """Navigate to a relative or absolute planar goal."""

  schema_path = "eos/schemas/eos/actions/base_navigate.v1.json"
  type: Literal["base.navigate"] = "base.navigate"
  reference: Literal["relative", "absolute"]
  frame_id: str | None = Field(default=None, min_length=1)
  x_m: float
  y_m: float
  yaw_rad: float

  @model_validator(mode="after")
  def _frame_matches_reference(self) -> Self:
    if (self.reference == "absolute") != (self.frame_id is not None):
      raise ValueError("frame_id is required only for an absolute target")
    return self


class BasePostureCommand(PrimitiveCommand):
  """Request one supported high-level base posture."""

  schema_path = "eos/schemas/eos/actions/base_posture.v1.json"
  type: Literal["base.posture"] = "base.posture"
  posture: Literal["stand", "walk", "lie"]


class GimbalMotionCommand(PrimitiveCommand):
  """Move a gimbal using relative or absolute pan and tilt targets."""

  schema_path = "eos/schemas/eos/actions/gimbal_motion.v1.json"
  type: Literal["gimbal.motion"] = "gimbal.motion"
  reference: Literal["relative", "absolute"]
  pan_rad: float
  tilt_rad: float
  speed_rad_s: float | None = None


class ArmMoveJCommand(PrimitiveCommand):
  """Move every configured arm joint by or to an ordered angular target."""

  schema_path = "eos/schemas/eos/actions/arm_movej.v1.json"
  type: Literal["arm.movej"] = "arm.movej"
  reference: Literal["relative", "absolute"]
  joint_positions_rad: list[float] = Field(min_length=1)
  speed_rad_s: float | None = None


class ArmMoveLCommand(PrimitiveCommand):
  """Move an arm tool using a relative transform or absolute pose."""

  schema_path = "eos/schemas/eos/actions/arm_movel.v1.json"
  type: Literal["arm.movel"] = "arm.movel"
  reference: Literal["relative", "absolute"]
  frame_id: str | None = Field(default=None, min_length=1)
  position_m: tuple[float, float, float]
  orientation_quat_xyzw: tuple[float, float, float, float]
  speed_m_s: float | None = None

  @model_validator(mode="after")
  def _frame_matches_reference(self) -> Self:
    if (self.reference == "absolute") != (self.frame_id is not None):
      raise ValueError("frame_id is required only for an absolute target")
    return self


class ArmPickCommand(PrimitiveCommand):
  """Pick one typed non-human object from the current world state."""

  schema_path = "eos/schemas/eos/actions/arm_pick.v1.json"
  type: Literal["arm.pick"] = "arm.pick"
  object_id: str = Field(pattern=r"^obj:[^\s:]+$")


class GripperMotionCommand(PrimitiveCommand):
  """Open or close a gripper with an optional admitted force."""

  schema_path = "eos/schemas/eos/actions/gripper_motion.v1.json"
  type: Literal["gripper.motion"] = "gripper.motion"
  action: Literal["open", "close"]
  force_n: float | None = None


class SpeakerPlayCommand(PrimitiveCommand):
  """Play one EOS-readable audio asset through a speaker."""

  schema_path = "eos/schemas/eos/actions/speaker_play.v1.json"
  type: Literal["speaker.play"] = "speaker.play"
  uri: str = Field(
    min_length=1,
    max_length=2048,
    pattern=r"^[A-Za-z][A-Za-z0-9+.-]*:[^\s]+$",
  )


class SpeakerSpeakCommand(PrimitiveCommand):
  """Synthesize text and play it through the bound speaker."""

  schema_path = "eos/schemas/eos/actions/speaker_speak.v1.json"
  type: Literal["speaker.speak"] = "speaker.speak"
  text: str = Field(min_length=1, max_length=4096)
  language: str | None = Field(default=None, min_length=2, max_length=35)
  voice: str | None = Field(default=None, min_length=1, max_length=128)


class FaceRegisterCommand(PrimitiveCommand):
  """Register a named face using the bound camera's latest valid image."""

  schema_path = "eos/schemas/eos/actions/face_register.v1.json"
  type: Literal["face.register"] = "face.register"
  name: str = Field(min_length=1, max_length=256)

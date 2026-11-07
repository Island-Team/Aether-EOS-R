"""EOS discovery classes, readable resource classes, and primitive names."""

from __future__ import annotations

from enum import StrEnum


class DeviceKind(StrEnum):
  """Canonical class names admitted from EOS device discovery."""

  BASE = "base"
  GIMBAL = "gimbal"
  MANIPULATOR = "manipulator"
  GRIPPER = "gripper"
  CAMERA = "camera"
  MICROPHONE = "microphone"
  SPEAKER = "speaker"


BODY_DEVICE_KINDS: tuple[DeviceKind, ...] = tuple(DeviceKind)


class ResourceKind(StrEnum):
  """Canonical readable EOS resource classes admitted by EGIA."""

  WORLD = "world"
  DETECTION = "detection"
  ASR = "asr"


WORLD_RESOURCE_KINDS: tuple[ResourceKind, ...] = (
  ResourceKind.WORLD,
  ResourceKind.DETECTION,
  ResourceKind.ASR,
)


class PrimitiveKind(StrEnum):
  """Canonical EOS primitive names admitted by EGIA."""

  BASE_VELOCITY = "base.velocity"
  BASE_NAVIGATE = "base.navigate"
  BASE_POSTURE = "base.posture"
  GIMBAL_MOTION = "gimbal.motion"
  ARM_INIT = "arm.init"
  ARM_MOVEJ = "arm.movej"
  ARM_MOVEL = "arm.movel"
  ARM_PICK = "arm.pick"
  GRIPPER_MOTION = "gripper.motion"
  SPEAKER_PLAY = "speaker.play"
  SPEAKER_SPEAK = "speaker.speak"
  FACE_REGISTER = "face.register"
  MOTION_STOP = "motion.stop"
  ROBOT_STOP = "robot.stop"
  SAFETY_ESTOP = "safety.estop"


STANDARD_PRIMITIVE_KINDS: tuple[PrimitiveKind, ...] = (
  PrimitiveKind.BASE_VELOCITY,
  PrimitiveKind.BASE_NAVIGATE,
  PrimitiveKind.BASE_POSTURE,
  PrimitiveKind.GIMBAL_MOTION,
  PrimitiveKind.GRIPPER_MOTION,
  PrimitiveKind.SPEAKER_PLAY,
  PrimitiveKind.ARM_PICK,
  PrimitiveKind.SPEAKER_SPEAK,
  PrimitiveKind.FACE_REGISTER,
)
OPERATOR_PRIMITIVE_KINDS: tuple[PrimitiveKind, ...] = (
  PrimitiveKind.ARM_INIT,
  PrimitiveKind.ARM_MOVEJ,
  PrimitiveKind.ARM_MOVEL,
)
PRIVILEGED_PRIMITIVE_KINDS: tuple[PrimitiveKind, ...] = (
  PrimitiveKind.MOTION_STOP,
  PrimitiveKind.ROBOT_STOP,
  PrimitiveKind.SAFETY_ESTOP,
)
PRIMITIVE_KINDS = (
  *STANDARD_PRIMITIVE_KINDS,
  *OPERATOR_PRIMITIVE_KINDS,
  *PRIVILEGED_PRIMITIVE_KINDS,
)


def resource_kind(resource_class: str | None, uri: str) -> ResourceKind | None:
  """Resolve one EOS resource listing to a supported typed resource."""
  try:
    kind = ResourceKind(resource_class or "")
  except ValueError:
    return None
  expected_suffix = {
    ResourceKind.WORLD: "/world/state",
    ResourceKind.DETECTION: "/detection/state",
    ResourceKind.ASR: "/world/asr/state",
  }[kind]
  if uri.endswith(expected_suffix):
    return kind
  return None

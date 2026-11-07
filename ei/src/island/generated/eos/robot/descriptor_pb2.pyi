# Generated from in-repository share/eos/contracts.
from island.generated.eos.robot import primitive_pb2 as _primitive_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class RobotDescriptor(_message.Message):
    __slots__ = ("id", "name", "model", "state", "health", "estop", "power_pct", "description", "primitives")
    ID_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    MODEL_FIELD_NUMBER: _ClassVar[int]
    STATE_FIELD_NUMBER: _ClassVar[int]
    HEALTH_FIELD_NUMBER: _ClassVar[int]
    ESTOP_FIELD_NUMBER: _ClassVar[int]
    POWER_PCT_FIELD_NUMBER: _ClassVar[int]
    DESCRIPTION_FIELD_NUMBER: _ClassVar[int]
    PRIMITIVES_FIELD_NUMBER: _ClassVar[int]
    id: str
    name: str
    model: str
    state: str
    health: str
    estop: bool
    power_pct: float
    description: str
    primitives: _containers.RepeatedCompositeFieldContainer[_primitive_pb2.PrimitiveBinding]
    def __init__(self, id: _Optional[str] = ..., name: _Optional[str] = ..., model: _Optional[str] = ..., state: _Optional[str] = ..., health: _Optional[str] = ..., estop: _Optional[bool] = ..., power_pct: _Optional[float] = ..., description: _Optional[str] = ..., primitives: _Optional[_Iterable[_Union[_primitive_pb2.PrimitiveBinding, _Mapping]]] = ...) -> None: ...

class DeviceDescriptor(_message.Message):
    __slots__ = ("id", "part", "state", "health", "primitives", "description", "state_uri")
    ID_FIELD_NUMBER: _ClassVar[int]
    CLASS_FIELD_NUMBER: _ClassVar[int]
    PART_FIELD_NUMBER: _ClassVar[int]
    STATE_FIELD_NUMBER: _ClassVar[int]
    HEALTH_FIELD_NUMBER: _ClassVar[int]
    PRIMITIVES_FIELD_NUMBER: _ClassVar[int]
    DESCRIPTION_FIELD_NUMBER: _ClassVar[int]
    STATE_URI_FIELD_NUMBER: _ClassVar[int]
    id: str
    part: str
    state: str
    health: str
    primitives: _containers.RepeatedCompositeFieldContainer[_primitive_pb2.PrimitiveBinding]
    description: str
    state_uri: str
    def __init__(self, id: _Optional[str] = ..., part: _Optional[str] = ..., state: _Optional[str] = ..., health: _Optional[str] = ..., primitives: _Optional[_Iterable[_Union[_primitive_pb2.PrimitiveBinding, _Mapping]]] = ..., description: _Optional[str] = ..., state_uri: _Optional[str] = ..., **kwargs) -> None: ...

class DiscoverResponse(_message.Message):
    __slots__ = ("robot", "devices")
    ROBOT_FIELD_NUMBER: _ClassVar[int]
    DEVICES_FIELD_NUMBER: _ClassVar[int]
    robot: RobotDescriptor
    devices: _containers.RepeatedCompositeFieldContainer[DeviceDescriptor]
    def __init__(self, robot: _Optional[_Union[RobotDescriptor, _Mapping]] = ..., devices: _Optional[_Iterable[_Union[DeviceDescriptor, _Mapping]]] = ...) -> None: ...

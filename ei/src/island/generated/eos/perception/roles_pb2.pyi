# Generated from in-repository share/eos/contracts.
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class PerceptionRole(_message.Message):
    __slots__ = ("role", "model_id", "available", "continuous")
    ROLE_FIELD_NUMBER: _ClassVar[int]
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    AVAILABLE_FIELD_NUMBER: _ClassVar[int]
    CONTINUOUS_FIELD_NUMBER: _ClassVar[int]
    role: str
    model_id: str
    available: bool
    continuous: bool
    def __init__(self, role: _Optional[str] = ..., model_id: _Optional[str] = ..., available: _Optional[bool] = ..., continuous: _Optional[bool] = ...) -> None: ...

class PerceptionRolesResponse(_message.Message):
    __slots__ = ("roles", "camera_device_id")
    ROLES_FIELD_NUMBER: _ClassVar[int]
    CAMERA_DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    roles: _containers.RepeatedCompositeFieldContainer[PerceptionRole]
    camera_device_id: str
    def __init__(self, roles: _Optional[_Iterable[_Union[PerceptionRole, _Mapping]]] = ..., camera_device_id: _Optional[str] = ...) -> None: ...

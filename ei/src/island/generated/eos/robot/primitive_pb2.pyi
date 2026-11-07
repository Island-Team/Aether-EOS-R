# Generated from in-repository share/eos/contracts.
from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class PrimitiveDependency(_message.Message):
    __slots__ = ("name", "health")
    NAME_FIELD_NUMBER: _ClassVar[int]
    HEALTH_FIELD_NUMBER: _ClassVar[int]
    name: str
    health: str
    def __init__(self, name: _Optional[str] = ..., health: _Optional[str] = ...) -> None: ...

class PrimitiveBinding(_message.Message):
    __slots__ = ("name", "version", "kind", "references", "limits", "health", "dependencies", "accepts", "absolute_frame", "description", "parameters")
    NAME_FIELD_NUMBER: _ClassVar[int]
    VERSION_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    REFERENCES_FIELD_NUMBER: _ClassVar[int]
    LIMITS_FIELD_NUMBER: _ClassVar[int]
    HEALTH_FIELD_NUMBER: _ClassVar[int]
    DEPENDENCIES_FIELD_NUMBER: _ClassVar[int]
    ACCEPTS_FIELD_NUMBER: _ClassVar[int]
    ABSOLUTE_FRAME_FIELD_NUMBER: _ClassVar[int]
    DESCRIPTION_FIELD_NUMBER: _ClassVar[int]
    PARAMETERS_FIELD_NUMBER: _ClassVar[int]
    name: str
    version: str
    kind: str
    references: _containers.RepeatedScalarFieldContainer[str]
    limits: _struct_pb2.Struct
    health: str
    dependencies: _containers.RepeatedCompositeFieldContainer[PrimitiveDependency]
    accepts: _containers.RepeatedScalarFieldContainer[str]
    absolute_frame: str
    description: str
    parameters: _struct_pb2.Struct
    def __init__(self, name: _Optional[str] = ..., version: _Optional[str] = ..., kind: _Optional[str] = ..., references: _Optional[_Iterable[str]] = ..., limits: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., health: _Optional[str] = ..., dependencies: _Optional[_Iterable[_Union[PrimitiveDependency, _Mapping]]] = ..., accepts: _Optional[_Iterable[str]] = ..., absolute_frame: _Optional[str] = ..., description: _Optional[str] = ..., parameters: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class CallPrimitiveRequest(_message.Message):
    __slots__ = ("name", "device_id", "arguments", "version", "command_id")
    NAME_FIELD_NUMBER: _ClassVar[int]
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    ARGUMENTS_FIELD_NUMBER: _ClassVar[int]
    VERSION_FIELD_NUMBER: _ClassVar[int]
    COMMAND_ID_FIELD_NUMBER: _ClassVar[int]
    name: str
    device_id: str
    arguments: _struct_pb2.Struct
    version: str
    command_id: str
    def __init__(self, name: _Optional[str] = ..., device_id: _Optional[str] = ..., arguments: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., version: _Optional[str] = ..., command_id: _Optional[str] = ...) -> None: ...

class PrimitiveCommandRequest(_message.Message):
    __slots__ = ("command_id",)
    COMMAND_ID_FIELD_NUMBER: _ClassVar[int]
    command_id: str
    def __init__(self, command_id: _Optional[str] = ...) -> None: ...

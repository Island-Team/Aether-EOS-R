# Generated from in-repository share/eos/contracts.
from google.api import annotations_pb2 as _annotations_pb2
from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class OperationStatus(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    OPERATION_STATUS_UNSPECIFIED: _ClassVar[OperationStatus]
    OPERATION_STATUS_PENDING: _ClassVar[OperationStatus]
    OPERATION_STATUS_SUCCEEDED: _ClassVar[OperationStatus]
    OPERATION_STATUS_REJECTED: _ClassVar[OperationStatus]
    OPERATION_STATUS_FAILED: _ClassVar[OperationStatus]
    OPERATION_STATUS_CANCELLED: _ClassVar[OperationStatus]
    OPERATION_STATUS_TIMED_OUT: _ClassVar[OperationStatus]
    OPERATION_STATUS_OUTCOME_UNKNOWN: _ClassVar[OperationStatus]
OPERATION_STATUS_UNSPECIFIED: OperationStatus
OPERATION_STATUS_PENDING: OperationStatus
OPERATION_STATUS_SUCCEEDED: OperationStatus
OPERATION_STATUS_REJECTED: OperationStatus
OPERATION_STATUS_FAILED: OperationStatus
OPERATION_STATUS_CANCELLED: OperationStatus
OPERATION_STATUS_TIMED_OUT: OperationStatus
OPERATION_STATUS_OUTCOME_UNKNOWN: OperationStatus

class CapabilityDescriptor(_message.Message):
    __slots__ = ("name", "available", "primitive", "version", "device_id")
    NAME_FIELD_NUMBER: _ClassVar[int]
    AVAILABLE_FIELD_NUMBER: _ClassVar[int]
    PRIMITIVE_FIELD_NUMBER: _ClassVar[int]
    VERSION_FIELD_NUMBER: _ClassVar[int]
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    name: str
    available: bool
    primitive: str
    version: str
    device_id: str
    def __init__(self, name: _Optional[str] = ..., available: _Optional[bool] = ..., primitive: _Optional[str] = ..., version: _Optional[str] = ..., device_id: _Optional[str] = ...) -> None: ...

class ListCapabilitiesRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class ListCapabilitiesResponse(_message.Message):
    __slots__ = ("capabilities",)
    CAPABILITIES_FIELD_NUMBER: _ClassVar[int]
    capabilities: _containers.RepeatedCompositeFieldContainer[CapabilityDescriptor]
    def __init__(self, capabilities: _Optional[_Iterable[_Union[CapabilityDescriptor, _Mapping]]] = ...) -> None: ...

class SubmitOperationRequest(_message.Message):
    __slots__ = ("capability_name", "arguments", "operation_id")
    CAPABILITY_NAME_FIELD_NUMBER: _ClassVar[int]
    ARGUMENTS_FIELD_NUMBER: _ClassVar[int]
    OPERATION_ID_FIELD_NUMBER: _ClassVar[int]
    capability_name: str
    arguments: _struct_pb2.Struct
    operation_id: str
    def __init__(self, capability_name: _Optional[str] = ..., arguments: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., operation_id: _Optional[str] = ...) -> None: ...

class OperationRequest(_message.Message):
    __slots__ = ("operation_id",)
    OPERATION_ID_FIELD_NUMBER: _ClassVar[int]
    operation_id: str
    def __init__(self, operation_id: _Optional[str] = ...) -> None: ...

class Operation(_message.Message):
    __slots__ = ("operation_id", "status", "accepted", "reason_code", "acknowledgement")
    OPERATION_ID_FIELD_NUMBER: _ClassVar[int]
    STATUS_FIELD_NUMBER: _ClassVar[int]
    ACCEPTED_FIELD_NUMBER: _ClassVar[int]
    REASON_CODE_FIELD_NUMBER: _ClassVar[int]
    ACKNOWLEDGEMENT_FIELD_NUMBER: _ClassVar[int]
    operation_id: str
    status: OperationStatus
    accepted: bool
    reason_code: str
    acknowledgement: _struct_pb2.Struct
    def __init__(self, operation_id: _Optional[str] = ..., status: _Optional[_Union[OperationStatus, str]] = ..., accepted: _Optional[bool] = ..., reason_code: _Optional[str] = ..., acknowledgement: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

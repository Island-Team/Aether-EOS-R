# Generated from in-repository share/eos/contracts.
import datetime

from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf import timestamp_pb2 as _timestamp_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class ResourceRequest(_message.Message):
    __slots__ = ("uri",)
    URI_FIELD_NUMBER: _ClassVar[int]
    uri: str
    def __init__(self, uri: _Optional[str] = ...) -> None: ...

class ModelStatusRequest(_message.Message):
    __slots__ = ("model_id",)
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    model_id: str
    def __init__(self, model_id: _Optional[str] = ...) -> None: ...

class ModelStatus(_message.Message):
    __slots__ = ("model_id", "capability", "enabled", "status", "checked_at", "message")
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    CAPABILITY_FIELD_NUMBER: _ClassVar[int]
    ENABLED_FIELD_NUMBER: _ClassVar[int]
    STATUS_FIELD_NUMBER: _ClassVar[int]
    CHECKED_AT_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    model_id: str
    capability: str
    enabled: bool
    status: str
    checked_at: _timestamp_pb2.Timestamp
    message: str
    def __init__(self, model_id: _Optional[str] = ..., capability: _Optional[str] = ..., enabled: _Optional[bool] = ..., status: _Optional[str] = ..., checked_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., message: _Optional[str] = ...) -> None: ...

class ResourceChanged(_message.Message):
    __slots__ = ("uri", "resource")
    URI_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_FIELD_NUMBER: _ClassVar[int]
    uri: str
    resource: _struct_pb2.Struct
    def __init__(self, uri: _Optional[str] = ..., resource: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class ResourceSequenceRange(_message.Message):
    __slots__ = ("first_sequence", "last_sequence", "limit")
    FIRST_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    LAST_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    LIMIT_FIELD_NUMBER: _ClassVar[int]
    first_sequence: int
    last_sequence: int
    limit: int
    def __init__(self, first_sequence: _Optional[int] = ..., last_sequence: _Optional[int] = ..., limit: _Optional[int] = ...) -> None: ...

class QueryResourceHistoryRequest(_message.Message):
    __slots__ = ("uri", "sequence", "before", "after", "nearest", "sequence_range")
    URI_FIELD_NUMBER: _ClassVar[int]
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    BEFORE_FIELD_NUMBER: _ClassVar[int]
    AFTER_FIELD_NUMBER: _ClassVar[int]
    NEAREST_FIELD_NUMBER: _ClassVar[int]
    SEQUENCE_RANGE_FIELD_NUMBER: _ClassVar[int]
    uri: str
    sequence: int
    before: _timestamp_pb2.Timestamp
    after: _timestamp_pb2.Timestamp
    nearest: _timestamp_pb2.Timestamp
    sequence_range: ResourceSequenceRange
    def __init__(self, uri: _Optional[str] = ..., sequence: _Optional[int] = ..., before: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., after: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., nearest: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., sequence_range: _Optional[_Union[ResourceSequenceRange, _Mapping]] = ...) -> None: ...

class ResourceHistoryProduct(_message.Message):
    __slots__ = ("sequence", "observed_at", "boot_id", "epoch", "value")
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    OBSERVED_AT_FIELD_NUMBER: _ClassVar[int]
    BOOT_ID_FIELD_NUMBER: _ClassVar[int]
    EPOCH_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    sequence: int
    observed_at: _timestamp_pb2.Timestamp
    boot_id: str
    epoch: str
    value: _struct_pb2.Struct
    def __init__(self, sequence: _Optional[int] = ..., observed_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., boot_id: _Optional[str] = ..., epoch: _Optional[str] = ..., value: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class ResourceRetention(_message.Message):
    __slots__ = ("first_available_sequence", "latest_sequence", "evicted_through_sequence", "closed")
    FIRST_AVAILABLE_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    LATEST_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    EVICTED_THROUGH_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    CLOSED_FIELD_NUMBER: _ClassVar[int]
    first_available_sequence: int
    latest_sequence: int
    evicted_through_sequence: int
    closed: bool
    def __init__(self, first_available_sequence: _Optional[int] = ..., latest_sequence: _Optional[int] = ..., evicted_through_sequence: _Optional[int] = ..., closed: _Optional[bool] = ...) -> None: ...

class QueryResourceHistoryResponse(_message.Message):
    __slots__ = ("uri", "schema_uri", "products", "retention", "retention_gap", "truncated")
    URI_FIELD_NUMBER: _ClassVar[int]
    SCHEMA_URI_FIELD_NUMBER: _ClassVar[int]
    PRODUCTS_FIELD_NUMBER: _ClassVar[int]
    RETENTION_FIELD_NUMBER: _ClassVar[int]
    RETENTION_GAP_FIELD_NUMBER: _ClassVar[int]
    TRUNCATED_FIELD_NUMBER: _ClassVar[int]
    uri: str
    schema_uri: str
    products: _containers.RepeatedCompositeFieldContainer[ResourceHistoryProduct]
    retention: ResourceRetention
    retention_gap: bool
    truncated: bool
    def __init__(self, uri: _Optional[str] = ..., schema_uri: _Optional[str] = ..., products: _Optional[_Iterable[_Union[ResourceHistoryProduct, _Mapping]]] = ..., retention: _Optional[_Union[ResourceRetention, _Mapping]] = ..., retention_gap: _Optional[bool] = ..., truncated: _Optional[bool] = ...) -> None: ...

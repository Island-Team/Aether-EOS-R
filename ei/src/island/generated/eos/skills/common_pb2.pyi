# Generated from in-repository share/eos/contracts.
import datetime

from google.protobuf import timestamp_pb2 as _timestamp_pb2
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class SkillEventTime(_message.Message):
    __slots__ = ("source_timestamp_ns", "source_clock_id", "observed_at", "observed_monotonic_ns", "boot_id", "sequence", "event_id")
    SOURCE_TIMESTAMP_NS_FIELD_NUMBER: _ClassVar[int]
    SOURCE_CLOCK_ID_FIELD_NUMBER: _ClassVar[int]
    OBSERVED_AT_FIELD_NUMBER: _ClassVar[int]
    OBSERVED_MONOTONIC_NS_FIELD_NUMBER: _ClassVar[int]
    BOOT_ID_FIELD_NUMBER: _ClassVar[int]
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    EVENT_ID_FIELD_NUMBER: _ClassVar[int]
    source_timestamp_ns: int
    source_clock_id: str
    observed_at: _timestamp_pb2.Timestamp
    observed_monotonic_ns: int
    boot_id: str
    sequence: int
    event_id: str
    def __init__(self, source_timestamp_ns: _Optional[int] = ..., source_clock_id: _Optional[str] = ..., observed_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., observed_monotonic_ns: _Optional[int] = ..., boot_id: _Optional[str] = ..., sequence: _Optional[int] = ..., event_id: _Optional[str] = ...) -> None: ...

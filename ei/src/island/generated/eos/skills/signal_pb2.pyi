# Generated from in-repository share/eos/contracts.
from island.generated.eos.skills import common_pb2 as _common_pb2
from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class SkillSignalDirection(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    SKILL_SIGNAL_DIRECTION_UNSPECIFIED: _ClassVar[SkillSignalDirection]
    SKILL_SIGNAL_DIRECTION_AGENT_TO_SKILL: _ClassVar[SkillSignalDirection]
    SKILL_SIGNAL_DIRECTION_SKILL_TO_AGENT: _ClassVar[SkillSignalDirection]
SKILL_SIGNAL_DIRECTION_UNSPECIFIED: SkillSignalDirection
SKILL_SIGNAL_DIRECTION_AGENT_TO_SKILL: SkillSignalDirection
SKILL_SIGNAL_DIRECTION_SKILL_TO_AGENT: SkillSignalDirection

class SignalExecutionRequest(_message.Message):
    __slots__ = ("execution_id", "request_id", "name", "payload")
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    PAYLOAD_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    request_id: str
    name: str
    payload: _struct_pb2.Struct
    def __init__(self, execution_id: _Optional[str] = ..., request_id: _Optional[str] = ..., name: _Optional[str] = ..., payload: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class SkillSignalReceipt(_message.Message):
    __slots__ = ("signal_id", "execution_id", "request_id", "accepted")
    SIGNAL_ID_FIELD_NUMBER: _ClassVar[int]
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    ACCEPTED_FIELD_NUMBER: _ClassVar[int]
    signal_id: str
    execution_id: str
    request_id: str
    accepted: bool
    def __init__(self, signal_id: _Optional[str] = ..., execution_id: _Optional[str] = ..., request_id: _Optional[str] = ..., accepted: _Optional[bool] = ...) -> None: ...

class SkillSignal(_message.Message):
    __slots__ = ("signal_id", "execution_id", "direction", "name", "journal_sequence", "payload", "event_time")
    SIGNAL_ID_FIELD_NUMBER: _ClassVar[int]
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    DIRECTION_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    JOURNAL_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    PAYLOAD_FIELD_NUMBER: _ClassVar[int]
    EVENT_TIME_FIELD_NUMBER: _ClassVar[int]
    signal_id: str
    execution_id: str
    direction: SkillSignalDirection
    name: str
    journal_sequence: int
    payload: _struct_pb2.Struct
    event_time: _common_pb2.SkillEventTime
    def __init__(self, signal_id: _Optional[str] = ..., execution_id: _Optional[str] = ..., direction: _Optional[_Union[SkillSignalDirection, str]] = ..., name: _Optional[str] = ..., journal_sequence: _Optional[int] = ..., payload: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., event_time: _Optional[_Union[_common_pb2.SkillEventTime, _Mapping]] = ...) -> None: ...

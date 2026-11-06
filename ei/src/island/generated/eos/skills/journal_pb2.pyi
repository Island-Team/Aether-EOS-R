# Generated from in-repository share/eos/contracts.
import datetime

from island.generated.eos.skills import execution_pb2 as _execution_pb2
from island.generated.eos.skills import signal_pb2 as _signal_pb2
from google.protobuf import duration_pb2 as _duration_pb2
from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf import timestamp_pb2 as _timestamp_pb2
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class SkillJournalSource(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    SKILL_JOURNAL_SOURCE_UNSPECIFIED: _ClassVar[SkillJournalSource]
    SKILL_JOURNAL_SOURCE_SYSTEM: _ClassVar[SkillJournalSource]
    SKILL_JOURNAL_SOURCE_SKILL: _ClassVar[SkillJournalSource]

class SkillJournalKind(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    SKILL_JOURNAL_KIND_UNSPECIFIED: _ClassVar[SkillJournalKind]
    SKILL_JOURNAL_KIND_ACTIVITY: _ClassVar[SkillJournalKind]
    SKILL_JOURNAL_KIND_CHECKPOINT: _ClassVar[SkillJournalKind]
    SKILL_JOURNAL_KIND_SUPPRESSION: _ClassVar[SkillJournalKind]
SKILL_JOURNAL_SOURCE_UNSPECIFIED: SkillJournalSource
SKILL_JOURNAL_SOURCE_SYSTEM: SkillJournalSource
SKILL_JOURNAL_SOURCE_SKILL: SkillJournalSource
SKILL_JOURNAL_KIND_UNSPECIFIED: SkillJournalKind
SKILL_JOURNAL_KIND_ACTIVITY: SkillJournalKind
SKILL_JOURNAL_KIND_CHECKPOINT: SkillJournalKind
SKILL_JOURNAL_KIND_SUPPRESSION: SkillJournalKind

class SkillJournalGap(_message.Message):
    __slots__ = ("after_sequence", "first_available_sequence")
    AFTER_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    FIRST_AVAILABLE_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    after_sequence: int
    first_available_sequence: int
    def __init__(self, after_sequence: _Optional[int] = ..., first_available_sequence: _Optional[int] = ...) -> None: ...

class FollowExecutionJournalRequest(_message.Message):
    __slots__ = ("execution_id", "after_journal_sequence")
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    AFTER_JOURNAL_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    after_journal_sequence: int
    def __init__(self, execution_id: _Optional[str] = ..., after_journal_sequence: _Optional[int] = ...) -> None: ...

class SkillJournalEntry(_message.Message):
    __slots__ = ("execution_id", "journal_sequence", "timestamp", "snapshot", "signal", "record", "gap")
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    JOURNAL_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    TIMESTAMP_FIELD_NUMBER: _ClassVar[int]
    SNAPSHOT_FIELD_NUMBER: _ClassVar[int]
    SIGNAL_FIELD_NUMBER: _ClassVar[int]
    RECORD_FIELD_NUMBER: _ClassVar[int]
    GAP_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    journal_sequence: int
    timestamp: _timestamp_pb2.Timestamp
    snapshot: _execution_pb2.SkillExecution
    signal: _signal_pb2.SkillSignal
    record: SkillJournalRecord
    gap: SkillJournalGap
    def __init__(self, execution_id: _Optional[str] = ..., journal_sequence: _Optional[int] = ..., timestamp: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., snapshot: _Optional[_Union[_execution_pb2.SkillExecution, _Mapping]] = ..., signal: _Optional[_Union[_signal_pb2.SkillSignal, _Mapping]] = ..., record: _Optional[_Union[SkillJournalRecord, _Mapping]] = ..., gap: _Optional[_Union[SkillJournalGap, _Mapping]] = ...) -> None: ...

class SkillJournalRecord(_message.Message):
    __slots__ = ("event_id", "execution_id", "journal_sequence", "source", "kind", "name", "outcome", "message", "elapsed", "duration", "progress", "fields")
    EVENT_ID_FIELD_NUMBER: _ClassVar[int]
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    JOURNAL_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    SOURCE_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    OUTCOME_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    ELAPSED_FIELD_NUMBER: _ClassVar[int]
    DURATION_FIELD_NUMBER: _ClassVar[int]
    PROGRESS_FIELD_NUMBER: _ClassVar[int]
    FIELDS_FIELD_NUMBER: _ClassVar[int]
    event_id: str
    execution_id: str
    journal_sequence: int
    source: SkillJournalSource
    kind: SkillJournalKind
    name: str
    outcome: str
    message: str
    elapsed: _duration_pb2.Duration
    duration: _duration_pb2.Duration
    progress: float
    fields: _struct_pb2.Struct
    def __init__(self, event_id: _Optional[str] = ..., execution_id: _Optional[str] = ..., journal_sequence: _Optional[int] = ..., source: _Optional[_Union[SkillJournalSource, str]] = ..., kind: _Optional[_Union[SkillJournalKind, str]] = ..., name: _Optional[str] = ..., outcome: _Optional[str] = ..., message: _Optional[str] = ..., elapsed: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ..., duration: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ..., progress: _Optional[float] = ..., fields: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

# Generated from in-repository share/eos/contracts.
import datetime

from island.generated.eos.skills import catalog_pb2 as _catalog_pb2
from island.generated.eos.skills import common_pb2 as _common_pb2
from google.protobuf import duration_pb2 as _duration_pb2
from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf import timestamp_pb2 as _timestamp_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class ExecutionStatus(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    EXECUTION_STATUS_UNSPECIFIED: _ClassVar[ExecutionStatus]
    EXECUTION_STATUS_QUEUED: _ClassVar[ExecutionStatus]
    EXECUTION_STATUS_RUNNING: _ClassVar[ExecutionStatus]
    EXECUTION_STATUS_SUCCEEDED: _ClassVar[ExecutionStatus]
    EXECUTION_STATUS_REJECTED: _ClassVar[ExecutionStatus]
    EXECUTION_STATUS_CANCELLED: _ClassVar[ExecutionStatus]
    EXECUTION_STATUS_TIMED_OUT: _ClassVar[ExecutionStatus]
    EXECUTION_STATUS_FAILED: _ClassVar[ExecutionStatus]
EXECUTION_STATUS_UNSPECIFIED: ExecutionStatus
EXECUTION_STATUS_QUEUED: ExecutionStatus
EXECUTION_STATUS_RUNNING: ExecutionStatus
EXECUTION_STATUS_SUCCEEDED: ExecutionStatus
EXECUTION_STATUS_REJECTED: ExecutionStatus
EXECUTION_STATUS_CANCELLED: ExecutionStatus
EXECUTION_STATUS_TIMED_OUT: ExecutionStatus
EXECUTION_STATUS_FAILED: ExecutionStatus

class StartExecutionRequest(_message.Message):
    __slots__ = ("reference", "arguments", "request_id", "timeout")
    REFERENCE_FIELD_NUMBER: _ClassVar[int]
    ARGUMENTS_FIELD_NUMBER: _ClassVar[int]
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    TIMEOUT_FIELD_NUMBER: _ClassVar[int]
    reference: _catalog_pb2.SkillReference
    arguments: _struct_pb2.Struct
    request_id: str
    timeout: _duration_pb2.Duration
    def __init__(self, reference: _Optional[_Union[_catalog_pb2.SkillReference, _Mapping]] = ..., arguments: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., request_id: _Optional[str] = ..., timeout: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ...) -> None: ...

class GetExecutionRequest(_message.Message):
    __slots__ = ("execution_id",)
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    def __init__(self, execution_id: _Optional[str] = ...) -> None: ...

class WatchExecutionRequest(_message.Message):
    __slots__ = ("execution_id", "after_sequence")
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    AFTER_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    after_sequence: int
    def __init__(self, execution_id: _Optional[str] = ..., after_sequence: _Optional[int] = ...) -> None: ...

class GetExecutionStateRequest(_message.Message):
    __slots__ = ("execution_id",)
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    def __init__(self, execution_id: _Optional[str] = ...) -> None: ...

class WatchExecutionStateRequest(_message.Message):
    __slots__ = ("execution_id", "after_revision")
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    AFTER_REVISION_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    after_revision: int
    def __init__(self, execution_id: _Optional[str] = ..., after_revision: _Optional[int] = ...) -> None: ...

class SkillExecutionState(_message.Message):
    __slots__ = ("execution_id", "revision", "published_at", "payload")
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    REVISION_FIELD_NUMBER: _ClassVar[int]
    PUBLISHED_AT_FIELD_NUMBER: _ClassVar[int]
    PAYLOAD_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    revision: int
    published_at: _timestamp_pb2.Timestamp
    payload: _struct_pb2.Struct
    def __init__(self, execution_id: _Optional[str] = ..., revision: _Optional[int] = ..., published_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., payload: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class ListExecutionsRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class ListExecutionsResponse(_message.Message):
    __slots__ = ("executions",)
    EXECUTIONS_FIELD_NUMBER: _ClassVar[int]
    executions: _containers.RepeatedCompositeFieldContainer[SkillExecution]
    def __init__(self, executions: _Optional[_Iterable[_Union[SkillExecution, _Mapping]]] = ...) -> None: ...

class SkillCurrentActivity(_message.Message):
    __slots__ = ("activity_id", "name", "started_at", "fields")
    ACTIVITY_ID_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    STARTED_AT_FIELD_NUMBER: _ClassVar[int]
    FIELDS_FIELD_NUMBER: _ClassVar[int]
    activity_id: str
    name: str
    started_at: _timestamp_pb2.Timestamp
    fields: _struct_pb2.Struct
    def __init__(self, activity_id: _Optional[str] = ..., name: _Optional[str] = ..., started_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., fields: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class SkillExecutionAccounting(_message.Message):
    __slots__ = ("primitive_calls", "resource_reads", "signals_received", "signals_emitted", "diagnostic_logs", "diagnostic_logs_dropped", "checkpoints", "checkpoints_dropped", "activity_events", "activity_events_dropped", "claim_wait_duration", "signal_wait_duration", "capability_duration")
    PRIMITIVE_CALLS_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_READS_FIELD_NUMBER: _ClassVar[int]
    SIGNALS_RECEIVED_FIELD_NUMBER: _ClassVar[int]
    SIGNALS_EMITTED_FIELD_NUMBER: _ClassVar[int]
    DIAGNOSTIC_LOGS_FIELD_NUMBER: _ClassVar[int]
    DIAGNOSTIC_LOGS_DROPPED_FIELD_NUMBER: _ClassVar[int]
    CHECKPOINTS_FIELD_NUMBER: _ClassVar[int]
    CHECKPOINTS_DROPPED_FIELD_NUMBER: _ClassVar[int]
    ACTIVITY_EVENTS_FIELD_NUMBER: _ClassVar[int]
    ACTIVITY_EVENTS_DROPPED_FIELD_NUMBER: _ClassVar[int]
    CLAIM_WAIT_DURATION_FIELD_NUMBER: _ClassVar[int]
    SIGNAL_WAIT_DURATION_FIELD_NUMBER: _ClassVar[int]
    CAPABILITY_DURATION_FIELD_NUMBER: _ClassVar[int]
    primitive_calls: int
    resource_reads: int
    signals_received: int
    signals_emitted: int
    diagnostic_logs: int
    diagnostic_logs_dropped: int
    checkpoints: int
    checkpoints_dropped: int
    activity_events: int
    activity_events_dropped: int
    claim_wait_duration: _duration_pb2.Duration
    signal_wait_duration: _duration_pb2.Duration
    capability_duration: _duration_pb2.Duration
    def __init__(self, primitive_calls: _Optional[int] = ..., resource_reads: _Optional[int] = ..., signals_received: _Optional[int] = ..., signals_emitted: _Optional[int] = ..., diagnostic_logs: _Optional[int] = ..., diagnostic_logs_dropped: _Optional[int] = ..., checkpoints: _Optional[int] = ..., checkpoints_dropped: _Optional[int] = ..., activity_events: _Optional[int] = ..., activity_events_dropped: _Optional[int] = ..., claim_wait_duration: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ..., signal_wait_duration: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ..., capability_duration: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ...) -> None: ...

class SkillExecution(_message.Message):
    __slots__ = ("execution_id", "request_id", "reference", "status", "sequence", "cancellation_requested", "reason_code", "message", "result", "queue_reason", "claims", "latest_journal_sequence", "active_device_claims", "created_at", "deadline_at", "started_at", "cancellation_requested_at", "finished_at", "queue_duration", "run_duration", "total_duration", "current_activity", "accounting", "runtime_id", "boot_id", "event_time", "parent_execution_id", "active_claims")
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    REFERENCE_FIELD_NUMBER: _ClassVar[int]
    STATUS_FIELD_NUMBER: _ClassVar[int]
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    CANCELLATION_REQUESTED_FIELD_NUMBER: _ClassVar[int]
    REASON_CODE_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    RESULT_FIELD_NUMBER: _ClassVar[int]
    QUEUE_REASON_FIELD_NUMBER: _ClassVar[int]
    CLAIMS_FIELD_NUMBER: _ClassVar[int]
    LATEST_JOURNAL_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    ACTIVE_DEVICE_CLAIMS_FIELD_NUMBER: _ClassVar[int]
    CREATED_AT_FIELD_NUMBER: _ClassVar[int]
    DEADLINE_AT_FIELD_NUMBER: _ClassVar[int]
    STARTED_AT_FIELD_NUMBER: _ClassVar[int]
    CANCELLATION_REQUESTED_AT_FIELD_NUMBER: _ClassVar[int]
    FINISHED_AT_FIELD_NUMBER: _ClassVar[int]
    QUEUE_DURATION_FIELD_NUMBER: _ClassVar[int]
    RUN_DURATION_FIELD_NUMBER: _ClassVar[int]
    TOTAL_DURATION_FIELD_NUMBER: _ClassVar[int]
    CURRENT_ACTIVITY_FIELD_NUMBER: _ClassVar[int]
    ACCOUNTING_FIELD_NUMBER: _ClassVar[int]
    RUNTIME_ID_FIELD_NUMBER: _ClassVar[int]
    BOOT_ID_FIELD_NUMBER: _ClassVar[int]
    EVENT_TIME_FIELD_NUMBER: _ClassVar[int]
    PARENT_EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    ACTIVE_CLAIMS_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    request_id: str
    reference: _catalog_pb2.SkillReference
    status: ExecutionStatus
    sequence: int
    cancellation_requested: bool
    reason_code: str
    message: str
    result: _struct_pb2.Struct
    queue_reason: str
    claims: _containers.RepeatedCompositeFieldContainer[_catalog_pb2.SkillClaim]
    latest_journal_sequence: int
    active_device_claims: _containers.RepeatedCompositeFieldContainer[_catalog_pb2.SkillClaim]
    created_at: _timestamp_pb2.Timestamp
    deadline_at: _timestamp_pb2.Timestamp
    started_at: _timestamp_pb2.Timestamp
    cancellation_requested_at: _timestamp_pb2.Timestamp
    finished_at: _timestamp_pb2.Timestamp
    queue_duration: _duration_pb2.Duration
    run_duration: _duration_pb2.Duration
    total_duration: _duration_pb2.Duration
    current_activity: SkillCurrentActivity
    accounting: SkillExecutionAccounting
    runtime_id: str
    boot_id: str
    event_time: _common_pb2.SkillEventTime
    parent_execution_id: str
    active_claims: _containers.RepeatedCompositeFieldContainer[_catalog_pb2.SkillClaim]
    def __init__(self, execution_id: _Optional[str] = ..., request_id: _Optional[str] = ..., reference: _Optional[_Union[_catalog_pb2.SkillReference, _Mapping]] = ..., status: _Optional[_Union[ExecutionStatus, str]] = ..., sequence: _Optional[int] = ..., cancellation_requested: _Optional[bool] = ..., reason_code: _Optional[str] = ..., message: _Optional[str] = ..., result: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., queue_reason: _Optional[str] = ..., claims: _Optional[_Iterable[_Union[_catalog_pb2.SkillClaim, _Mapping]]] = ..., latest_journal_sequence: _Optional[int] = ..., active_device_claims: _Optional[_Iterable[_Union[_catalog_pb2.SkillClaim, _Mapping]]] = ..., created_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., deadline_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., started_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., cancellation_requested_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., finished_at: _Optional[_Union[datetime.datetime, _timestamp_pb2.Timestamp, _Mapping]] = ..., queue_duration: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ..., run_duration: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ..., total_duration: _Optional[_Union[datetime.timedelta, _duration_pb2.Duration, _Mapping]] = ..., current_activity: _Optional[_Union[SkillCurrentActivity, _Mapping]] = ..., accounting: _Optional[_Union[SkillExecutionAccounting, _Mapping]] = ..., runtime_id: _Optional[str] = ..., boot_id: _Optional[str] = ..., event_time: _Optional[_Union[_common_pb2.SkillEventTime, _Mapping]] = ..., parent_execution_id: _Optional[str] = ..., active_claims: _Optional[_Iterable[_Union[_catalog_pb2.SkillClaim, _Mapping]]] = ...) -> None: ...

class SkillExecutionSelector(_message.Message):
    __slots__ = ("reference", "statuses")
    REFERENCE_FIELD_NUMBER: _ClassVar[int]
    STATUSES_FIELD_NUMBER: _ClassVar[int]
    reference: _catalog_pb2.SkillReference
    statuses: _containers.RepeatedScalarFieldContainer[ExecutionStatus]
    def __init__(self, reference: _Optional[_Union[_catalog_pb2.SkillReference, _Mapping]] = ..., statuses: _Optional[_Iterable[_Union[ExecutionStatus, str]]] = ...) -> None: ...

class MonitorExecutionsRequest(_message.Message):
    __slots__ = ("selector", "after_monitor_sequence")
    SELECTOR_FIELD_NUMBER: _ClassVar[int]
    AFTER_MONITOR_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    selector: SkillExecutionSelector
    after_monitor_sequence: int
    def __init__(self, selector: _Optional[_Union[SkillExecutionSelector, _Mapping]] = ..., after_monitor_sequence: _Optional[int] = ...) -> None: ...

class SkillMonitorUpdate(_message.Message):
    __slots__ = ("monitor_sequence", "reset", "upserted", "removed_execution_ids")
    MONITOR_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    RESET_FIELD_NUMBER: _ClassVar[int]
    UPSERTED_FIELD_NUMBER: _ClassVar[int]
    REMOVED_EXECUTION_IDS_FIELD_NUMBER: _ClassVar[int]
    monitor_sequence: int
    reset: bool
    upserted: _containers.RepeatedCompositeFieldContainer[SkillExecution]
    removed_execution_ids: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, monitor_sequence: _Optional[int] = ..., reset: _Optional[bool] = ..., upserted: _Optional[_Iterable[_Union[SkillExecution, _Mapping]]] = ..., removed_execution_ids: _Optional[_Iterable[str]] = ...) -> None: ...

class TerminateExecutionRequest(_message.Message):
    __slots__ = ("execution_id",)
    EXECUTION_ID_FIELD_NUMBER: _ClassVar[int]
    execution_id: str
    def __init__(self, execution_id: _Optional[str] = ...) -> None: ...

class TerminateExecutionsRequest(_message.Message):
    __slots__ = ("reference", "request_id")
    REFERENCE_FIELD_NUMBER: _ClassVar[int]
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    reference: _catalog_pb2.SkillReference
    request_id: str
    def __init__(self, reference: _Optional[_Union[_catalog_pb2.SkillReference, _Mapping]] = ..., request_id: _Optional[str] = ...) -> None: ...

class TerminateExecutionsResponse(_message.Message):
    __slots__ = ("request_id", "execution_ids")
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    EXECUTION_IDS_FIELD_NUMBER: _ClassVar[int]
    request_id: str
    execution_ids: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, request_id: _Optional[str] = ..., execution_ids: _Optional[_Iterable[str]] = ...) -> None: ...

# Generated from in-repository share/eos/contracts.
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional

DESCRIPTOR: _descriptor.FileDescriptor

class PerceptionMedia(_message.Message):
    __slots__ = ("data", "filename", "media_type")
    DATA_FIELD_NUMBER: _ClassVar[int]
    FILENAME_FIELD_NUMBER: _ClassVar[int]
    MEDIA_TYPE_FIELD_NUMBER: _ClassVar[int]
    data: bytes
    filename: str
    media_type: str
    def __init__(self, data: _Optional[bytes] = ..., filename: _Optional[str] = ..., media_type: _Optional[str] = ...) -> None: ...

class AudioMedia(_message.Message):
    __slots__ = ("data", "filename", "media_type", "sample_rate_hz", "channels")
    DATA_FIELD_NUMBER: _ClassVar[int]
    FILENAME_FIELD_NUMBER: _ClassVar[int]
    MEDIA_TYPE_FIELD_NUMBER: _ClassVar[int]
    SAMPLE_RATE_HZ_FIELD_NUMBER: _ClassVar[int]
    CHANNELS_FIELD_NUMBER: _ClassVar[int]
    data: bytes
    filename: str
    media_type: str
    sample_rate_hz: int
    channels: int
    def __init__(self, data: _Optional[bytes] = ..., filename: _Optional[str] = ..., media_type: _Optional[str] = ..., sample_rate_hz: _Optional[int] = ..., channels: _Optional[int] = ...) -> None: ...

class BoundingBox(_message.Message):
    __slots__ = ("x1", "y1", "x2", "y2")
    X1_FIELD_NUMBER: _ClassVar[int]
    Y1_FIELD_NUMBER: _ClassVar[int]
    X2_FIELD_NUMBER: _ClassVar[int]
    Y2_FIELD_NUMBER: _ClassVar[int]
    x1: float
    y1: float
    x2: float
    y2: float
    def __init__(self, x1: _Optional[float] = ..., y1: _Optional[float] = ..., x2: _Optional[float] = ..., y2: _Optional[float] = ...) -> None: ...

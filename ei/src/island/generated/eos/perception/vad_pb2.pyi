# Generated from in-repository share/eos/contracts.
from island.generated.eos.perception import media_pb2 as _media_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class InferVadRequest(_message.Message):
    __slots__ = ("model_id", "input", "threshold")
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    INPUT_FIELD_NUMBER: _ClassVar[int]
    THRESHOLD_FIELD_NUMBER: _ClassVar[int]
    model_id: str
    input: _media_pb2.AudioMedia
    threshold: float
    def __init__(self, model_id: _Optional[str] = ..., input: _Optional[_Union[_media_pb2.AudioMedia, _Mapping]] = ..., threshold: _Optional[float] = ...) -> None: ...

class VadSegment(_message.Message):
    __slots__ = ("start_s", "end_s", "confidence")
    START_S_FIELD_NUMBER: _ClassVar[int]
    END_S_FIELD_NUMBER: _ClassVar[int]
    CONFIDENCE_FIELD_NUMBER: _ClassVar[int]
    start_s: float
    end_s: float
    confidence: float
    def __init__(self, start_s: _Optional[float] = ..., end_s: _Optional[float] = ..., confidence: _Optional[float] = ...) -> None: ...

class InferVadResponse(_message.Message):
    __slots__ = ("speech", "probability", "segments")
    SPEECH_FIELD_NUMBER: _ClassVar[int]
    PROBABILITY_FIELD_NUMBER: _ClassVar[int]
    SEGMENTS_FIELD_NUMBER: _ClassVar[int]
    speech: bool
    probability: float
    segments: _containers.RepeatedCompositeFieldContainer[VadSegment]
    def __init__(self, speech: _Optional[bool] = ..., probability: _Optional[float] = ..., segments: _Optional[_Iterable[_Union[VadSegment, _Mapping]]] = ...) -> None: ...

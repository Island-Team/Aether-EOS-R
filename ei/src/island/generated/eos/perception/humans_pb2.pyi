# Generated from in-repository share/eos/contracts.
from island.generated.eos.perception import media_pb2 as _media_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class TrackedHuman(_message.Message):
    __slots__ = ("track_id", "bbox", "confidence", "distance_m")
    TRACK_ID_FIELD_NUMBER: _ClassVar[int]
    BBOX_FIELD_NUMBER: _ClassVar[int]
    CONFIDENCE_FIELD_NUMBER: _ClassVar[int]
    DISTANCE_M_FIELD_NUMBER: _ClassVar[int]
    track_id: int
    bbox: _media_pb2.BoundingBox
    confidence: float
    distance_m: float
    def __init__(self, track_id: _Optional[int] = ..., bbox: _Optional[_Union[_media_pb2.BoundingBox, _Mapping]] = ..., confidence: _Optional[float] = ..., distance_m: _Optional[float] = ...) -> None: ...

class DetectHumansRequest(_message.Message):
    __slots__ = ("model_id", "input", "depth_input", "depth_scale", "confidence_threshold")
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    INPUT_FIELD_NUMBER: _ClassVar[int]
    DEPTH_INPUT_FIELD_NUMBER: _ClassVar[int]
    DEPTH_SCALE_FIELD_NUMBER: _ClassVar[int]
    CONFIDENCE_THRESHOLD_FIELD_NUMBER: _ClassVar[int]
    model_id: str
    input: _media_pb2.PerceptionMedia
    depth_input: _media_pb2.PerceptionMedia
    depth_scale: float
    confidence_threshold: float
    def __init__(self, model_id: _Optional[str] = ..., input: _Optional[_Union[_media_pb2.PerceptionMedia, _Mapping]] = ..., depth_input: _Optional[_Union[_media_pb2.PerceptionMedia, _Mapping]] = ..., depth_scale: _Optional[float] = ..., confidence_threshold: _Optional[float] = ...) -> None: ...

class DetectHumansResponse(_message.Message):
    __slots__ = ("persons", "person_count", "furniture")
    PERSONS_FIELD_NUMBER: _ClassVar[int]
    PERSON_COUNT_FIELD_NUMBER: _ClassVar[int]
    FURNITURE_FIELD_NUMBER: _ClassVar[int]
    persons: _containers.RepeatedCompositeFieldContainer[TrackedHuman]
    person_count: int
    furniture: _containers.RepeatedCompositeFieldContainer[DetectedFurniture]
    def __init__(self, persons: _Optional[_Iterable[_Union[TrackedHuman, _Mapping]]] = ..., person_count: _Optional[int] = ..., furniture: _Optional[_Iterable[_Union[DetectedFurniture, _Mapping]]] = ...) -> None: ...

class DetectedFurniture(_message.Message):
    __slots__ = ("bbox", "class_name")
    BBOX_FIELD_NUMBER: _ClassVar[int]
    CLASS_NAME_FIELD_NUMBER: _ClassVar[int]
    bbox: _media_pb2.BoundingBox
    class_name: str
    def __init__(self, bbox: _Optional[_Union[_media_pb2.BoundingBox, _Mapping]] = ..., class_name: _Optional[str] = ...) -> None: ...

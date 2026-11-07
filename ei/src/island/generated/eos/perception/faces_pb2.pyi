# Generated from in-repository share/eos/contracts.
from island.generated.eos.perception import media_pb2 as _media_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class DetectFacesRequest(_message.Message):
    __slots__ = ("model_id", "input", "return_image")
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    INPUT_FIELD_NUMBER: _ClassVar[int]
    RETURN_IMAGE_FIELD_NUMBER: _ClassVar[int]
    model_id: str
    input: _media_pb2.PerceptionMedia
    return_image: bool
    def __init__(self, model_id: _Optional[str] = ..., input: _Optional[_Union[_media_pb2.PerceptionMedia, _Mapping]] = ..., return_image: _Optional[bool] = ...) -> None: ...

class DetectedFace(_message.Message):
    __slots__ = ("identity", "known", "similarity", "bbox", "direction_px")
    IDENTITY_FIELD_NUMBER: _ClassVar[int]
    KNOWN_FIELD_NUMBER: _ClassVar[int]
    SIMILARITY_FIELD_NUMBER: _ClassVar[int]
    BBOX_FIELD_NUMBER: _ClassVar[int]
    DIRECTION_PX_FIELD_NUMBER: _ClassVar[int]
    identity: str
    known: bool
    similarity: float
    bbox: _media_pb2.BoundingBox
    direction_px: float
    def __init__(self, identity: _Optional[str] = ..., known: _Optional[bool] = ..., similarity: _Optional[float] = ..., bbox: _Optional[_Union[_media_pb2.BoundingBox, _Mapping]] = ..., direction_px: _Optional[float] = ...) -> None: ...

class FaceDetectionData(_message.Message):
    __slots__ = ("faces", "face_count", "annotated_image")
    FACES_FIELD_NUMBER: _ClassVar[int]
    FACE_COUNT_FIELD_NUMBER: _ClassVar[int]
    ANNOTATED_IMAGE_FIELD_NUMBER: _ClassVar[int]
    faces: _containers.RepeatedCompositeFieldContainer[DetectedFace]
    face_count: int
    annotated_image: bytes
    def __init__(self, faces: _Optional[_Iterable[_Union[DetectedFace, _Mapping]]] = ..., face_count: _Optional[int] = ..., annotated_image: _Optional[bytes] = ...) -> None: ...

class DetectFacesResponse(_message.Message):
    __slots__ = ("status", "operation", "data", "message")
    STATUS_FIELD_NUMBER: _ClassVar[int]
    OPERATION_FIELD_NUMBER: _ClassVar[int]
    DATA_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    status: str
    operation: str
    data: FaceDetectionData
    message: str
    def __init__(self, status: _Optional[str] = ..., operation: _Optional[str] = ..., data: _Optional[_Union[FaceDetectionData, _Mapping]] = ..., message: _Optional[str] = ...) -> None: ...

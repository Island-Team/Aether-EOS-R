# Generated from in-repository share/eos/contracts.
from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class CaptureFramesRequest(_message.Message):
    __slots__ = ("robot_name", "device_id", "frame_count")
    ROBOT_NAME_FIELD_NUMBER: _ClassVar[int]
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    FRAME_COUNT_FIELD_NUMBER: _ClassVar[int]
    robot_name: str
    device_id: str
    frame_count: int
    def __init__(self, robot_name: _Optional[str] = ..., device_id: _Optional[str] = ..., frame_count: _Optional[int] = ...) -> None: ...

class CameraFrame(_message.Message):
    __slots__ = ("jpeg", "width_px", "height_px", "captured_at")
    JPEG_FIELD_NUMBER: _ClassVar[int]
    WIDTH_PX_FIELD_NUMBER: _ClassVar[int]
    HEIGHT_PX_FIELD_NUMBER: _ClassVar[int]
    CAPTURED_AT_FIELD_NUMBER: _ClassVar[int]
    jpeg: bytes
    width_px: int
    height_px: int
    captured_at: str
    def __init__(self, jpeg: _Optional[bytes] = ..., width_px: _Optional[int] = ..., height_px: _Optional[int] = ..., captured_at: _Optional[str] = ...) -> None: ...

class CaptureFramesResponse(_message.Message):
    __slots__ = ("type", "schema_version", "device_id", "frames")
    TYPE_FIELD_NUMBER: _ClassVar[int]
    SCHEMA_VERSION_FIELD_NUMBER: _ClassVar[int]
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    FRAMES_FIELD_NUMBER: _ClassVar[int]
    type: str
    schema_version: str
    device_id: str
    frames: _containers.RepeatedCompositeFieldContainer[CameraFrame]
    def __init__(self, type: _Optional[str] = ..., schema_version: _Optional[str] = ..., device_id: _Optional[str] = ..., frames: _Optional[_Iterable[_Union[CameraFrame, _Mapping]]] = ...) -> None: ...

class ObserveObjectsRequest(_message.Message):
    __slots__ = ("robot_name", "device_id", "model_id", "source_timestamp_ns")
    ROBOT_NAME_FIELD_NUMBER: _ClassVar[int]
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    SOURCE_TIMESTAMP_NS_FIELD_NUMBER: _ClassVar[int]
    robot_name: str
    device_id: str
    model_id: str
    source_timestamp_ns: int
    def __init__(self, robot_name: _Optional[str] = ..., device_id: _Optional[str] = ..., model_id: _Optional[str] = ..., source_timestamp_ns: _Optional[int] = ...) -> None: ...

class ObserveObjectsResponse(_message.Message):
    __slots__ = ("device_id", "model_id", "frame_id", "source_timestamp_ns", "width_px", "height_px", "jpeg", "objects")
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    FRAME_ID_FIELD_NUMBER: _ClassVar[int]
    SOURCE_TIMESTAMP_NS_FIELD_NUMBER: _ClassVar[int]
    WIDTH_PX_FIELD_NUMBER: _ClassVar[int]
    HEIGHT_PX_FIELD_NUMBER: _ClassVar[int]
    JPEG_FIELD_NUMBER: _ClassVar[int]
    OBJECTS_FIELD_NUMBER: _ClassVar[int]
    device_id: str
    model_id: str
    frame_id: str
    source_timestamp_ns: int
    width_px: int
    height_px: int
    jpeg: bytes
    objects: _containers.RepeatedCompositeFieldContainer[_struct_pb2.Struct]
    def __init__(self, device_id: _Optional[str] = ..., model_id: _Optional[str] = ..., frame_id: _Optional[str] = ..., source_timestamp_ns: _Optional[int] = ..., width_px: _Optional[int] = ..., height_px: _Optional[int] = ..., jpeg: _Optional[bytes] = ..., objects: _Optional[_Iterable[_Union[_struct_pb2.Struct, _Mapping]]] = ...) -> None: ...

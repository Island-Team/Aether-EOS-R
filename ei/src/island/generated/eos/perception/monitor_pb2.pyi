# Generated from in-repository share/eos/contracts.
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class MonitorRequest(_message.Message):
    __slots__ = ("camera_device_id", "microphone_device_id", "max_camera_fps", "version", "include_detections", "detection_source")
    CAMERA_DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    MICROPHONE_DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    MAX_CAMERA_FPS_FIELD_NUMBER: _ClassVar[int]
    VERSION_FIELD_NUMBER: _ClassVar[int]
    INCLUDE_DETECTIONS_FIELD_NUMBER: _ClassVar[int]
    DETECTION_SOURCE_FIELD_NUMBER: _ClassVar[int]
    camera_device_id: str
    microphone_device_id: str
    max_camera_fps: int
    version: int
    include_detections: bool
    detection_source: str
    def __init__(self, camera_device_id: _Optional[str] = ..., microphone_device_id: _Optional[str] = ..., max_camera_fps: _Optional[int] = ..., version: _Optional[int] = ..., include_detections: _Optional[bool] = ..., detection_source: _Optional[str] = ...) -> None: ...

class MonitorReady(_message.Message):
    __slots__ = ("camera_device_id", "microphone_device_id", "camera_width_px", "camera_height_px", "camera_fps", "audio_sample_rate_hz", "audio_channels", "detection_status")
    CAMERA_DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    MICROPHONE_DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    CAMERA_WIDTH_PX_FIELD_NUMBER: _ClassVar[int]
    CAMERA_HEIGHT_PX_FIELD_NUMBER: _ClassVar[int]
    CAMERA_FPS_FIELD_NUMBER: _ClassVar[int]
    AUDIO_SAMPLE_RATE_HZ_FIELD_NUMBER: _ClassVar[int]
    AUDIO_CHANNELS_FIELD_NUMBER: _ClassVar[int]
    DETECTION_STATUS_FIELD_NUMBER: _ClassVar[int]
    camera_device_id: str
    microphone_device_id: str
    camera_width_px: int
    camera_height_px: int
    camera_fps: int
    audio_sample_rate_hz: int
    audio_channels: int
    detection_status: str
    def __init__(self, camera_device_id: _Optional[str] = ..., microphone_device_id: _Optional[str] = ..., camera_width_px: _Optional[int] = ..., camera_height_px: _Optional[int] = ..., camera_fps: _Optional[int] = ..., audio_sample_rate_hz: _Optional[int] = ..., audio_channels: _Optional[int] = ..., detection_status: _Optional[str] = ...) -> None: ...

class MonitorStatus(_message.Message):
    __slots__ = ("code", "message")
    CODE_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    code: str
    message: str
    def __init__(self, code: _Optional[str] = ..., message: _Optional[str] = ...) -> None: ...

class MonitorCameraFrame(_message.Message):
    __slots__ = ("jpeg", "width_px", "height_px", "sequence", "source_timestamp_ns", "captured_at", "dropped_frames")
    JPEG_FIELD_NUMBER: _ClassVar[int]
    WIDTH_PX_FIELD_NUMBER: _ClassVar[int]
    HEIGHT_PX_FIELD_NUMBER: _ClassVar[int]
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    SOURCE_TIMESTAMP_NS_FIELD_NUMBER: _ClassVar[int]
    CAPTURED_AT_FIELD_NUMBER: _ClassVar[int]
    DROPPED_FRAMES_FIELD_NUMBER: _ClassVar[int]
    jpeg: bytes
    width_px: int
    height_px: int
    sequence: int
    source_timestamp_ns: int
    captured_at: str
    dropped_frames: int
    def __init__(self, jpeg: _Optional[bytes] = ..., width_px: _Optional[int] = ..., height_px: _Optional[int] = ..., sequence: _Optional[int] = ..., source_timestamp_ns: _Optional[int] = ..., captured_at: _Optional[str] = ..., dropped_frames: _Optional[int] = ...) -> None: ...

class MonitorDetectionFrame(_message.Message):
    __slots__ = ("jpeg", "width_px", "height_px", "sequence", "source_timestamp_ns", "detection_json", "generation")
    JPEG_FIELD_NUMBER: _ClassVar[int]
    WIDTH_PX_FIELD_NUMBER: _ClassVar[int]
    HEIGHT_PX_FIELD_NUMBER: _ClassVar[int]
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    SOURCE_TIMESTAMP_NS_FIELD_NUMBER: _ClassVar[int]
    DETECTION_JSON_FIELD_NUMBER: _ClassVar[int]
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    jpeg: bytes
    width_px: int
    height_px: int
    sequence: int
    source_timestamp_ns: int
    detection_json: str
    generation: int
    def __init__(self, jpeg: _Optional[bytes] = ..., width_px: _Optional[int] = ..., height_px: _Optional[int] = ..., sequence: _Optional[int] = ..., source_timestamp_ns: _Optional[int] = ..., detection_json: _Optional[str] = ..., generation: _Optional[int] = ...) -> None: ...

class MonitorAudioEnergy(_message.Message):
    __slots__ = ("rms", "peak", "dbfs", "clipping", "sequence", "source_timestamp_ns", "sample_rate_hz", "channels", "captured_at")
    RMS_FIELD_NUMBER: _ClassVar[int]
    PEAK_FIELD_NUMBER: _ClassVar[int]
    DBFS_FIELD_NUMBER: _ClassVar[int]
    CLIPPING_FIELD_NUMBER: _ClassVar[int]
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    SOURCE_TIMESTAMP_NS_FIELD_NUMBER: _ClassVar[int]
    SAMPLE_RATE_HZ_FIELD_NUMBER: _ClassVar[int]
    CHANNELS_FIELD_NUMBER: _ClassVar[int]
    CAPTURED_AT_FIELD_NUMBER: _ClassVar[int]
    rms: float
    peak: float
    dbfs: float
    clipping: bool
    sequence: int
    source_timestamp_ns: int
    sample_rate_hz: int
    channels: int
    captured_at: str
    def __init__(self, rms: _Optional[float] = ..., peak: _Optional[float] = ..., dbfs: _Optional[float] = ..., clipping: _Optional[bool] = ..., sequence: _Optional[int] = ..., source_timestamp_ns: _Optional[int] = ..., sample_rate_hz: _Optional[int] = ..., channels: _Optional[int] = ..., captured_at: _Optional[str] = ...) -> None: ...

class MonitorEvent(_message.Message):
    __slots__ = ("ready", "status", "camera", "audio", "detection")
    READY_FIELD_NUMBER: _ClassVar[int]
    STATUS_FIELD_NUMBER: _ClassVar[int]
    CAMERA_FIELD_NUMBER: _ClassVar[int]
    AUDIO_FIELD_NUMBER: _ClassVar[int]
    DETECTION_FIELD_NUMBER: _ClassVar[int]
    ready: MonitorReady
    status: MonitorStatus
    camera: MonitorCameraFrame
    audio: MonitorAudioEnergy
    detection: MonitorDetectionFrame
    def __init__(self, ready: _Optional[_Union[MonitorReady, _Mapping]] = ..., status: _Optional[_Union[MonitorStatus, _Mapping]] = ..., camera: _Optional[_Union[MonitorCameraFrame, _Mapping]] = ..., audio: _Optional[_Union[MonitorAudioEnergy, _Mapping]] = ..., detection: _Optional[_Union[MonitorDetectionFrame, _Mapping]] = ...) -> None: ...

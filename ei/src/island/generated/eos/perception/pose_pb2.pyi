# Generated from in-repository share/eos/contracts.
from island.generated.eos.perception import humans_pb2 as _humans_pb2
from island.generated.eos.perception import media_pb2 as _media_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class EstimateHumanPoseRequest(_message.Message):
    __slots__ = ("model_id", "input", "depth_input", "depth_scale", "persons", "confidence_threshold", "keypoint_confidence_threshold", "furniture")
    MODEL_ID_FIELD_NUMBER: _ClassVar[int]
    INPUT_FIELD_NUMBER: _ClassVar[int]
    DEPTH_INPUT_FIELD_NUMBER: _ClassVar[int]
    DEPTH_SCALE_FIELD_NUMBER: _ClassVar[int]
    PERSONS_FIELD_NUMBER: _ClassVar[int]
    CONFIDENCE_THRESHOLD_FIELD_NUMBER: _ClassVar[int]
    KEYPOINT_CONFIDENCE_THRESHOLD_FIELD_NUMBER: _ClassVar[int]
    FURNITURE_FIELD_NUMBER: _ClassVar[int]
    model_id: str
    input: _media_pb2.PerceptionMedia
    depth_input: _media_pb2.PerceptionMedia
    depth_scale: float
    persons: _containers.RepeatedCompositeFieldContainer[_humans_pb2.TrackedHuman]
    confidence_threshold: float
    keypoint_confidence_threshold: float
    furniture: _containers.RepeatedCompositeFieldContainer[_humans_pb2.DetectedFurniture]
    def __init__(self, model_id: _Optional[str] = ..., input: _Optional[_Union[_media_pb2.PerceptionMedia, _Mapping]] = ..., depth_input: _Optional[_Union[_media_pb2.PerceptionMedia, _Mapping]] = ..., depth_scale: _Optional[float] = ..., persons: _Optional[_Iterable[_Union[_humans_pb2.TrackedHuman, _Mapping]]] = ..., confidence_threshold: _Optional[float] = ..., keypoint_confidence_threshold: _Optional[float] = ..., furniture: _Optional[_Iterable[_Union[_humans_pb2.DetectedFurniture, _Mapping]]] = ...) -> None: ...

class Point2d(_message.Message):
    __slots__ = ("x", "y")
    X_FIELD_NUMBER: _ClassVar[int]
    Y_FIELD_NUMBER: _ClassVar[int]
    x: float
    y: float
    def __init__(self, x: _Optional[float] = ..., y: _Optional[float] = ...) -> None: ...

class HumanPose(_message.Message):
    __slots__ = ("track_id", "bbox", "confidence", "distance_m", "keypoints", "keypoint_scores", "posture_state", "posture", "posture_confidence", "behavior_event", "behavior_confidence", "pose", "pose_confidence", "action", "action_confidence")
    TRACK_ID_FIELD_NUMBER: _ClassVar[int]
    BBOX_FIELD_NUMBER: _ClassVar[int]
    CONFIDENCE_FIELD_NUMBER: _ClassVar[int]
    DISTANCE_M_FIELD_NUMBER: _ClassVar[int]
    KEYPOINTS_FIELD_NUMBER: _ClassVar[int]
    KEYPOINT_SCORES_FIELD_NUMBER: _ClassVar[int]
    POSTURE_STATE_FIELD_NUMBER: _ClassVar[int]
    POSTURE_FIELD_NUMBER: _ClassVar[int]
    POSTURE_CONFIDENCE_FIELD_NUMBER: _ClassVar[int]
    BEHAVIOR_EVENT_FIELD_NUMBER: _ClassVar[int]
    BEHAVIOR_CONFIDENCE_FIELD_NUMBER: _ClassVar[int]
    POSE_FIELD_NUMBER: _ClassVar[int]
    POSE_CONFIDENCE_FIELD_NUMBER: _ClassVar[int]
    ACTION_FIELD_NUMBER: _ClassVar[int]
    ACTION_CONFIDENCE_FIELD_NUMBER: _ClassVar[int]
    track_id: int
    bbox: _media_pb2.BoundingBox
    confidence: float
    distance_m: float
    keypoints: _containers.RepeatedCompositeFieldContainer[Point2d]
    keypoint_scores: _containers.RepeatedScalarFieldContainer[float]
    posture_state: str
    posture: str
    posture_confidence: float
    behavior_event: str
    behavior_confidence: float
    pose: str
    pose_confidence: float
    action: str
    action_confidence: float
    def __init__(self, track_id: _Optional[int] = ..., bbox: _Optional[_Union[_media_pb2.BoundingBox, _Mapping]] = ..., confidence: _Optional[float] = ..., distance_m: _Optional[float] = ..., keypoints: _Optional[_Iterable[_Union[Point2d, _Mapping]]] = ..., keypoint_scores: _Optional[_Iterable[float]] = ..., posture_state: _Optional[str] = ..., posture: _Optional[str] = ..., posture_confidence: _Optional[float] = ..., behavior_event: _Optional[str] = ..., behavior_confidence: _Optional[float] = ..., pose: _Optional[str] = ..., pose_confidence: _Optional[float] = ..., action: _Optional[str] = ..., action_confidence: _Optional[float] = ...) -> None: ...

class EstimateHumanPoseResponse(_message.Message):
    __slots__ = ("persons", "person_count")
    PERSONS_FIELD_NUMBER: _ClassVar[int]
    PERSON_COUNT_FIELD_NUMBER: _ClassVar[int]
    persons: _containers.RepeatedCompositeFieldContainer[HumanPose]
    person_count: int
    def __init__(self, persons: _Optional[_Iterable[_Union[HumanPose, _Mapping]]] = ..., person_count: _Optional[int] = ...) -> None: ...

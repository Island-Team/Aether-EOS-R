# Generated from in-repository share/eos/contracts.
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class NavigationPoint(_message.Message):
    __slots__ = ("name", "x_m", "y_m", "yaw_rad")
    NAME_FIELD_NUMBER: _ClassVar[int]
    X_M_FIELD_NUMBER: _ClassVar[int]
    Y_M_FIELD_NUMBER: _ClassVar[int]
    YAW_RAD_FIELD_NUMBER: _ClassVar[int]
    name: str
    x_m: float
    y_m: float
    yaw_rad: float
    def __init__(self, name: _Optional[str] = ..., x_m: _Optional[float] = ..., y_m: _Optional[float] = ..., yaw_rad: _Optional[float] = ...) -> None: ...

class ListNavigationPointsRequest(_message.Message):
    __slots__ = ("robot_name",)
    ROBOT_NAME_FIELD_NUMBER: _ClassVar[int]
    robot_name: str
    def __init__(self, robot_name: _Optional[str] = ...) -> None: ...

class ListNavigationPointsResponse(_message.Message):
    __slots__ = ("points",)
    POINTS_FIELD_NUMBER: _ClassVar[int]
    points: _containers.RepeatedCompositeFieldContainer[NavigationPoint]
    def __init__(self, points: _Optional[_Iterable[_Union[NavigationPoint, _Mapping]]] = ...) -> None: ...

class GetNavigationPointRequest(_message.Message):
    __slots__ = ("robot_name", "name")
    ROBOT_NAME_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    robot_name: str
    name: str
    def __init__(self, robot_name: _Optional[str] = ..., name: _Optional[str] = ...) -> None: ...

class PutNavigationPointRequest(_message.Message):
    __slots__ = ("robot_name", "name", "x_m", "y_m", "yaw_rad")
    ROBOT_NAME_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    X_M_FIELD_NUMBER: _ClassVar[int]
    Y_M_FIELD_NUMBER: _ClassVar[int]
    YAW_RAD_FIELD_NUMBER: _ClassVar[int]
    robot_name: str
    name: str
    x_m: float
    y_m: float
    yaw_rad: float
    def __init__(self, robot_name: _Optional[str] = ..., name: _Optional[str] = ..., x_m: _Optional[float] = ..., y_m: _Optional[float] = ..., yaw_rad: _Optional[float] = ...) -> None: ...

class ReloadNavigationPointsResponse(_message.Message):
    __slots__ = ("point_count",)
    POINT_COUNT_FIELD_NUMBER: _ClassVar[int]
    point_count: int
    def __init__(self, point_count: _Optional[int] = ...) -> None: ...

# Generated from in-repository share/eos/contracts.
from google.protobuf import descriptor_pb2 as _descriptor_pb2
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional

DESCRIPTOR: _descriptor.FileDescriptor
HTTP_ROUTE_FIELD_NUMBER: _ClassVar[int]
http_route: _descriptor.FieldDescriptor

class HttpRoute(_message.Message):
    __slots__ = ("method", "path", "transport", "request_type", "response_type", "summary")
    METHOD_FIELD_NUMBER: _ClassVar[int]
    PATH_FIELD_NUMBER: _ClassVar[int]
    TRANSPORT_FIELD_NUMBER: _ClassVar[int]
    REQUEST_TYPE_FIELD_NUMBER: _ClassVar[int]
    RESPONSE_TYPE_FIELD_NUMBER: _ClassVar[int]
    SUMMARY_FIELD_NUMBER: _ClassVar[int]
    method: str
    path: str
    transport: str
    request_type: str
    response_type: str
    summary: str
    def __init__(self, method: _Optional[str] = ..., path: _Optional[str] = ..., transport: _Optional[str] = ..., request_type: _Optional[str] = ..., response_type: _Optional[str] = ..., summary: _Optional[str] = ...) -> None: ...

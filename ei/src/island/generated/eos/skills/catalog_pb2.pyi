# Generated from in-repository share/eos/contracts.
from google.protobuf import struct_pb2 as _struct_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class SkillSourceAuthority(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    SKILL_SOURCE_AUTHORITY_UNSPECIFIED: _ClassVar[SkillSourceAuthority]
    SKILL_SOURCE_AUTHORITY_READ: _ClassVar[SkillSourceAuthority]
    SKILL_SOURCE_AUTHORITY_WRITE: _ClassVar[SkillSourceAuthority]

class SkillInferenceAuthority(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    SKILL_INFERENCE_AUTHORITY_UNSPECIFIED: _ClassVar[SkillInferenceAuthority]
    SKILL_INFERENCE_AUTHORITY_VAD: _ClassVar[SkillInferenceAuthority]
    SKILL_INFERENCE_AUTHORITY_HUMAN: _ClassVar[SkillInferenceAuthority]
    SKILL_INFERENCE_AUTHORITY_HUMAN_POSE: _ClassVar[SkillInferenceAuthority]
    SKILL_INFERENCE_AUTHORITY_FACE_RECOGNITION: _ClassVar[SkillInferenceAuthority]
    SKILL_INFERENCE_AUTHORITY_CHAT_COMPLETION: _ClassVar[SkillInferenceAuthority]
    SKILL_INFERENCE_AUTHORITY_VLM: _ClassVar[SkillInferenceAuthority]

class SkillClaimKind(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    SKILL_CLAIM_KIND_UNSPECIFIED: _ClassVar[SkillClaimKind]
    SKILL_CLAIM_KIND_DEVICE: _ClassVar[SkillClaimKind]
    SKILL_CLAIM_KIND_RESOURCE: _ClassVar[SkillClaimKind]
    SKILL_CLAIM_KIND_PROVIDER_CONTEXT: _ClassVar[SkillClaimKind]

class SkillClaimMode(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    SKILL_CLAIM_MODE_UNSPECIFIED: _ClassVar[SkillClaimMode]
    SKILL_CLAIM_MODE_SHARED_READ: _ClassVar[SkillClaimMode]
    SKILL_CLAIM_MODE_EXCLUSIVE_USE: _ClassVar[SkillClaimMode]
SKILL_SOURCE_AUTHORITY_UNSPECIFIED: SkillSourceAuthority
SKILL_SOURCE_AUTHORITY_READ: SkillSourceAuthority
SKILL_SOURCE_AUTHORITY_WRITE: SkillSourceAuthority
SKILL_INFERENCE_AUTHORITY_UNSPECIFIED: SkillInferenceAuthority
SKILL_INFERENCE_AUTHORITY_VAD: SkillInferenceAuthority
SKILL_INFERENCE_AUTHORITY_HUMAN: SkillInferenceAuthority
SKILL_INFERENCE_AUTHORITY_HUMAN_POSE: SkillInferenceAuthority
SKILL_INFERENCE_AUTHORITY_FACE_RECOGNITION: SkillInferenceAuthority
SKILL_INFERENCE_AUTHORITY_CHAT_COMPLETION: SkillInferenceAuthority
SKILL_INFERENCE_AUTHORITY_VLM: SkillInferenceAuthority
SKILL_CLAIM_KIND_UNSPECIFIED: SkillClaimKind
SKILL_CLAIM_KIND_DEVICE: SkillClaimKind
SKILL_CLAIM_KIND_RESOURCE: SkillClaimKind
SKILL_CLAIM_KIND_PROVIDER_CONTEXT: SkillClaimKind
SKILL_CLAIM_MODE_UNSPECIFIED: SkillClaimMode
SKILL_CLAIM_MODE_SHARED_READ: SkillClaimMode
SKILL_CLAIM_MODE_EXCLUSIVE_USE: SkillClaimMode

class SkillReference(_message.Message):
    __slots__ = ("name", "version")
    NAME_FIELD_NUMBER: _ClassVar[int]
    VERSION_FIELD_NUMBER: _ClassVar[int]
    name: str
    version: str
    def __init__(self, name: _Optional[str] = ..., version: _Optional[str] = ...) -> None: ...

class SkillPrimitiveReference(_message.Message):
    __slots__ = ("name", "version")
    NAME_FIELD_NUMBER: _ClassVar[int]
    VERSION_FIELD_NUMBER: _ClassVar[int]
    name: str
    version: str
    def __init__(self, name: _Optional[str] = ..., version: _Optional[str] = ...) -> None: ...

class SkillResource(_message.Message):
    __slots__ = ("uri_template", "description")
    URI_TEMPLATE_FIELD_NUMBER: _ClassVar[int]
    DESCRIPTION_FIELD_NUMBER: _ClassVar[int]
    uri_template: str
    description: str
    def __init__(self, uri_template: _Optional[str] = ..., description: _Optional[str] = ...) -> None: ...

class SkillSignalDescriptor(_message.Message):
    __slots__ = ("name", "payload_schema")
    NAME_FIELD_NUMBER: _ClassVar[int]
    PAYLOAD_SCHEMA_FIELD_NUMBER: _ClassVar[int]
    name: str
    payload_schema: _struct_pb2.Struct
    def __init__(self, name: _Optional[str] = ..., payload_schema: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class SkillSourceDescriptor(_message.Message):
    __slots__ = ("topic", "schema", "authority")
    TOPIC_FIELD_NUMBER: _ClassVar[int]
    SCHEMA_FIELD_NUMBER: _ClassVar[int]
    AUTHORITY_FIELD_NUMBER: _ClassVar[int]
    topic: str
    schema: _struct_pb2.Struct
    authority: SkillSourceAuthority
    def __init__(self, topic: _Optional[str] = ..., schema: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., authority: _Optional[_Union[SkillSourceAuthority, str]] = ...) -> None: ...

class SkillIpcChannelDescriptor(_message.Message):
    __slots__ = ("name", "payload_schema", "can_send", "can_receive")
    NAME_FIELD_NUMBER: _ClassVar[int]
    PAYLOAD_SCHEMA_FIELD_NUMBER: _ClassVar[int]
    CAN_SEND_FIELD_NUMBER: _ClassVar[int]
    CAN_RECEIVE_FIELD_NUMBER: _ClassVar[int]
    name: str
    payload_schema: _struct_pb2.Struct
    can_send: bool
    can_receive: bool
    def __init__(self, name: _Optional[str] = ..., payload_schema: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., can_send: _Optional[bool] = ..., can_receive: _Optional[bool] = ...) -> None: ...

class SkillClaimTemplate(_message.Message):
    __slots__ = ("kind", "mode", "argument_json_pointer")
    KIND_FIELD_NUMBER: _ClassVar[int]
    MODE_FIELD_NUMBER: _ClassVar[int]
    ARGUMENT_JSON_POINTER_FIELD_NUMBER: _ClassVar[int]
    kind: SkillClaimKind
    mode: SkillClaimMode
    argument_json_pointer: str
    def __init__(self, kind: _Optional[_Union[SkillClaimKind, str]] = ..., mode: _Optional[_Union[SkillClaimMode, str]] = ..., argument_json_pointer: _Optional[str] = ...) -> None: ...

class SkillClaim(_message.Message):
    __slots__ = ("kind", "mode", "identity")
    KIND_FIELD_NUMBER: _ClassVar[int]
    MODE_FIELD_NUMBER: _ClassVar[int]
    IDENTITY_FIELD_NUMBER: _ClassVar[int]
    kind: SkillClaimKind
    mode: SkillClaimMode
    identity: str
    def __init__(self, kind: _Optional[_Union[SkillClaimKind, str]] = ..., mode: _Optional[_Union[SkillClaimMode, str]] = ..., identity: _Optional[str] = ...) -> None: ...

class SkillDescriptor(_message.Message):
    __slots__ = ("reference", "description", "parameters", "returns", "primitives", "resources", "accepted_signals", "emitted_signals", "claims", "reentrant", "sources", "ipc_channels", "inference_authorities", "execution_state_schema")
    REFERENCE_FIELD_NUMBER: _ClassVar[int]
    DESCRIPTION_FIELD_NUMBER: _ClassVar[int]
    PARAMETERS_FIELD_NUMBER: _ClassVar[int]
    RETURNS_FIELD_NUMBER: _ClassVar[int]
    PRIMITIVES_FIELD_NUMBER: _ClassVar[int]
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    ACCEPTED_SIGNALS_FIELD_NUMBER: _ClassVar[int]
    EMITTED_SIGNALS_FIELD_NUMBER: _ClassVar[int]
    CLAIMS_FIELD_NUMBER: _ClassVar[int]
    REENTRANT_FIELD_NUMBER: _ClassVar[int]
    SOURCES_FIELD_NUMBER: _ClassVar[int]
    IPC_CHANNELS_FIELD_NUMBER: _ClassVar[int]
    INFERENCE_AUTHORITIES_FIELD_NUMBER: _ClassVar[int]
    EXECUTION_STATE_SCHEMA_FIELD_NUMBER: _ClassVar[int]
    reference: SkillReference
    description: str
    parameters: _struct_pb2.Struct
    returns: _struct_pb2.Struct
    primitives: _containers.RepeatedCompositeFieldContainer[SkillPrimitiveReference]
    resources: _containers.RepeatedCompositeFieldContainer[SkillResource]
    accepted_signals: _containers.RepeatedCompositeFieldContainer[SkillSignalDescriptor]
    emitted_signals: _containers.RepeatedCompositeFieldContainer[SkillSignalDescriptor]
    claims: _containers.RepeatedCompositeFieldContainer[SkillClaimTemplate]
    reentrant: bool
    sources: _containers.RepeatedCompositeFieldContainer[SkillSourceDescriptor]
    ipc_channels: _containers.RepeatedCompositeFieldContainer[SkillIpcChannelDescriptor]
    inference_authorities: _containers.RepeatedScalarFieldContainer[SkillInferenceAuthority]
    execution_state_schema: _struct_pb2.Struct
    def __init__(self, reference: _Optional[_Union[SkillReference, _Mapping]] = ..., description: _Optional[str] = ..., parameters: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., returns: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ..., primitives: _Optional[_Iterable[_Union[SkillPrimitiveReference, _Mapping]]] = ..., resources: _Optional[_Iterable[_Union[SkillResource, _Mapping]]] = ..., accepted_signals: _Optional[_Iterable[_Union[SkillSignalDescriptor, _Mapping]]] = ..., emitted_signals: _Optional[_Iterable[_Union[SkillSignalDescriptor, _Mapping]]] = ..., claims: _Optional[_Iterable[_Union[SkillClaimTemplate, _Mapping]]] = ..., reentrant: _Optional[bool] = ..., sources: _Optional[_Iterable[_Union[SkillSourceDescriptor, _Mapping]]] = ..., ipc_channels: _Optional[_Iterable[_Union[SkillIpcChannelDescriptor, _Mapping]]] = ..., inference_authorities: _Optional[_Iterable[_Union[SkillInferenceAuthority, str]]] = ..., execution_state_schema: _Optional[_Union[_struct_pb2.Struct, _Mapping]] = ...) -> None: ...

class ListSkillsRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class ListSkillsResponse(_message.Message):
    __slots__ = ("skills",)
    SKILLS_FIELD_NUMBER: _ClassVar[int]
    skills: _containers.RepeatedCompositeFieldContainer[SkillDescriptor]
    def __init__(self, skills: _Optional[_Iterable[_Union[SkillDescriptor, _Mapping]]] = ...) -> None: ...

class GetSkillRequest(_message.Message):
    __slots__ = ("reference",)
    REFERENCE_FIELD_NUMBER: _ClassVar[int]
    reference: SkillReference
    def __init__(self, reference: _Optional[_Union[SkillReference, _Mapping]] = ...) -> None: ...

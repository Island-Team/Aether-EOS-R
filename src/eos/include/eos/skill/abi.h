/// Stable C ABI for externally authored native EOS skill libraries.
///
/// Skill libraries are trusted in-process extensions. They may request only
/// declared capabilities through the supplied host interface; no EOS C++ type
/// or backend handle crosses this boundary.
#ifndef EOS_SKILL_ABI_H_
#define EOS_SKILL_ABI_H_

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define EOS_SKILL_EXPORT __attribute__((visibility("default")))
#else
#define EOS_SKILL_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define EOS_SKILL_ABI_VERSION_V2 2u
#define EOS_SKILL_QUERY_SYMBOL_V2 "eos_skill_library_query_v2"

/// Borrowed UTF-8 bytes. Empty text is represented by `{NULL, 0}`.
typedef struct eos_skill_text_v2 {
  const char* data;
  uint32_t size;
} eos_skill_text_v2;

typedef uint32_t eos_skill_host_status_v2;
#define EOS_SKILL_HOST_OK 0u
#define EOS_SKILL_HOST_INVALID_ARGUMENT 1u
#define EOS_SKILL_HOST_NOT_FOUND 2u
#define EOS_SKILL_HOST_UNAVAILABLE 3u
#define EOS_SKILL_HOST_INTERNAL 4u

/// One request from skill code to the EOS primitive boundary.
typedef struct eos_skill_primitive_request_v2 {
  uint32_t struct_size;              ///< `sizeof(eos_skill_primitive_request_v2)`.
  eos_skill_text_v2 primitive;       ///< Declared canonical primitive name.
  eos_skill_text_v2 version;         ///< Declared exact primitive SemVer.
  eos_skill_text_v2 device_id;       ///< Generated EOS target; empty only for estop.
  eos_skill_text_v2 arguments_json;  ///< JSON object; EOS owns type and command_id.
} eos_skill_primitive_request_v2;

/// Host-owned response valid until the next host call or execute returns.
typedef struct eos_skill_primitive_response_v2 {
  uint32_t struct_size;  ///< `sizeof(eos_skill_primitive_response_v2)`.
  eos_skill_text_v2
      acknowledgement_json;         ///< Canonical ack; accepted calls have a terminal result.
  eos_skill_text_v2 error_message;  ///< Diagnostic text when host status is not OK.
} eos_skill_primitive_response_v2;

typedef eos_skill_host_status_v2 (*eos_skill_call_primitive_v2)(
    void* host_context, const eos_skill_primitive_request_v2* request,
    eos_skill_primitive_response_v2* response);

/// Host-owned start response valid until the next asynchronous primitive call.
typedef struct eos_skill_primitive_start_response_v2 {
  uint32_t struct_size;
  uint64_t operation_id;             ///< Non-zero invocation-scoped operation token.
  eos_skill_text_v2 error_message;  ///< Diagnostic text when host status is not OK.
} eos_skill_primitive_start_response_v2;

/// Host-owned wait response valid until the next asynchronous primitive call.
typedef struct eos_skill_primitive_wait_response_v2 {
  uint32_t struct_size;
  uint32_t completed;                ///< Exactly one when acknowledgement_json is terminal.
  eos_skill_text_v2 acknowledgement_json;
  eos_skill_text_v2 error_message;
} eos_skill_primitive_wait_response_v2;

typedef eos_skill_host_status_v2 (*eos_skill_start_primitive_v2)(
    void* host_context, const eos_skill_primitive_request_v2* request,
    eos_skill_primitive_start_response_v2* response);

/// Waits for an asynchronous primitive. A zero timeout performs a poll.
typedef eos_skill_host_status_v2 (*eos_skill_wait_primitive_v2)(
    void* host_context, uint64_t operation_id, uint32_t timeout_ms,
    eos_skill_primitive_wait_response_v2* response);

typedef eos_skill_host_status_v2 (*eos_skill_cancel_primitive_v2)(void* host_context,
                                                                  uint64_t operation_id);

/// One read of a concrete URI covered by the descriptor's resource templates.
typedef struct eos_skill_resource_request_v2 {
  uint32_t struct_size;   ///< `sizeof(eos_skill_resource_request_v2)`.
  eos_skill_text_v2 uri;  ///< Concrete EOS resource URI.
} eos_skill_resource_request_v2;

/// Host-owned response valid until the next resource read or execute returns.
typedef struct eos_skill_resource_response_v2 {
  uint32_t struct_size;             ///< `sizeof(eos_skill_resource_response_v2)`.
  eos_skill_text_v2 value_json;     ///< Canonical resource snapshot when status is OK.
  eos_skill_text_v2 error_message;  ///< Diagnostic text when status is not OK.
} eos_skill_resource_response_v2;

typedef eos_skill_host_status_v2 (*eos_skill_read_resource_v2)(
    void* host_context, const eos_skill_resource_request_v2* request,
    eos_skill_resource_response_v2* response);

typedef uint32_t (*eos_skill_is_cancelled_v2)(void* host_context);

typedef uint32_t eos_skill_signal_receive_status_v2;
#define EOS_SKILL_SIGNAL_RECEIVED 0u
#define EOS_SKILL_SIGNAL_TIMEOUT 1u
#define EOS_SKILL_SIGNAL_CLOSED 2u
#define EOS_SKILL_SIGNAL_RECEIVE_INVALID_ARGUMENT 3u
#define EOS_SKILL_SIGNAL_RECEIVE_INTERNAL 4u

/// Host-owned SIGNAL value. Text remains valid until the next host callback.
typedef struct eos_skill_signal_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 payload_json;
} eos_skill_signal_v2;

/// Waits for the next accepted SIGNAL. A zero timeout performs a poll.
typedef eos_skill_signal_receive_status_v2 (*eos_skill_receive_signal_v2)(
    void* host_context, uint32_t timeout_ms, eos_skill_signal_v2* signal);

/// Emits one declared SIGNAL. The payload must encode a JSON object.
typedef eos_skill_host_status_v2 (*eos_skill_emit_signal_v2)(void* host_context,
                                                             const eos_skill_signal_v2* signal);

typedef uint32_t eos_skill_log_level_v2;
#define EOS_SKILL_LOG_TRACE 0u
#define EOS_SKILL_LOG_DEBUG 1u
#define EOS_SKILL_LOG_INFO 2u
#define EOS_SKILL_LOG_WARN 3u
#define EOS_SKILL_LOG_ERROR 4u
#define EOS_SKILL_LOG_CRITICAL 5u

/// One structured operational diagnostic emitted by skill code.
typedef struct eos_skill_log_record_v2 {
  uint32_t struct_size;
  eos_skill_log_level_v2 level;
  eos_skill_text_v2 event_name;
  eos_skill_text_v2 message;
  eos_skill_text_v2 fields_json;
} eos_skill_log_record_v2;

typedef eos_skill_host_status_v2 (*eos_skill_log_v2)(void* host_context,
                                                     const eos_skill_log_record_v2* record);

/// One bounded, Agent-visible execution milestone.
typedef struct eos_skill_checkpoint_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 message;
  uint32_t has_progress;
  double progress;
  eos_skill_text_v2 fields_json;
} eos_skill_checkpoint_v2;

typedef eos_skill_host_status_v2 (*eos_skill_emit_checkpoint_v2)(
    void* host_context, const eos_skill_checkpoint_v2* checkpoint);

/// One bounded terminal diagnostic for a non-OK execute return.
typedef struct eos_skill_failure_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 reason_code;  ///< Stable machine-readable skill-owned code.
  eos_skill_text_v2 message;      ///< Optional bounded operator-facing diagnostic.
} eos_skill_failure_v2;

typedef eos_skill_host_status_v2 (*eos_skill_report_failure_v2)(
    void* host_context, const eos_skill_failure_v2* failure);

typedef uint32_t eos_skill_source_lookup_v2;
#define EOS_SKILL_SOURCE_LATEST 0u
#define EOS_SKILL_SOURCE_EXACT 1u
#define EOS_SKILL_SOURCE_BEFORE 2u
#define EOS_SKILL_SOURCE_AFTER 3u
#define EOS_SKILL_SOURCE_NEAREST 4u

typedef struct eos_skill_source_sample_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 topic;
  int64_t source_timestamp_ns;
  uint64_t sequence;
  eos_skill_text_v2 value_json;
} eos_skill_source_sample_v2;

typedef struct eos_skill_source_query_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 topic;
  eos_skill_source_lookup_v2 lookup;
  int64_t source_timestamp_ns;
} eos_skill_source_query_v2;

typedef struct eos_skill_source_response_v2 {
  uint32_t struct_size;
  uint32_t has_sample;
  eos_skill_source_sample_v2 sample;
  eos_skill_text_v2 error_message;
} eos_skill_source_response_v2;

typedef eos_skill_host_status_v2 (*eos_skill_publish_source_v2)(
    void*, const eos_skill_source_sample_v2*, eos_skill_source_response_v2*);
typedef eos_skill_host_status_v2 (*eos_skill_query_source_v2)(
    void*, const eos_skill_source_query_v2*, eos_skill_source_response_v2*);
typedef eos_skill_host_status_v2 (*eos_skill_wait_source_v2)(
    void*, eos_skill_text_v2 topic, uint64_t after_sequence, uint32_t timeout_ms,
    eos_skill_source_response_v2*);

typedef uint32_t eos_skill_inference_authority_v2;
#define EOS_SKILL_INFERENCE_VAD 1u
#define EOS_SKILL_INFERENCE_HUMAN 2u
#define EOS_SKILL_INFERENCE_HUMAN_POSE 3u
#define EOS_SKILL_INFERENCE_FACE_RECOGNITION 4u
#define EOS_SKILL_INFERENCE_CHAT_COMPLETION 5u
#define EOS_SKILL_INFERENCE_VLM 6u

/// A one-shot inference request. Chat completion requires empty/zero source
/// provenance and carries its text request in `options_json`. VLM requires
/// messages and accepts text only, one inline JPEG, or one exact retained frame. Vision
/// authorities require either one exact frame retained by the host or
/// caller-supplied canonical media in `options_json.input` (and optional
/// `depth_input`), never both. Inline vision
/// and VAD use empty/zero source provenance. VAD always uses canonical audio in
/// `options_json.input`.
typedef struct eos_skill_inference_request_v2 {
  uint32_t struct_size;
  eos_skill_inference_authority_v2 authority;
  eos_skill_text_v2 source;
  uint64_t source_sequence;
  uint64_t source_generation;
  eos_skill_text_v2 options_json;
} eos_skill_inference_request_v2;

typedef struct eos_skill_inference_response_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 result_json;
  eos_skill_text_v2 error_message;
} eos_skill_inference_response_v2;

typedef eos_skill_host_status_v2 (*eos_skill_infer_once_v2)(
    void*, const eos_skill_inference_request_v2*, eos_skill_inference_response_v2*);

/// Capabilities exposed for one synchronous execution.
typedef struct eos_skill_host_v2 {
  uint32_t abi_version;                          ///< `EOS_SKILL_ABI_VERSION_V2`.
  uint32_t struct_size;                          ///< `sizeof(eos_skill_host_v2)`.
  void* context;                                 ///< Opaque invocation-scoped EOS context.
  eos_skill_call_primitive_v2 call_primitive;    ///< Synchronous terminal primitive interface.
  eos_skill_is_cancelled_v2 is_cancelled;        ///< Cooperative cancellation probe.
  eos_skill_read_resource_v2 read_resource;      ///< Declared read-only resource interface.
  eos_skill_receive_signal_v2 receive_signal;    ///< Receives declared inbound SIGNALs.
  eos_skill_emit_signal_v2 emit_signal;          ///< Emits declared outbound SIGNALs.
  eos_skill_log_v2 log;                          ///< Optional structured diagnostic interface.
  eos_skill_emit_checkpoint_v2 emit_checkpoint;  ///< Optional public journal interface.
  eos_skill_report_failure_v2 report_failure;    ///< Optional terminal failure diagnostic.
  eos_skill_start_primitive_v2 start_primitive;  ///< Optional nonblocking primitive release.
  eos_skill_wait_primitive_v2 wait_primitive;    ///< Optional primitive poll/wait interface.
  eos_skill_cancel_primitive_v2 cancel_primitive;  ///< Optional correlated cancellation.
  eos_skill_publish_source_v2 publish_source;      ///< Optional timestamped source publication.
  eos_skill_query_source_v2 query_source;          ///< Optional timestamped source lookup.
  eos_skill_wait_source_v2 wait_source;            ///< Optional wait for a newer source sample.
  eos_skill_infer_once_v2 infer_once;               ///< Optional typed one-shot inference.
} eos_skill_host_v2;

/// Frozen size of the original ABI v2 host table. Optional appended callbacks
/// must be guarded by their individual member boundary before being read.
#define EOS_SKILL_HOST_V2_BASE_SIZE \
  ((uint32_t)(offsetof(eos_skill_host_v2, emit_signal) + sizeof(eos_skill_emit_signal_v2)))

#define EOS_SKILL_HOST_V2_HAS(host, member) \
  ((host) != NULL && (host)->struct_size >= \
                         (uint32_t)(offsetof(eos_skill_host_v2, member) + sizeof((host)->member)))

/// One synchronous skill invocation. All members are borrowed for the call.
typedef struct eos_skill_execution_v2 {
  uint32_t struct_size;              ///< `sizeof(eos_skill_execution_v2)`.
  eos_skill_text_v2 command_id;      ///< Non-empty Agent-provided root command ID.
  eos_skill_text_v2 arguments_json;  ///< Schema-validated JSON object.
  const eos_skill_host_v2* host;     ///< Valid only on this thread during execute.
} eos_skill_execution_v2;

/// Completion supplied synchronously by a skill. `result_json` is an object.
typedef struct eos_skill_completion_v2 {
  uint32_t struct_size;           ///< `sizeof(eos_skill_completion_v2)`.
  uint32_t accepted;              ///< Exactly zero or one.
  eos_skill_text_v2 reason_code;  ///< Stable skill-owned result code.
  eos_skill_text_v2 result_json;  ///< Optional JSON object returned to the Agent.
  eos_skill_text_v2 message;      ///< Optional bounded operator-facing diagnostic.
} eos_skill_completion_v2;

/// Frozen size of the original ABI v2 completion. Appended members must be
/// guarded by their individual member boundary before being read.
#define EOS_SKILL_COMPLETION_V2_BASE_SIZE                      \
  ((uint32_t)(offsetof(eos_skill_completion_v2, result_json) + \
              sizeof(((eos_skill_completion_v2*)0)->result_json)))

#define EOS_SKILL_COMPLETION_V2_HAS(completion, member) \
  ((completion) != NULL &&                              \
   (completion)->struct_size >=                         \
       (uint32_t)(offsetof(eos_skill_completion_v2, member) + sizeof((completion)->member)))

typedef eos_skill_host_status_v2 (*eos_skill_complete_v2)(
    void* completion_context, const eos_skill_completion_v2* completion);

typedef uint32_t eos_skill_execute_status_v2;
#define EOS_SKILL_EXECUTE_OK 0u
#define EOS_SKILL_EXECUTE_INVALID_ARGUMENT 1u
#define EOS_SKILL_EXECUTE_CANCELLED 2u
#define EOS_SKILL_EXECUTE_FAILED 3u

typedef eos_skill_execute_status_v2 (*eos_skill_execute_v2)(void* skill_context,
                                                            const eos_skill_execution_v2* execution,
                                                            eos_skill_complete_v2 complete,
                                                            void* completion_context);

/// One exact primitive dependency declared by a skill.
typedef struct eos_skill_primitive_reference_v2 {
  uint32_t struct_size;       ///< `sizeof(eos_skill_primitive_reference_v2)`.
  eos_skill_text_v2 name;     ///< Canonical primitive name.
  eos_skill_text_v2 version;  ///< Exact primitive SemVer.
} eos_skill_primitive_reference_v2;

/// One resource URI template advertised by a skill.
///
/// Each `{variable}` matches one non-empty URI segment. EOS authorizes every
/// `read_resource` call against this allowlist before consulting a provider.
typedef struct eos_skill_resource_v2 {
  uint32_t struct_size;            ///< `sizeof(eos_skill_resource_v2)`.
  eos_skill_text_v2 uri_template;  ///< Required URI template.
  eos_skill_text_v2 description;   ///< Optional human-readable purpose.
} eos_skill_resource_v2;

typedef uint32_t eos_skill_signal_direction_v2;
#define EOS_SKILL_SIGNAL_ACCEPTED 1u
#define EOS_SKILL_SIGNAL_EMITTED 2u

typedef struct eos_skill_signal_descriptor_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 schema_json;
  eos_skill_signal_direction_v2 direction;
} eos_skill_signal_descriptor_v2;

typedef uint32_t eos_skill_claim_kind_v2;
#define EOS_SKILL_CLAIM_DEVICE 1u
#define EOS_SKILL_CLAIM_RESOURCE 2u

typedef uint32_t eos_skill_claim_mode_v2;
#define EOS_SKILL_CLAIM_SHARED_READ 1u
#define EOS_SKILL_CLAIM_EXCLUSIVE_USE 2u

/// A claim value is resolved from invocation arguments using RFC 6901.
typedef struct eos_skill_claim_template_v2 {
  uint32_t struct_size;
  eos_skill_claim_kind_v2 kind;
  eos_skill_claim_mode_v2 mode;
  eos_skill_text_v2 argument_pointer;
} eos_skill_claim_template_v2;

typedef uint32_t eos_skill_source_authority_v2;
#define EOS_SKILL_SOURCE_READ 1u
#define EOS_SKILL_SOURCE_WRITE 2u
typedef struct eos_skill_source_descriptor_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 topic;
  eos_skill_text_v2 schema_json;
  eos_skill_source_authority_v2 authority;
} eos_skill_source_descriptor_v2;

typedef struct eos_skill_inference_descriptor_v2 {
  uint32_t struct_size;
  eos_skill_inference_authority_v2 authority;
} eos_skill_inference_descriptor_v2;

/// One executable skill embedded in a native library.
typedef struct eos_skill_descriptor_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
  eos_skill_text_v2 description;
  eos_skill_text_v2 parameters_schema_json;
  eos_skill_text_v2 returns_schema_json;
  uint32_t primitive_count;
  const eos_skill_primitive_reference_v2* primitives;
  uint32_t resource_count;
  const eos_skill_resource_v2* resources;
  uint32_t signal_count;
  const eos_skill_signal_descriptor_v2* signals;
  uint32_t claim_count;
  const eos_skill_claim_template_v2* claims;
  uint32_t reentrant;
  void* context;
  eos_skill_execute_v2 execute;
  uint32_t source_count;
  const eos_skill_source_descriptor_v2* sources;
  uint32_t inference_count;
  const eos_skill_inference_descriptor_v2* inferences;
} eos_skill_descriptor_v2;

#define EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE \
  ((uint32_t)(offsetof(eos_skill_descriptor_v2, execute) + sizeof(((eos_skill_descriptor_v2*)0)->execute)))
#define EOS_SKILL_DESCRIPTOR_V2_HAS(descriptor, member) \
  ((descriptor) != NULL && (descriptor)->struct_size >= \
      (uint32_t)(offsetof(eos_skill_descriptor_v2, member) + sizeof((descriptor)->member)))

/// Immutable manifest returned by one loaded native library.
typedef struct eos_skill_library_v2 {
  uint32_t abi_version;                   ///< `EOS_SKILL_ABI_VERSION_V2`.
  uint32_t struct_size;                   ///< `sizeof(eos_skill_library_v2)`.
  eos_skill_text_v2 name;                 ///< Exact lower-case library identity.
  eos_skill_text_v2 version;              ///< Library SemVer version.
  eos_skill_text_v2 build_id;             ///< Optional deployment provenance string.
  uint32_t skill_count;                   ///< Number of embedded descriptors.
  const eos_skill_descriptor_v2* skills;  ///< Process-lifetime descriptor array.
  uint32_t skill_descriptor_size;         ///< Byte stride of each `skills` record.
} eos_skill_library_v2;

#define EOS_SKILL_LIBRARY_V2_BASE_SIZE \
  ((uint32_t)(offsetof(eos_skill_library_v2, skills) + sizeof(((eos_skill_library_v2*)0)->skills)))
#define EOS_SKILL_LIBRARY_V2_HAS(library, member) \
  ((library) != NULL && (library)->struct_size >= \
      (uint32_t)(offsetof(eos_skill_library_v2, member) + sizeof((library)->member)))

typedef const eos_skill_library_v2* (*eos_skill_library_query_v2_fn)(uint32_t host_abi_version);

/// Required versioned entry point for every native skill library.
EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif

#endif  // EOS_SKILL_ABI_H_

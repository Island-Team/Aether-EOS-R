# Native skill ABI v2

[简体中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/abi_cn.md)

[Authoring overview](README.md) · [Capabilities](capabilities.md) · [Complete example](example.md)

The recommended C++17 interface is `<eos/skill/sdk.hpp>`. Include the raw installed contract directly only for C or ABI-level integration:

```c
#include <eos/skill/abi.h>
```

The ABI uses fixed-width integers, borrowed byte slices, explicit structure sizes, opaque contexts, and C function pointers. Do not pass C++ standard library objects, protobuf objects, EOS internal classes, exceptions, or allocation ownership across the boundary. Compile against the header installed for the target EOS and set every `struct_size` to the exact `sizeof` value.

ABI v2 is append-only. The current host table adds optional `log`, `emit_checkpoint`, `report_failure`, asynchronous primitive, and timestamped source callbacks after its original fields. Code must first test `EOS_SKILL_HOST_V2_HAS(host, member)` and then the callback pointer. A skill built with the current header still runs on an older v2 host; logging and checkpoints return unavailable and failure reporting becomes a no-op while the original capabilities continue to work. Existing v2 skill binaries remain loadable by the current host.

## Text, status, and identity

`eos_skill_text_v2` is a borrowed UTF-8 byte slice and is not necessarily NUL-terminated:

```cpp
template <std::size_t N>
constexpr eos_skill_text_v2 Text(const char (&value)[N]) {
  return {value, static_cast<uint32_t>(N - 1)};
}

eos_skill_text_v2 Borrow(const std::string& value) {
  return {value.data(), static_cast<uint32_t>(value.size())};
}
```

Use `{NULL, 0}` for empty text. Metadata slices must live for the process lifetime; invocation slices need live only through the host call that consumes them.

Library, skill, and primitive names match:

```text
^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$
```

Versions are case-sensitive SemVer values. A skill's exact `{name, version}` pair is its discovery and execution identity.

Host callbacks return `EOS_SKILL_HOST_OK`, `EOS_SKILL_HOST_INVALID_ARGUMENT`, `EOS_SKILL_HOST_NOT_FOUND`, `EOS_SKILL_HOST_UNAVAILABLE`, or `EOS_SKILL_HOST_INTERNAL`. Execute returns `EOS_SKILL_EXECUTE_OK`, `EOS_SKILL_EXECUTE_INVALID_ARGUMENT`, `EOS_SKILL_EXECUTE_CANCELLED`, or `EOS_SKILL_EXECUTE_FAILED`.

## C++ facade

`eos::skill::Execution` parses object arguments and copies host responses into owned JSON. It exposes cancellation, synchronous and asynchronous primitive calls, resource reads, SIGNAL receive/emit, timestamped source publish/query/wait, `logger()`, and `checkpoint()`. Capability operations return `Result<T>`; a rejected or unavailable observability call does not alter the execution unless the skill explicitly chooses to do so. `Outcome::Success` and `Outcome::Reject` complete normally, while `Cancelled`, `InvalidArgument`, and `Failure` map to the corresponding execute status without completion.

`SkillBuilder` owns descriptor strings, schemas, and declaration arrays; `LibraryBuilder::Build` creates a process-lifetime library table. `EOS_SKILL_EXPORT_LIBRARY(builder_function)` supplies the versioned query symbol and contains exceptions through `eos::skill::Run`.

The optional asynchronous primitive callbacks are append-only ABI v2 host extensions. Skills must guard them with `EOS_SKILL_HOST_V2_HAS`. Up to 64 operations may be outstanding per invocation. Each retains its device claim until its terminal acknowledgement is consumed. A zero-duration wait polls, cancellation targets the operation's derived command ID, and EOS cancels when needed and drains every operation left outstanding when execute returns.

## Library and descriptor

One module may publish multiple descriptors:

```c
typedef struct eos_skill_library_v2 {
  uint32_t abi_version;
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
  eos_skill_text_v2 build_id;
  uint32_t skill_count;
  const eos_skill_descriptor_v2* skills;
  uint32_t skill_descriptor_size;
} eos_skill_library_v2;
```

Current producers set `skill_descriptor_size` to `sizeof(eos_skill_descriptor_v2)`. Hosts guard this appended library field with `EOS_SKILL_LIBRARY_V2_HAS`; libraries whose header ends at `skills` are iterated with `EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE`. This keeps the array stride independent of fields appended to the descriptor itself.

Each descriptor provides the complete contract for one skill:

```c
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
```

Use zero counts with `NULL` tables. Set `reentrant` to exactly zero or one. Choose one only when the descriptor context and every callback path are safe for concurrent invocation.

Source declarations independently grant `EOS_SKILL_SOURCE_READ` or `EOS_SKILL_SOURCE_WRITE` authority for one topic and carry a self-contained object JSON Schema. Older v2 descriptors end at `execute`; hosts use `EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE` and `EOS_SKILL_DESCRIPTOR_V2_HAS` rather than requiring the current full size.

The manager assigns each accepted sample a process-monotonic sequence while preserving its producer-supplied nanosecond timestamp. Queries support latest, exact, at-or-before, at-or-after, and nearest lookup. Waits select the oldest retained sequence newer than the supplied cursor and are cancellation-aware.

Inference declarations grant one of `VAD`, `HUMAN`, `HUMAN_POSE`, or `FACE_RECOGNITION`. `inference_count` and `inferences` are append-only ABI v2 members and must be checked with `EOS_SKILL_DESCRIPTOR_V2_HAS`. Calls outside the descriptor's declared authorities fail before reaching the provider. Vision requests use either exact retained-frame provenance or canonical inline media in `options_json.input` with optional `depth_input`, never both. Inline vision uses empty `source` and zero `source_sequence`/`source_generation`; its successful result retains that empty/zero provenance. VAD always uses the same empty/zero provenance with caller-supplied canonical audio in `options_json.input`. The existing `options_json` member carries inline media, so this requires no ABI layout change.

Export the required symbol with default visibility:

```cpp
extern "C" EOS_SKILL_EXPORT
const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t version) {
  return version == EOS_SKILL_ABI_VERSION_V2 ? &kLibrary : nullptr;
}
```

A missing symbol, `NULL` result, wrong ABI version, incomplete table, descriptor stride that is not naturally aligned, or descriptor `struct_size` larger than its stride rejects the configured skill load. The query function must not perform invocation work or inspect devices.

## Parameter and result schemas

Both schemas are required, self-contained JSON Schema objects. EOS compiles them at startup, validates arguments before entering the skill, and validates the completed result before exposing it. Defaults are not inserted.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "properties": {
    "robot_name": { "type": "string", "minLength": 1 },
    "device_id": { "type": "string", "minLength": 1 }
  },
  "required": ["robot_name", "device_id"],
  "additionalProperties": false
}
```

Use an object result even when no data is returned. Put deployment-specific robot names, device IDs, resource URIs, and targets in invocation arguments, not in reusable descriptors.

## Execute and complete

EOS calls the descriptor synchronously:

```c
typedef eos_skill_execute_status_v2 (*eos_skill_execute_v2)(
    void* skill_context,
    const eos_skill_execution_v2* execution,
    eos_skill_complete_v2 complete,
    void* completion_context);
```

The execution carries the root command ID, validated argument JSON, and an invocation-scoped `eos_skill_host_v2`. Validate pointers, `struct_size`, and `host->abi_version` before use. Always pass `host->context` to host callbacks, never the descriptor context.

The callback must keep every loop, wait, and operation bounded; check `host->is_cancelled` before expensive work and between steps; catch every exception; and return promptly. It must not retain execution data or use the host from another thread.

Completion contains `accepted`, a stable skill-owned `reason_code`, an object-valued `result_json`, and an optional operator-facing `message`. The message is an append-only field: C consumers must accept `EOS_SKILL_COMPLETION_V2_BASE_SIZE` and test `EOS_SKILL_COMPLETION_V2_HAS(completion, message)` before reading it. Call completion exactly once only on the `EOS_SKILL_EXECUTE_OK` path, and return OK only when completion itself returns `EOS_SKILL_HOST_OK`. A semantically rejected result still uses execute OK with `accepted = 0`. Return CANCELLED without completion after observing cancellation.

For `EOS_SKILL_EXECUTE_INVALID_ARGUMENT` or `EOS_SKILL_EXECUTE_FAILED`, a current skill may first call the optional `report_failure` callback with one `eos_skill_failure_v2`. Its `reason_code` is the stable machine-readable value; its optional `message` is non-contractual operator text. Report at most one terminal failure and then return the matching execute status without completion. The C++ facade does this for `Outcome::InvalidArgument(reason, message)` and `Outcome::Failure(reason, message)`; cancellation deliberately reports neither. Escaped handler exceptions become `skill.exception` with a generic safe message, never exception text.

Reason codes are at most 128 bytes and messages at most 1,024 bytes. Messages must not contain arguments, results, primitive acknowledgements, resource values, device IDs, concrete URIs, SIGNAL payloads, credentials, recognized text, or other personal data. Clients branch on status and `reason_code`, never on `message`.

## SIGNAL declarations

A SIGNAL declaration has a lower-case dotted name, object-valued JSON Schema, and direction:

```cpp
const eos_skill_signal_descriptor_v2 kSignals[] = {
    {sizeof(eos_skill_signal_descriptor_v2), Text("control.update"),
     Text(R"({"type":"object","additionalProperties":false})"),
     EOS_SKILL_SIGNAL_ACCEPTED},
    {sizeof(eos_skill_signal_descriptor_v2), Text("progress.changed"),
     Text(R"({"type":"object","additionalProperties":false})"),
     EOS_SKILL_SIGNAL_EMITTED},
};
```

Names match `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)+$`. The same name may appear once in each direction. `CANCEL` is manager-reserved and must not be declared.

`receive_signal(context, timeout_ms, &signal)` returns RECEIVED, TIMEOUT, CLOSED, or an error. Zero timeout polls. Copy returned name and payload bytes before the next host callback. A received `CANCEL` requests bounded shutdown and an `EOS_SKILL_EXECUTE_CANCELLED` return.

`emit_signal` accepts only declared emitted names with schema-valid object payloads. Success means the event entered bounded execution history; EOS does not route it directly to another skill.

## Claim declarations

A claim resolves a non-empty string from validated invocation arguments using an RFC 6901 JSON Pointer:

```cpp
const eos_skill_claim_template_v2 kClaims[] = {
    {sizeof(eos_skill_claim_template_v2),
     EOS_SKILL_CLAIM_DEVICE,
     EOS_SKILL_CLAIM_EXCLUSIVE_USE,
     Text("/arm_device_id")},
    {sizeof(eos_skill_claim_template_v2),
     EOS_SKILL_CLAIM_RESOURCE,
     EOS_SKILL_CLAIM_SHARED_READ,
     Text("/world_resource_uri")},
};
```

`SHARED_READ` coexists with readers of the same kind and identity. `EXCLUSIVE_USE` conflicts with either mode. Descriptor claims reserve authority for the whole execution. Primitive calls separately acquire a transient exclusive claim on their target device, so a skill that only needs device ownership during each call can omit a whole-execution device claim. Declare resource claims and any device reservation needed across calls or waits. SIGNALs cannot add claims or introduce devices.

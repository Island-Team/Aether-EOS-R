# Native skill host capabilities

[简体中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/capabilities_cn.md)

[Skill authoring overview](README.md) · [ABI contract](abi.md) · [Complete example](example.md)

Examples below show raw ABI records where lifetime or wire behavior matters. Normal C++ skill code should use the equivalent `eos::skill::Execution` methods, which validate object JSON and copy borrowed host responses.

## Calling primitives

Declare every primitive that any code path may call:

```c
typedef struct eos_skill_primitive_reference_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
} eos_skill_primitive_reference_v2;

static const eos_skill_primitive_reference_v2 kPrimitives[] = {
    {sizeof(eos_skill_primitive_reference_v2),
     Text("gimbal.motion"), Text("1.0.0")},
};
```

The exact identity must be available in the deployment's Dispatcher catalog at startup. This depends on configured devices, primitive exposure, and profile admission, not only on the schemas present in the source tree. Duplicate or unknown declarations reject the whole library. Consult [the primitive interface](../../actions.md) for canonical versions, argument fields, units, terminal behavior, and deployment constraints.

The primitive call records are:

```c
typedef struct eos_skill_primitive_request_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 primitive;
  eos_skill_text_v2 version;
  eos_skill_text_v2 device_id;
  eos_skill_text_v2 arguments_json;
} eos_skill_primitive_request_v2;

typedef struct eos_skill_primitive_response_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 acknowledgement_json;
  eos_skill_text_v2 error_message;
} eos_skill_primitive_response_v2;
```

Make a synchronous call through the invocation host:

```cpp
std::string arguments =
    R"({"reference":"relative","pan_rad":0.5,"tilt_rad":0.0})";
eos_skill_primitive_request_v2 request{
    sizeof(eos_skill_primitive_request_v2),
    Text("gimbal.motion"),
    Text("1.0.0"),
    Borrow(device_id),
    Borrow(arguments),
};
eos_skill_primitive_response_v2 response{
    sizeof(eos_skill_primitive_response_v2), {}, {}};

const eos_skill_host_status_v2 status =
    execution->host->call_primitive(
        execution->host->context, &request, &response);
```

`arguments_json` must be an object containing only the primitive-specific arguments. Omit `type`, `command_id`, and version metadata. EOS selects the canonical schema, injects the primitive type and a child command ID, verifies the exact dependency and device binding, applies configured limits, and sends the action through the same Dispatcher as `CallPrimitive`.

Child IDs are `<root-command-id>/<zero-based-call-index>`. A call waits for its terminal boundary. On `EOS_SKILL_HOST_OK`, copy or parse `acknowledgement_json` immediately and inspect both `accepted` and `result.status`; host `OK` does not mean the primitive was accepted. The response slices are host-owned and remain valid only until the next primitive host call or until execute returns. On a non-`OK` host status, `error_message` is bounded diagnostic text for logging or failure mapping; it is not a stable reason code or a retry signal.

Once a call fails, is rejected, is cancelled, has a terminal failure, or has an ambiguous outcome, do not retry it. EOS latches the invocation against later primitive releases. There is deliberately no automatic retry after hardware may have received an action.

For work that must continue while a primitive is active, ABI v2 hosts may provide `start_primitive`, `wait_primitive`, and `cancel_primitive`. Check each optional member with `EOS_SKILL_HOST_V2_HAS` before use. The C++ facade exposes the same lifecycle as `Execution::start_primitive`, `wait_primitive`, and `cancel_primitive`. A zero-duration wait is a poll and returns an empty optional while the operation remains active. Up to 64 asynchronous primitives may be outstanding per invocation. Each operation has an independent ID, cancellation, activity, and dynamic device claim; its claim remains held until that operation's terminal acknowledgement is consumed. Synchronous primitive calls are rejected while any asynchronous operation is active. Returning from the skill drains all outstanding operations.

Bound each wait and check `execution.cancelled()` plus declared inbound control SIGNALs between polls. On pause, cancel an outstanding primitive when its semantics permit and start the next unit only after resume. Do not detach work or call the invocation host from a background thread.

## One-shot inference

ABI v2 hosts may append `infer_once` to `eos_skill_host_v2`. Guard it with `EOS_SKILL_HOST_V2_HAS(host, infer_once)` before reading it. Each request names one declared authority; `options_json` and the successful result are JSON objects. Human, human-pose, and face-recognition requests use exactly one input mode: an exact retained camera frame identified by non-empty `source` plus non-zero `source_sequence` and `source_generation`, or caller-supplied canonical media in `options_json.input` with optional `options_json.depth_input` and empty/ zero provenance. Mixing modes or supplying a partial provenance triple is invalid. VLM (`EOS_SKILL_INFERENCE_VLM = 6`) accepts required `messages` and optional camera input: text only with empty provenance, one inline JPEG in `options_json.input`, or one exact retained camera frame. VLM rejects depth media and mixed input modes. Owner interaction requests use text only. VAD always operates on caller-supplied canonical audio in `options_json.input` with empty/zero provenance. EOS rejects undeclared authorities and mismatched result provenance. Retained-frame results echo the requested triple; inline vision and VAD results retain empty/zero provenance.

The C++ facade exposes typed tags through `Execution::infer_once<HumanPoseInference>(frame, options)` and the equivalent `HumanInference`, `FaceRecognitionInference`, and `VlmInference` tags. Inline vision, text-only VLM, and VAD use `Execution::infer_once<Inference>(options)`. Canonical inline option names match the model APIs: `input`, optional `depth_input`/`depth_scale`, `conf`, and for human pose `persons`/`kpt_conf`; face recognition accepts `input` and optional `return_image` while EOS supplies `operation = "recognize"`. A call is one-shot: skill code decides whether to retry, and workflows that require exactly one analysis must not retry. Built-in skills use the asynchronous `StartInference`, `WaitInference`, and `CancelInference` capability so tracking or control handling can continue while independent inference completes.

## Reading resources

Declare every readable URI family:

```c
typedef struct eos_skill_resource_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 uri_template;
  eos_skill_text_v2 description;
} eos_skill_resource_v2;

static const eos_skill_resource_v2 kResources[] = {
    {sizeof(eos_skill_resource_v2),
     Text("robots/{robot_name}/detection/state"),
     Text("Latest detection snapshot for the selected robot.")},
};
```

Resource calls use these records:

```c
typedef struct eos_skill_resource_request_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 uri;
} eos_skill_resource_request_v2;

typedef struct eos_skill_resource_response_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 value_json;
  eos_skill_text_v2 error_message;
} eos_skill_resource_response_v2;
```

Use only literal slash-separated segments and full-segment variables of the form `{name}`. Each variable matches exactly one non-empty segment. For example, the declaration above permits `robots/island/detection/state`, but not an empty robot name or additional path segments. Do not use RFC 6570 operators, explode or prefix modifiers, comma-separated variables, or a variable embedded in a literal segment.

Read a concrete URI through the host:

```cpp
eos_skill_resource_request_v2 request{
    sizeof(eos_skill_resource_request_v2), Borrow(uri)};
eos_skill_resource_response_v2 response{
    sizeof(eos_skill_resource_response_v2), {}, {}};

const eos_skill_host_status_v2 status =
    execution->host->read_resource(
        execution->host->context, &request, &response);
```

On `EOS_SKILL_HOST_OK`, `value_json` is an object-valued snapshot. Copy or parse it before the next resource read or before execute returns; its bytes are host-owned. A declaration authorizes reads but does not guarantee that the deployment has a provider. EOS checks the concrete URI against the allowlist and reads through the normal perception Reader. This grants no backend, hardware, model, filesystem, network, or gateway access.

Resources have no write counterpart. State-changing work must use an admitted primitive, preserving schema validation, claims, cancellation, and operation accounting.

Trusted built-in/share-library skills additionally have a provider-neutral, move-only resource subscription handle. Opening one concrete URI still requires a matching metadata resource declaration and acquires producer demand for the handle lifetime. Resource templates, descriptions, and concrete URIs are bounded to 4 KiB. A skill can read retained latest, exact sequence/time, before, after, nearest, inclusive sequence-range, or inclusive time-range selections, and can wait for the first publication after a sequence. Range results are limited to 256 publications and waits to 60 seconds per call; waits also observe execution cancellation. Explicit close, destruction, or move-assignment releases demand. The handle contains read/wait authority only and cannot publish perception. The dynamically loaded native ABI remains unary `read_resource`; this extended lifecycle is currently available only to built-ins linked into the share native library.

On a non-`OK` status, `error_message` is borrowed diagnostic text. It is valid until the next resource read or execute return and must not be treated as a stable API value.

## Terminal diagnostics

Map a capability failure to a skill-owned reason code and a short safe stage description instead of copying capability payloads. For example:

```cpp
auto snapshot = execution.read_resource(uri);
if (!snapshot) {
  return eos::skill::Outcome::Failure(
      "detection.read_failed", "detection snapshot read failed: capability unavailable");
}
```

`Outcome::Success`, `Reject`, `InvalidArgument`, and `Failure` accept an optional message. `Failure` and `InvalidArgument` also accept a stable reason code and use the optional ABI `report_failure` callback before returning their non-OK execute status. Success and rejection carry the message in the appended completion field. Messages are operator diagnostics, not an interface: automation branches on status and reason code only. Keep messages within 1,024 bytes and apply the same payload and personal-data exclusions as logs and checkpoints. Cancellation remains canonical and has no skill-authored terminal diagnostic.

## Logs and checkpoints

`execution.logger()` writes structured operational diagnostics through EOS's `skills` logger. Levels are trace, debug, info, warn, error, and critical. A checkpoint is instead an Agent-visible record in the execution journal:

```cpp
(void)execution.logger().info("scan_started", "Starting scan");
(void)execution.checkpoint(
    "scan.started",
    eos::skill::CheckpointOptions().Progress(0.1).Fields({{"phase", 1}}));
```

Event or checkpoint names are at most 128 bytes, messages 1 KiB, and encoded object fields 16 KiB. Checkpoint names are lower-case dotted identities; `eos.*` and `execution.*` are reserved. Progress, when present, is finite and in `[0,1]`. Both callbacks are optional ABI v2 extensions: an older host or a host without journal support returns `ErrorCode::kUnavailable`. Treat this as loss of observability, not execution failure.

Do not put arguments, results, concrete resource URIs or values, device IDs, SIGNAL payloads, primitive acknowledgements, schemas, credentials, names, recognized speech, or other personal data in messages or fields. EOS sanitizes fields and adds immutable execution/runtime/library identity. Per execution, diagnostic logs use a 16 records/second token bucket with burst 64 and a maximum of 1,024 accepted records; checkpoints are limited to 256. Dropped records are accounted and summarized at termination.

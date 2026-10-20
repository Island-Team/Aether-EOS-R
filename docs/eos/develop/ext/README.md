# Extensions and backends overview

[Chinese](../../../../docs_cn/eos/develop/ext/README_cn.md)

Extensions connect EOS semantic contracts to simulated worlds, host devices, vendor SDKs, ROS 2, and operator tooling. Runtime code owns policy and canonical data; an extension owns only acquisition, transport, or device-specific conversion. See the [runtime overview](../../design/runtime/README.md) for the integrated lifecycle.

## Data boundary

Every backend receives an admitted, typed configuration and emits backend-neutral identity, health, and feed samples. Command-capable backends receive already validated semantic actions through dispatch. They must preserve units, timestamps, ordering, and ownership; they never create a second command path or fabricate a successful sample when a device or dependency is absent.

Input is rejected when configuration is missing or incompatible, a device cannot be opened or negotiated, a payload is malformed or exceeds its bound, paired data is incomplete, a sample is stale where freshness is required, or an operation is unsupported. Empty optional data remains empty. Capture end-of-stream, transport loss, child-process failure, cancellation, halt, and shutdown become explicit health or operation outcomes rather than silent reuse of invalid data.

Backends do not retry commands unless their focused contract says so. Concurrent capture may replace an older unread sample, but commands preserve dispatch ordering and acknowledgements. Feed-only devices reject commands. Stop and emergency behavior is fail-closed and scoped as described by dispatch and the backend page.

## Documentation structure

Extension documentation follows the implementation boundary:

```text
ext/
├── backends/
│   ├── demo.md     deterministic adapter and simulated driver world
│   ├── host.md     reusable host-device capture and speaker output
│   ├── ros2.md     ROS 2 transport
├── north_http/       standalone REST/SSE process, config, shared gRPC client, server, and audit
├── models/          provider request and response adapters
└── tools/          standalone extension-side utilities
```

Model adapters translate the private canonical model contracts without adding provider names or aliases to `runtime/models`; see [Models runtime](../../design/runtime/models.md).

### Client-facing process

- [North HTTP](north_http/README.md): standalone process composition, one shared EOS gRPC channel/stub set, strict configuration, HTTP/SSE server ownership, optional external ActionList audit, logging, and tests.

### Demo

- [Demo backend](backends/demo.md): EOS HAL mappings, deterministic driver-style state, and simulation.

### Host devices

- [Host devices](backends/host.md): bounded ALSA microphone and negotiated V4L2 camera capture.

### Transport and vendor backends

- [ROS 2](backends/ros2.md): optional topic and service transport.

### Tools

- [Diagnostic REPL](tools/repl.md): contract-driven operator client.

Each page owns its complete condition set and verification procedure. Backend-set qualification combines unit tests with simulation or hardware-in-loop evidence; one backend passing does not qualify another.

## Profile semantics

An EOS profile binds one EOS process to one robot and its devices. There is no separate northbound `hardware` object: robot identity, devices, transports, descriptions, limits, and feed envelopes are all part of the selected profile. The JSON Schemas under `share/ext/eos/actions/` define portable payload shapes; the TOML profile is the authority for what a particular robot advertises and accepts.

The selected `[[mode.binding]]` names exactly one binding file and profile. At startup, EOS validates every declared primitive against the catalog, device class, backend implementation, required endpoint, and numeric limits. Only admitted declarations appear in primitive discovery. A catalog primitive omitted by a device is unsupported for that device, even when another profile or another device supports it.

### Robot identity and fleets

`[profile.<name>.robot]` describes the robot owned by this EOS instance:

- `robot_id` is the stable machine key binding the robot to EOS. It may be a readable slug; UUID syntax is not required. When omitted, EOS generates a process-lifetime ID and logs a warning because resource bookmarks and fleet membership are then unstable across restart.
- `robot_name` is the human-readable name used for display and resource addressing. It defaults to `robot-<index>` (`robot-0` for a standalone instance) and must be unique in a fleet.
- `robot_desc` describes this particular robot, not its model family.
- `model` and `vendor` are optional inventory labels. In particular, `model = "eos-demo"` means only that the example robot belongs to the EOS demo model family; it neither selects an inference model nor grants a capability.

One runtime never silently owns several robots. A fleet is built by registering several EOS instances and rejecting duplicate `robot_id` or `robot_name` values. This keeps failures, status, devices, and commands attributable to exactly one EOS/robot pair.

### Reading a profile

For each device, read the data in this order:

1. `class`, `part_id`, and generated class index establish the target and its semantic resource family. Every device has its own `description`; do not use the robot description as a substitute.
2. `[[device.primitive]]` establishes the callable device-specific surface with an exact `{name, version}` identity. Its `description` explains this deployment's behavior; `limits` narrows numeric parameters without changing the shared payload schema.
3. `[device.backend]` and its typed table establish transport, endpoints, device paths, timeouts, and backend-owned defaults.
4. `[device.feeds]` establishes which data classes may be published and their maximum rates. An omitted data class is disabled; health is required.
5. Device part and scene data establish static geometry or simulation facts. They do not weaken dispatch validation or replace the physical controller's safety limits.

Optional primitive parameters require special care. If the profile declares a limit for an optional numeric field, callers may send a value inside that interval. If it omits that limit, callers must omit the field and the backend chooses its configured or native default; EOS logs a startup warning so operators know that no explicit deployment envelope is published. Required numeric fields always have profile limits. Enum and structural conditions remain in the shared schema.

Values in `demo.toml` are safe only for the loopback simulation. They are fallback examples, not hardware certification. A real profile must override every limit-capable parameter with a controller-verified interval. EOS admission is an additional software boundary and never replaces drive limits, collision protection, watchdogs, or an emergency stop.

`motion.stop` and `safety.estop` are implicit privileged operations and must not appear in `[[device.primitive]]`. `motion.stop` may target an admitted base, arm, or gimbal and uses that backend's clamp path. `safety.estop` uses an empty device ID, halts every admitted device, clears velocity leases, and latches the runtime until restart. Whether a physical device acknowledges or completes a command remains backend-specific; `accepted: true` never expands beyond the guarantee stated by that backend.

### Maintained profiles

- [Demo](backends/demo.md#profiles-and-operation): complete deterministic seven-device loopback, its host-capture variant, and the canonical profile-authoring example.
- [Demo host](backends/demo.md#profiles-and-operation): compatibility pointer for existing deployments that select the separate host-capture file.
- [Live](backends/ros2.md#illustrative-site-local-profile): minimal one-arm ROS 2 integration profile.

When adding hardware, copy the profile shape rather than its limits. Measure or obtain limits from the device/controller integration, declare only executable primitives, document omitted optional fields, and exercise every declared operation plus rejection, timeout, stop, estop, dependency-loss, and shutdown paths. See [configuration](../../usage/configuration.md), [southbound primitives](../interfaces/southbound/primitives.md), and [backend behavior](README.md).

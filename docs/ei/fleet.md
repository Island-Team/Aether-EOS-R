# EGIA fleet runtime — stable aliases over EOS processes

One EGIA process may connect to several external EOS processes. Each configured endpoint has a stable alias, while `Discover.robot_id` is the current EOS runtime identity. Planning, status, resource keys, and direct invocation expose both identities without conflating them.

## Configuration

```toml
[[modes.fleet.backends]]
id = "robot-a"
name = "west robot"
intro = "mobile manipulator in the west cell"
gateway = { host = "127.0.0.1", grpc_port = 16003 }

[[modes.fleet.backends]]
id = "robot-b"
name = "east robot"
intro = "inspection robot in the east cell"
gateway = { host = "127.0.0.1", grpc_port = 18110 }
```

`id` is the stable EGIA `robot_id` alias and must be unique. `gateway` selects an already-running plaintext EOS listener and must also be unique so EGIA owns one client session per endpoint. Both tables reject unknown keys and blank identifiers or hosts. EGIA no longer spawns or configures bundled sandbox/line backends; sandbox, live, and fleet profiles are connection profiles only. A mode without `backends` uses its single `[gateway]` as alias `default`.

## Discovery and addressing

Initialization independently health-checks and discovers every endpoint. For each alias EGIA retains:

- the current EOS runtime identity and version/start/health;
- hardware profile, model, vendor, and description;
- every generated device ID, class, part, backend metadata, and health;
- runtime-scoped world resources and versioned primitive descriptors with per-device numeric limits.

Resources and primitives are keyed by alias plus device ID. Two robots, or two devices of the same class on one robot, therefore remain distinct. An action target is always explicit:

```json
{
  "robot_id": "robot-a",
  "device_id": "base-1",
  "primitive": "base.velocity",
  "arguments": {
    "linear_x_m_s": 0.2,
    "linear_y_m_s": 0.0,
    "angular_z_rad_s": 0.0,
    "valid_for_ms": 250
  }
}
```

Rediscovery atomically removes old device/primitive targets. A stale alias/name/device tuple is rejected rather than silently resolving to another device.

## Coordination

Coordination bidders are compiled from discovered EOS devices. A device bids only for primitives in its `supported_primitives` list and only while healthy and admitted. EOS does not advertise station, tag, or reachability bindings, so EGIA does not infer them; those bidder fields remain empty unless a future contract supplies them.

## Safety

The estop latch spans the whole fleet. EGIA stops all discovered base, arm, and gimbal devices and sends one empty-target `safety.estop` per EOS process. The latch persists across EOS and EGIA restarts. Only the stopped-process local `egia safety reset` authorization followed by fresh complete, selected-revision checks for every affected alias can clear it. See [safety](core/safety.md).

## Current limits

- The first endpoint remains the source of the aggregate scene projection.
- EOS can execute deployment-owned native skills, but EGIA intentionally retains their inline schemas and metadata as planning guidance so governed egress stays primitive-only.
- Zone/station reachability cannot be derived from EOS discovery today.

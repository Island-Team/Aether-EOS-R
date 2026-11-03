# V2 dispatcher tree (in progress)

[中文](../../../../docs_cn/eos/design/runtime/dispatcher_cn.md) · [Hardware design](hardware.md) · [Field-level interface](../../interfaces/hardware.md)

## Why the tree exists

The dispatcher gives each core a stable address and a single route to its nodes. Its hierarchy is `core → layer → group → node`, supplied by the unified `1.0.0` node profile; the first implemented hardware paths are `cores/eos/layers/hardware/groups/{camera,base,manipulator}/nodes/{node_id}`. The same profile can describe software nodes in other layers and cores. EOS is an actual core. EGIA is reserved as a peer core, with no implicit access to EOS device adapters. Hardware nodes expose `status` and `health` plus only the `observation`, `movement`, and `safety` facets their kind supports.

The dispatcher is a Rust module separate from the HAL. Discovery reads an immutable registry snapshot built from the validated node profile. A movement request names one node and one typed mode. The dispatcher resolves that node once, then checks the profile's declared capability, observed health, required sample freshness, command deadline, numerical limits, and safety state before handing the command to its adapter. The adapter is the final physical authority. Navigation planning, trajectory generation, and inverse kinematics are outside this layer.

Package-contributed [node types](../../interfaces/node-contributions.md) use the same dispatcher edge and admission rules. Group names such as `mujoco` affect placement and display; the registered type supplies hardware kind. A package safety declaration cannot grant an actuator route without a compiled adapter and stop implementation.

## Command lifecycle

Every command has a caller-supplied `command_id` and monotonic deadline. `Dispatcher::new_durable` opens an exclusive, bounded journal directory. It syncs a versioned reservation and then a `releasing` record before calling an adapter. A duplicate ID returns its stored outcome; conflicting reuse is rejected. On restart, an unfinished record becomes `outcome_unknown` and is never automatically released again. Corrupt or oversized records prevent startup. The in-memory constructor remains for tests and in-process prototypes; the future Rust daemon must use the durable constructor. A failed journal write before release rejects the command; a failed terminal write after release leaves its outcome uncertain.

Emergency stop latches process safety before calling every actuator's independent stop method. The dispatcher no longer holds a release lock across a potentially blocking adapter call. The adapter must latch stop at its physical write boundary so a delayed submission cannot actuate after stop; the CXX mocks implement and test this rule. The mock test proves stop returns while a submission is blocked, but a real controller's stop latency and independent velocity watchdog still require measurement before cutover. An observation or validation failure degrades that node's Rust-side health; `refresh_node` clears the fault only after a valid, fresh probe. Health, safety, and physical stop errors remain visible separately.

## Integration with the current runtime

This tree is the v2 foundation, not the present C++ `Dispatcher::ExecuteCommand` behavior. Existing workflows and skills continue to use their current transport and C++ gate until they move to the v2 node API and registry. During migration, there must be one active actuator owner per node, so the old and new dispatchers cannot both release commands to the same device.

## Verification

Use a fake HAL registry to prove exact path resolution, facet discovery, and isolation between nodes. Integration tests cover healthy movement, stale observation rejection, limits, expired deadline, duplicate ID replay across restart, corrupt journal rejection, health degradation and recovery, cancellation before release, and stop during a blocked mock release. Physical stop latency, controller-enforced velocity lease expiry, and a deployed ROS 2 graph remain cutover acceptance cases. Record reproducible test and benchmark commands in the [development guide](../../develop/hardware.md), with full output in `logs/test/<run-id>/`.

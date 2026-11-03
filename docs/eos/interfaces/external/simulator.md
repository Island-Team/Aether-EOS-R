# External simulator interface

This page describes the opt-in C++ simulator client and its `eos.simulator.v1` protocol. The default Rust v2 path uses the [MuJoCo v2 interface](../mujoco.md): IsOS verifies the external package, and EOS launches the simulator as its direct child. Agents use EOS public services and never connect to the private simulator socket directly.

## Boundary

| Transport | Endpoint | Caller |
| --- | --- | --- |
| Private gRPC over Unix socket | `<run>/simulator.sock` | EOS MuJoCo backend adapter |

In the C++ side chain, the external simulator package owns the socket server, world state, physics, rendering, and clock. The optional benchmark socket is private to its evaluator and is not part of the EOS contract. The historical launcher starts the simulator before EOS and stops it according to its stack dependencies.

## Contract

The `eos.simulator.v1.SimulatorService` contract is defined in [`simulator.proto`](../../../../share/eos/contracts/eos/external/simulator/simulator.proto). EOS sends bounded `Health`, `Execute`, `ReadTelemetry`, and `ReadCameraFrame` calls over the owner-only socket. Calls use finite deadlines; the client reconnects when the service returns. If the socket is missing, backend health is unavailable, commands reject, and camera reads return no frame while other EOS services continue.

MuJoCo package implementation, launch arguments, readiness, and benchmark behavior are documented in its [usage guide](../../usage/mujoco.md), [design](../../design/mujoco.md), and [interface copy](https://github.com/Island-Team/isos-mujoco-sim/blob/master/docs/mujoco/interfaces/simulator.md).

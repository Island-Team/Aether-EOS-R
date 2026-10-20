# Hardware driver contract

[Southbound index](README.md) · [中文](../../../../../docs_cn/eos/develop/interfaces/southbound/driver_cn.md)

The authoritative API is [`eos/hardware/backend.h`](../../../../../src/eos/include/eos/hardware/backend.h). A backend reports identity, feed rates, health, and telemetry; it implements only commands supported by its admitted device kind. EOS owns binding admission, validation, target resolution, dispatch, and lifecycle. The driver owns physical limits, hardware communication, and safe execution.

Drivers return accepted, rejected, or timed-out release results; unsupported optional operations reject. Commands and telemetry use explicit SI units. Drivers must preserve joint order and quaternion order, reject unsafe targets, implement `Clamp()` and `Halt()`, and expose absence when no valid sensor sample exists. See [body](body.md), [commands](primitives.md), and [world inputs](world.md) for examples.

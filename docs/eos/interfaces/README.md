# EOS interface catalog

[中文](../../../docs_cn/eos/interfaces/README_cn.md) · [EOS](../README.md) · [Node catalog](../layers/README.md)

This directory is the authoritative home for EOS-owned contracts. IsOS owns graph persistence, package registry, and management APIs; EOS owns registered node types, typed hardware payloads, control, adapter boundaries, and its process APIs. A node page links here rather than restating field definitions.

| Boundary | Contract | Implementation status |
| --- | --- | --- |
| Registered node profile and payloads | [Hardware](hardware.md), [node contributions](node-contributions.md), [primitive bindings](primitive-bindings.md), [graph control](control.md), [Vision](vision.md) | Rust current plus stated 0.x extensions. |
| Selected binary reader support | [Reader features and identities](reader-features.md), [setup adapter](setup-v1.md) | G05 version arrays current; P05 exact type and feature preflight is target design. |
| Simulator and adapter | [MuJoCo binding](mujoco.md), [R06 managed stop](simulator-stop.md), [package adapter ABI in hardware](hardware.md#package-adapter-library-abi) | R05 strict socket current; R06 physical stop target design. |
| Process APIs | [Rust control and planned gRPC](grpc.md), [CapabilityService](capabilities.md), [contract source map](contracts.md) | gRPC protobuf identity published; default Rust gateway is target design. |
| Rust diagnostic events | [JSONL event producer](events.md) | P06 target design; IsOS owns aggregation and cursors. |
| External services | [HTTP/SSE](external/http.md), [model providers](external/models.md), [simulator compatibility](external/simulator.md) | Mostly opt-in C++ compatibility. |

The opt-in C++ southbound HAL, SkillService, WorkflowService, and earlier graph setup adapter are cataloged under [historical interfaces](../legacy/README.md). Preserve their published wire and ABI identities for existing callers; the 0.x target does not add a Workflow engine.

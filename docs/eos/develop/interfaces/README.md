# EOS interface guide

[中文](../../../../docs_cn/eos/develop/interfaces/README_cn.md)

Other processes use EOS through the `island.eos.v1` gRPC services. WebUI uses the separate HTTP/SSE adapter, which calls those same services. EOS alone accesses hardware through the native C++ HAL. The contract source is organized by domain, not by compass direction or transport.

## EOS process API

- [gRPC services and access model](grpc.md): Gateway, Skill, and Workflow services, listener configuration, request limits, and read/compute/write behavior.
- [Actions](actions.md), [body](body.md), [world](world.md), [workflow](workflow.md), and [skills](skills.md): behavior and payloads for the process API.
- [Contract source map](contracts.md), including [system](contracts.md#system-resources-and-facts), [robot](contracts.md#robot-discovery-and-commands), [perception](contracts.md#perception-requests-and-media), and [annotations](contracts.md#contract-annotations): authored protobuf and generated validation artifacts.

## Other interfaces

- [HTTP/SSE adapter](external/http.md), [model providers](external/models.md), and [simulator](external/simulator.md): named process interfaces under `external/`.
- [Hardware HAL](southbound/README.md): C++ driver contract and hardware data flow.
- [Native skill authoring](skills/native/README.md): built-in C++ skills, supported ABI v2, and the local [mailbox IPC guide](skills/native/ipc.md). [External skill execution](skills.md#external-skill-execution) is not supported yet.

A gRPC call can cross a process boundary over loopback TCP or a Unix domain socket. IPC describes that connection, while the domain folders describe the payloads and operations. Internal `runtime/northbound` source names are implementation details.

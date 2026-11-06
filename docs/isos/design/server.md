# Server ownership

This page explains the current source boundaries for contributors. The [HTTP contracts](../develop/http/README.md) and [owner contracts](../develop/owner/README.md) define public fields; [verification](../maintenance/verification.md) records checks.

```text
src/
  main.rs               starts the application
  app/                  CLI, local HTTP, UI bundle
    server/routes/       HTTP handlers by domain
  domain/               graph, registry, packages, launch decisions
  runtime/              manager IPC, children, owner exchanges, logs
  interfaces/           typed boundary values
```

`app` admits local requests and parses them, then delegates decisions to `domain` and live operations to `runtime`. `domain` verifies exact pins and registered declarations without owning processes. `runtime` serializes manager mutations, supervises only IsOS-owned direct children, and retains run and stop state. `interfaces` holds exchanged values rather than a second state store. This split keeps package verification and revision checks out of transport handlers and maps typed errors once at the HTTP boundary.

The current Start/Restart wrapper contains a saved revision, ordered process specifications, resolved inputs, and ownership mode. The supervisor creates and binds the run ID; readiness and stop remain runtime responsibilities. The [package design](packages.md) describes admission, and the [data design](data.md) describes the presentation boundary. A fully consolidated admission plan is still [open](../maintenance/todo.md).

[中文](../../../docs_cn/isos/design/server_cn.md)

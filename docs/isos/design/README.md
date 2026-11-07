# Implemented IsOS design

This folder is for contributors who need the reasons behind IsOS's current boundaries. A **registered type** defines what a node may do, a **core** owns its execution, and an **exact pin** identifies verified package bytes. Field-level contracts live under [interfaces](../interfaces/README.md); these pages explain the flow without redefining them.

- [Server](server.md) follows a local request through `app`, `domain`, `runtime`, and exchanged values; it explains why transport handlers do not decide package or graph authority.
- [Packages](packages.md) follows an exact pin through release, resource, owner, and lifecycle admission; it explains why IsOS does not parse package-specific payloads.
- [Data](data.md) explains output descriptor qualification and why the producer retains payload bytes and endpoint authorization.
- [Control path](control-path.md) follows a key state through explicit HTTP bridges, EOS Dispatcher, base hardware, and a simulator package.

[中文](../../../docs_cn/isos/design/README_cn.md)

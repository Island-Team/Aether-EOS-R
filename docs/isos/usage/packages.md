# Operate packages

Search or inspect catalog metadata, then install or link an exact `{id,version}` release. Installation does not select it in the saved graph or start a process. A graph instance records the exact pin and stable resource IDs. Validate before revision-checked Apply; Run and Restart reverify the same release, payload, registered type, resources, owner, and lifecycle capability. Remove a package only after saved and active references are gone. See [release](../interfaces/packages/release.md), [verification](../interfaces/packages/verification.md), and [resources](../interfaces/packages/resources.md).

A failed operation leaves saved graph bytes and active run unchanged; see [package rejection](../debug/troubleshooting.md).

[中文](../../../docs_cn/isos/usage/packages_cn.md)

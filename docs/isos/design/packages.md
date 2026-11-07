# Package admission and launch

This design explains the implemented path from an exact package pin to a run candidate. The [release](../interfaces/packages/release.md), [resource](../interfaces/packages/resources.md), and [process](../interfaces/packages/process.md) pages own the public fields.

IsOS verifies the release and payload, resolves the registered node type and declared resource IDs, checks contained file digests, and selects the owner from registered placement. It uses the same exact-pin verifier during graph admission and launch. A package ID, executable filename, or metadata extension never selects a special parser. The owning core validates its lifecycle adapter and package-specific semantics before readiness.

The current typed Start/Restart wrapper groups the saved revision, ordered process specifications, resolved inputs, and ownership mode. The supervisor creates the run ID and manages readiness and stop. The bound handoff carries the verified executable and resources, selected owner, run ID, and revision. Only IsOS-owned direct children are supervised by IsOS. Today, package process handoff is supported for EOS; other owners return a typed unavailable error. Further plan consolidation and owner integrations are tracked in [TODO](../maintenance/todo.md).

[中文](../../../docs_cn/isos/design/packages_cn.md)

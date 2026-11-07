# Managed component node

[中文](../../../../docs_cn/isos/layers/process/managed-component_cn.md) · [Graph contract](../../interfaces/nodes/instance.md) · [Process contribution](../../interfaces/packages/process.md)

The registered process type places an IsOS-managed component under `isos/process/component`. Its type declaration determines allowed capabilities, settings, ports, and lifecycle; an instance supplies stable identity, dependencies, exact package pin, and declared setting values. IsOS resolves only a verified entrypoint and supervises only its own direct child. A package-contributed process for another core is handed to that owner. No arbitrary graph command or package-ID-specific launcher is accepted.

Readiness is owner-verified. Failed readiness or unknown stop cannot become a successful run.

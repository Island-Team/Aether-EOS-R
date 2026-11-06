# Registered IsOS type catalog

A **core** owns execution, a **layer** groups a major domain, a **group** names a type family, and a **node** is a configured instance. A [registered type](../interfaces/nodes/type.md) fixes placement, ports, capabilities, settings, and lifecycle; the saved instance chooses values.

- [Managed component](process/managed-component.md) describes the IsOS-owned process family: a verified package entrypoint is supervised as IsOS's direct child, with owner readiness and stop checks.
- [Vision panel consumer](ui/vision-panel.md) describes how a UI-owned node refers to an output port and selects a rendering method. IsosUI owns layout and subscriptions; IsOS does not render payloads.

Package-contributed types use the same [generic node contract](../interfaces/nodes/README.md) and [package verification](../interfaces/packages/README.md). This catalog explains roles; field-level authority stays in those contracts.

[中文](../../../docs_cn/isos/layers/README_cn.md)

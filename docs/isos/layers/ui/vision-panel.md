# Vision panel consumer

This page explains the IsosUI-owned Vision node that can reference an applied graph output. It is listed here so readers can follow the core → layer → group → node graph hierarchy; IsOS does not register or render this UI type itself. The selected package or owner catalog must provide any registered type declaration.

The graph reference identifies a source node path and output port. IsOS has a pure [data selector](../../interfaces/ui/selection.md), but its current [presentation route](../../develop/http/presentations.md) returns 503 because no owner publishes a verified public descriptor. IsosUI owns layout and subscription lifetime; the producer owns payload bytes and endpoint authorization. Closing a panel does not stop the producer.

[中文](../../../../docs_cn/isos/layers/ui/vision-panel_cn.md)

# UI node boundaries

These pages are for IsosUI producers and consumers. The graph gives each UI node a registered type and typed ports. IsOS currently accepts a run-fenced key-state event through a node output and can select display metadata from registered output types; the producer owns payload bytes and endpoint authorization.

- [Node events](node-events.md) defines the authenticated key-state request, activation ordering, and graph-owned movement settings for the first control input.
- [Selection](selection.md) defines codec, media, schema, payload, and run-fence checks when a UI chooses how to display an output.
- [Presentation](presentation.md) defines the owner descriptor and output identity that a UI may display; it does not grant a private endpoint.

[中文](../../../../docs_cn/isos/interfaces/ui/README_cn.md)

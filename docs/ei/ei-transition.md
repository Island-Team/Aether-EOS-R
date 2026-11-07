# EI naming transition (0.2.1)

[中文](../../docs_cn/ei/ei-transition.md)

EI means Embodied Intelligence and is the cognition and decision runtime inside IsOS (Island OS). EOS means Embodied OS and continues to own embodied execution and actuator admission. A single IsOS graph owns both; their runtime processes keep their existing safety and lifecycle boundaries.

For new operator commands use `bin/ei` and `bin/ei-config`; Python integrations may import `ei`. The published `bin/egia`, `bin/egia-config`, `egia` Python imports, `EGIA_PYTHON`, and `EGIA_ETC` remain accepted in 0.2.1. `EI_PYTHON` and `EI_ETC` take precedence when both names are set. Existing configuration files under `etc/egia/` and the existing wire identities remain compatible during this transition. The `ei` package is a public import path over the same implementation modules as `egia`, so do not load both paths as separate singletons in one Python process.

The graph-core migration and configuration projection are owned by IsOS. The `egia-config` adapter's legacy graph reader is retained for older deployments; a new graph must be validated and projected by IsOS before EI is started. Historical documents and fixture names retain EGIA where they record a released contract or qualification result.

For graph `0.2.0`, IsOS sends the EI owner only `ei_*` layers from the single `isos` core, together with the source graph revision. EI accepts exact `isos.ei_<layer>.<name>@v0.2` stage types, validates placement and local edges, and renders a revision-bound internal `stages.json`. Unknown types, misplaced nodes, and invalid edges return a JSON pointer diagnostic. The existing `egia` setup role and legacy `0.1.0`/`1.0.0` readers remain available for compatibility; the stage file retains the internal `egia` path until the runtime importer is renamed.

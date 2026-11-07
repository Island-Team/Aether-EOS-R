# IsosUI documentation

> **0.2.1 integration target (unreleased):** One `isos` graph core uses graph `0.2.0`; EOS, EI, and browser nodes occupy `eos_*`, `ei_*`, and `ui` layers. The editor treats older graphs as read-only pending explicit IsOS migration. Existing v0.1.0 qualification statements below are historical.


[中文](../../docs_cn/isosui/README_cn.md)

IsosUI source lives in `ui/` in the unified IsOS repository. Its unreleased source version is `0.2.1`. It edits graph `0.2.0` under the single `isos` core and reads revision-bound selected types; older graphs open read-only until IsOS migrates them. The public IsosUI `v0.1.0` release and qualification below are historical.  Read [first use](usage.md) in operator order, [the core → layer → group → node catalog projection](catalog/README.md), [the design](design.md) for the workspace and algorithms, [P07 log navigation](interfaces/log-cursors.md), [U03 workspace panels](interfaces/workspace-panels.md), then [the implementation TODO](todo.md). The IsOS graph and local API contracts belong to IsOS; EOS owns control and observation payloads. This UI renders and edits those contracts without defining a second graph format.

**Status:** IsosUI [`v0.1.0`](interfaces/releases.md#published-v010-evidence) is published, and Island-web deployed its pinned hosted archive. The cross-owner release replay and live public HTTPS browser first use remain separate qualification checks. The repository root README describes the implemented editor; design pages may also describe later targets.

[Shared Island conventions](../island/conventions.v1.3.1.md).

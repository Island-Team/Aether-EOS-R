# Unified source provenance for the 0.2.1 candidate

This record describes the private integration branch `refactor(v2)/unified-isos-core`. It is a source mapping, not a release receipt. The imported original commit IDs remain reachable through the subtree merge parents; later IsOS source IDs are on the local archive branch and its bundle. A local preservation archive at `../workspace-archive/isos-unification-20261106T025135Z` contains bundles, dirty-tree patches, untracked files, and release-asset checksums; it must be retained until the public mirror and repository cutover are verified.

| Source repository | Import branch / preserved head | Destination in this tree | Conflict decision |
| --- | --- | --- | --- |
| `Island-Team/Island-eos` | `refactor(v2)/unified-isos-core`; initial dirty work preserved at `bc286894` | Root Rust EOS crates, `adapters/eos-cxx`, `share/eos`, `docs/eos` | EOS owns its root fixtures and public 0.2.0 evidence. |
| `Island-Team/Island-egia` | `refactor(v2)/ei-rename` at `94affd1d` | `ei`, `docs/ei`, `docs_cn/ei` | EI owns its Python project and compatibility aliases; distinct shared contracts were promoted to root. |
| `Island-Team/isos` | `refactor(v2)/single-core-migration` (subtree imported at `47e81d29`; later source through `f89ec01d` copied at `ff9d46d5` and `80c3840e`, with original IDs retained on local `archive/isos-single-core-source`) | `crates/isos`, `docs/isos`, `docs_cn/isos` | IsOS owns manager graph authority; root composition and shared conventions stay with the unified tree. |
| `Island-Team/IsosUI` | `refactor(v2)/isosui-single-core` at `c90e6368` | `ui`, `docs/isosui`, `docs_cn/isosui` | Its browser fixture matrix stays under `ui/tests`; EOS keeps the root browser matrix. |

The external `island/mujoco-sim@0.1.1` package and its repository are outside this source merge. Public EOS `0.2.0` and old IsOS/IsosUI `v0.1.0` release evidence must remain identifiable as historical versions. The public source snapshot, legacy mirrors, and final branch and asset hashes are pending review.

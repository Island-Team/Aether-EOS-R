# TODO: `skill.name@major.minor.patch`

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Templates](README.md) / **builtin skill**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [templates/](README.md)
      - [backend-module.md](backend-module.md)
      - [backend-platform.md](backend-platform.md)
      - **builtin-skill.md**
      - [primitive.md](primitive.md)
      - [workflow.md](workflow.md)

</details>
<!-- docs-tree:end -->

## Introduction

Explain the user-visible outcome and why this skill is built into `eosd`. Built-in skills are trusted in-process registrations; see the [built-in runtime](../../../adapters/eos-cxx/runtime/skills/builtin/README.md) and [skill service](../interfaces/skills.md). Update paths after moving this template.

## Availability and identity

| Field | Value |
| --- | --- |
| Version | `TODO` |
| Registration source | `TODO` |
| Required robot/device capabilities | `TODO` |
| Primitive claims | `TODO` |
| Resource claims | `TODO` |

## Arguments

| Name | Type/schema | Required | Default | Units/bounds | Meaning |
| --- | --- | --- | --- | --- | --- |
| `TODO` | `TODO` | Yes/No | `TODO` | `TODO` | `TODO` |

## Outputs and events

Document the result schema, SIGNALs, progress events, checkpoints, and stable terminal reason categories. Link to the actual schema and do not copy private payloads into diagnostic examples.

## Execution and failure behavior

Explain scheduling, non-reentrancy, cancellation, timeouts, primitive cleanup, and missing dependency behavior. State which errors are retriable by the caller, if any.

## Example and verification

Show one tested REPL/API invocation, expected terminal result, and the focused test or manual verification. Link to [skill diagnostics](../debug/README.md#follow-domain-ownership). Update relative paths for the final route.

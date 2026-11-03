# TODO: `primitive.name@major.minor.patch`

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Templates](README.md) / **primitive**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [templates/](README.md)
      - [backend-module.md](backend-module.md)
      - [backend-platform.md](backend-platform.md)
      - [builtin-skill.md](builtin-skill.md)
      - **primitive.md**
      - [workflow.md](workflow.md)

</details>
<!-- docs-tree:end -->

## Introduction

State what the operation changes, its device class, and when to use it. State whether it is a target command, stream update, or safety operation. Link to [northbound discovery and release](../interfaces/actions.md) and the [southbound payload contract](../interfaces/southbound/primitives.md) after placing this page at its final route.

## Availability

| Field | Value |
| --- | --- |
| Identity and version | `TODO` |
| Target device class | `TODO` |
| Required backend capability | `TODO` |
| Declared by | `[[device.primitive]]` in a selected profile, or `TODO: implicit operation` |

## Parameters

| Name | Type | Required | Units | Valid range or enum | Meaning |
| --- | --- | --- | --- | --- | --- |
| `TODO` | `TODO` | Yes/No | `TODO` | `TODO` | `TODO` |

Describe defaults and omitted optional fields. Distinguish portable schema bounds from tighter deployment `limits` in the binding. Link each canonical field to its contract or schema.

## Result and lifecycle

Describe acknowledgement fields, terminal status, cancellation, timeout, and whether an accepted command proves physical completion. State stop and estop behavior, resource or lease requirements, and outcome-unknown cases.

## Example

Use one tested REPL or API request with a realistic but non-site-specific target. Add the expected observable result and a failure example.

## Troubleshooting and related pages

Link to [request tracing](../debug/README.md#correlate-one-request), the supporting backend module, and any profile example. Update relative paths for the final route.

# TODO: workflow ID

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Templates](README.md) / **workflow**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [templates/](README.md)
      - [backend-module.md](backend-module.md)
      - [backend-platform.md](backend-platform.md)
      - [builtin-skill.md](builtin-skill.md)
      - [primitive.md](primitive.md)
      - **workflow.md**

</details>
<!-- docs-tree:end -->

## Introduction

State the operator goal, prerequisites, and whether this is a registered template or a client-submitted definition. Link to the [WorkflowService contract](../interfaces/workflow.md).

## Inputs and prerequisites

| Input | Type/schema | Required | Default | Validation or source |
| --- | --- | --- | --- | --- |
| `TODO` | `TODO` | Yes/No | `TODO` | `TODO` |

List required skills, primitives, robot/device classes, model providers, and deployment profile assumptions.

## Action graph

| Action ID | Target skill/primitive | Depends on | Accepted facts or result | Failure effect |
| --- | --- | --- | --- | --- |
| `TODO` | `TODO` | `TODO` | `TODO` | `TODO` |

Add a small Mermaid DAG only when dependencies are hard to understand from the table. Explain supersession, no-retry semantics, and terminal conditions.

## Run and observe

Show a tested REPL/API invocation, the initial snapshot, and how to watch events with `epoch:sequence` cursor. Include the [resnapshot and resume algorithm](../interfaces/workflow.md#resnapshot-and-resume-algorithm) for long-lived clients.

## Failure and recovery

Document admission errors, missing targets, cancellation, interrupted EOS process, and journal behavior. Link to [workflow debugging](../debug/README.md#diagnose-workflow-sse-and-cursors). Update relative paths for the final route.

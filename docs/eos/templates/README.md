# EOS documentation templates

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Templates](README.md)

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [templates/](README.md)
      - [backend-module.md](backend-module.md)
      - [backend-platform.md](backend-platform.md)
      - [builtin-skill.md](builtin-skill.md)
      - [primitive.md](primitive.md)
      - [workflow.md](workflow.md)

</details>
<!-- docs-tree:end -->

[Documentation index](../README.md) · [Developer interfaces](../interfaces/README.md) · [Backend development](../develop/ext/README.md)

Use these Markdown templates when adding reference pages. Copy a template to a stable path, replace every `TODO`, and link to the authoritative contract rather than duplicating its schema.

| Page type | Template | Recommended route |
| --- | --- | --- |
| Primitive | [Primitive](primitive.md) | `reference/primitives/<name>.md` |
| Built-in skill | [Built-in skill](builtin-skill.md) | `reference/skills/builtin/<name>.md` |
| Workflow | [Workflow](workflow.md) | `reference/workflows/<name>.md` |
| Backend module | [Backend module](backend-module.md) | `reference/backends/<platform>/<module>.md` |
| Backend platform | [Backend platform](backend-platform.md) | `reference/backends/<platform>/index.md` |

Use lowercase stable filenames, relative links to other pages, and no repository-absolute links in published pages. Keep one page per public identity/version or concrete backend module. Put units, defaults, bounds, failure behavior, and a runnable example near the top. If a value is deployment-specific, label it and link to the profile rather than inventing a default. Keep navigation separate from implementation details: [backend catalog](../reference/backends/README.md), [primitive contract](../interfaces/southbound/primitives.md), [skill contract](../interfaces/skills.md), and [workflow contract](../interfaces/workflow.md).

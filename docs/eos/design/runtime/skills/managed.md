# Managed skill runtime

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Design](../../README.md) / [Runtime](../README.md) / [Skills](README.md) / **managed**

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [design/](../../README.md)
      - [runtime/](../README.md)
        - [skills/](README.md)
          - [authoring.md](authoring.md)
          - [data.md](data.md)
          - [external.md](external.md)
          - **managed.md**
          - [manager.md](manager.md)
          - [native.md](native.md)
          - [observability.md](observability.md)
          - [process-model.md](process-model.md)

</details>
<!-- docs-tree:end -->

[Skill runtime overview](../skills.md) · [SkillManager supervisor](manager.md) · [Skill authoring](../../../interfaces/skills/native/README.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/managed_cn.md)

The managed runtime folder is an obsolete fail-closed placeholder and does not contribute a `SkillRuntime` to SkillManager. Production startup registers only packaged ABI v2 worker providers; no managed language or package format is admitted.

## Ownership boundary

`adapters/eos-cxx/runtime/skills/managed/` contains no executable implementation and is not added by the skills CMake build. Its dormant local CMake file defines only an interface target if included explicitly. Do not add a parser, compiler, interpreter, package format, or executor here. The built-in registry remains a library/test component. A new language would require a separate isolated worker provider and brokered capability implementation.

## Current behavior

The normal build does not enter this folder or create its interface target. `eosd` does not link a managed parser, Python interpreter, compiler, executor, catalog, or fallback path. No `.eos.py` or managed-looking package can be discovered, admitted, registered, or launched.

```mermaid
flowchart LR
  package["managed package"] -->|"not admitted"| placeholder["managed placeholder"]
  placeholder -.->|"no SkillRuntime"| manager["SkillManager"]
```

## Admission and execution

There is no managed admission or execution API. A skill identity absent from registered runtimes fails exact lookup; SkillManager never guesses a provider from a filename, extension, language, or package metadata. Native execution is not a fallback for managed input.

## Logging and debugging

The placeholder starts no worker and emits no managed lifecycle records. An attempted start for an unregistered managed identity is reported through the normal `skills`/northbound unknown-skill path without logging package contents. The absence of a managed `SkillRuntime` in startup composition is the authoritative debug fact.

## Testing

Composition excludes the managed folder, and SkillManager unknown-identity tests verify fail-closed routing. There are no executable managed-runtime tests.

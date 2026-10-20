# Managed skill runtime

[Skill runtime overview](../skills.md) · [SkillManager supervisor](manager.md) · [Skill authoring](../../../develop/interfaces/skills/native/README.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/managed_cn.md)

The managed runtime folder is an obsolete fail-closed placeholder and does not contribute a `SkillRuntime` to SkillManager. Built-in C++ replaces it as the supported in-process authoring direction; no managed language or package format is planned by this redesign.

## Ownership boundary

`src/eos/runtime/skills/managed/` contains no executable implementation and is not added by the skills CMake build. Its dormant local CMake file defines only an interface target if included explicitly. Do not add a parser, compiler, interpreter, package format, or executor here. New in-process skills belong to the built-in registry; isolated language runtimes belong behind the future external boundary.

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

# Native skill runtime

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Design](../../README.md) / [Runtime](../README.md) / [Skills](README.md) / **native**

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
          - [managed.md](managed.md)
          - [manager.md](manager.md)
          - **native.md**
          - [observability.md](observability.md)
          - [process-model.md](process-model.md)

</details>
<!-- docs-tree:end -->

[Skill runtime overview](../skills.md) · [Worker boundary](external.md) · [Native ABI](../../../interfaces/skills/native/abi.md) · [Build, install, and test](../../../interfaces/skills/native/build-install-test.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/native_cn.md)

Native ABI v2 is the shared-library authoring contract. For enabled packaged skills, EOS admits the catalog from the verified locked release and executes each invocation in a separate `eos-skill-worker` process. The in-process `native::Runtime` still exists as an implementation and test component, but package activation selects the worker provider. The built-in coroutine framework remains available to tests, but production startup registers no built-in skills.

`adapters/eos-cxx/runtime/skills/native/` owns library discovery, ABI table validation, metadata copying, JSON Schema compilation, exact skill lookup, and callback/result validation. The worker provider owns subprocess lifecycle and IPC. SkillManager owns records, scheduling, claims, deadlines, and exact routing; Dispatcher owns primitive safety and device ordering. No skill library receives a Dispatcher pointer or backend socket.

The loader accepts only top-level regular, non-symlink `*.skill.so` files in lexical order. Each library exports `eos_skill_library_query_v2`; malformed ABI, duplicate identity, invalid schema, or unavailable declared primitive rejects admission. One library may declare at most 256 skills, and each descriptor at most 64 primitives, resources, SIGNALs, claims, and sources. Restart EOS to change the selected release.

Worker IPC **1.0.0** uses bounded Unix `SOCK_SEQPACKET` messages and optional sealed memfd payloads. EOS resolves declared capability slots to admitted devices and maps each primitive, version, and device to a canonical hardware capability. The production provider issues execution-scoped grants through the shared kernel Broker; primitive start, poll, and cancel return through that Broker to Dispatcher. Missing or mismatched grants fail closed. Declared resource, inference, source, SIGNAL, and checkpoint callbacks remain brokered by their existing owners. Child-process callbacks are not yet bridged to packaged workers. Cancellation reports an unknown outcome when terminal hardware state cannot be established. See [worker boundary](external.md) for the exact current limits.

`eos_native_skills_tests` verifies ABI loading and callback validation. Worker protocol and process tests verify framing, grants, pure execution, and denied hardware calls.

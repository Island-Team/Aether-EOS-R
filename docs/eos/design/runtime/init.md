# Runtime initialization

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Design](../README.md) / [Runtime](README.md) / **init**

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [design/](../README.md)
      - [runtime/](README.md)
        - [northbound/](northbound/README.md)
        - [skills/](skills/README.md)
        - [actionlist.md](actionlist.md)
        - [dispatch.md](dispatch.md)
        - [hardware.md](hardware-legacy.md)
        - **init.md**
        - [kernel.md](kernel.md)
        - [models.md](models.md)
        - [northbound.md](northbound.md)
        - [perception.md](perception.md)
        - [skills.md](skills.md)
        - [store.md](store.md)

</details>
<!-- docs-tree:end -->

[Runtime overview](README.md) describes the integrated dataflow and links the runtime module set.

`adapters/eos-cxx/runtime/init/` owns process configuration, immutable contract loading, and runtime composition. `eosd` only parses CLI flags, installs signal handling, and calls `eos::runtime::init::Run`.

`config.cpp` strictly parses `eos.toml`, including gateway/log settings, the single selected hardware binding, primitive exposure, and model-runtime providers. Unknown keys, malformed types, invalid ports, duplicate model IDs, and invalid endpoint or byte-limit settings fail before listeners start.

`schemas.cpp` implements the process-lifetime shared `SchemaStore`. It reads the build-validated `contracts.manifest.json`, rejects missing, extra, or identity-mismatched files, and compiles the primitive and northbound resource contract tree once. It resolves `$ref` entries and provides const validation for primitive requests, acknowledgements, and body/world resources. Northbound and dispatch use this shared store. Private model request and response schemas are loaded independently by `runtime/models` from `share/eos/contracts/eos/external/models/schemas/`; they are intentionally outside the shared contract manifest. Production startup does not invoke Python.

`runtime.cpp` loads EOS domain configuration and constructs models, hardware adapters, perception, dispatch, skills, and the northbound server. It does not start external services or drivers. The systemd user launch manager owns their lifecycle through declared units; ROS transport runs in a separate managed bridge. EOS opens its gRPC listener only after domain admission succeeds and tears down its own services in reverse construction order.

Tests under `adapters/eos-cxx/runtime/init/tests/` cover strict configuration, contract indexing and validation, cross-schema references, and startup-path errors.

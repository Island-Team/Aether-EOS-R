# Runtime initialization

[Runtime overview](README_cn.md) links the integrated runtime module set.

`src/eos/runtime/init/` owns process configuration, immutable contract loading, and runtime composition. `eosd` only parses CLI flags, installs signal handling, and calls `eos::runtime::init::Run`.

`config.cpp` strictly parses `eos.toml`, including gateway/log settings, the single selected hardware binding, primitive exposure, and outbound perception-model clients. Unknown keys, malformed types, invalid ports, duplicate model IDs, and invalid endpoint or byte-limit settings fail before listeners start.

`schemas.cpp` implements the process-lifetime shared `SchemaStore`. It reads `contracts.manifest.json`, rejects missing, extra, or identity-mismatched files, resolves `$ref`, and compiles primitive plus northbound resource contracts once. Dispatch, northbound, and perception projection use this store. Private model request/response schemas are loaded independently by `runtime/models` from `share/contracts/eos/external/models/schemas/` and are intentionally outside the shared manifest.

`runtime.cpp` resolves paths and logging, runs prelaunch entries, then constructs the shared schema store, model Runtime and external-adapter factory, hardware Registry, optional camera/VAD pumps, perception Reader, Dispatcher, native skill Runtime, and northbound server. The skill executor holds the same Dispatcher used by direct primitive calls. Startup is all-or-nothing and opens the gRPC listener only after domain admission succeeds. On shutdown, init calls the server's idempotent `Stop()`, joins its blocking `Run()` thread, and only then destroys stack-owned services in reverse construction order. Long-running shell prelaunch entries run in their own process groups, write `var/log/eos/runs/<launch-id>/eosd/prelaunch-<index>.log`, and receive `SIGTERM` during teardown.

Tests under `src/eos/runtime/init/tests/` cover strict configuration, contract indexing and validation, cross-schema references, and startup-path errors.

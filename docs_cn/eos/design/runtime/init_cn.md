# Runtime initialization

[Runtime overview](README_cn.md) links the integrated runtime module set.

`adapters/eos-cxx/runtime/init/` owns process configuration, immutable contract loading, and runtime composition. `eosd` only parses CLI flags, installs signal handling, and calls `eos::runtime::init::Run`.

`config.cpp` strictly parses `eos.toml`, including gateway/log settings, the single selected hardware binding, primitive exposure, and outbound perception-model clients. Unknown keys, malformed types, invalid ports, duplicate model IDs, and invalid endpoint or byte-limit settings fail before listeners start.

`schemas.cpp` implements the process-lifetime shared `SchemaStore`. It reads `contracts.manifest.json`, rejects missing, extra, or identity-mismatched files, resolves `$ref`, and compiles primitive plus northbound resource contracts once. Dispatch, northbound, and perception projection use this store. Private model request/response schemas are loaded independently by `runtime/models` from `share/eos/contracts/eos/external/models/schemas/` and are intentionally outside the shared manifest.

`runtime.cpp` 加载 EOS 领域配置并构造模型、硬件适配器、感知、分发、技能及北向服务。它不启动外部服务或驱动。systemd 用户启动管理器通过声明的单元管理进程生命周期；ROS 传输位于独立的受管理桥接进程。EOS 在领域准入成功后才开启 gRPC listener，关闭时按构造逆序释放自身服务。

Tests under `adapters/eos-cxx/runtime/init/tests/` cover strict configuration, contract indexing and validation, cross-schema references, and startup-path errors.

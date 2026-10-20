# 旧版原生 ABI v2 技能

[接口总览](../../README_cn.md) · [gRPC 接口](../../grpc_cn.md) · [技能运行机制](../../../../design/runtime/skills_cn.md)

> **Legacy SDK：** 本文记录现有 deployment-owned module 使用的 ABI v2。新的 first-party skill 应使用 [built-in C++ authoring API](../../../../design/runtime/skills/authoring_cn.md)。迁移期间继续支持 Native ABI v2，但它不再是首选 authoring surface。

本文只说明如何编写 EOS 原生技能。C++17 作者应优先使用 `<eos/skill/sdk.hpp>`、`EOS::SkillCpp` 与 `eos_add_skill`； `<eos/skill/abi.h>` 保留为稳定的 C ABI 与兼容接口。当前唯一支持的契约是 ABI v2；仅导出旧版查询符号的动态库会在启动准入时被拒绝。

原生技能是由部署方审核的 C/C++ `*.skill.so`，在 `eosd` 进程内运行。它不是沙箱：错误或恶意代码能够阻塞、崩溃或破坏进程。

## 编写要求

1. 通过 `find_package(EOSSkill CONFIG REQUIRED)` 使用安装后的 SDK。
2. 为参数、结果和 SIGNAL 定义自包含的 object JSON Schema。
3. 完整声明会调用的 primitive、会读取的 resource URI 模板、inference authority、SIGNAL 和 claim。
4. 实现同步、有界的 handler，并返回 `eos::skill::Outcome`；`Run` 会隔离异常并完成 C ABI 映射。
5. 发布进程生命周期内保持有效且不可变的 `eos_skill_descriptor_v2` 与 `eos_skill_library_v2`。
6. 只导出 `eos_skill_library_query_v2`。
7. 构建名称后缀严格为 `.skill.so` 的 `MODULE`，安装后重启 EOS。
8. 使用 `ListSkills` 核对准入后的 descriptor，再在 demo backend 上测试。

名称使用小写身份格式，版本使用精确 SemVer。`{name, version}` 是发现和执行身份。所有 `struct_size` 都必须使用当前头文件对应类型的精确 `sizeof`。

## 执行边界

EOS 在调用技能前验证参数 schema，并提供 invocation 范围内的 `eos_skill_host_v2`。技能只能通过 host：

- 调用 descriptor 中声明的 primitive；
- 读取声明 URI 模板覆盖的 resource；
- 检查协作式取消；
- 接收声明的入站 SIGNAL；
- 发出声明且 payload 通过 schema 的 SIGNAL。
- 只对 descriptor 声明的 `VAD`、`HUMAN`、`HUMAN_POSE` 或 `FACE_RECOGNITION` authority 执行 one-shot inference。
- 通过 `logger()` 写结构化诊断，并通过 `checkpoint()` 发布 execution milestone；
- 在非 OK execute 返回前，通过可选 `report_failure` 上报一次 terminal failure。

Host pointer、context 和返回的字节切片都不能跨 callback 保存，也不能从其他线程调用。循环、等待和操作必须有界；异常不能穿过 C ABI。

正常路径必须且只能调用一次 completion，并仅在 completion 成功时返回 `EOS_SKILL_EXECUTE_OK`。取消路径返回 `EOS_SKILL_EXECUTE_CANCELLED`，不调用 completion。

`log`、`emit_checkpoint` 与 `report_failure` 是追加在 ABI v2 host table 尾部的 可选 callback。SDK 会先用 `EOS_SKILL_HOST_V2_HAS` 检查 `struct_size`；在旧版 v2 host 上前两者返回 unavailable，failure reporting 则为 no-op，原有 capability 仍可使用，预期 skill outcome 不变。旧 ABI v2 skill binary 也可继续由新版 host 加载。

`infer_once` 同样是 ABI v2 host table 尾部的可选 callback；读取前必须用 `EOS_SKILL_HOST_V2_HAS(host, infer_once)` 检查。Vision request 以非空 `source`、非零 `source_sequence` 与 `source_generation` 指定一个 exact retained frame。VAD 则使用 caller 在 `options_json.input` 中提供的 canonical audio，并要求 `source` 为空且两个 provenance 数值均为零；result 也保持 empty/zero provenance。`options_json` 与成功 result 都是 JSON object。EOS 会在到达 provider 前拒绝未声明 authority、伪造的 VAD provenance，以及不对应所请求 frame 的 vision provider response。C++ facade 对 vision 提供 retained-frame typed call，对 VAD 提供 `infer_once<VadInference>(options)`；每次调用都是 one-shot，是否重试由 skill 明确决定。

`eos_skill_failure_v2` 携带稳定、machine-readable 的 `reason_code` 与可选 operator-facing `message`。`Outcome::InvalidArgument(reason, message)` 与 `Outcome::Failure(reason, message)` 会先尝试调用 `report_failure`，再返回对应的 非 OK execute status；cancellation 不上报 skill-authored failure。escaped exception 统一映射为 `skill.exception` 与安全的通用 message，不复制原始 exception text。 每次 invocation 最多接受一次 failure report。

completion 在原始 ABI v2 布局尾部追加了 `message`。C consumer 必须接受 `EOS_SKILL_COMPLETION_V2_BASE_SIZE`，并仅在 `EOS_SKILL_COMPLETION_V2_HAS(completion, message)` 为真时读取该字段。新版 skill 因此可在旧 host 上安全退化；新版 host 也可读取旧 completion。Success、Reject、 InvalidArgument 与 Failure 都可携带 message，reason 才是自动化契约，client 不得 解析 message。message 最多 1,024 bytes，reason 最多 128 bytes。

日志只进入受保护、仅 operator 可见的 `skills` logger；checkpoint 则成为 Agent 可见 journal record。name 最多 128 bytes、message 最多 1 KiB、object fields 最多 16 KiB；checkpoint 使用小写点分名称，且不能占用 `eos.*` 或 `execution.*`。每个 execution 的 日志限速为每秒 16 条、burst 64、最多接受 1,024 条；checkpoint 最多 256 条。 不得记录 argument、result、device ID、具体 resource URI/value、SIGNAL payload、 primitive acknowledgement、schema、credential、姓名、识别文本或其他个人数据。 相同限制也适用于 terminal message；它只能包含简短的阶段与安全错误类别。

capability 失败时，应返回具体阶段 reason 与安全 message，而不是 generic failure 或原始 payload：

```cpp
return eos::skill::Outcome::Failure(
    "detection.read_failed", "detection snapshot read failed: capability unavailable");
```

## SIGNAL、claim 与并发

SIGNAL 名称为小写点分格式，payload schema 的根类型必须是 object。`CANCEL` 由 manager 保留，技能不能声明。发出 SIGNAL 只会把事件写入 execution history，不会直接路由到另一个技能。

跨技能协作应保持 capability 边界：resource 只提供只读快照；修改硬件或外部状态 必须调用已声明 primitive；技能不能直接调用另一个技能。需要协作时，由 Agent 路由 SIGNAL。仓库内三个 Island 技能统一接收 `skill.command`（`pause`/`resume`）， 并发出包含 skill、phase、state 与 progress 的 `skill.event`。SIGNAL 不会扩大 primitive、resource 或 claim 权限；停止 execution 仍使用 manager 的 `TerminateExecution`，不能伪装成 SIGNAL。

可中断操作应优先使用 `start_primitive`、有界 `wait_primitive` 轮询与 `cancel_primitive`，并在等待之间检查取消和入站控制。只有旧 ABI v2 host 未提供 这些可选 callback 时才回退到同步调用。resource 没有写接口；写操作均通过 primitive，以保留 schema 校验、claim、取消与 operation accounting。

Claim 通过 RFC 6901 JSON Pointer 从已验证参数中解析非空 device ID 或 resource URI。`SHARED_READ` 可与同一身份的 reader 共存；`EXCLUSIVE_USE` 与两种模式都冲突。SIGNAL 不能新增 claim 或扩大权限。

只有 descriptor context 和全部代码路径都支持并发时，才能设置 `reentrant = 1`。否则使用零，manager 会加入隐式的独占 provider-context claim。

## 构建与准入

推荐的 CMake 与导出方式是：

```cmake
find_package(EOSSkill CONFIG REQUIRED)
eos_add_skill(TARGET example_skill OUTPUT_NAME example SOURCES example.cpp)
```

`EOS::SkillABI` 只提供 raw header；`EOS::SkillCpp` 提供 header-only C++17 facade 与 JSON dependency。`eos_add_skill` 生成 `.skill.so`、设置隐藏可见性， 并保证只导出查询符号。C 作者仍可直接实现并导出：

```cpp
extern "C" EOS_SKILL_EXPORT
const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t version) {
  return version == EOS_SKILL_ABI_VERSION_V2 ? &library : nullptr;
}
```

EOS 只扫描技能目录顶层的普通、非符号链接 `*.skill.so`。任一库存在错误 ABI、无效 table/schema/declaration、重复身份、未知 primitive 或不可读 resource 模板，都会拒绝整次配置加载。当前没有 hot reload。

ABI v2 descriptor 尾部可追加 `inference_count` 与 `inferences`，consumer 必须用 `EOS_SKILL_DESCRIPTOR_V2_HAS` 检查。当前 producer 还把 `skill_descriptor_size = sizeof(eos_skill_descriptor_v2)` 写入 library table，host 用 `EOS_SKILL_LIBRARY_V2_HAS` 检查该追加字段，并按此 byte stride 遍历 descriptor array；header 截止于 `skills` 的旧 library 使用 `EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE`。这使 array stride 不依赖后续追加到 descriptor 的字段。

完整且持续维护的 built-in coroutine 示例见 [`share/skills/native/observe_human`](../../../../../../share/skills/native/observe_human/README.md)： `1.0.0` 提供可控制的扫描与跟随流程，在三个严格递增的 source generation 上稳定同一 human，不执行私有 perception inference 或 attachment 日志。需要 pose、action 或 identity 的 skill 独立读取共享 perception resource。 英文权威编写指南见 [`docs/eos/develop/interfaces/skills/`](../../../../../../docs/eos/develop/interfaces/skills/native/README.md)。

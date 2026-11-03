# 编写 EOS 原生技能

[English](../../../../../docs/eos/interfaces/skills/native/README.md) · [SkillService](../../skills_cn.md) · [技能运行时](../../../design/runtime/skills_cn.md)

**先读 [ABI 所有权与 0.x 目标边界](host-abi_cn.md)。** EOS 提供 host 能力 ABI，软件包导出描述符和查询符号。当前实现在可选 C++ 侧链中；默认 Rust EOS runtime 尚未准入原生技能包。目标 Rust supervisor 将为每次执行启动隔离 worker，并通过 Broker 提供能力；技能代码不能直接访问硬件 HAL。

安装后的 `EOS::SkillABI` 与 `EOS::SkillCpp` 是技能编写接口。可选的 `EOS::SkillSDK` 提供共享库 `libeos_skill_sdk.so.2`；`<eos/skill/support.h>` 可查询 ABI 版本并检查库头与技能表布局。它不是 worker RPC client。当前不准入 Python 外部技能；未来的 pybind11 bridge 必须放在独立的受监督 Python worker 内，且只能调用 broker 授权的执行 API。准入和 sandbox 门槛见[worker 架构](../../../design/runtime/skills/external_cn.md)。

按首次使用顺序阅读：

1. [Host ABI 与软件包边界](host-abi_cn.md)：职责、准入、执行、故障和停止。
2. [软件包编写](package-abi_cn.md)与[示例](example_cn.md)：使用 `<eos/skill/sdk.hpp>` 或 `<eos/skill/abi.h>` 构建经审查的 `.skill.so`。
3. [构建、安装和测试](build-install-test_cn.md)：编译并选择精确的软件包版本。
4. [能力操作](capabilities_cn.md)与[技能社区](ipc_cn.md)：invocation 范围内的请求与协作。
5. [C 布局参考](abi_cn.md)：原始 ABI 字段、生命周期和可选尾字段检查。

[内置技能指南](builtin_cn.md)用于 C++ 库开发和测试，不是生产软件包路径。

技能经由 skill host 读取声明的 EOS 资源、调用声明的 primitive。EOS 在授予能力前校验声明、参数、claim 和执行请求。打包 ABI v2 技能使用[worker 执行路径](../../../design/runtime/skills/external_cn.md)。primitive、resource、inference、source、SIGNAL、checkpoint 与可选 ABI v2.1 社区 callback 经 worker broker 转发；子进程 callback 尚未桥接。

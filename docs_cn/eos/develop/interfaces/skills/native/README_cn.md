# 编写 EOS 原生技能

[English](../../../../../../docs/eos/develop/interfaces/skills/native/README.md) · [SkillService](../../skills_cn.md) · [技能运行时](../../../../design/runtime/skills_cn.md)

EOS 支持两种进程内 C++ 编写方式。**首选内置 C++ 技能**。部署方维护的模块在迁移期间仍可使用 C/C++ 共享库 ABI v2。两者都不创建外部技能进程，也不授予技能代码直接访问硬件 HAL 的权限。

1. [内置技能指南](builtin_cn.md)：定义并显式注册协程 handler、schema 和声明的能力。
2. [旧版 ABI v2 指南](legacy-abi-v2_cn.md)：使用 `<eos/skill/sdk.hpp>` 或 `<eos/skill/abi.h>` 构建经审查的 `.skill.so`。
3. [能力使用](capabilities_cn.md)、[本地 mailbox IPC](ipc_cn.md)和[示例](example_cn.md)：invocation 范围内的工具与协作。
4. [ABI 参考](abi_cn.md)及[构建、安装和测试](build-install-test_cn.md)：受支持的共享库路径的细节。

技能经由 skill host 读取声明的 EOS 资源、调用声明的 primitive。EOS 在授予能力前校验声明、参数、claim 和执行请求。[外部技能执行](../external_cn.md)尚不支持。

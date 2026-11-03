# 原生技能运行时

[Skill runtime 总览](../skills_cn.md) · [Worker 边界](external_cn.md) · [Native ABI](../../../../../docs/eos/interfaces/skills/native/abi.md) · [English](../../../../../docs/eos/design/runtime/skills/native.md)

Native ABI v2 是共享库编写接口。启用的打包技能从 `isos.lock` 验证的 release 准入 catalog，每次执行在独立 `eos-skill-worker` 进程中运行。进程内 `native::Runtime` 仍用于实现和测试，但 package activation 选择 worker provider。内置协程框架仍用于代码和测试，但 production startup 不注册内置技能。

`native/` 负责 ABI validation、metadata、JSON Schema 和 callback/result validation；worker provider 负责进程与 IPC；SkillManager 负责调度、claim 和 execution record；Dispatcher 负责 primitive 安全与设备排序。技能库不持有 Dispatcher 指针或 backend socket。

加载器按字典序接受顶层普通、非 symlink 的 `*.skill.so`，要求导出 `eos_skill_library_query_v2`。ABI 错误、重复身份、schema 错误或声明的 primitive 不可用时拒绝准入。更改 release 后需重启 EOS。

worker IPC **1.0.0** 使用有界 Unix `SOCK_SEQPACKET` packet 和可选 sealed memfd。EOS 将已声明的 capability slot 映射到已准入设备，再把每个 primitive、版本和设备映射到规范硬件能力。生产 provider 通过共享内核 Broker 颁发执行范围内的授权；primitive 启动、轮询和取消通过该 Broker 到达 Dispatcher。缺失或不匹配的授权默认拒绝。resource、inference、source、SIGNAL 与 checkpoint callback 继续由原有所有者代理；子进程 callback 尚未桥接。无法确认硬件终态时报告 unknown outcome。详细限制见 [worker 边界](external_cn.md)。

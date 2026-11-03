# 外部技能执行

[SkillService](../skills_cn.md) · [原生技能编写](native/README_cn.md) · [English](../../../../docs/eos/interfaces/skills.md#external-skill-execution)

EOS 当前会在独立 worker 中运行打包的 C/C++ ABI v2 技能，并通过内部 IPC 与 Broker 转发受声明约束的能力。本文讨论的外部语言 profile（Python）尚未准入，也没有可用于 Python 软件包的运行契约。

Python 支持需要独立的静态描述验证、worker provider、解释器 sandbox bootstrap，以及 worker 内的 pybind11 Broker bridge。不得将 Python 导入 `eosd` 或 IsOS manager，也不得在 catalog admission 阶段导入技能代码。当前 ABI v2 worker 的限制和 Python 3.12 profile 的精确门槛见[worker 架构](../../design/runtime/skills/external_cn.md)。在该 profile 经过准入和进程隔离测试前，Python skill package 必须被拒绝。

[本地 mailbox IPC 指南](native/ipc_cn.md)说明由 SkillManager 管理的协作语义，不会向技能包授予额外能力。

# 外部技能执行

[SkillService](../skills_cn.md) · [原生技能编写](native/README_cn.md) · [English](../../../../../docs/eos/develop/interfaces/skills.md#external-skill-execution)

EOS 目前不接纳或运行外部技能进程。外部执行 profile 是拒绝执行的占位实现，不定义可运行的 IPC 契约。新技能使用内置 C++ API；已有原生模块仍可使用 ABI v2。[本地 mailbox IPC 指南](native/ipc_cn.md)说明受支持技能运行时内部的协作，不授予外部进程技能权限。

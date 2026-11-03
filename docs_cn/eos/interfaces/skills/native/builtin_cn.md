# 编写内置 C++ 技能

[原生技能索引](README_cn.md) · [English](../../../../../docs/eos/interfaces/skills/native/builtin.md) · [运行时细节](../../../design/runtime/skills/authoring_cn.md)

内置 C++ 框架保留用于库开发和测试。production startup 不注册内置技能；部署技能使用打包 ABI v2 worker 路径。测试或其他组合可将定义编译进去，并在其 island skill registry 中显式注册。使用 `eos::runtime::skills::builtin::Definition` 指定精确名称和版本、object 参数与结果 schema、声明的 primitive 与 resource 能力、SIGNAL 与 mailbox channel、claim，以及 `Task<Outcome>` handler。启动准入会拒绝无效或重复的定义。

这种测试组合中的定义可以使用 `builtin::Execution` 接口。handler 通过 `builtin::Execution` wrapper 进行声明的资源读取、primitive start/wait/cancel、取消检查、SIGNAL、父子 mailbox 通信、checkpoint 和诊断。在该组合的 island registry 中显式注册；不支持静态注册。

handler 不应直接访问 Dispatcher、硬件、模型内部、工作流状态、文件系统或网络。所有操作应有界并协作式处理取消。返回稳定的原因码和有界的运维消息。部署前使用 fake hook 测试准入、成功、拒绝、取消、超时、claim 冲突、mailbox 背压和清理。

[运行时编写页面](../../../design/runtime/skills/authoring_cn.md)给出最小 `Definition` 示例及进程模型。其他进程通过 [SkillService](../../skills_cn.md)发现并运行已接纳的技能。

# 编写内置 C++ 技能

[原生技能索引](README_cn.md) · [English](../../../../../../docs/eos/develop/interfaces/skills/native/builtin.md) · [运行时细节](../../../../design/runtime/skills/authoring_cn.md)

内置技能编译进 EOS，并在 island skill registry 中显式注册。使用 `eos::runtime::skills::builtin::Definition` 指定精确名称和版本、object 参数与结果 schema、声明的 primitive 与 resource 能力、SIGNAL 与 mailbox channel、claim，以及 `Task<Outcome>` handler。启动准入会拒绝无效或重复的定义。

可以从维护中的 [`navigate_waypoint`](../../../../../../share/skills/native/navigate_waypoint/navigate_waypoint.cpp) 示例开始。handler 通过 `builtin::Execution` wrapper 进行声明的资源读取、primitive start/wait/cancel、取消检查、SIGNAL、父子 mailbox 通信、checkpoint 和诊断。正常初始化时在 island registry 中注册；不支持静态注册。

handler 不应直接访问 Dispatcher、硬件、模型内部、工作流状态、文件系统或网络。所有操作应有界并协作式处理取消。返回稳定的原因码和有界的运维消息。部署前使用 fake hook 测试准入、成功、拒绝、取消、超时、claim 冲突、mailbox 背压和清理。

[运行时编写页面](../../../../design/runtime/skills/authoring_cn.md)给出最小 `Definition` 示例及进程模型。其他进程通过 [SkillService](../../skills_cn.md)发现并运行已接纳的技能。

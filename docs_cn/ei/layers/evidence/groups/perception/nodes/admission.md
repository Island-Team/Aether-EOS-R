# `egia / evidence / perception / admission`

[English](../../../../../../../docs/ei/layers/evidence/groups/perception/nodes/admission.md) · [阶段契约](../../../../../interfaces/pipeline.md) · [当前模块算法](../../../../../core/perception.md)

**目标类型：** `egia.perception.admission@v0.1`。**类型说明：** 将带来源的 EOS 与操作员证据接纳到有界观测状态。一个已配置实例是 Python 进程中可观察的一个阶段，不是独立子进程。实例有自己的非空操作员说明。A01 在 managed EGIA 启动时把此图实例绑定到现有 Python 所有者。带 stub 的 EOS 已验证有类型的 link → perception → context → decision 证据路径；Mission、result、health 与受治理命令行为仍沿用当前 core 模块，直到后续阶段集成切片。

**输入：** resource、result、可选 hearing。**输出：** observations。[阶段契约](../../../../../interfaces/pipeline.md)是端口值类型、单位、可选性、默认值、设置校验和兼容规则的权威来源；图契约拥有连线优先级与频率。

**算法与故障：** 验证来源身份与事件结构；保留事件顺序，按键合并可替换状态并计数有界遗漏。畸形证据被拒绝并记录，绝不变成空成功。

**兼容：** 此 `@v0.1` 阶段是新身份，没有一对一的 `@v1.0` 前身。已知历史 `egia.common.source/context/planner/guard@v1.0` 设计身份只保留既有、不可执行的行为，绝不暗中映射到此阶段。新增可选设置须有默认值；改变端口语义、单位、方向、安全规则或动作权限须使用新的类型 ID。未知未来版本须报告节点路径错误。

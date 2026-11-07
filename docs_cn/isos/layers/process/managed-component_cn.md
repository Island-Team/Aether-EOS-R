# 受管理组件节点

[English](../../../../docs/isos/layers/process/managed-component.md) · [图契约](../../interfaces/nodes/instance_cn.md) · [进程贡献](../../interfaces/packages/process_cn.md)

注册进程类型把 IsOS 管理的组件放在 `isos/process/component` 下。类型声明决定允许的能力、设置、端口及生命周期；实例提供稳定身份、依赖、精确软件包锁定和声明设置值。IsOS 仅解析经过验证的入口，并只监护自己的直接子进程。其他核心的软件包贡献进程交给该所有者。不接受任意图命令或按包 ID 特制的启动器。

就绪由所有者验证。就绪失败或停止不确定不能变成成功运行。实例必须使用当前已注册的类型 ID。

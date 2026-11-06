# 软件包准入与启动

本设计说明已实现的从精确软件包版本引用到候选运行的路径。[发布](../interfaces/packages/release_cn.md)、[资源](../interfaces/packages/resources_cn.md)和[进程](../interfaces/packages/process_cn.md)页面定义公开字段。

IsOS 验证发布与负载，解析注册节点类型和声明的资源 ID，检查受控文件摘要，并依据注册位置选择归属方。图准入和启动使用同一个精确版本验证器。软件包 ID、可执行文件名和元数据扩展都不会选择特殊解析器。归属核心在就绪前验证自己的生命周期适配器及软件包特有语义。

当前有类型的 Start/Restart 包装器组合保存修订、有序进程规格、已解析输入和归属模式。监管器创建运行 ID 并管理就绪与停止。绑定的交接信息携带已验证的可执行文件和资源、所选归属方、运行 ID 与修订。IsOS 只监管自己直接启动的子进程。当前软件包进程交接支持 EOS；其他归属方返回有类型的不可用错误。后续计划整合和归属方集成列于[待办](../maintenance/todo_cn.md)。

[English](../../../docs/isos/design/packages.md)

# 服务端归属

本页面向贡献者说明当前源码边界。[HTTP 契约](../develop/http/README_cn.md)和[归属方契约](../develop/owner/README_cn.md)定义公开字段；[验证](../maintenance/verification_cn.md)记录检查结果。

```text
src/
  main.rs               启动应用
  app/                  CLI、本地 HTTP、UI 包
    server/routes/       按领域划分的 HTTP 处理器
  domain/               图、注册表、软件包、启动决策
  runtime/              manager IPC、子进程、归属方交换、日志
  interfaces/           有类型的边界值
```

`app` 验证身份并解析请求，再把决策交给 `domain`，把实时操作交给 `runtime`。`domain` 验证精确版本和注册声明，但不拥有进程。`runtime` 串行化 manager 变更，只监管 IsOS 直接启动的子进程，并保存运行及停止状态。`interfaces` 保存交换值，而不是第二份状态。这种分工使软件包验证和修订检查不落在传输处理器中，并在 HTTP 边界统一映射有类型错误。

当前 Start/Restart 包装器包含保存修订、有序进程规格、已解析输入和归属模式。监管器创建并绑定运行 ID；就绪与停止仍归 `runtime` 负责。[软件包设计](packages_cn.md)说明准入，[数据设计](data_cn.md)说明展示边界。完整统一的准入计划仍是[待办](../maintenance/todo_cn.md)。

[English](../../../docs/isos/design/server.md)

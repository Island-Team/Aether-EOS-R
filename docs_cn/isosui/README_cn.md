# IsosUI 文档

> **0.2.1 集成目标（尚未发布）：** 单个 `isos` 图核心使用图格式 `0.2.0`；EOS、EI 和浏览器节点分别位于 `eos_*`、`ei_*` 和 `ui` 层。旧图在 IsOS 显式迁移前保持只读。下文的 v0.1.0 验证记录属于历史证据。


[English](../../docs/isosui/README.md)

IsosUI 源码位于统一 IsOS 仓库的 `ui/`，尚未发布的源码版本为 `0.2.1`。它编辑唯一 `isos` 核心下的 `0.2.0` 图并读取绑定修订的已选类型；旧图在 IsOS 迁移前只读。下文公开 IsosUI `v0.1.0` 发布与资格记录属于历史证据。首次使用请按[使用指南](usage_cn.md)的操作顺序阅读，再看[core → layer → group → node 目录呈现](catalog/README_cn.md)、[设计](design_cn.md)中的工作区与算法、[P07 日志导航](interfaces/log-cursors_cn.md)、[U03 工作区面板](interfaces/workspace-panels_cn.md)，最后看[实现 TODO](todo_cn.md)。IsOS 拥有图和本地 API 契约，EOS 拥有控制与观察负载契约。界面呈现、编辑这些契约，不再定义第二种图格式。

**状态：** IsosUI [`v0.1.0`](interfaces/releases_cn.md) 已发布，Island-web 已部署固定的托管归档。跨所有者发布回放与真实公网 HTTPS 浏览器首次使用仍需分别验证。仓库根目录 README 描述已实现的编辑器；设计页也可能描述后续目标。

[Island 共享规范](../island/conventions.v1.3.1_cn.md).

# IsOS 共用契约

[English](../../docs/island/contracts.md) · [接口目录](interfaces/README_cn.md) · [EOS 通信所有权](contracts_aether_cn.md)

统一仓库为每条公开边界保留一套编写源。EOS 拥有 `share/eos/contracts/eos/` 中的 protobuf 与类型化设备负载源，及 `share/eos/schemas/eos/` 中可审查的生成产物。`ei/` 中的 EI 从仓库内 EOS 源生成 Python 传输绑定，不另写一套 EOS 模式。IsOS 拥有 `0.2.0` 图、已选类型目录、软件包凭据及绑定修订的 EOS/EI 投影。IsosUI 消费所有者契约，不创建第二条执行器路径。

新图节点 ID 在唯一 `isos` 核心下使用 `isos.<layer>.<name>@v0.2`。这种图身份不重命名已发布的 `island.eos.v1`、`island.eos.v2`、历史 `egia.contracts.*` 模式 ID、HTTP 路由或外部 MuJoCo 软件包及套接字契约。EOS Dispatcher 校验每个执行器命令；EI 安全可拒绝提议，但不能绕过准入。即使节点共用 `isos` 图核心，跨进程连线仍需显式传输。

契约变更须同步更新所有者接口、生成绑定或模式、有界成功与失败测试，以及中英文文档。准确字段与校验见 [EOS 接口目录](../eos/interfaces/README_cn.md)、[EI 契约指南](../ei/contracts.md)、[IsOS 接口](../isos/interfaces/README_cn.md)和[界面接口](../isosui/README_cn.md)。历史源码固定版本与资格声明仍保留在注明日期的证据页；源码版本 `0.2.1` 不是已发布版本。

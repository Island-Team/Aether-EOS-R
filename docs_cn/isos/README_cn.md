# IsOS 指南

IsOS（Island OS）是产品，也是新 `0.2.0` 图中唯一的核心。其源码位于本仓库 `crates/isos/`，目标源码版本 `0.2.1` 尚未发布。IsOS 保存完整部署图、验证精确软件包锁定及声明资源、提供本地 API，并直接监管 EOS 与 EI。IsosUI 源码位于 `ui/`；MuJoCo 仍是外部验证软件包。受支持的旧图在显式迁移前保持只读。**注册类型**定义节点的归属核心、位置、设置、能力、端口和生命周期。**实例**选择该类型并连接声明的端口。**归属方**是运行节点并验证自身负载语义的核心。**运行围栏**将事件及交接绑定到一个活动运行和保存图修订。

操作人员按以下路径阅读：

- [首次使用](usage/README_cn.md)建立本地主目录，说明检查状态、选择已验证节点、验证、应用和运行的顺序。
- [配置](usage/configuration_cn.md)说明哪个文件提供默认值、如何选择生产覆盖值，以及如何修复启动错误。
- [软件包选择](usage/packages_cn.md)解释如何安装或链接精确发行、检查贡献、选择稳定资源 ID，而不依赖软件包特有路径。
- [发布者登录](usage/authentication_cn.md)解释用于发布软件包的独立 Island-port CLI 登录；本地图控制不使用此凭据。
- [运行部署](usage/run_cn.md)说明依据保存修订和归属方结果进行 Validate、Apply、Run、Restart、Stop 及状态检查。
- [故障排查](debug/troubleshooting_cn.md)使用结构化诊断定位软件包拒绝、启动、就绪、日志和停止不确定等失败。

作者与贡献者按以下路径阅读：

- [外部接口](interfaces/README_cn.md)包含发布者和图作者需要的通用节点、软件包、核心及 IsosUI 契约。
- [IsOS 类型目录](layers/README_cn.md)描述当前由 IsOS 拥有的进程类型族，并解释 UI 所属的消费者。
- [设计](design/README_cn.md)解释模块归属、软件包准入、数据选择和键盘到基座的控制路径，不重复定义通信字段。
- [开发](develop/README_cn.md)为实现者定位本地 HTTP 路由、管理器 IPC、源码边界和验证命令。
- [维护](maintenance/README_cn.md)索引当前证据和剩余集成门槛。

当前 IsOS API 使用 `/api/isos/v1/*`；需要模式的 JSON 负载使用 `"schema_version":"0.1.0"`。软件包及核心作者从[外部接口](interfaces/README_cn.md)开始。

[English](../../docs/isos/README.md)

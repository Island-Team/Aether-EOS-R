# 开发 IsOS

本指南供修改当前 Rust 服务的贡献者使用。单个 Cargo 软件包生成 `isos`；`main` 进入 `app`，再把图与软件包决策交给 `domain`、实时状态交给 `runtime`。公开的节点、软件包、核心和 UI 契约位于[接口](../interfaces/README_cn.md)。

- [HTTP 路由](http/README_cn.md)列出本地请求领域、认证、封套和示例。它面向服务端及客户端实现者；外部节点事件边界另有[UI 契约](../interfaces/ui/node-events_cn.md)。
- [归属方内部](owner/README_cn.md)记录本地管理器 IPC、服务配置及日志游标。核心交接与停止字段仍位于[外部核心契约](../interfaces/cores/README_cn.md)。
- [服务设计](../design/server_cn.md)解释模块归属及请求流程，使新处理器把验证和运行时状态放在正确归属处。
- [验证](../maintenance/verification_cn.md)记录变更契约与路由的可复现命令和证据。

变更时，更新权威契约及中文镜像，测试成功和失败，再运行 `cargo fmt --all -- --check`、`cargo clippy --all-targets -- -D warnings` 和 `cargo test --all-targets --no-fail-fast`。完整输出保存到忽略的 `logs/test/<run-id>/`。代码分支和示例不要依赖软件包 ID。

[English](../../../docs/isos/develop/README.md)

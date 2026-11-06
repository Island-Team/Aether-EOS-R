# 首次使用

本顺序面向启动本地 IsOS 部署的操作人员。**精确锁定**指定一个软件包 ID 和版本；**图修订**标识保存图的字节。IsOS 在改变活动运行前验证两者。所选**核心**拥有执行并报告就绪与停止。

1. 运行 `isos init --mode dev` 创建本地开发主目录。
   [配置 IsOS](configuration_cn.md)说明示例默认值、本地覆盖、生产激活，以及如何修复启动配置错误。
2. 运行 `isos status --json` 和 `isos logs all --current`，在变更前检查状态。
3. [安装或链接软件包](packages_cn.md)。该页说明如何检查已验证发行、选择注册类型并在草稿图中引用稳定资源 ID。
4. 验证草稿，依据当前修订 Apply，然后[运行部署](run_cn.md)。该页解释 Run、Restart、Stop 和状态检查；缺少归属方或生命周期适配器时，应先解决该边界再重试。

发布者通过 CLI 使用[Island-port 登录](authentication_cn.md)认证发布；这与本地 IsOS HTTP API 分开。

[保存实例契约](../interfaces/nodes/instance_cn.md)解释图字段；[本地 HTTP 路由](../develop/http/README_cn.md)解释服务请求。软件包特有设置由归属核心记录，不放在 IsOS 中。

[English](../../../docs/isos/usage/README.md)

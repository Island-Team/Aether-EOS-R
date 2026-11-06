# 已实现的 IsOS 设计

本目录供需要理解 IsOS 当前边界原因的贡献者使用。**注册类型**规定节点可以做什么，**核心**拥有执行，**精确锁定**标识经过验证的软件包字节。字段级契约位于[接口](../interfaces/README_cn.md)；本目录解释流程而不重复定义字段。

- [服务](server_cn.md)跟踪本地请求经过 `app`、`domain`、`runtime` 和交换值的过程，说明传输处理器为何不决定软件包或图权限。
- [软件包](packages_cn.md)跟踪精确锁定通过发行、资源、归属方和生命周期准入的过程，说明 IsOS 为何不解析软件包专属负载。
- [数据](data_cn.md)解释输出描述符的资格检查，以及生产者为何保留负载字节和端点授权。
- [控制路径](control-path_cn.md)跟踪按键状态经过显式 HTTP 桥、EOS Dispatcher、基座硬件和模拟器软件包。

[English](../../../docs/isos/design/README.md)

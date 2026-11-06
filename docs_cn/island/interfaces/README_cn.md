# 共用接口目录

[Island 架构](../architecture_cn.md) · [软件包契约](packages_cn.md)

统一的 IsOS 源码使用一个 `isos` 图核心。各所有者页面定义准确字段、单位、校验与兼容性；本目录只负责导航，不建立第二套模式。

| 边界 | 所有者契约 |
| --- | --- |
| 图、软件包、本地 API 与进程生命周期 | [IsOS 接口](../../isos/interfaces/README_cn.md) |
| 硬件、Dispatcher、类型化负载、gRPC 和 MuJoCo 适配器 | [EOS 接口](../../eos/interfaces/README_cn.md) |
| EI 阶段、安全与 EOS 连接 | [EI 接口](../../ei/README_cn.md) |
| 浏览器图、日志、Vision 与控制 | [IsosUI 接口](../../isosui/README_cn.md) |
| 共用软件包组合 | [软件包契约](packages_cn.md) |

新 `0.2.0` 图类型 ID 使用 `isos.<layer>.<name>@v0.2`；产品源码版本是 `0.2.1`。HTTP、protobuf 和外部软件包分别维护版本。历史接口记录保留原已发布身份。

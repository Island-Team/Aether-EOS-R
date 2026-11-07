# IsOS 层连接与数据流

[English](../../docs/island/layer-dataflow.md) · [架构](architecture_cn.md) · [合并设计](unification-v0.2.1_cn.md)

**0.2.1 集成目标，尚未发布。** 单个 `isos` 图核心包含管理器层、`eos_*` 执行层、`ei_*` 认知层和 `ui` 浏览器层。路径表示位置，输入来源表示类型化数据边，引用或进程依赖表示顺序。它们都不授予命令权限，也不能代替显式跨进程传输。

| 步骤 | 边界与所有者 | 证据 |
| --- | --- | --- |
| 安装 | IsOS 验证精确的软件包凭据与资源；外部 MuJoCo 软件包不属于本仓库。 | 固定版本、摘要、可用性或准确故障。 |
| 编图 | IsosUI 编辑注册图；IsOS 校验类型位置、端口、选择器和连线。 | 已选目录、描述及按路径定位的诊断。 |
| 应用 | IsOS 比较保存修订，写入不可变且绑定修订的 EOS/EI 投影。 | 分离的保存与活动修订。 |
| 运行 | IsOS 启动受监管的 EOS、EI 进程；EOS 启动经过验证的 MuJoCo 子进程与适配器。 | 各所有者报告的就绪状态。 |
| 观察与动作 | EOS 产生相机和硬件证据；EI 通过类型化 gRPC 访问 EOS；浏览器通过 EOS 回环 HTTP/WS 桥接访问 EOS。两条控制路径都进入 EOS Dispatcher。 | 来源、目标健康、命令 ID 及准确结果。 |
| 停止 | IsOS 关闭部署；EOS 在终止 MuJoCo 前停止已准入执行器；IsOS 回收依赖子进程。 | 已确认或明确未知的结果。 |

即使两个节点都位于 `isos` 下，跨进程数据仍需声明传输。EI 安全可拒绝提议，但不能绕过 EOS 准入。图连线声明可能的数据路径，不会执行命令。IsosUI 不会因画线或编辑连线而发送命令。精确负载语义由 [EOS 硬件接口](../eos/interfaces/hardware_cn.md)、[调度器设计](../eos/design/runtime/dispatcher_cn.md)和 [EI 南向契约](../ei/southbound.md)负责。

受支持的 `0.1.0` 图在 IsOS 显式原子迁移路径、类型、连线和绑定之前保持只读。历史资格验证页面只证明所注明提交与版本的行为，不证明新的单核心路径。[首次使用回放](q01-first-use-design_cn.md)与[实现里程碑](implementation-milestones_cn.md)记录所需的集成验证。

# MuJoCo 仿真进程节点

[English](../../../../../../../docs/eos/layers/process/groups/mujoco/nodes/simulator.md) · [EOS 层目录](../../../../README_cn.md) · [node-contributions](../../../../../interfaces/node-contributions_cn.md) · [mujoco](../../../../../interfaces/mujoco_cn.md) · [hardware](../../../../../interfaces/hardware_cn.md)

**位置：**`eos/process/mujoco/<instance-id>` · **类型族：**`eos.process.mujoco`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**当前包 sidecar 已贡献 `@v1.0`；新图的 `@v0.1` 属于目标设计。旧内置 `eos.process.backend@v1.0` 保留安全读取兼容。当前身份为 `eos.process.mujoco@v1.0`。只读旧图时不得改写其内容。

## 用途与边界

IsOS 验证 `island/mujoco-sim@0.1.1` 包，解析场景和可执行文件并应用图。EOS 直接监管仿真子进程，并拥有内部 socket 适配器。

## 端口与设置

包固定版本：精确的 `node.package`。设置：`scene`、`socket`，可选 `env` 和 `start_timeout_ms`。输出：`status: Status`、`health: Health`。硬件适配器引用此进程及相同的已验证 socket/设备 ID。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

验证包和场景，启动唯一子进程，等待仅限所有者访问的 socket 就绪，再准入声明的设备适配器。停止时先关闭准入并停止执行器，然后终止并回收仿真进程。停止失败必须可见。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

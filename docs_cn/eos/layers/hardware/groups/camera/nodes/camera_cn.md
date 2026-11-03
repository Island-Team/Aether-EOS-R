# 相机适配器节点

[English](../../../../../../../docs/eos/layers/hardware/groups/camera/nodes/camera.md) · [EOS 层目录](../../../../README_cn.md) · [hardware](../../../../../interfaces/hardware_cn.md) · [vision](../../../../../interfaces/vision_cn.md) · [mujoco](../../../../../interfaces/mujoco_cn.md)

**位置：**`eos/hardware/camera/<instance-id>` · **类型族：**`eos.hardware.camera`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**`eos.hardware.camera@v1.0` 和 `eos.hardware.camera@v0.1` 均已注册；新图使用 `@v0.1`。只读旧图时不得改写其内容。

## 用途与边界

一个相机实例拥有一个适配器，输出类型化帧、健康状态和生命周期状态。声明的 RGB 必须可用；配准深度是可选能力，缺失时必须明确标记不可用。

## 端口与设置

输入：无。输出：`frame: CameraFrame`、`health: Health`、`status: Status`。能力：`rgb8`，以及可选的 `aligned_depth_u16`。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

验证节点配置和适配器族，为设备构造唯一适配器，将样本写入有界最新值缓存，只发布通过校验且新鲜的帧。错误帧只降级本相机，不伪造替代帧。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

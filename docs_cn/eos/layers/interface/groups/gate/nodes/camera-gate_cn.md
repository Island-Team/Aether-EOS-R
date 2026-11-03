# 相机预览网关节点

[English](../../../../../../../docs/eos/layers/interface/groups/gate/nodes/camera-gate.md) · [EOS 层目录](../../../../README_cn.md) · [vision](../../../../../interfaces/vision_cn.md) · [hardware](../../../../../interfaces/hardware_cn.md)

**位置：**`eos/interface/gate/<instance-id>` · **类型族：**`eos.interface.camera_gate`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**`eos.interface.camera_gate@v1.0` 和 `eos.interface.camera_gate@v0.1` 均已注册；新图使用 `@v0.1`。预览路由是否可用取决于运行时准入。只读旧图时不得改写其内容。

## 用途与边界

将已准入的相机帧转换为有界只读预览，供 IsosUI Vision 使用。它不能发送相机控制或执行器命令。

## 端口与设置

输入：`frame: CameraFrame`（图像，进程内）。输出：`preview: VisionFrame`（图像，WebSocket）。无节点专用设置。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

绑定一个兼容相机来源，验证并限制帧尺寸和时效，编码或转发最新允许的预览，在背压下丢弃过期帧。查看器断开不影响相机采集和安全。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

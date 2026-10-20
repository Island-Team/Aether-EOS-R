# Body 发现与状态

[English](../../../../docs/eos/develop/interfaces/body.md) · [接口总览](README_cn.md) · [World](world_cn.md) · [Primitive](actions_cn.md)

Body 是 EOS 拥有的机器人设备清单。它告诉客户端有哪些设备、设备是否可用，以及哪些 primitive 已绑定。设备注册、后端配置、原始遥测、图像、音频和模型结果不属于这个接口。EOS 从选定 profile、已接纳的后端、primitive schema 和依赖健康状态生成清单；客户端不能注册或修改设备。

## Discover

`GatewayService.Discover` 使用空请求。响应中的 `robot.primitives` 包含无设备目标的 `robot.stop` 和 `safety.estop`；`devices[]` 列出设备身份、类别、部件、状态、健康和已接纳的 primitive binding。下例仅截取一个 base binding：

```json
{"robot":{"id":"demo-0","name":"demo-0","model":"eos-demo","state":"idle","health":"ready","estop":false,"primitives":[{"name":"robot.stop","version":"1.0.0","kind":"direct","health":"ready","parameters":{"type":"object","properties":{},"additionalProperties":false}}]},"devices":[{"id":"base-0","class":"base","part":"default","state_uri":"robots/demo-0/devices/base-0/state","state":"idle","health":"ready","primitives":[{"name":"base.navigate","version":"1.0.0","kind":"direct","health":"ready","references":["relative","absolute"],"absoluteFrame":"map","limits":{"x_m":{"minimum":-10.0,"maximum":10.0}},"parameters":{"type":"object","properties":{"reference":{"type":"string","enum":["relative","absolute"]},"x_m":{"type":"number"},"y_m":{"type":"number"},"yaw_rad":{"type":"number"}},"required":["reference","x_m","y_m","yaw_rad"],"additionalProperties":false}}]}]}
```

设备 ID 采用按类别独立编号的 `<class>-<index>` 形式，例如 `base-0` 和 `arm-1`。`state_uri` 指向可读取的精简状态资源。`primitives[]` 中每项都是当前部署接纳的准确 name/version 绑定，而不是全局 catalog 的别名。`parameters` 是调用方 `arguments` 对象的有效 JSON Schema；`references`、`absoluteFrame`、`limits`、`accepts` 和依赖健康进一步约束该设备上的调用。只有 binding 公布的 reference 模式可用。

`state` 可表示 `disabled`、`idle`、`active` 或 `fault`；遥测不足以证明活动或静止时为 `unknown`。传感器还可能报告 `capturing` 或 `listening`。`health` 描述可用性，不能从 `state` 推断。缺少某类设备表示该机器人没有接纳该类绑定，客户端不应假设七类设备都存在。

## 设备状态资源

`ListResources` 返回已接纳的可读 URI。对设备的 `state_uri` 调用 `ReadResource` 获取完整快照；`WatchResource` 只通知 URI 变化，收到通知后仍须重新读取。状态资源保持紧凑，只含类别相关的状态与健康事实，不发布相机帧、麦克风音频或模型 tensor。设备命令必须通过 [primitive 调用](actions_cn.md) 发送，不能写状态资源。

```protobuf
ReadResourceRequest { uri: "robots/demo-0/devices/base-0/state" }
```

Base、机械臂、云台和夹爪的完整 descriptor 示例、类别字段和资源形状见 [英文完整接口](../../../../docs/eos/develop/interfaces/body.md)。调用前以当前 `Discover` 响应为准，尤其要读取具体设备的关节结构、frame 和范围。

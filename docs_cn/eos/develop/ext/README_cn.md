# 扩展与后端总览

[English](../../../../docs/eos/develop/ext/README.md)

扩展层连接 EOS 语义契约与模拟环境、主机设备、厂商 SDK、ROS 2 和运维工具。运行时负责策略和规范数据；扩展只负责采集、传输或设备专用转换。

## 数据边界

每个后端接收已准入的类型化配置，并输出后端无关的身份、健康状态和数据流。可执行命令的后端只通过 dispatch 接收已校验动作。后端必须保留单位、时间戳、顺序和所有权；依赖缺失时不得伪造成功样本。

配置缺失或不兼容、设备打开或协商失败、载荷格式错误或超限、配对数据不完整、样本过期以及操作不受支持，都必须显式失败。可选空值保持为空。流结束、传输中断、子进程失败、取消、停止和关闭都应成为可观察结果。

除非专题契约明确要求，后端不会重试命令。并发采集可以替换未读取的旧样本，但命令必须保持 dispatch 顺序和 acknowledgement。仅 feed 设备会拒绝命令。Stop 与急停行为必须 fail-closed，并遵守 dispatch 及后端页面定义的作用域。

## 文档结构

```text
ext/
├── backends/
│   ├── demo_cn.md  确定性适配器、模拟驱动世界与 Demo profile
│   ├── host_cn.md  可复用的主机设备采集与扬声器输出
│   ├── ros2_cn.md  ROS 2 传输
├── north_http/       独立 REST/SSE 进程、config、shared gRPC client、server 与 audit
└── tools/          独立扩展工具
```

### Client-facing 进程

- [North HTTP](north_http/README_cn.md)：独立 process composition、一个 shared EOS gRPC channel/stub set、strict configuration、HTTP/SSE server ownership、可选 external ActionList audit、logging 与 test。

### Demo

- [Demo 后端](backends/demo_cn.md)：七类设备的 EOS HAL 映射、确定性驱动状态、模拟逻辑与 profile。

### 主机设备

- [主机设备后端](backends/host_cn.md)：有界 ALSA 麦克风采集与协商后的 V4L2 摄像头采集。

### 传输与厂商后端

- [ROS 2](backends/ros2_cn.md)：可选 topic/service 传输与 Live profile。

### 工具

- [诊断 REPL](tools/repl_cn.md)：契约驱动的运维客户端。

每个专题页面负责完整的条件与验证流程；一个后端通过验证不代表其他后端也合格。

## Profile 语义

EOS profile 将一个 EOS 进程绑定到一台机器人及其设备。Northbound 不存在独立的 `hardware` 对象：机器人身份、设备、传输、描述、限制和 feed envelope 均由所选 profile 定义。`share/ext/eos/actions/` 下的 JSON Schema 定义可移植的 payload 形状；TOML profile 是特定机器人对外声明和接受能力的权威来源。

所选 `[[mode.binding]]` 只能命名一个 binding 文件和 profile。启动时，EOS 会根据 catalog、设备类别、后端实现、必需 endpoint 和数值限制验证每个 primitive。只有准入的声明才会出现在 primitive discovery 中；设备未声明的 catalog primitive 对该设备始终不可用。

### 机器人身份与机群

`[profile.<name>.robot]` 描述当前 EOS 实例所拥有的机器人：

- `robot_id` 是绑定机器人与 EOS 的稳定键，不强制 UUID 语法。省略时仅生成进程生命周期内有效的 ID 并记录警告。
- `robot_name` 是用于显示和资源寻址的名称，默认为 `robot-<index>`，且在机群中必须唯一。
- `robot_desc` 描述这一台具体机器人，而非其型号家族。
- `model` 和 `vendor` 是可选资产标签。`model = "eos-demo"` 不会选择推理模型或授予能力。

一个 runtime 不会隐式拥有多台机器人。机群由多个 EOS 实例注册形成，并拒绝重复的 `robot_id` 或 `robot_name`。

### 阅读 profile

对每个设备，按以下顺序阅读：

1. `class`、`part_id` 和生成的类别索引确定 target 及其语义资源家族；每个设备均须拥有自己的 `description`。
2. `[[device.primitive]]` 以精确的 `{name, version}` 定义可调用能力；`limits` 只收紧数值参数，不改变共享 payload schema。
3. `[device.backend]` 及其类型化子表定义传输、endpoint、设备路径、超时和后端默认值。
4. `[device.feeds]` 定义可发布的数据类别及最大频率；未列出的类别默认禁用，health 为必需项。
5. 设备 part 和 scene 数据只描述静态几何或模拟事实，不会放宽 dispatch 验证或替代物理控制器安全限制。

如果 profile 为可选数值字段声明了限制，调用方可在范围内传值；如果未声明，调用方必须省略该字段，由后端选择配置或原生默认值。`demo.toml` 中的数值仅适用于 loopback 模拟，真实硬件 profile 必须使用控制器验证过的范围。EOS 准入是额外的软件边界，不替代驱动限制、碰撞保护、watchdog 或急停。

`motion.stop` 和 `safety.estop` 是隐式特权操作，不得出现在 `[[device.primitive]]` 中。前者可面向已准入的 base、arm 或 gimbal，并使用后端 clamp 路径；后者使用空设备 ID，停止所有已准入设备、清除速度 lease，并将 runtime 锁存至重启。

### 维护的 profile

- [Demo](backends/demo_cn.md#profile-与运行)：完整的七设备确定性 loopback、主机采集变体和标准 profile 编写示例。
- [Demo host](backends/demo_cn.md#profile-与运行)：保留独立主机采集 binding 文件的兼容变体。
- [Live](backends/ros2_cn.md#站点本地-profile-示例)：最小单臂 ROS 2 集成 profile。

添加硬件时，应复用 profile 结构而非其限制值。从设备或控制器实测并声明限制，只暴露可执行的 primitive，并验证正常操作以及拒绝、超时、stop、estop、依赖丢失和关闭路径。参见[配置](../../usage/configuration_cn.md)、[southbound primitive](../interfaces/southbound/driver_cn.md) 与[后端行为](README_cn.md)。

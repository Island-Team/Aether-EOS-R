# 节点端口

本页供核心和软件包作者声明节点如何交换数据。IsOS 当前在注册类型时检查各端口声明，并在图准入时检查引用的端口。归属方仍负责传输及负载授权。[数据类型](data-types_cn.md)定义负载形状，[边](edges_cn.md)定义连接兼容性。

| 字段 | 类型／单位 | 含义与验证 | 缺失／默认值 |
| --- | --- | --- | --- |
| `name` | 非空字符串／局部端口 ID | 标识一个输入或输出；每个方向内部唯一。 | 必填。 |
| `data_type` | 非空字符串／已注册数据类型 ID | 将端口绑定到负载模式。 | 必填。 |
| `required` | 布尔值／连接规则 | 声明归属方是否要求实例具备该输入或输出。 | 必填。 |
| `transport` | 枚举／通信方式 | `in_process`、`unix_socket`、`websocket`、`ros2` 或 `http`；边的两端必须一致，或经过声明的桥。 | `in_process`。 |
| `delivery_mode` | 枚举／传输语义 | `message`、`event` 或 `stream`；源端与目标端必须一致。 | `message`。 |
| `cardinality` | 枚举／连接数 | `one` 或 `many`；准入限制输入连接。 | `one`。 |
| `expected_rate_hz` | 有限正数／每秒事件数 | 发布者期望速率，供规划和显示；不授予调度权。 | 可选；无预期值。 |
| `max_rate_hz` | 有限正数／每秒事件数 | 声明归属方应执行的速率上限；注册时检查其数值边界。 | 可选；无声明上限。 |
| `description` | 非空字符串／展示文字 | 向图作者解释数据值和单位。 | 必填。 |

`http` 锚点表示 HTTP 传输，`in_process` 表示本地类型化交接。这些是图声明，不是端点 URL。运行时端点身份和授权在准入后解析。桥类型声明 `adapter` 以及命名的 `inputs`／`outputs`；能力列表必须包含 `bridge`。

[English](../../../../docs/isos/interfaces/nodes/ports.md)

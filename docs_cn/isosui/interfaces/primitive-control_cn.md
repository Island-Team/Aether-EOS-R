# U04 节点信息与原语控制

[English](../../../docs/isosui/interfaces/primitive-control.md) · [设计](../design-primitive-control_cn.md) · [首次使用](../usage_cn.md)

IsOS 拥有节点图字节、保存和活动 SHA-256 修订、运行 ID 与部署 Stop。EOS 拥有活动绑定、目标准入、适配器实时状态、日志和终态结果。IsosUI 只读取所有者状态，并仅通过 `POST /api/eos/v2/control` 发送操作员点击的命令。图连线只声明绑定，不执行命令。

## 节点信息卡

鼠标在节点上停留约 550 毫秒、键盘聚焦或触摸信息按钮可打开唯一的非模态信息卡。点击或拖动节点时，该节点位于相邻节点之上；原来显示在节点下方的详情卡已移除。卡片通过 body portal 浮于重叠节点上方，限制在视口内（边距 8 px，标称宽高最多 340 px）；Escape 或关闭按钮可退出。信息按钮使用 React Flow 的 `nodrag`／`nowheel`，节点拖动与连线锚点继续可用。卡片分别显示目录类型描述与实例描述、输入／输出名称、精确数据类型 ID、目录提供的 `x-unit` 单位、传输方式和端口帮助。缺失的目录描述明确标为兼容回退。仅当 IsOS 报告运行中、保存修订等于活动修订且图无未保存编辑时，EOS 硬件节点才读取 EOS 的 `status` 与 `health`；读取失败时不编造健康状态。卡片打开且页面可见时，每两秒刷新所有者状态／健康；读取失败或样本超过五秒时标为不可用或过期。草稿节点不显示推断的实时状态。

## 活动绑定与请求

IsosUI 同时读取已保存图与 `GET /node-types/selected`，要求返回的 `graph_revision` 与图的保存字节修订一致，随后检查 EOS 和 EGIA 的全部类型。修订变化时仅重读图和目录一次；再次不一致或归属方目录不可用时，Graph、EOS 控件和节点信息保持只读。全局 `GET /node-types` 目录继续用于发现软件包；所选归属方类型和数据模式描述已应用的图。IsOS 的 `read_only` 判定也会隔离控件。

EOS 工作台在页面可见时刷新归属方节点图／目录及进程状态。五秒定时器同一时间最多执行一次读取；即使后续定时器触发，较慢的读取仍可发布。新保存修订或运行 ID 出现时，在显示新状态前清除上次运行的发现结果和健康数据。操作员点击 **刷新** 会立即禁用 Invoke。若正在读取，多次点击合并为一次紧接着执行的读取，旧结果不再发布；只有后续读取的已保存图、所选目录、进程状态及 EOS 发现结果一致完成后，Invoke 才重新开放。归属方故障或修订不匹配时控件保持只读。卸载后忽略迟到回复。

每个精确的 `eos.dispatcher.primitive_base@v0.1` 或 `eos.dispatcher.primitive_manipulator@v0.1` 绑定各有一张控制卡，前提是它以唯一 `command` 边连到对应的 `eos.hardware.base@v0.1` 或 `eos.hardware.manipulator@v0.1` 目标。支持 `base.velocity`／`body_velocity`、`base.navigate`／`relative_pose`、`arm.movej`／`joint_positions`、`arm.movel`／`end_effector_pose`，原语版本均为 `1.0.0`。其他、无效、缺失、歧义或旧版绑定在 U04 控件中不可用。目标显示为图解析的固定路径；更改须编辑图并 Apply。卡片显示绑定和目标的类型／实例描述、原语、模式及 SI 字段单位。Invoke 前用 `movementValid` 检查有限数值、目标模式、配置限值、坐标系 ID、关节名及归一化四元数；还要求 EOS 新鲜的 `status:ready` 和 `health:healthy`。

发送前要求 IsOS 的 `run_id_state:known`、非空运行 ID、`state:running`、64 位十六进制保存修订与活动修订及所显示修订一致，且没有草稿修改或兼容只读状态。随后重读保存图与进程状态，验证相同的精确绑定和目标，并在读取 EOS 进程单调时间前完成最后一次 IsOS 运行／修订围栏检查。读取时间后，立即再次检查最新的本地运行、修订、绑定、Stop、编辑和取消状态，然后以新命令 ID 最多发送一次 `primitive_typed` POST。四秒截止时间从最后一次阻塞的所有者读取之后开始计算；不可用或过期的发送不会以同一 ID 重放。请求头包含 `data_type_id`（`base_movement` 或 `manipulator_movement`）、`data_type_version:1.0`、精确原语名／版本、规范绑定／目标路径、活动修订、新生成的 1–128 字节命令 ID、正序号、四秒单调截止时间、UTF-8 JSON 负载长度（1–16384 字节）和 `codec:json_v1`。负载为已验证的类型化运动对象。浏览器不传授权令牌或私有 IPC 操作。

浏览器先显示本地 `Sending`，然后只有命令 ID 相同的 EOS 响应才能确认 `Completed`、`Failed`、`Cancelled`、`Rejected`、`InProgress` 或 `OutcomeUnknown`。丢失提交响应时绝不再次提交。`InProgress` 仅使用同一公开桥接上的 `{op:"result",command_id,active_revision}`，每次读取前后检查 IsOS 运行／修订。自动读取最多十二次或一分钟，此后提供手动刷新。运行轮换、修订过期、IsOS 身份不可用、Stop、绑定切换或卸载都会隔离旧读取和迟到回复。传输错误显示不可用，绝不显示成功。即使 EOS 调用被禁用或桥接断开，IsOS Stop 按钮仍可见且可调用。点击后在等待 Stop 响应前，EOS 工作台设置按运行 ID 隔离的围栏，所有绑定卡立即禁用 Invoke。待定、未知或不可用的 Stop 与状态刷新失败都保持该运行被围栏隔离；同时取消其他绑定卡的待定结果读取、封存该运行的记录并忽略迟到终态；新观察到的运行 ID 使用独立围栏。

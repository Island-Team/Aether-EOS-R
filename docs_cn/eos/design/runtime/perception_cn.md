# 感知运行时

[English](../../../../docs/eos/design/runtime/perception.md) · [运行时总览](README_cn.md) · [Body 接口](../../develop/interfaces/body_cn.md) · [World 与检测接口](../../develop/interfaces/world_cn.md) · [模型接口](../../develop/interfaces/external/models_cn.md)

`src/eos/runtime/perception/` 是只读语义投影的所有权边界。它把准入后的硬件观测和规范模型结果转换为 body 与语义资源；不拥有硬件 backend，不直接调用 provider API，不执行技能，也不下发执行器命令。

## 结构与依赖

`product_history` 保存有界、通过校验的历史结果；`producer_coordinator` 管理需求和按需启动；`resource_catalog` 负责声明、路由和历史查询；`body/` 负责设备状态、检测、视觉和语音的规范投影。Northbound 使用合并后的 catalog 边界，不自己管理 producer 生命周期或保留模型状态。

感知从[硬件运行时](hardware_cn.md)读取不可变的设备标识、配置、健康和 feed 快照，并从[模型运行时](models_cn.md)接收 camera pose、VAD 和 ASR 快照。各来源独立可用：缺少模型结果不会抹去有效的硬件清单，设备不可用也不会产生虚构观测。嵌套 Reader 向 [northbound](northbound_cn.md) 提供由 [body](../../develop/interfaces/body_cn.md) 与 [world](../../develop/interfaces/world_cn.md) 契约定义的载荷。

## 投影

Body discovery 把已准入设备的简要身份、健康、状态、primitive 绑定和 `state_uri` 投影到 `Discover`。每个受支持的设备也可通过 `robots/{robot_name}/devices/{device_id}/state` 读取，不需要模型 producer。模型资源分别为 `robots/{robot_name}/detection/state`、`robots/{robot_name}/world/vad/state` 和 `robots/{robot_name}/world/asr/state`。

配置的保留资源会在首个结果产生前声明和列出。冷读或 watch 获取 producer lease 并等待新结果；连续流也持有 lease。检测结果仅含紧凑的 `human:*`、`obj:*` 语义观测，不含 mask、关键点或模型 tensor。相机帧不是可读资源；northbound 单独维护供 `CaptureFrames` 使用的有界双画面缓存。VAD 只投影语音标志、概率以及源序列和时间；ASR 只投影文本、语言和置信度。麦克风 PCM、WAV、语音片段音频、模型 segment 及 provider 元数据留在 EOS 内部。`world.snapshot` schema 预留给未来真正的融合 producer，当前 reader 不声明或路由它。

发布的历史只接收通过对应规范 schema ID 的不可变校验器验证的 JSON object。历史有界且仅存于当前进程，但其值和 `boot_id`、epoch、序列、观测时间等关联元数据仍可能敏感。它们通过与最新值相同的 northbound 资源边界暴露；访问控制由部署策略负责，本组件不再增加独立授权层。

设备状态投影通用健康和运行状态。云台还包含实测 pan/tilt 位置及角速度，供跟踪进程观测物理运动。`inventory.cpp` 列出资源，`router.cpp` 解析规范 URI，`semantic.cpp` 投影 VAD 和 ASR；envelope 使用单调递增的序列值和时间戳。

## 边界与验证

感知只读，无法取得可变 backend handle 或调用 dispatch。Provider 字段名止于 `ext/models`。原始麦克风音频留在 EOS 内部；相机帧不作为状态资源。部分数据保持为部分数据，缺失设备、过期 feed 或缺少模型结果按照资源契约省略或报告为不可用。

`src/eos/runtime/perception/tests/` 覆盖历史边界与 selector、schema 准入、producer lease 转换、按需启停和 catalog 路由。`body/tests/` 覆盖各设备类别、部分清单、设备所有权和语义投影。

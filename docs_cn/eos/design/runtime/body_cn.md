# Perception runtime

[运行时总览](README_cn.md) · [English](../../../../docs/eos/design/runtime/perception.md)

`src/eos/runtime/perception/` 是只读语义投影的 namespace 与 ownership boundary。它把已准入的硬件观测和已归一化的模型结果转换为规范 body 与 semantic resource；不拥有硬件 backend，不调用 provider-specific API，不执行 skill，也不释放 actuator command。

顶层目录构建 `eos_perception_history`、`eos_perception_producers` 与 `eos_resource_catalog`：分别拥有有界且经 schema 校验的产品历史、producer demand/惰性启停，以及 resource declaration、routing 与 history lookup。`perception/body/` 下的 `eos_body` Reader 保留低成本 device state 与 canonical semantic projection。Northbound 消费组合后的 catalog boundary，不自行拥有 producer lifecycle 或 retained model state。

## Resource routing

Catalog 会在第一个产品出现前声明并列出已配置的 retained resource。cold read、watch 或 skill subscription 获取共享 producer lease；已配置 continuous stream 保持同一 demand。URI 的第二段是 fleet-unique `robot_name`，不是 `robot_id`：

- `robots/{robot_name}/detection/state` — latest semantic detection snapshot
- `robots/{robot_name}/world/vad/state` — latest compact VAD snapshot
- `robots/{robot_name}/world/asr/state` — latest compact ASR snapshot
- `robots/{robot_name}/devices/{device_id}/state` — admitted device 的精简 health/state

硬件清单通过 `Discover` 获取，每个 admitted、受支持设备的 `state_uri` 指向统一设备状态资源。设备状态读取开销低，不启动 model producer，watch 继续使用 polling。Detection 只投影精简 `human:*` 与 `obj:*` observation，不包含 mask、keypoint 或 model tensor。VAD 仅投影 speech、probability 与 source sequence/timestamp；ASR 仅投影 text、language 与 confidence。相机 JPEG 只通过有界 `CaptureFrames` cache 返回；PCM、WAV、utterance audio、model segment 与 provider metadata 不进入 resource history。规范 `world.snapshot` schema 保留给未来的真实 fusion producer。

历史只接受通过 declaration 的 immutable canonical schema validator 的 JSON object。历史有界且仅存于当前进程；值及 `boot_id`、`epoch`、sequence、observation time 与 correlation metadata 使用与 latest read 相同的 northbound privacy boundary。

## Projection choices

- Base、gimbal、transform、object 和 detection 使用 backend-neutral HAL telemetry，不再向下转型 demo backend。
- 场景中的机器人、末端执行器和融合对象均为可选，适配部分硬件机器人。
- 相机 JPEG 只进入按相机划分、最多两帧的捕获缓存。
- 麦克风 PCM 只在 southbound/模型流水线内部；成功的 VAD 与 ASR 结果分别成为独立 northbound world 产品。
- Robot `estop` mirrors the explicit process-wide `safety.estop` latch in discovery and `GetStatus`.

## Testing

`eos_perception_history_tests`、`eos_perception_producer_tests` 与 `eos_resource_catalog_tests` 覆盖 history bound/selector、schema admission、producer lease transition、lazy start/stop 与 catalog routing。`eos_body_tests` 覆盖各设备类别、partial inventory、ownership 与 semantic projection；`perception_chain_test` 与 gateway integration test 保留 cross-module coverage。

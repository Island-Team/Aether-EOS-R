# `egia.contracts` 校验边界

[English](../../docs/ei/contracts.md)

## 目录

- [概述](#概述)
- [架构](#架构)
- [所属模块](#所属模块)
- [数据流](#数据流)
- [运行条件](#运行条件)
- [相关文档](#相关文档)

## 概述

`egia.contracts` 把共享 EOS JSON 文档和 protobuf payload 转换为严格的 EGIA 类型。它是不可信 southbound 数据与 runtime 之间的语义边界。Contract 模块不发现设备、不选择 target，也不执行 primitive。

## 架构

```mermaid
flowchart LR
  Roots["逻辑 contracts/ 与 primitives/ root"] --> Source["source.py schema registry"]
  EOS["EOS payload"] --> Validate["规范 schema 与类型校验"]
  Source --> Validate
  Validate --> Types["Body descriptor、world snapshot、action、acknowledgement"]
  Types --> Core["link、perception、registry 与 safety"]
```

## 所属模块

`source.py` 通过已检查 collision 的 `SharedResources` 解析 JSON 文档、加载全部 schema 并构建跨 schema registry。`validate.py` 执行规范 JSON Schema 与 Pydantic 校验。`registry.py` 将 resource/primitive kind 映射到 schema path 和 Python model。`endpoints.py` 分类 world-resource URI，并声明支持的 resource/primitive kind。`limits.py` 解析 numeric capability interval 并检查 invocation argument。

`actions/commands.py` 与 `actions/emergency.py` 定义类型化 primitive argument；`actions/ack.py` 定义带版本的 acknowledgement。`perception/body/` 管理 base、camera、gimbal、gripper、manipulator、microphone、speaker 的 class-specific discovery value 以及 robot status。`perception/world/` 管理 scene、vision、pose、VAD 与 ASR snapshot。`schemas/northbound/` 包含生成的 EGIA 自有 wire schema，不是共享 EOS schema source。

## 数据流

启动时扫描逻辑 `contracts/` 与 `primitives/` 下的 JSON，并注册每个非空 `$id`。Southbound object 先按其规范 schema 校验，再转换为类型化 model。Discovery 把 protobuf common field 与 `device_class` 选定的 body contract 组合起来；world resource payload 根据 URI 和 discriminator 选择 model。

Primitive discovery 把精确 `{name, version}` descriptor 映射到对应 action schema。Descriptor version 必须是 SemVer，并等于 schema 的 `x-island-primitive-version`。Egress 前校验可移植 argument shape，再按选定 device limit 检查 numeric value。EOS acknowledgement 在成为 safety verdict 或操作端可见 result 前会连同 `primitive_version` 一起校验。

Camera frame 使用 protobuf 而不是 JSON Schema。数据到达 consumer 之前，link validation 会检查已准入 camera identity、请求数量、JPEG 内容、dimension、帧差异及可选 source timestamp。内联 skill parameter/return 值是作为已校验 discovery metadata 携带的自包含 JSON Schema 文档；EGIA 不执行 skill。

## 运行条件

正常 payload 生成且只生成一个类型化 body descriptor、world snapshot、command 或 acknowledgement。Nullable world field 保持 `null`；缺失不会被转换为健康数据。选定 class 所要求的 body `configuration` 与 `operational_state` field 必须存在，只有 schema 允许时，明确的未知值才可为 null。

在 `configure` 前使用资源、逻辑文件缺失、JSON malformed 或非 object、schema 缺少 `$id`、两个 schema 复用 `$id`，或无法构建 Draft 2020-12 registry 时配置失败。Payload 在规范 schema 或类型 model 不匹配时失败。未知 device class、primitive kind/version、resource kind 和 schema name 会被拒绝而非透传。

Numeric range 拒绝 boolean 和非 number、非有限 endpoint、没有 endpoint 的 range、缺少对应 endpoint 的 exclusivity、空 interval、缺失 required limit name 和未知 limit name。Invocation 在 capability 未公布或值超出准入 interval 时被拒绝。

Accepted acknowledgement 要求 rejection field 为 null。Rejected acknowledgement 要求非空 `reason` 和规范 `reason_code`；所有 acknowledgement 都要求精确 SemVer `primitive_version`。`speaker.play` 会拒绝缺失 stream identity、非法 media type 或 base64、超限 chunk、负 sequence 和未知 field。

World contract 执行其语义 invariant，包括 timestamp、固定 pose array 长度、有序 speech segment 和 discriminator。EOS transport unavailable 与 timeout 由 `egia.core.link` 处理；restart 从配置 root 重建 schema registry。本 package 不拥有 background task，shutdown 无需清理。

## 相关文档

EOS interface 见 [southbound.md](southbound.md)，生成的 northbound schema 见 [northbound.md](northbound.md)，transport 见 [core/link.md](core/link.md)，admission 见 [core/perception.md](core/perception.md)，target binding 见 [core/registry.md](core/registry.md)，资源 root 所有权见 [Island shared contracts](../island/contracts.md)。

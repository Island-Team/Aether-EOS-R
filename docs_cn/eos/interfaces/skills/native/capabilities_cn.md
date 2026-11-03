# 原生技能 host 能力

[English](../../../../../docs/eos/interfaces/skills/native/capabilities.md)

[编写总览](README_cn.md) · [ABI 契约](abi_cn.md) · [完整示例](example_cn.md)

下例在生命周期或 wire 行为需要明确时使用原始 ABI；一般 C++ 技能应调用 `eos::skill::Execution`，由其验证对象 JSON 并复制 host 的借用响应。

## 调用原语

每条执行路径所需的原语都必须声明精确名称与版本，并且在部署的 Dispatcher 目录中可用。同步调用返回 canonical acknowledgement；可中断工作优先启动异步原语，有限等待期间检查取消，并按操作 ID 取消。

```c
typedef struct eos_skill_primitive_reference_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
} eos_skill_primitive_reference_v2;

static const eos_skill_primitive_reference_v2 kPrimitives[] = {
    {sizeof(eos_skill_primitive_reference_v2),
     Text("gimbal.motion"), Text("1.0.0")},
};
```

```c
typedef struct eos_skill_primitive_request_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 primitive;
  eos_skill_text_v2 version;
  eos_skill_text_v2 device_id;
  eos_skill_text_v2 arguments_json;
} eos_skill_primitive_request_v2;

typedef struct eos_skill_primitive_response_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 acknowledgement_json;
  eos_skill_text_v2 error_message;
} eos_skill_primitive_response_v2;
```

```cpp
std::string arguments =
    R"({"reference":"relative","pan_rad":0.5,"tilt_rad":0.0})";
eos_skill_primitive_request_v2 request{
    sizeof(eos_skill_primitive_request_v2),
    Text("gimbal.motion"),
    Text("1.0.0"),
    Borrow(device_id),
    Borrow(arguments),
};
eos_skill_primitive_response_v2 response{
    sizeof(eos_skill_primitive_response_v2), {}, {}};

const eos_skill_host_status_v2 status =
    execution->host->call_primitive(
        execution->host->context, &request, &response);
```

原语的 `arguments_json` 只包含该原语的对象参数，不传 `type`、`command_id` 或版本元数据。`<根命令 ID>/<从零开始的调用序号>` 是历史可选 C++ 宿主的子命令 ID 规则；R04 Rust 宿主按[所有者契约](host-abi_cn.md)由运行、技能身份、公开请求 ID 与调用序号派生持久 `sk-` SHA-256 ID。`EOS_SKILL_HOST_OK` 时应立即复制或解析 `acknowledgement_json`，同时检查 `accepted` 和 `result.status`；host 状态 OK 不代表执行器请求已被接受。一次调用失败、拒绝、取消或结果不确定后，Rust Broker 会锁闭本次 invocation 的后续原语释放。

## 一次性推理

推理 authority 必须预先声明。视觉推理使用精确的保留帧标识；VAD 使用请求对象里的 canonical 音频，不伪造视觉 provenance。响应也按对应来源检查。

## 读取资源

资源 URI 必须匹配描述符声明的模板。host 返回的是借用切片，下一次 host 调用前应复制；资源只读，修改状态要使用原语。

```c
typedef struct eos_skill_resource_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 uri_template;
  eos_skill_text_v2 description;
} eos_skill_resource_v2;

static const eos_skill_resource_v2 kResources[] = {
    {sizeof(eos_skill_resource_v2),
     Text("robots/{robot_name}/detection/state"),
     Text("Latest detection snapshot for the selected robot.")},
};
```

```c
typedef struct eos_skill_resource_request_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 uri;
} eos_skill_resource_request_v2;

typedef struct eos_skill_resource_response_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 value_json;
  eos_skill_text_v2 error_message;
} eos_skill_resource_response_v2;
```

```cpp
eos_skill_resource_request_v2 request{
    sizeof(eos_skill_resource_request_v2), Borrow(uri)};
eos_skill_resource_response_v2 response{
    sizeof(eos_skill_resource_response_v2), {}, {}};

const eos_skill_host_status_v2 status =
    execution->host->read_resource(
        execution->host->context, &request, &response);
```

## 终态诊断

非成功执行使用稳定 reason 和安全、有限长度的消息；不可把 capability payload 或个人数据放入消息。

```cpp
auto snapshot = execution.read_resource(uri);
if (!snapshot) {
  return eos::skill::Outcome::Failure(
      "detection.read_failed", "detection snapshot read failed: capability unavailable");
}
```

## 日志和检查点

日志是受保护的操作员诊断，checkpoint 是 Agent 可见里程碑。检查可选 callback，限制字段大小、频率和内容；日志或 checkpoint 失败不应改变技能结果。

```cpp
(void)execution.logger().info("scan_started", "Starting scan");
(void)execution.checkpoint(
    "scan.started",
    eos::skill::CheckpointOptions().Progress(0.1).Fields({{"phase", 1}}));
```

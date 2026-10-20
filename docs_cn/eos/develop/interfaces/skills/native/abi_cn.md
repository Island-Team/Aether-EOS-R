# 原生技能 ABI v2

[English](../../../../../../docs/eos/develop/interfaces/skills/native/abi.md)

[编写总览](README_cn.md) · [能力](capabilities_cn.md) · [完整示例](example_cn.md)

C++17 优先使用 `<eos/skill/sdk.hpp>`；C 或底层集成使用 `<eos/skill/abi.h>`。ABI 使用固定宽度整数、借用字节、结构体大小和不透明上下文，不跨边界传递 C++ 对象、异常或内存所有权。v2 仅追加字段，访问可选 host callback 前必须检查表大小和函数指针。

## 文本、状态与身份

文本是借用 UTF-8 字节切片，不保证 NUL 结尾。元数据必须在进程生命周期有效；调用中的切片只需覆盖该 host 调用。名称遵守小写身份格式，版本采用精确且区分大小写的 SemVer。

```cpp
template <std::size_t N>
constexpr eos_skill_text_v2 Text(const char (&value)[N]) {
  return {value, static_cast<uint32_t>(N - 1)};
}

eos_skill_text_v2 Borrow(const std::string& value) {
  return {value.data(), static_cast<uint32_t>(value.size())};
}
```

```text
^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$
```

## C++ 门面

SDK 包装借用值、对象 JSON、错误与完成语义；C++ 代码应通过该门面调用 host。

## 库与描述符

库导出唯一查询符号 `eos_skill_library_query_v2`。描述符、Schema、声明数组及其引用在整个进程生命周期有效；每个字段的 `struct_size` 使用目标头文件对应的 `sizeof`。

```c
typedef struct eos_skill_library_v2 {
  uint32_t abi_version;
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
  eos_skill_text_v2 build_id;
  uint32_t skill_count;
  const eos_skill_descriptor_v2* skills;
  uint32_t skill_descriptor_size;
} eos_skill_library_v2;
```

```c
typedef struct eos_skill_descriptor_v2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
  eos_skill_text_v2 description;
  eos_skill_text_v2 parameters_schema_json;
  eos_skill_text_v2 returns_schema_json;
  uint32_t primitive_count;
  const eos_skill_primitive_reference_v2* primitives;
  uint32_t resource_count;
  const eos_skill_resource_v2* resources;
  uint32_t signal_count;
  const eos_skill_signal_descriptor_v2* signals;
  uint32_t claim_count;
  const eos_skill_claim_template_v2* claims;
  uint32_t reentrant;
  void* context;
  eos_skill_execute_v2 execute;
  uint32_t source_count;
  const eos_skill_source_descriptor_v2* sources;
  uint32_t inference_count;
  const eos_skill_inference_descriptor_v2* inferences;
} eos_skill_descriptor_v2;
```

```cpp
extern "C" EOS_SKILL_EXPORT
const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t version) {
  return version == EOS_SKILL_ABI_VERSION_V2 ? &kLibrary : nullptr;
}
```

## 参数与结果 Schema

参数、结果及 SIGNAL payload 必须是自包含的对象 JSON Schema，不使用文件或远程 `$ref`。EOS 在调用前验证参数，在完成后验证结果。

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "properties": {
    "robot_name": { "type": "string", "minLength": 1 },
    "device_id": { "type": "string", "minLength": 1 }
  },
  "required": ["robot_name", "device_id"],
  "additionalProperties": false
}
```

## 执行与完成

host 指针和返回的借用切片不可跨调用保存。正常执行恰好完成一次；取消不调用完成回调；失败要提供稳定 reason 和不含 payload 的安全诊断。可选 callback 缺失时按 ABI v2 兼容规则退化。

```c
typedef eos_skill_execute_status_v2 (*eos_skill_execute_v2)(
    void* skill_context,
    const eos_skill_execution_v2* execution,
    eos_skill_complete_v2 complete,
    void* completion_context);
```

## SIGNAL 声明

SIGNAL 名称是小写点分形式，payload 根节点为对象。接收和发出都受描述符声明限制；`CANCEL` 由管理器保留。

```cpp
const eos_skill_signal_descriptor_v2 kSignals[] = {
    {sizeof(eos_skill_signal_descriptor_v2), Text("control.update"),
     Text(R"({"type":"object","additionalProperties":false})"),
     EOS_SKILL_SIGNAL_ACCEPTED},
    {sizeof(eos_skill_signal_descriptor_v2), Text("progress.changed"),
     Text(R"({"type":"object","additionalProperties":false})"),
     EOS_SKILL_SIGNAL_EMITTED},
};
```

## Claim 声明

claim 通过 RFC 6901 JSON Pointer 从验证后的参数解析。共享读可并存，独占使用与其他使用冲突；SIGNAL 不扩大 claim 权限。

```cpp
const eos_skill_claim_template_v2 kClaims[] = {
    {sizeof(eos_skill_claim_template_v2),
     EOS_SKILL_CLAIM_DEVICE,
     EOS_SKILL_CLAIM_EXCLUSIVE_USE,
     Text("/arm_device_id")},
    {sizeof(eos_skill_claim_template_v2),
     EOS_SKILL_CLAIM_RESOURCE,
     EOS_SKILL_CLAIM_SHARED_READ,
     Text("/world_resource_uri")},
};
```

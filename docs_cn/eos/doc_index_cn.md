# 文档索引

本仓库 EOS 文档的一行索引。产品级布局和约定位于相邻的 Island 文档树中。

[English](../../docs/eos/doc_index.md)

[使用 EOS](usage/README_cn.md) · [开发 EOS](develop/README_cn.md) · [设计](design/README_cn.md) · [发布](chore/release_cn.md) · [Aether 契约](../island/contracts_aether_cn.md)

## Complete map

- [README.md](README_cn.md): component overview, build, launch, installation, and selected references.
- [usage/arm64_cn.md](usage/arm64_cn.md): native Linux ARM64 prerequisites, build, test, launch, and ROS 2 guidance.
- [design/eos-2607_cn.md](design/eos-2607_cn.md): technical direction, architecture, invariants, and roadmap.
- [../island/layout.md](../island/layout_cn.md): standalone EOS repository roles and combined-product mapping.
- [../island/conventions.md](../island/conventions_cn.md): cross-repository code, documentation, module, and Git workflow.
- [chore/release_cn.md](chore/release_cn.md): release checklist.
- [chore/todo_cn.md](chore/todo_cn.md)：EOS 路线图与运行时后续工作（[English](../../todo.md)）。
- [design/runtime/README_cn.md](design/runtime/README_cn.md): runtime 集成数据生命周期与跨模块条件。
- [design/runtime/actionlist_cn.md](design/runtime/actionlist_cn.md): ActionList ownership、DAG scheduling、fact、supersession、no-retry semantic、logging 与 verification。
- [usage/debugging_cn.md](usage/debugging_cn.md): cross-process request、cursor、audit、logging、privacy 与 verification runbook。
- [develop/ext/README_cn.md](develop/ext/README_cn.md): 扩展所有权、backend 验证、profile 语义和编写规则。
- [develop/ext/north_http/README_cn.md](develop/ext/north_http/README_cn.md): 独立 north HTTP process composition、ownership、lifecycle、logging 与 test map。
- [develop/ext/north_http/client_cn.md](develop/ext/north_http/client_cn.md): 单一 shared EOS gRPC channel、fixed generated stub set、lifetime 与 diagnostics。
- [develop/ext/north_http/server_cn.md](develop/ext/north_http/server_cn.md): HTTP/SSE server ownership、bound、cancellation、logging 与 test。
- [develop/ext/north_http/config_cn.md](develop/ext/north_http/config_cn.md): strict north HTTP configuration、precedence、default、validation 与 security boundary。
- [develop/ext/north_http/audit_cn.md](develop/ext/north_http/audit_cn.md): external non-authoritative ActionList projection、privacy allowlist、durable journal、recovery、health、logging 与 test。
- [develop/interfaces/README_cn.md](develop/interfaces/README_cn.md): northbound、规范 payload 与 southbound 契约导航。
- [develop/interfaces/external/models_cn.md](develop/interfaces/external/models_cn.md)：模型请求、响应与 provider 边界。
- [develop/interfaces/grpc_cn.md](develop/interfaces/grpc_cn.md): EOS gRPC services and contract behavior.
- [develop/interfaces/workflow_cn.md](develop/interfaces/workflow_cn.md): native gRPC ActionList、event、cursor retention、atomic snapshot、error 与 recovery。
- [develop/interfaces/external/http_cn.md](develop/interfaces/external/http_cn.md): 完整 REST/SSE WebUI contract、CORS、error、request ID 与 migration。
- [develop/interfaces/actions-payloads_cn.md](develop/interfaces/actions-payloads_cn.md): primitive release payloads, acknowledgement envelopes, and canonical action schemas.
- [develop/interfaces/skills/native/README_cn.md](develop/interfaces/skills/native/README_cn.md): 原生技能编写、C ABI、嵌入 schema 与直接 primitive 调用。
- [develop/interfaces/southbound/driver_cn.md](develop/interfaces/southbound/driver_cn.md): backend and device boundary.
- [design/algorithms_cn.md](design/algorithms_cn.md): algorithm and edge-compute direction.
- [usage/configuration_cn.md](usage/configuration_cn.md): configuration keys, precedence, and validation.
- [develop/interfaces/contracts_cn.md](develop/interfaces/contracts_cn.md): canonical schema loading and validation.
- [design/lib_cn.md](design/lib_cn.md): shared clocks, logging, paths, and slots.
- [Demo 后端](develop/ext/backends/demo_cn.md): demo HAL、模拟世界与 Demo profile。
- [独立模拟器接口](develop/interfaces/external/simulator_cn.md)：独立 lifecycle、v2 存储、私有 IPC 与 WebUI HTTP/WebSocket API。
- [主机设备后端](develop/ext/backends/host_cn.md): 原生 ALSA 麦克风与 Video4Linux2 摄像头采集。
- [ROS 2 后端](develop/ext/backends/ros2_cn.md): ROS 2 映射、新鲜度、失败语义与站点本地 profile 指引。
- [诊断 REPL](develop/ext/tools/repl_cn.md): 契约级诊断客户端。
- [design/runtime/hardware_cn.md](design/runtime/hardware_cn.md): backend admission, generated identity, and the process hardware binding.
- [design/runtime/init_cn.md](design/runtime/init_cn.md): strict configuration, immutable schema ownership, and process composition.
- [design/runtime/perception_cn.md](design/runtime/perception_cn.md)：只读 body 与 scene 投影。
- [design/runtime/models_cn.md](design/runtime/models_cn.md): configured outbound perception-model clients, contracts, and workers.
- [design/runtime/time_cn.md](design/runtime/time_cn.md)：观测时间与新鲜度语义。
- [design/runtime/store_cn.md](design/runtime/store_cn.md)：私有类型化状态存储。
- [design/runtime/dispatch_cn.md](design/runtime/dispatch_cn.md): validated primitive dispatch and stop behavior.
- [design/runtime/northbound_cn.md](design/runtime/northbound_cn.md): gRPC transport ownership、service、replay behavior、logging 与 shutdown lifecycle。
- [design/runtime/northbound/workflow_cn.md](design/runtime/northbound/workflow_cn.md): WorkflowService protobuf conversion、event streaming、cursor error、logging 与 test。
- [design/runtime/skills_cn.md](design/runtime/skills_cn.md): multi-runtime skill subsystem 总览与 module map。
- [design/runtime/skills/manager_cn.md](design/runtime/skills/manager_cn.md): SkillManager runtime registry、exact routing、scheduling、claim、event、retention、logging 与 test。
- [design/runtime/skills/native_cn.md](design/runtime/skills/native_cn.md): trusted native ABI v2 runtime admission、capability、execution、isolation、logging 与 test。
- [design/runtime/skills/authoring_cn.md](design/runtime/skills/authoring_cn.md): execution-profile-neutral skill authoring requirement 与 review checklist。
- [design/runtime/skills/data_cn.md](design/runtime/skills/data_cn.md): descriptor、execution snapshot、event、signal 与 retained data ownership。
- [develop/interfaces/skills/ipc_cn.md](develop/interfaces/skills/native/ipc_cn.md): provider IPC framing 与 protocol boundary。
- [design/runtime/skills/observability_cn.md](design/runtime/skills/observability_cn.md): bounded log、metric、privacy 与 diagnostic field。
- [design/runtime/skills/process-model_cn.md](design/runtime/skills/process-model_cn.md): process isolation、lifecycle 与 supervision model。
- [design/runtime/skills/managed_cn.md](design/runtime/skills/managed_cn.md): fail-closed managed-runtime placeholder boundary。
- [design/runtime/skills/external_cn.md](design/runtime/skills/external_cn.md): fail-closed external-runtime placeholder boundary。
- [外部 decision client](../../src/eos/ext/decision/README.md): 仓库内 decision client、policy 与 event runner。

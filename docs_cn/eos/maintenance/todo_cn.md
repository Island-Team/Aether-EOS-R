# 0.x 实现路线图

[English](../../../docs/eos/maintenance/todo.md) · [目标架构](../../island/architecture_cn.md) · [层数据流](../../island/layer-dataflow_cn.md) · [跨仓详细 TODO](../../island/implementation-todo_cn.md) · [实现里程碑](../../island/implementation-milestones_cn.md) · [运行时基础](../design/runtime/foundation_cn.md)

**目标规划，部分已实现。** 本页记录 EOS 0.2.0、EGIA 0.1.0、IsOS 0.1.0、IsosUI 0.1.0 和 MuJoCo 0.1.1 的工作。各所有者页面区分已实现部分与计划行为。使用聚焦的 `refactor(v2)/<topic>` 分支，并把完整输出保存在 `logs/test/<run-id>/`。

本 EOS 摘要按 M1–M5 里程碑推进。[跨仓详细 TODO](../../island/implementation-todo_cn.md)为[层连接图](../../island/layer-dataflow_cn.md)的每个边界分配任务编号、依赖和验收证据。0.x 目标**不包含 WorkflowService、ActionList DAG 执行器或 workflow 模板**。历史 protobuf 与旧 C++ 文档仅用于兼容和最后调用者审计；不新增 workflow 节点或 UI。

## 1. 图与接口兼容

- [ ] IsOS、EOS、EGIA 配置工具和 IsosUI 读取安全的 `1.0.0` 图与 `@v1.0` 类型，同时写出 `0.1.0` 与 `@v0.1`。按版本解析，在内存中规范化；只有显式且按修订检查的迁移才修改保存内容。拒绝未知未来版本。旧任意命令 IsOS 进程类型只供迁移展示，绝不执行。
- [ ] 增加模块自有 `NodeType` 描述符；新类型要求非空描述，旧 sidecar 使用兼容回退；设置目录增加描述、单位、边界、选项和选择器元数据。Rust 描述字段及回退已实现，完整类型元数据和编辑器仍待完成。保留实例 `description` 与旧配置行为，保留 protobuf 字段号和 JSON 名称。Apply 前校验连线类型/单位并指出具体拒绝连线；EOS 实时健康单独显示。
- [ ] 连线增加可选的 nice 式 `priority` 与正有限数 `frequency_hz`。实现[有界的实际路由传递](../design/runtime/scheduler_cn.md)、溢出/间断计数及声明性连线状态；确认不能绕过 Dispatcher。

## 2. IsOS 软件包、配置、日志与预检

- [ ] 从已验证安装包描述、注册类型和图草稿解析选择器选项。扩展 node-options，返回值、标签、描述、可用性和原因。首批提供 MuJoCo 场景/设备选择，任意模型权重执行留待后续。
- [ ] 在 IsOS 数据目录中生成按修订命名、不可变的 EOS/EGIA 配置，并把路径传给子进程。保留 XDG/`ISOS_HOME`、密钥引用和精确软件包固定版本。启动前将程序声明支持的图/类型版本及功能与实际图比较。
- [ ] 把 EOS tracing、EGIA 事件、MuJoCo 子进程输出和 IsOS 聚合统一到[共同 JSONL 字段](../../island/conventions.v1.3.0_cn.md#统一可观测性与发布记录)。记录精确已验证包来源，并以管理器会话 `run_id` 和可选关联 ID 记录启动前故障。增加一次启动/就绪事件、状态变化、脱敏、有界轮转、筛选和游标恢复。

## 3. 运行时、Agent 与工作台

- [ ] 为现有 Python EGIA 客户端添加 Rust EOS gRPC 健康、Gateway Discover/ListResources/GetStatus 与 CapabilityService。保留已发布传输标识和类型化错误；动作只经过 Dispatcher。
- [ ] 提供版本化 EOS 宿主技能 ABI：EOS 提供有界能力回调和生命周期；已验证技能包导出描述符和单一入口。未配置宿主、无效包和未声明能力均须在 guest 执行前拒绝。技能不得持有 dispatcher、硬件、图编辑或 workflow manager 句柄。旧包的现有 C ABI 符号按明确兼容策略保留。
- [ ] 将 EGIA Python 的 link、perception、context、mission/decision、safety、monitor 阶段表示为类型化图节点，各状态只有一个所有者。类模型服务仍在图外，`egia.tensor` 延后；降级就绪不同于仅绑定 TCP 端口。
- [ ] MuJoCo 留在 EOS 内部适配器注册表，IsOS 管理外部精确版本软件包。在 Apply 和运行时验证程序、场景、资源、套接字、适配器/设备绑定、命令身份及停止行为。先停止已准入执行器，再终止并回收模拟器。
- [ ] IsosUI 使用可搜索、目录驱动的选择器，并保留不可用的当前值。每条线显示/编辑优先级和频率，提供图例及无障碍文字；类型/单位和具体拒绝原因与实时健康分开显示。增加区分类型与实例描述的浮动节点信息卡。控制、监视、Vision、日志面板可独立停靠或浮动，保存布局且始终显示停止/故障状态。首个控制演示选择一个活动原语绑定和目标，通过现有浏览器桥接调用 `primitive_typed`，显示关联结果。

## 4. 清理、验收与发布

- [ ] 检查最后调用方后，删除未使用的 `src/eos/` C++ 运行时、旧启动器、CMake 分支、测试、示例及指南。保留有效 CXX mock、适配器 C ABI 与 EGIA 所需 EOS schema。余下 `v2` 示例/脚本改用主题文件名，不更改传输 ID。
- [ ] 对新旧图、软件包 sidecar、选择器、版本不符、QoS 公平/过载、日志、UI、EGIA 降级启动、模拟器崩溃/重启、不确定命令和安全停止运行格式、静态、单元及集成测试。性能声明前记录固定负载的吞吐量、中位数、p99 和环境。
- [ ] 从干净 home 完成安装 → 图预览 → Apply → 启动 → 观察 → 控制 → 日志 → 故障/恢复 → 停止 → 升级。按实际命令审查中英文指南，然后评审并 squash merge 各主题 PR。通过验收后才发布 0.x 标签；历史 1.x 标签不改。

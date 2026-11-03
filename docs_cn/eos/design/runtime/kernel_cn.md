# 硬件能力内核

[English](../../../../docs/eos/design/runtime/kernel.md) · [运行时总览](README_cn.md) · [中间件架构图](../eos-middleware-map_cn.md)

**EOS 内核**是应用级准入层，不是操作系统内核。第一阶段在可信运行时调用方与现有 [Dispatcher](dispatch_cn.md) 之间增加硬件 [Catalog 和 Broker](../../../../src/eos/runtime/kernel/)。Dispatcher 继续负责 schema 校验、命令身份、顺序、租约、stop 与 estop，以及唯一的后端释放路径。

## 目录与名称

启动时，`Catalog` 将已准入设备声明的每个 primitive 投影为规范名称：`hardware/{robot_id}/{device_class}/{part_id}/{primitive}@{version}`。必要时会对各段做百分号转义。稳定的机器人与部件身份标识目标硬件；目录在调用前解析出进程内生成的 `device_id`。隐式设备 `motion.stop` 使用同一形式；整机 `robot.stop` 和 `safety.estop` 使用 `runtime/{robot_id}/{primitive}@1.0.0`。规范名称重复会使启动失败。目录也可接受通过现有硬件解析器配置的命名部署槽位；目标缺失、歧义或未声明时该解析器默认拒绝。当前 `eosd` 组合没有传入额外槽位。

目录的 `Available` 检查读取普通设备动作的当前健康状态，同时要求 `alive` 与 `admitted`；stop 与 estop 能力保持可用。它用于实时 [v2 发现](../../interfaces/capabilities_cn.md)和颁发授权，不是已准入命令的额外关口。Dispatcher 负责命令重放、安全与健康状态判定、命令有效性和物理释放。当前目录只有硬件操作；资源、模型、技能与社区能力仍属于[中间件架构图](../eos-middleware-map_cn.md)中的后续阶段。

## Broker 与调用方

Broker 解析硬件能力，然后把精确的 primitive、版本、设备 ID、参数与命令 ID 交给 Dispatcher。对于 worker 风格的调用方，它可颁发绑定到一个能力目标、`execution_id`、代次和过期时间的不透明授权；颁发时要求当前可用。执行必须具有相同作用域和有效且未撤销的令牌；可按执行或代次撤销。仅知道能力名称并不授予权限。命令一旦准入，Broker 的转发必须让 Dispatcher 决定终态重放以及当前安全或健康结果。

当前运行时在硬件准入后组合一个 Catalog 和 Broker。ActionList 能力执行与内部可信 primitive 执行都经过 Broker。v2 [CapabilityService](../../interfaces/capabilities_cn.md)通过同一个 Broker 解析规范名称，并启动以操作 ID 为命令 ID 的 Dispatcher 命令。查询、观察和取消读取 Dispatcher 命令记录；恢复的 `Operation` 无法重建调用方选择的能力别名。独立 HTTP 适配器已通过 REST 和 SSE 暴露 v2 能力路由。生产环境中的打包 worker 使用同一个 Broker：provider 将每个已声明 primitive、版本和已准入设备解析为规范目录名称，再为这一精确能力和执行作用域申请不透明授权。worker 的 primitive 启动、轮询和取消请求通过 Broker 返回 Dispatcher。带私有授权表的旧版独立 worker-provider 构造函数仅留给隔离测试。可信入口由 EOS 组合时选择，不能通过请求字段选择。

## 失败与迁移边界

未知或未准入的硬件操作在调度前失败；不可用设备不能获得新授权。缺失、过期或撤销的作用域授权不能通过校验。可信调用方仍受到 Dispatcher 准入、安全、健康及重放规则约束；Broker 不重试命令，也不伪造成功确认。当前 northbound 传输没有调用方认证，因此部署仍依赖其文档化的 listener 边界；可信方法不能作为不受保护的 worker 或外部调用选项暴露。

HTTP、REPL、ActionList 工作流与智能体可以使用版本化能力动作接口。其他操作所有者仍属于后续工作。Broker 本身仍是内部 C++ 边界；对外契约是 v2 gRPC 与 HTTP 服务。

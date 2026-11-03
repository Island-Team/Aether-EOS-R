# 打包技能 worker 边界

[Skill runtime 总览](../skills_cn.md) · [SkillManager supervisor](manager_cn.md) · [English](../../../../../docs/eos/design/runtime/skills/external.md)

**当前范围：** 本页描述可选 C++ 技能侧链及其 `eos-skill-worker`。默认 Rust runtime 已另行实现 R04 原生宿主：经过验证的显式文件选择、私有 `aether-runtime --skill-worker` 重新执行、仅 primitive 的描述符准入和七方法 SkillService 子集。详见 [R04 Rust 宿主设计](host-rust_cn.md)。C++ worker 在 Q02 末次调用者检查前仍是兼容参考。

## 所有权和数据流

`SkillManager` 管理身份、排队、claim、生命周期和记录保留。worker provider 管理子进程及 IPC。EOS runtime binding manager 将稳定的 robot/class/part selector 解析为已准入设备。生产 provider 在内核 Catalog 中查找对应的规范硬件能力，并向共享 Broker 申请执行范围内的令牌。worker 不持有 Dispatcher 指针或 backend socket。Broker 在启动 primitive 前校验精确能力、execution、generation、device 和有效期。provider 随后通过 Broker 的命令 ID 轮询及取消方法访问 Dispatcher；Dispatcher 仍负责硬件安全准入、命令重放与同设备排序。

worker IPC **1.0.0**（wire encoding `1`）使用 Unix `SOCK_SEQPACKET`，每个 packet 最多包含 64 KiB inline payload；可选 sealed read-only memfd 最大 64 MiB。宿主通过 `SO_PEERCRED` 验证 worker 身份。版本、身份、framing 或 grant 不匹配时拒绝执行，不回退到旧协议。

每次执行使用一个 worker。正常完成、取消、期限届满、worker 退出或协议错误时，provider 都撤销其 Broker 授权；缺少 capability slot 时硬件请求默认拒绝。活动 primitive 的取消先通过 Broker 交给 Dispatcher，再终止 worker；若无法确认硬件终态，EOS 报告 `worker.outcome_unknown`，绝不自动重试。带私有授权表的旧版独立 provider 构造函数仅用于隔离测试，不用于生产组合。

当前 bridge 支持打包 ABI v2 技能，以及经 broker 授权的 primitive、resource read、inference、source publish/query/wait、SIGNAL receive/emit、checkpoint 和可选 ABI v2.1 社区调用。worker 把定向发送、主题发布和接收转交给 SkillManager；交付语义见[社区契约](../../../interfaces/skills/native/ipc_cn.md)。子进程 callback 尚未桥接。production startup 不注册内置技能。加载技能代码前，worker 强制应用 `no_new_privs`、资源限制、Landlock 只读目录规则，以及禁止新网络 socket 和子进程创建的 seccomp 规则；无法启用时拒绝准入。对外执行入口仍是 SkillService；worker IPC 只供 EOS 内部使用。

## Python 3.12 worker profile（计划中）

当前尚未准入 Python 软件包代码。原生 worker 只接受 ABI v2 共享库；`NativeWorkerProvider` 启动固定的 `eos-skill-worker`，由 `Runtime::LoadDirectoryWithBroker` 加载共享库。已安装的 `EOS::SkillSDK` 只查询 ABI 版本和检查布局，并非可复用的 broker client。当前协议和 loader 不支持 Python entry point 或解释器选择。

不得在 `eosd` 中嵌入 Python，不得在 catalog 准入期间导入软件包，也不得放宽原生 worker sandbox 来容纳解释器。必须新增独立的 C++ `PythonWorkerProvider`，读取静态验证的软件包描述，并为每次执行启动一个 Python worker。可复用版本化 `SOCK_SEQPACKET`、peer 验证、capability Broker 和取消语义，但须新增 worker 身份和元数据准入路径。pybind11 模块只在 worker 内，并只公开类型化 broker 调用；不得暴露 EOS 指针、Dispatcher、原生 socket 或不受限文件系统。

C++ bootstrap 必须在运行 Python 前设置 `no_new_privs`、资源限制和 Landlock，只读开放所选 Python runtime 与准入包。Python 以隔离模式启动且不初始化 site；受信任 bridge 安装最终 seccomp 策略后，才可导入包 entry point。策略继续禁止网络和子进程，仅允许通过 broker 使用能力。用户可配置 Python 3.12 的明确路径、uv 环境或 Conda 环境；解释器和其原生库路径是明确的受信任主机配置。

准入需要版本化、纯数据格式的 Python 技能描述，包含精确身份、entry point、参数/结果 schema、能力和 Python ABI/runtime 限制。EOS 在启动 worker 前验证路径、schema、大小限制和声明；catalog 检查绝不导入 entry point。这是独立执行 profile，不能作为原生 ABI v2 的另一种文件后缀。

发布前测试必须证明错误软件包在 entry point 导入前被拒绝；worker 不能在声明的临时目录外写文件、打开网络 socket、创建子进程，或在退出后留下 broker descriptor；成功、取消、崩溃或协议故障后都能回收 worker。还须测试解释器路径限制、Python 3.12 的明确路径/uv/Conda runtime、未声明能力拒绝、能力调用期间取消、peer 身份不符、导入 hook，以及 sandbox 功能不可用。实现必须证明所有 sandbox 控制生效前不会运行包代码。占位实现和限制见[外部语言 worker 状态](../../../../../src/eos/runtime/skills/external/README.md)。

# 开发 v2 硬件基础层

[English](../../../docs/eos/develop/hardware.md) · [接口字段](../interfaces/hardware_cn.md) · [硬件设计](../design/runtime/hardware_cn.md) · [分发层设计](../design/runtime/dispatcher_cn.md)

v2 Rust crate 位于 `crates/aether-*`：`common` 提供统一时钟和有界 I/O，`node-profile` 解析并校验统一的 `1.0.0` 节点树，`hal` 管理类型化硬件值和适配器，`dispatcher` 负责树形查找、命令准入和安全，`aether-runtime` 通过 API 和[Rust 进程](../design/runtime/process_cn.md)组合它们。C++ 的设备库与 ROS 2 代码只留在 HAL 的 CXX 边界后。公开的 `eosd` 启动脚本现已选择 Rust。C++ 进程保留为未启动的侧链。

不打开设备即可检查已校验的节点树：

```bash
cargo run -p aether-runtime --bin aether-profile -- etc/eos/backends/mock-cxx.toml
```

仓库演示 profile 在 EOS 硬件层包含 CXX mock 相机、底盘和机械臂，还包含 EOS 分发软件节点及 EGIA 请求保护设计节点。机械臂按静态目标顺序声明 `joint1` 至 `joint6`。软件节点可声明能力及指向其他节点的完整路径引用，但不拥有硬件适配器。此检查显示 profile 准入与发现，不表示实体机器人或 ROS 2 已连接。

对于软件包贡献的图，IsOS 通过可重复的 `--node-types <absolute-path>` 参数为每个软件包传入一个已验证侧文件。`aether-profile validate --file <graph> --node-types <sidecar>` 在不打开设备的情况下检查软件包引用、类型位置、设置和类型化边；`aether-profile catalog --node-types <sidecar>` 包含贡献类型。运行时和配置服务也接受同一参数。编写软件包节点集之前请阅读[软件包节点 IDL](../interfaces/node-contributions_cn.md)。

IsOS 还将 `aether-profile setup --json` 用作按需调用的 [v1 图设置适配器](../interfaces/setup-v1_cn.md)。它在 EOS 启动前验证或生成共享图配置，再把配置内容交给 IsOS 原子安装。

IsOS 以 `{"schema_version":2,"op":"identity","core":"eos"}` 调用 `setup --json`，报告 EOS 的产品版本与兼容性。软件包适配器应基于 [`include/aether/adapter_v1.h`](../../../include/aether/adapter_v1.h) 构建共享库，在软件包清单中声明适配器族、软件包内相对路径和 SHA-256，并通过已验证节点类型侧文件贡献对应的适配器族。IsOS 通过 `--adapter-library family=/绝对路径/library.so@sha256` 将解析后的共享库传给 `aether-runtime`。部署运行时为每个节点创建独立适配器宿主进程；宿主在创建设备前再次检查摘要与 ABI。编译后的 C stub 测试覆盖 ABI 拒绝、类型化状态与健康、并发停止和心跳失效；实体适配器切换仍需硬件集成测试。

身份与共享库切片通过工作区格式检查、公开头文件格式检查、聚焦 Clippy 与测试，以及文档链接检查。完整输出位于本地 `logs/test/20261031-eos-identity-adapter-library/eos-adapter-final.log`。实时 MuJoCo 集成测试需要外部仿真器，仍需显式启用；聚焦测试已运行现有的直接子进程 stub 和进程测试。

每节点适配器宿主切片于 2026-10-31 通过 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace --no-fail-fast` 和 `cargo build -p aether-runtime --bins`。完整输出位于 `logs/test/20261031-adapter-host/eos.log`；实时 MuJoCo 测试仍须显式启用。C stub 验证子进程准入、摘要拒绝、调用阻塞时停止，以及 500 ms 父进程心跳租约。实体 CAN FD 时序与高频获准目标流在硬件切换前仍需实体测量和类型化流契约。

集成后，在隔离的 `ISOS_HOME` 中运行 IsOS 仓库 `tests/real_home_v2.py` 的临时 30 帧 WebSocket 采样版本，且未设置 `MUJOCO_GUI`，通过了 IsOS → EOS → 无窗口 MuJoCo、两个 Vision gate、底盘/机械臂调度、已链接资源变更拒绝、重启与停止检查。声明的源速率为 30 Hz；两个 gate 的实际交付速率分别为 30.946 与 30.931 fps，帧间隔 p50/p99 分别为 30.412/40.346 ms 与 30.360/42.166 ms。直接控制接口采集仍较慢（20 次调用，中位数 379.206 ms），因此应单独测量 Vision 预览路径。完整输出位于 `logs/test/20261031-adapter-host/real-home-30-frames.log`。

设置侧文件后续修复验证 `validate` 和 `render` 会注册 IsOS 请求中列出的软件包贡献，并测试软件包身份不匹配的拒绝行为。格式、Clippy、`aether-profile` 测试与构建、文档链接均通过；输出保存在本地 `logs/test/20261031-eos-setup-package-sidecars/setup.log`。

设置适配器切片通过 `cargo fmt --all -- --check`、`cargo clippy -p aether-runtime --all-targets -- -D warnings`、`cargo test -p aether-runtime` 和 `python3 scripts/check_doc_links.py --all`。完整输出保存在本地 `logs/test/20261031-core-setup-adapter/`；命令行生成仓库内演示图时也返回了 `eos/graph.json`，没有启动 EOS。

进程内集成测试可调用 `Runtime::from_cxx_mock_profile(profile, clock)`，按演示 profile 为三个节点各构造一个隔离的 CXX mock。对应测试发现三者、抓取 RGB-D、分发运动并对两个执行器急停。该辅助函数只用于开发，不连接 ROS 2。

进程集成测试可用独立的日志和 socket 路径启动 Rust 程序：

```bash
cargo run -p aether-runtime --bin aether-runtime -- --profile etc/eos/backends/mock-cxx.toml --journal-dir /tmp/eos-v2-journal --socket /tmp/eos-v2.sock
```

全部准入后程序输出 `1.0.0` READY JSON。权限为 0600 的 Unix socket 接受有界逐行 JSON 的发现、健康、时间、类型化 mock 命令、结果、停止和关闭请求。使用持久化日志目录检验重启幂等性；重放必须使用完全相同的原始命令字段。字段规则见[进程设计](../design/runtime/process_cn.md)。该程序接纳 mock 和 MuJoCo v2 适配器。公开的 `eosd` 脚本与安装的 `aether-runtime` 运行它；实体 ROS 2 与厂商适配器仍需后续补齐。

任何可向物理设备下发命令的进程都应使用 `Runtime::new_durable(profile, adapters, clock, journal_directory)`，并将日志目录放在持久存储上。打开目录时会获取进程独占锁。命令 ID 在重启后仍被保留；未完成记录恢复为不确定结果，损坏记录阻止启动。`Dispatcher::new` 和 `Runtime::new` 的内存日志只用于测试。读取失败或错误样本会使对应节点降级；仅在适配器重新提供有效且新鲜的样本后调用 `refresh_node`。CXX 驱动的 `stop` 必须独立于阻塞的常规调用，并在物理写入边界锁存。

R02-24/32/33 journal 故障重放：`cargo test -p aether-runtime --test grpc_journal_fault -- --nocapture` 的四个公开回环 gRPC 用例通过；`cargo clippy -p aether-runtime --test grpc_journal_fault -- -D warnings` 与 `cargo fmt -p aether-runtime -- --check` 通过。完整本地输出位于 `logs/test/20261101-r02-stop-checked/literal-journal-faults.log`、`literal-journal-clippy.log` 与 `literal-corrections-fmt.log`。第一个释放前用例阻断私有控制记录写入，核对 `INTERNAL`、适配器停止调用次数为零且急停未锁存。R02-32 容量用例提交含两个部件的 `robot.stop`，只把测试 journal 的记录上限降为 512 字节，核对 `RESOURCE_EXHAUSTED`、部件调用为零、无抑制或锁存且没有预留操作。释放后用例通过有计数的终态结果测试适配器，在 `Releasing` 持久化后阻断终态记录替换，核对实时 `OUTCOME_UNKNOWN`；随后在隔离的临时 journal 中恢复原始 `Releasing` 记录，核对重启恢复、相同 ID 查询／重放以及物理调用恰好一次。R02-33 用例在第一个部件结果和第二个部件尝试持久记录之后，由第二个部件的测试适配器降低记录上限。公开 Submit/Get 显示 `OUTCOME_UNKNOWN`，第一个部件已确认、第二个部件未知；重启后仍保持抑制和相同 ID 仅可查询的结果，且不再调用部件。这些是通过公开 gRPC 监听器执行的确定性同进程故障注入，不模拟文件系统操作中的 OS 崩溃。上限覆盖只存在于编译时的 `aether-dispatcher/test-hooks` 特性，测试构建时由 `aether-runtime` 开发依赖启用；Cargo 特性统一可能使同一次构建中的其他工作区测试目标也启用此特性，但运行时配置或用户输入均不能启用。普通 `cargo build --workspace` 与 `cargo rustc -p aether-dispatcher --lib -- --print cfg` 通过，且 `logs/test/20261101-r02-stop-checked/{workspace-build-normal,dispatcher-normal-cfg,normal-feature-check}.log` 没有 `feature="test-hooks"` 条目。

R02-31/34 Dispatcher 重放：`cargo test -p aether-dispatcher --test gateway_stop -- --nocapture` 的执行中扇出运动拒绝与变更注册表恢复用例通过；完整输出位于 `logs/test/20261101-r02-stop-checked/literal-stop-restart.log`。RobotStop 部件阻塞时，新运动在释放门超期，执行器调用次数为零且停止仍在等待。重启后准入集合从 `a,b` 改为 `a,c`：旧的部分确认 `OUTCOME_UNKNOWN` ID 仍仅可查询，且保留的目标 `b` 不可用，因此新恢复 ID 在没有释放任何部件时失败。准入 `a,b,c` 后，journal 快照三个路径的并集；一个未确认的部件继续保持抑制，仅在稍后授权的新 ID 停止中三个部件全部确认终态后才解除。

R02-30 崩溃间隙重放：`cargo test -p aether-dispatcher --lib crash_after_first_confirmed_part_leaves_second_unattempted_and_query_only -- --nocapture` 在第一个 RobotStop 部件结果完成同步后、第二个部件标记为尝试前注入进程中断。重新打开的 journal 报告 `OUTCOME_UNKNOWN`，保留首部件已确认与次部件未尝试的状态；同 ID 重放不会再次调用物理适配器，调用次数为 `(1,0)`。这与第二个适配器 panic 的“可能已调用”用例不同。完整输出位于 `logs/test/20261101-r02-stop-checked/r02-30-crash-gap.log`。

R02 停止通道饱和重放：`cargo test -p aether-runtime --test grpc_stop_lane -- --nocapture` 提交 16 个在物理释放前阻塞的运动请求。第 17 个返回 `RESOURCE_EXHAUSTED`，且没有预留 ID；`robot.stop` 仍进入 Dispatcher 并持久记录抑制，`safety.estop` 在运动请求解除阻塞前通过独立通道成功。随后解除阻塞的运动请求全部被拒绝，物理调用次数为零。测试采用能证明停止结果的桩适配器，不宣称 MuJoCo 支持受控停止。完整输出位于 `logs/test/20261102-r02-stop-lane/focused.log`。

## 修改或增加节点

1. 先定义 Rust 采样与命令类型；在[接口字段表](../interfaces/hardware_cn.md)写明每个字段的类型、单位、用途和校验。使用显式可选值表示设备不支持的观测。
2. 在唯一的 `1.0.0` 节点 profile 中声明节点的 `core → layer → group → node` 位置、能力和模式、限值、适配器设置及完整路径引用。构造驱动前拒绝未知键、不存在的引用、重复路径和错误限值。不增加第二份清单，也不翻译 v1 profile。
3. 每个节点只创建一个 C++ 适配器实例。SDK 与 ROS 2 类型留在 C++ 侧，通过 CXX 传送有界的类型化数据。驱动不得自行注册另一条执行路径。
4. 编写 mock 单元测试和跨边界集成测试，覆盖正常执行、错误输入、健康降级、超时、停止以及两个节点之间的隔离。
5. 同步修改中英文文档，执行针对性检查，并将完整输出保存到 `logs/test/<run-id>/`。每个里程碑在专用 `refactor(v2)/<topic>` 分支开发，验证后压缩合并到 `refactor(v2)/runtime-main`。

## 公共运行时组件

使用 `aether_common::time::Clock` 读取本地单调时间，用 `ManualClock` 进行可重复的时限和新鲜度测试。命令准入不得直接读取墙钟。设备或 ROS 2 源时间戳仅表示来源；本地接收单调时间用于期限与新鲜度。读取 profile 和适配器数据时使用 `common` 有界 I/O，在分配或反序列化前拒绝超大数据。统一组件避免模块各自制定时间和大小规则。

## 检查与日志

`refactor(v2)/vision-fps` 分支通过 `cargo fmt --all`、`cargo test --workspace --all-targets`、`cargo clippy --workspace --all-targets -- -D warnings` 和 `python3 scripts/check_doc_links.py --all`。完整输出保存在本地 `logs/test/20261031-vision-fps/`。修改前在本机对 30 次固定的 640×480 MuJoCo Unix 套接字相机请求测得响应中位数 2.21 ms、p99 5.29 ms；30 帧 EOS Vision WebSocket 样本为 11.78 fps，帧间隔中位数 82.52 ms、p99 100.19 ms。这是本机 debug 构建的观察值，不是吞吐保证。现在相机为经过验证的完整宽度 Vision 请求保留仿真器的质量 85 JPEG，源和查看端的采集间隔也计入处理时间。修改后的端到端测量需部署重新构建的 EOS 二进制。

在 `refactor(v2)/canonical-device-primitive` 分支上，`/home/operator/.cargo/bin/cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace --no-fail-fast` 和 `python3 scripts/check_doc_links.py --all` 均通过。完整输出保存在本地 `logs/test/20261031-canonical-device-primitive/{fmt,clippy,test,doc-links}.log`。针对性测试覆盖短路径导入和混合路径拒绝、旧 Vision 所有者迁移、适配器及绑定图校验、通过 Dispatcher 准入底盘和机械臂类型化命令、伪造目标和命令头不匹配拒绝、相机 Vision WebSocket 行为以及模拟器子进程停止与回收。MuJoCo 软件包的八节点图通过 `aether-profile validate --node-types <已验证侧文件>`。同一日志目录中的直接子进程 smoke 脚本启动该图及 EOS HTTP bridge，经相机 gate 的二进制 WebSocket 收到声明为 30 Hz 的 640×480 JPEG，完成类型化底盘与机械臂命令，在伪造目标生成日志结果前将其拒绝，最后终止 EOS 并确认模拟器子进程已回收。结果在 `vision-smoke.log`，脚本为 `vision_smoke.py`，子进程输出在 `vision-runtime.log`。实体 ROS/供应商等价性及专用高频执行器通道仍待完成；当前边界见[原语绑定契约](../interfaces/primitive-bindings_cn.md)。

在 `refactor(v2)/general-node-interfaces` 上执行 `/home/operator/.cargo/bin/cargo fmt --all -- --check`、`/home/operator/.cargo/bin/cargo clippy --workspace --all-targets -- -D warnings` 和 `/home/operator/.cargo/bin/cargo test --workspace --no-fail-fast`。完整输出保存在 `logs/test/20261030-general-node-interfaces/eos-general.log`。HAL 测试覆盖类型化最新值订阅和 JPEG 转换限值；运行时测试覆盖 MuJoCo 子进程结构化日志与图 SHA256；桥接测试覆盖带掩码订阅帧、拒绝浏览器来源与过期/非活动订阅，以及 WebSocket 断开重连。IsOS 的 `logs/test/20261030-general-interfaces/real-home-vision-r2.log` 记录了真实 MuJoCo 集成，覆盖两个 gate 的 JPEG 流、执行器停止、重启和退出。浏览器线协议见 [Vision 契约](../interfaces/vision_cn.md)。

IsOS 以 `--log-dir <absolute-dir>` 启动 EOS 时，EOS 是 `<dir>/mujoco.log` 的唯一写入者。EOS 将仿真器标准输出和错误输出转为 JSONL，包含 UTC 毫秒时间戳、`info` 或 `error` 等级、`mujoco` 来源及有界 UTF-8 消息。轮转使用 `ISOS_LOG_MAX_BYTES` 与 `ISOS_LOG_BACKUPS`；`ISOS_LOG_LEVEL=warn|error` 会抑制标准输出行，但仍持续读取管道。

软件包节点契约切片在 `refactor(v2)/package-node-contract` 上通过 `/home/operator/.cargo/bin/cargo fmt --all -- --check`、`/home/operator/.cargo/bin/cargo clippy --workspace --all-targets -- -D warnings` 和 `/home/operator/.cargo/bin/cargo test --workspace --no-fail-fast`。完整输出保存在本地 `logs/test/20261030-package-node-contract/eos-contract.log`；若未运行仿真器，MuJoCo 实机测试仍被忽略。

软件包 group 日志修复通过格式检查、针对性 Clippy 与测试，并构建 EOS runtime、profile、config server 和 HTTP bridge。完整输出保存在本地 `logs/test/20261030-package-node-journal/eos-journal.log`。回归测试持久化并恢复通过 `groups/mujoco` 寻址的执行器命令。

在仓库根目录执行 Rust 格式、Clippy、HAL 与分发层测试、`aether-runtime` 组合测试，以及适用的 C++ 适配器构建。仓库脚本创建唯一运行目录并记录每项命令：

```bash
./scripts/test_rust_workspace.sh
./scripts/test_rust_workspace.sh --bench  # 加入 release 模式的分发基准
```

脚本写入 `logs/test/<run-id>/{fmt,clippy,test,bench}.log`，只有请求基准时才生成 `bench.log`。记录提交、工具链、机器、命令和结果摘要；完整 `.log` 保留在本地且被 Git 忽略。只有退出码为零且日志完整时才能声称测试通过。

基准测试使用固定 profile、固定帧大小和固定种子，分别测量准入、采样传输与分发，报告吞吐量、中位数、p99、构建模式和机器。先建立基线，再决定阈值。常规构建不依赖 ROS 2 或供应商 SDK；可选 C++ 集成在编译期启用，mock 路径始终可以独立测试。只有可复现的基准发现瓶颈后才优化热路径。

已验证的本地基线见 `logs/test/20261029T165952Z-933236/`（中国时间 2026-10-30，提交 `455c87ba`，x86_64 Intel Core i5-12490F，Rust 1.98.1，优化 release 基准构建）：两个节点的 profile 准入为 27,168 ops/s，中位数 34.54 µs、p99 58.33 µs；5,000 次分发为 935,054 ops/s，中位数 0.76 µs、p99 2.52 µs；1,000 次 Rust mock 640×480 RGB8 加 depth16 抓取为 16,080 ops/s，中位数 59.62 µs、p99 76.06 µs。该次运行通过格式检查、Clippy 和 26 个 Rust 测试。全量文档链接检查了 202 个 Markdown 文件，0 个断链。这是本地 mock 测量，不代表实体硬件性能或发布阈值。

安全日志阶段的运行记录在 `logs/test/20261029T171721Z-944811/`（同一 CPU 和 Rust 工具链，中国时间 2026-10-30，集成基点 `268bc65e`，release 基准）：格式、Clippy、39 个 Rust 测试及 204 个文档文件的链接检查均通过。固定 100 条命令的持久分发测得 125 ops/s，中位数 7,873.38 µs、p99 10,421.69 µs；每条命令同步写入预留、释放和结果记录。5,000 条命令的内存分发测得 561,180 ops/s，中位数 1.46 µs、p99 3.39 µs。在独立目标目录中全新执行 `cargo build --workspace --timings` 用时 5.78 s，完整输出位于上次运行的 `logs/test/20261029T171552Z-943087/compile.log`。持久结果取决于本机文件系统，仅作为未来日志批处理或存储设计的基线，不是生产目标；优化不能削弱物理下发前的同步保证。

## 切换规则

将生产路径从 C++ 迁入 Rust 时，为每项旧操作建立唯一的类型化节点路由，验证观测和停止行为，并确保每台设备只有一个分发者可释放动作。替代功能和测试齐备后，在同一次切换中删除旧代码并修订旧文档。在此之前，v2 文档只描述进行中的基础工作；部署操作仍参考现有运行时指南。

## 包后端注册表检查（2026-10-30）

在 `refactor(v2)/package-registry` 上运行 `cargo fmt --all -- --check`、`cargo clippy -p aether-node-profile -p aether-dispatcher -p aether-mujoco --all-targets -- -D warnings` 和 `cargo test -p aether-node-profile -p aether-dispatcher -p aether-mujoco`。针对性检查的完整输出保存于本地 `logs/test/20261030-package-registry/eos-registry.log`；最终全 workspace 的格式检查、Clippy 和测试均通过，记录在 `workspace-final.log`（一个原有测试被忽略）。测试解析由 IsOS 管理的 MuJoCo 图，并拒绝不一致的套接字、设备 ID、包版本或后端引用。

在 `refactor(v2)/eos-mujoco-owner` 分支运行 `cargo fmt --all -- --check`、`cargo clippy -p aether-node-profile -p aether-runtime --all-targets -- -D warnings` 和 `cargo test -p aether-node-profile -p aether-runtime`，验证由 EOS 管理的 MuJoCo 生命周期。桩子进程测试覆盖套接字就绪、直接子进程回收、停止清理、启动失败和错误后端身份。IsOS 通过 `--backend-node`、`--backend-executable` 和 `--backend-scene` 传入已验证的包路径；EOS 继承 IsOS 解析的 MuJoCo 环境，并在终止仿真器前停止执行器。完整本地输出在 `logs/test/20261030-eos-mujoco-owner/`。

EOS 控制节点切片加入经图准入的手动与 primitive 调用，以及有界的 skill 序列队列。移动仍使用具体的 Rust 类型；`Runtime::invoke_control` 先从已应用图解析硬件目标，再交给 dispatcher 准入。浏览器调用使用 [v2 图控制接口](../interfaces/control_cn.md)；HTTP bridge 限制 Origin 并拒绝原始 dispatch、stop 和 shutdown。合并前运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace` 及 `aether-profile validate --file examples/v2/mujoco-isos.toml`。完整本地输出位于 `logs/test/20261030-eos-control/`。

停止后观察和 MuJoCo 完成确认切片增加只读的 `Dispatcher::status`，并允许安全停止后读取新鲜的底盘与机械臂样本。分发仍要求 `Ready`，mock 测试确认停止后没有再次下发。MuJoCo 线协议测试覆盖租约完成证据、证据缺失、期限到期、确认中停止、机械臂收敛和目标不匹配。2026-10-30 在 x86_64 Intel Core i5-12490F、Rust 1.98.1 上运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings` 和 `cargo test --workspace`；完整本地输出位于 `logs/test/20261030-eos-stop-observe/`。实际 IsOS → EOS → MuJoCo 双步骤 skill 以两条结果完成；`manual_stop` 后状态为 `stopped`，底盘观察仍有效。外部运行日志位于 IsOS 的 `logs/test/20261030-positive-skill/smoke.log`。

## Q04 受管理后端恢复

若受管理 Stop 报告 `backend_group_unreaped`，保持本次运行 unknown，在重启前检查选中的 EOS 凭据与进程树。EOS 不会仅因一个直属子进程在 MuJoCo 启动后出现就向其发信号或回收：它也可能是 skill worker 或适配器宿主。只有通过后端 session/进程组或已证实的存活父进程观察到身份，EOS 才可向脱离后端的后代发信号。父进程消失后才被收养且归属不明的进程保持不动。这是保守的停止结果；应使用 IsOS 的 degraded 运行恢复流程，不能把套接字删除或 leader 消失当作停止确认。Q04 归属回归测试与完整命令记录于 `logs/test/20261103-q04-backend-ownership/`。

`refactor(v2)/q04-backend-ownership` 基于 `runtime-main` 的 `1dfd513f`，运行 `/home/operator/.cargo/bin/cargo fmt --all -- --check`、`/home/operator/.cargo/bin/cargo clippy --workspace --all-targets -- -D warnings`、`/home/operator/.cargo/bin/cargo test -p aether-runtime --bin aether-runtime backend_child::tests -- --test-threads=1`、`/home/operator/.cargo/bin/cargo test --workspace --no-fail-fast -- --test-threads=1`、`/home/operator/.cargo/bin/cargo build --workspace` 和 `python3 scripts/check_doc_links.py --all`。聚焦测试 15/15 通过，串行工作区测试通过；完整输出位于上述本地日志目录的 `*-final.log`。较早的默认并行运行保留作诊断：一轮在旧集成提交上遇到无关的 Dispatcher 时序断言，另一轮暴露测试间进程级子进程普查干扰。Q01 资格门要求串行工作区运行。

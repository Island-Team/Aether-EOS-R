# R02 gRPC 首次使用与验证

[English](../../../docs/eos/develop/grpc-gateway.md) · [所有者契约](../interfaces/grpc_cn.md#r02-冻结的-rust-子集) · [设计](../design/runtime/grpc-gateway_cn.md) · [机器用例](../../../tests/fixtures/r02_grpc_gateway/README_cn.md)

**R02 所有者阶段记录：** R02 实施已通过本地验收和独立实施评审。Rust runtime 已在回环 gRPC listener 上实现 R02 要求的 11 个 RPC，并通过所选 EGIA typed client 的 mock profile 重放。测试注入的生产者及具备证明能力的 adapter 覆盖正向资源读取和受控停止边界。在 R02 所有者阶段，生产 MuJoCo 运动仍待 R05 物理代次隔离与 R06 模拟器停止/回收顺序。后续[限定范围 M3 验收](../../island/m3-qualification_cn.md)在合并输入上关闭完成条件。

1. 选择精确的 IsOS 已 Apply 图和生成配置。用已验证 profile、journal、私有控制 socket、MuJoCo 子进程和新回环 gRPC listener 启动 `eosd`。当前实现遇到缺失 adapter 或损坏 journal 时，在 listener 绑定前失败且不发 ready 事件。若未来在准入前绑定，health 必须保持 `NOT_SERVING`，必需 RPC 必须返回 `UNAVAILABLE`。
2. `SERVING` 后使所选 EGIA typed client 连接 listener。它必须完成 health、`Discover`、`ListResources`、`GetStatus`、`ListCapabilities`，以及可选的 `ListSkills`（未实现时 `UNAVAILABLE`）。将匹配的机器人 ID/名称、非空状态/版本/启动时间、真实设备、精确 primitive/能力配对、资源和健康与保存图及运行时投影比较。不得虚构空 world/detection/ASR 资源。发布前拒绝无效候选代际，确认现有 listener 保留同一不可变 binding 代际，实时健康可以降级。backend 丢失后再次检查。
3. 读取已声明资源，启动 `WatchResource`，改变其已校验产品，检查同 URI 通知后再 `ReadResource`。断开 stream，再以新 watch 加读取重连。覆盖冷、过期、未知、慢消费者与取消路径。当前 EGIA 对慢消费者的 `RESOURCE_EXHAUSTED` 不自动恢复；记录传给调用方的错误，并由调用方显式重建 watch 和读取。请求一帧和两帧相机图像，验证精确数量、不同 JPEG、尺寸、采集时间、大小上限、截止时间和缺帧路径。
4. 以调用方 operation ID 提交一个已准入 primitive。将 v2 `Operation` 与同一个 Dispatcher 命令及持久 journal 记录比较：ID、精确 binding、准入、状态、规范 `primitive.ack` 必须一致。并发重复及重启后重复该请求，adapter 只能释放一次。以不同参数或另一个已公开 binding 复用 ID 返回 `ALREADY_EXISTS`。丢失 Submit 响应与 Watch stream 后，以相同 ID 调用 `GetOperation`。绝不自动重试不确定运动。只取消该命令，需钳制运动时显式 stop。另分别验证 journal 化的单设备 `motion.stop`、无紧急 latch 的全机器人 `robot.stop`、接合紧急路径的 `safety.estop`，包括已停止、部分失败、未确认和跨重启重复 ID。使 `robot.stop` 在第一个部件已确认后、第二个部件前崩溃，核对 `OUTCOME_UNKNOWN`、已知/未知逐部件结果、同一 ID 重启不再次调用任何部件，以及超过 64 个可停止部件时不得就绪。扇出期间并发提交运动须在 adapter 前拒绝。让先前运动在同步写入 `Releasing` 后、调用 adapter 前暂停，再启动 RobotStop，确认停止扇出不能越过该运动；对未决运动结果重复该实验，确认 inhibit 在隔离或物理静止恢复前保持。分别注入释放前及可能释放后的结果溢出，核对零调用/`RESOURCE_EXHAUSTED` 与保留 inhibit 的 `OUTCOME_UNKNOWN`。重启后用新的授权 ID 对旧目标和当前目标的并集执行完整受控停止，确认持久清除 inhibit；紧急 latch 不受影响。检查 v2→v3 journal 迁移，确认全机器人停止使用 typed 机器人目标，重启后不确定的 estop 保持 latch 且不再次释放。
5. 关闭时先观察 `NOT_SERVING`、新写入拒绝及有界 watch 关闭，再核对 Dispatcher stop 先于 MuJoCo 终止/回收。后者的物理顺序属于 R06 验收；记录 R02 当前行为，不标记 R06 通过。

Rust listener 为每个已准入投影使用一个有界后台工作线程采样 adapter 健康和状态。Discover、GetStatus、ListCapabilities 与 CaptureFrames 只读取保留快照，不在 tonic 请求线程调用 HAL。样本缺失、失败或超过 `min(health_timing.stale_after_ms, 1000 ms)` 时，相应设备不可用；CaptureFrames 还要求图像帧不超过一秒。前三种响应在完整新鲜投影时以 `eos-sample-age-ms` 提供最旧健康/状态样本的年龄。新鲜 ReadResource 与成功的 CaptureFrames 提供保留产品或帧的年龄。缺失或过期样本不提供年龄头，必要的正文读取按关闭原则失败。探测在调用健康和状态接口之前记时，延迟的状态响应不能把旧健康结果伪装成新样本。延迟状态网络重放证明迟到结果保持过期和无年龄头，下一次完整探测才恢复可用。关闭时采样线程停止发布，Dispatcher 安全停止无需等待阻塞的 adapter。运动拥有十六个工作许可；受控停止和紧急停止各有单独保留许可，因此排队运动不能占用停止路线。

处于 pending 的 `WatchOperation` 先发送当前状态；如果在调用方截止时间前到达终态，则再发送一次终态。达到调用方截止时间（默认五秒）时发送 `DEADLINE_EXCEEDED`，不会静默结束；客户端随后以同一 ID 调用 `GetOperation`。丢失 Submit 响应、取消 watch 或达到截止时间都不授权新的物理释放。

## 当前进度证据

R01 合并至 `3469af893707ab6bbb1e138e2282aa2bff0d0bfa` 后，`cargo test -p aether-runtime` 通过当前 runtime 套件：26 项 binary 单元测试、4 项 adapter、2 项事件、1 项 gRPC operation、13 项进程测试；真实 MuJoCo 测试按预期仍被忽略。完整输出位于被忽略的 `logs/test/20261101-r02-post-r01/runtime-tests.log`。可复现的聚焦命令 `cargo test -p aether-runtime --test process grpc_health_discovery_and_mock_operation_share_one_admitted_identity` 检查同一 mock profile 的 gRPC health、发现、状态、空资源列表、有限能力和 operation 身份；此前聚焦输出位于 `logs/test/20261101-r02-grpc-progress/process-focused.log`。

所选 EGIA 类型化客户端重放可在本 EOS 检出目录依次运行 `cargo build -p aether-runtime` 与 `/home/ashex/projects/Island-egia/.venv/bin/python tests/r02_egia_client_replay.py`；脚本支持 `EGIA_SRC` 和 `AETHER_RUNTIME_BIN` 覆盖。当前 `logs/test/20261101-r02-gateway-final/egia-client.log` 记录 3 个设备、4 个可执行 primitive binding、可选技能不可用、mock 运动结果 `outcome_unknown` 后运动不可用，以及已确认的 `safety.estop` 结果。这证明 mock 的类型化客户端与协议对等，不证明 MuJoCo 物理运动就绪或 R06 模拟器停止顺序。EOS、EGIA、IsOS 的精确修订及最终关卡见下文。

Dispatcher 停止切片于 2026-11-01 执行 `/home/ashex/.cargo/bin/cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test -p aether-dispatcher`、`cargo test --workspace`、`cargo build --workspace` 和 `python3 tests/fixtures/r02_grpc_gateway/check_cases.py`；结果保存在被忽略的 `logs/test/20261101-r02-dispatcher-stop/{fmt,clippy,dispatcher-tests,workspace-tests,workspace-build,fixture-check}.log`。当前九个聚焦停止集成测试涵盖已知部分失败后的新授权 ID 恢复与持久化未授权拒绝、单设备非锁存 MotionStop 与持久紧急锁存、先前运动暂停、物理停止线程 panic 后的持久未知结果与同 ID 不重放、变化后登记表中的旧/新目标并集、扇出期间拒绝运动、受控停止阻塞时的抢占式紧急停止、紧急停止阻塞后保持的持久锁存，以及持久 inhibit 清除失败时不能发布 Completed。六个单元测试在运动同步写入 `Releasing` 后、进入适配器前注入暂停，覆盖不确定运动跨重启重放、排队许可的截止时间，并仅降低测试中的 journal 记录上限，证明释放前溢出零调用且不设置 inhibit、调用后溢出保持未知与 inhibit 且同 ID 不重放。journal 单元测试覆盖持久清除之后、终态记录之前崩溃：启动时先恢复 inhibit，再将旧 ID 标记为未知。阻塞时原始结果保持 `OUTCOME_UNKNOWN`，新 ID 不能对同一目标发起第二次同类别物理调用，分离线程结束前继续持有 journal 所有者锁。永不返回的同步适配器会让该线程存活至进程退出；Rust 超时只能限制调用方等待，无法杀死适配器调用或证明物理终态。这些检查仅覆盖 Dispatcher 切片；完整 gRPC 与模拟器边界仍需要独立证据。

## 集成关卡记录

EOS `ce2b19595fe1776344be0f1119f729bac8ec190b` 的最终本地关卡运行于 Linux `7.0.0-38-generic` x86_64，Rust 1.98.1。所选 `aether-runtime` 的 SHA-256 为 `d7a1bb31823c51cc9b236145ce9e19d578c72e8b6bdd4ae8d2bcd7b4e6d00f12`；所选 EGIA 检出为 `a03981fadf88d87e13f33a3ada577c6a1a8dd0df`，IsOS 为 `ace8f4c60b8a74b3a998fe2ae78da4c38afa2020`。这些是 mock 和测试 stub 重放，不是生产 MuJoCo 运动证明。

| 可复现命令或证据 | 结果及完整日志 |
| --- | --- |
| `/home/ashex/.cargo/bin/cargo fmt --all -- --check`；`/home/ashex/.cargo/bin/cargo clippy --workspace --all-targets -- -D warnings` | 通过；`logs/test/20261102-r02-health-final-v3/{fmt,clippy}.log` |
| `/home/ashex/.cargo/bin/cargo test --workspace`；`/home/ashex/.cargo/bin/cargo build --workspace` | 通过：50 个测试套件中 232 项通过、零失败，一项真实 MuJoCo 测试按预期忽略；`logs/test/20261102-r02-health-final-v3/{workspace-tests,workspace-build}.log` |
| `python3 tests/fixtures/r02_grpc_gateway/check_cases.py`；`python3 tests/eos/test_doc_links.py`；定向 `scripts/check_doc_links.py` | 通过，35 个冻结用例结构已检查；`logs/test/20261102-r02-health-final-v3/{fixture-check,doc-links,doc-links-target}.log` |
| `/home/ashex/projects/Island-egia/.venv/bin/python tests/r02_egia_client_replay.py`；同一解释器执行 `tests/fixtures/r02_grpc_gateway/egia_watch_replay.py` | 所选 typed client 与 watch 恢复重放通过；`logs/test/20261102-r02-health-final-v3/{egia-client,egia-watch}.log` |

独立实施复审在 [PR #314](https://github.com/Island-Team/Island-eos/pull/314) 前批准了 R02 的精确代码和关卡证据。P1 读取路径回归以 250 ms 截止时间反复向阻塞的 socket 等价相机健康探测施压：十二条并发读取链、health Check 和 safety.estop 均不占住 tonic 工作线程；采样器同时最多一项探测，过期设备降级，新鲜探测恢复健康，停机无需等待阻塞 I/O 即可阻止继续发布。公开 stop 许可测试占满十六个运动许可时，robot.stop 和 safety.estop 仍经单独保留许可进入。新鲜 ReadResource 与 CaptureFrames 包含样本年龄元数据；缺失或过期的投影省略年龄。聚焦证据位于 `logs/test/20261102-r02-health/{repeat-stalled-deadline,focused-coherent,delayed-status-repeat}.log` 和 `logs/test/20261102-r02-stop-lane/`。R02-23 现有直接 stub 子进程死亡重放：health 变为 `NOT_SERVING`，随后有效写入被拒绝，health watch 关闭，EOS 退出。与 listener 拆除竞速的写入可能收到传输层 `CANCELLED` 而非 handler 的 `UNAVAILABLE`；经验证的 MuJoCo 物理停止与回收仍属 R06。暂停 health 发布的回归测试证明新进入的 Submit 在状态转换期间等待，不会在 `NOT_SERVING` 前预约命令或调用 adapter。已经交给 Dispatcher 的 public stop 仍可能正在执行；R06 将验证它与模拟器终止之间的物理顺序。R02-04 验证三个已声明资源 URI 的稳定规范排序；R02-18 验证旧 ID 与变更后的参数和 binding 冲突；R02-26 验证已停止设备的新 MotionStop 以及重启后相同 ID 的重放。R02-30 在第一部件结果持久写入后、第二部件尝试前使用仅供测试的崩溃切口；重启保留第一部件确认结果，同一旧 ID 不重放。完整逐用例测试映射保存在 `logs/test/20261102-r02-health-final-v3/case-map.md`，供评审使用。

不得根据当前进度记录勾选 R02。任何性能主张都须先记录环境、固定负载、吞吐量、中位数与 p99。执行器停止先于 MuJoCo 终止/回收属于单独的 R06 关卡。仅设计阶段的检查命令仍见[夹具说明](../../../tests/fixtures/r02_grpc_gateway/README_cn.md)。

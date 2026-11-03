# EOS MuJoCo 适配器绑定

[English](../../../docs/eos/interfaces/mujoco.md) · [硬件接口](hardware_cn.md) · [MuJoCo 生命周期](../design/mujoco_cn.md) · [R06 停止](simulator-stop_cn.md)

EOS 拥有内部 `mujoco-v2` 适配器工厂及已准入硬件节点到模拟器设备的映射。外部 `island/mujoco-sim@0.1.1` 包拥有[分帧套接字契约](https://github.com/Island-Team/isos-mujoco-sim/blob/master/docs_cn/mujoco/interfaces/socket_cn.md)，其中定义线协议字段、单位、示例和错误。IsOS 拥有[包验证及图准入](https://github.com/Island-Team/isos/blob/master/docs_cn/isos/interfaces/node-graph_cn.md)。套接字协议 `2.0.0`、图版本、类型 ID 与包版本分别独立。

## 当前适配器设置

EOS 相机、底盘或机械臂实例选择 `adapter.type = "mujoco-v2"`。通用字段和限制见[硬件档案](hardware_cn.md)。图准入后适配器读取下列设置；未知键或与已验证后端不一致时拒绝准入。

| 字段 | 类型 / 单位 | 用途、默认值、校验和兼容性 |
| --- | --- | --- |
| `adapter.settings.socket_path` | 绝对字符串 / Unix 路径 | 必填，仅所有者可访问；须等于所选后端的 `socket` 并符合平台路径上限。无隐式默认值，保留现有键。 |
| `adapter.settings.simulator_node` | 非空字符串 / 设备 ID | 必填，来自包描述符，例如 `cam-0`、`base-0` 或 `arm-0`。它是现有套接字 `node` 值，**不是**规范图路径；保留现有语义。 |
| `adapter.settings.device_id` | 非空字符串 / 设备 ID | 托管图必填；须等于 `simulator_node` 及描述符中对应硬件种类的设备。保留现有语义。 |
| `adapter.settings.backend` | 规范节点路径 / — | 托管图必填；指向唯一声明的 MuJoCo 进程节点，且必须出现在 `references` 中。保留现有语义。 |
| `node.frame_id` | 非空字符串 / 坐标系 | 硬件观测必填；EOS 将其加入类型化数据，不由模拟器决定图坐标系。 |
| `node.joint_names` | 六个互异字符串 / 关节名 | 机械臂必填；确定观测及目标的顺序。其他硬件种类省略。 |

旧版内置 `eos.process.backend@v1.0` 在设置中保存 `package = "island/mujoco-sim"`、`version = "0.1.1"`、`scene` 和 `socket`。当前包贡献也提供 `eos.process.mujoco@v1.0`，其精确 `node.package` 锁定代替重复的包设置。M1/P02 已确认新 `0.1.0` 图身份、`@v0.1` 进程和通用硬件类型，以及场景/设备选择器；旧 `@v1.0` 图身份在显式迁移前保持安全的既有行为。在受管理交接中，IsOS 根据已验证的 P01 包收据解析可执行文件和场景。下方的 R05 EOS 严格客户端已通过独立实现审查和限定范围的跨所有者资格验证；受管理运动仍需等待 R06 的最终停止栅栏。

## R05 受管理严格客户端目标

**R05 EOS 实现已通过独立审查及限定范围的资格验证。** IsOS 拥有 P01 包收据、P02 场景/设备选择、已应用图修订，以及已验证可执行文件/场景的交接。由 IsOS 管理运行时，EOS 收到这些已验证路径并启动所选场景。本地 EOS 开发启动若显式提供 `--backend-*` 参数，则没有 IsOS 收据：所提供的 SHA 值只是标签，不能独立证明包来源。两种路径都由 EOS 直接持有子进程，因此 EOS 均推导 `--managed-strict-socket`，只传 `--v2-socket <backend.settings.socket>` 和 `--scene <所选路径>`，绝不传 `--socket` 或 `--benchmark-socket`；后两者会在 Dispatcher 之外提供更改同一引擎的第二条路线。图没有严格模式开关。EOS 之外不带标志而直接启动的独立模拟器保留旧 `2.0.0` 行为。[MuJoCo 所有者契约](https://github.com/Island-Team/isos-mujoco-sim/blob/master/docs_cn/mujoco/interfaces/socket_cn.md)定义标志与套接字特性；EOS 不重命名该线协议。

EOS 在启动子进程前验证不可变 profile：恰好一个受支持的后端包锁定，每个适配器的 `references` 与 `adapter.settings.backend` 指向同一后端，套接字路径相同且仅所有者可访问，`simulator_node == device_id`，以及节点硬件类别和所选场景对应的包支持设备 ID。精确的 `0.1.1` 目录有 `pick-place` 与 `navigation` 场景，相机/底盘/机械臂设备 ID 分别为 `cam-0`/`base-0`/`arm-0`。IsOS 在 Apply 及 Run/Restart 前验证描述符字节和场景支持；EOS 检查自己的图到设备绑定，然后从子进程取得实时确认。包锁定、场景、类别或绑定不匹配时，任何适配器都不能就绪。不增加第二份图清单或图作者可控的绕过开关。

Q01/R06 归档验证器在不修改任一记录的前提下比较由 SHA 绑定的所选发布清单和 IsOS 安装视图。它仅归一化两种契约允许的空字段省略：所选发布清单省略 `node_contributions.data_types` 时，IsOS 可补入 `[]`；所选资产把 `required_libraries` 或 `tested_ubuntu` 声明为空数组时，安装资产可省略该字段。非空 SONAME 和已测试 Ubuntu 数组必须按顺序和值完全一致。意外的非空数据类型、变更或缺失的库、变更或缺失的非空 Ubuntu 声明均使离线验证失败。语义比较前，安装收据的 `release_sha256` 仍须绑定安装发布清单原始字节。

启动就绪需要有界健康交换，连接 Unix 套接字成功不足以证明。对于图声明的每台 MuJoCo 设备，EOS 以该精确模拟器设备 ID 和新的传输请求 ID 发送 `health`，然后要求响应中的 `version:"2.0.0"`、回显 ID、`ok:true`、`accepted:true`，以及 `features` 包含精确字符串 `strict-socket-v1`。相机健康还须证明渲染器能捕获画面；即使 Unix 套接字已存在，GUI/EGL 或 JPEG 失败也使其不可用。未来未知的可选特性可忽略。证明缺失、畸形、为假或矛盾时，后端启动失败，EOS gRPC 健康和 `eos_ready` 保持不可用；直接子进程会终止并回收，故障事件包含运行及后端节点身份。健康交换使用一个单调时钟截止时间，覆盖非阻塞连接、分帧写入和所有部分读取。先绑定后监听造成的连接拒绝或交换超时会在后端总启动期限内重试；畸形或矛盾响应立即失败。之后每次适配器健康刷新也要求相同证明。套接字或子进程丢失时，受影响节点不可用，不能伪造观察。

EOS 传输将元数据限制为 64 KiB、负载限制为 8 MiB，检查帧长、JSON 类型、协议和回显请求 ID，要求请求/命令 ID 是最多 128 字节的非空 UTF-8 字符串。适配器对每个操作发送一个图绑定的设备 ID；模拟器检查精确设备和操作类别、有限的场景限制，并在整个子进程代际按 `(node,command_id)` 去重。EOS 仅将套接字 `accepted:true` 视为下发确认。只有从新观察确认底盘租期到期或机械臂到位，才记录 `Completed`；超时、断线或相互矛盾的证据产生持久 `OutcomeUnknown`。对于已下发但结果不确定的命令，EOS 可查询或核对状态，绝不自动重发，即使模拟器能对重发去重。同 ID 不同负载在到达套接字前就由 Dispatcher 拒绝。

Rust `BackendChild::wait_ready` 现在探测每台声明设备，`Endpoint::health` 每次刷新都要求严格证明。这两处是 R05 准入边界。`BackendReady` 和运行时对外就绪行须移到严格证明之后。已有仅连接成功的测试替身须响应健康帧，负面测试要证明旧模式或伪造特性不能使 EOS 就绪。R06 实现了 Dispatcher 停止栅栏、可抢占的配套停止 lane、底盘/机械臂独立确认及终止/回收顺序。只有两个套接字证明同一 generation 的 `strict-stop-v1` 后才公开受管理运动；单独的 `accepted:true` 不能确认物理安全。限定范围的跨所有者重放及未覆盖故障见[R06 停止设计](../design/runtime/simulator-stop_cn.md)。

## 请求与生命周期算法

适配器将一个图节点映射到一个已验证模拟器设备 ID，连接私有套接字完成有界请求，检查响应身份、大小和类型，再将相机 JPEG 或模拟器观测转换为 EOS 类型化硬件数据。它不执行导航或逆运动学。移动指令先由 EOS dispatcher 检查节点健康、限制、截止时间、指令身份和安全，再由适配器发送。套接字确认本身不能证明物理完成：EOS 通过新的观测确认底盘租约到期或机械臂位置，之后才记录 `Completed`。证据缺失或矛盾时为 `OutcomeUnknown`；EOS 不会自动重放结果不确定的已释放指令。

由 IsOS 管理启动时，EOS 将已验证模拟器作为直接子进程启动；本地显式开发启动则在同一严格模式规则下启动所选但无收据的程序。R05 要求严格健康证明之后，图声明的适配器和后端状态才能就绪。套接字断开或设备不可用会使受影响节点不可用。[R06 受管理停止契约](simulator-stop_cn.md)定义计划中的配套 lane、物理 epoch 栅栏、分别确认、终端凭据及子进程回收顺序。物理停止失败保持可见，不可宣称已确认安全。见[基础设计](../design/runtime/foundation_cn.md)与[实现待办](../maintenance/todo_cn.md)。

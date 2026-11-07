# EOS 运行时基础

[English](../../../../docs/eos/design/runtime/foundation.md) · [节点接口](../../interfaces/hardware_cn.md) · [进程设计](process_cn.md) · [调度器设计](dispatcher_cn.md)

本页说明当前 Rust 基础并保留带日期的验证记录。后面的设计章节同时记录已实现的图、就绪、路由和停止行为，以及尚待完成的实体适配器工作；契约由[硬件](../../interfaces/hardware_cn.md)、[节点贡献](../../interfaces/node-contributions_cn.md)、[控制](../../interfaces/control_cn.md)与 [gRPC](../../interfaces/grpc_cn.md) 页面负责。

## 运行时所有权

IsOS 编写的图是配置的唯一来源。新图使用 `0.1.0` 和已注册 `@v0.1` 类型；旧版 `1.0.0` 图仍可读取。注册的节点类型决定实例的 core、layer、group、类型化端口、设置、能力和生命周期。在启动子进程之前，IsOS 校验整张图；在创建适配器之前，EOS 校验所属 core 的投影。IsOS 管理 EOS、HTTP bridge 和配置服务进程，并解析已验证的仿真器软件包；EOS 直接管理仿真器子进程、内部节点、持久命令日志及每个执行器唯一的 dispatcher 命令路径。节点配置只配置节点；相机帧、观察、移动命令、健康和结果分别使用具体的 Rust 数据结构。

默认 EOS 构建及公开启动命令使用 Rust。当前准入 CXX mock 和基于分帧 Unix 套接字的 MuJoCo 适配器。MuJoCo 是首个端到端运行目标和 Aether v2 软件包候选。相机提供 RGB 并声明不提供深度；底盘支持受限的速度租约和相对移动；机械臂仅在节点类型与配置声明对应模式时接受有序关节位置或末端目标。C++ 硬件及 ROS 2 支线需要显式开启，默认不启动。不支持的物理适配器在启动前被拒绝。Rust 运行时现在通过有界代理加载已验证的宿主技能 worker；存储、任意模型权重执行、图内技能序列及 Workflow/ActionList 执行不属于 0.x。

## 边界与故障规则

- 节点类型定义模式及层级位置；节点实例定义身份和设置。类型化连线声明数据流；只有 dispatcher 可以放行执行器命令并持久记录结果。
- IsOS 启动 EOS；EOS 启动已验证的仿真器子进程并等待其 Unix 套接字就绪，然后报告自身就绪。仿真器故障时 EOS 退出，IsOS 可按有限重启策略重新启动 EOS 组件。
- EOS 在绑定控制套接字前取得命令日志的所有权。重启时仅当路径确实是 Unix 套接字且没有进程接受连接时清理旧路径。可能已执行但结果未知的命令不会自动重放。
- EOS 正常关闭时向所有已准入执行器发送停止。仿真器执行底盘速度租约。实际硬件部署仍须测量控制器停止时限。
- HTTP bridge 和只读配置服务仅绑定回环地址。bridge 不直接访问适配器，只转发有界请求。

## 已完成与后续

集成分支为 `refactor(v2)/runtime-main`，通过测试的 `refactor(v2)/…` 切片 squash merge。基础库、注册表、dispatcher/日志、进程管理器、服务、MuJoCo 适配器、图编辑器及默认 Rust 启动均已实现。IsOS、IsosUI 各自维护集成分支。旧 C++ 可执行程序及历史模块指南不在默认 Rust 启动路径中；仍须保留活跃技能 SDK、公开契约和部分 C++ 硬件调用者。删除组件前应遵循 [Q02 最后调用者契约](../../interfaces/cpp-last-caller_cn.md)。物理 ROS 2/厂商适配器与硬件停止时限属于后续切片，不在本 MVP 准入范围内。

## 验证记录

下文命令和图文件名是 **2026-10-29 使用早期 `isos v2` CLI 的历史验证记录**，不是当前首次使用命令。当前 `isos config validate`、`isos config apply` 和 `isos run` 请见 [EOS 使用指南](../../usage/README_cn.md)。

Linux x86_64、Intel i5-12490F 的 IsOS → EOS → MuJoCo 实时测试使用 `isos v2 apply <graph>`、`isos v2 run`、回环 HTTP 控制、`isos v2 status`、`isos v2 stop`。节点目录与发现、921,600 字节 RGB 帧及缺省可选深度、底盘与六关节观察、两个执行器命令、安全停止、未知节点拒绝、强制 EOS 崩溃后重启、重启后的拍摄和命令、子进程完全关闭，均通过。输出见 `logs/test/20261029T183000Z/isos-e2e.log`；历史只读图样例见 [MuJoCo/IsOS](../../../../examples/v2/mujoco-isos.toml)。格式、Clippy、测试、CMake 构建/安装命令及构建耗时见 [默认启动验证](../../develop/build-and-run_cn.md)；[服务指南](../../develop/services_cn.md)记录了调度吞吐、中位数及 p99，MuJoCo 仓库记录了固定套接字负载。

2026-11-01 的 EOS G05 读取器切片通过 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace`（117 项通过，原有 1 项忽略）、`cargo build --workspace` 和 `python3 scripts/check_doc_links.py --all`（检查 290 个文件、无失效相对链接）。`cargo test -p aether-runtime --test process setup_projection_starts_runtime_with_new_graph_version` 经由真实 `0.1.0` 运行时启动与关闭验证了 setup 投影。已检查三个可执行程序的 `--capabilities` 输出；独立实现审查未发现 EOS 遗留阻碍。完整本地输出在忽略的 `logs/test/20261101-g05-reader-features/`；IsOS 按修订生成快照的交接属于其独立切片。

## 0.x 运行时设计与后续工作

图写入器输出 `0.1.0` 和 `@v0.1` 节点类型。IsOS 校验完整节点图；EOS 的 setup 读取器投影并校验 `eos` core，核对精确的软件包 sidecar 与注册类型描述，然后才构造适配器。IsOS 与 EGIA 分别校验自己的 core，IsOS 校验跨 core 连线。对于 `0.1.0`，IsOS 通过启动参数向 EOS 运行时及配置服务传递按修订验证的 `eos/graph.json` 投影，而不是保存的整图路径。现有 `1.0.0` setup 读取保留整图校验以兼容旧图。EOS 不改写 IsOS 管理的文件。[连接调度器](scheduler_cn.md)说明实际路由的连线如何执行优先级与频率，同时不产生第二条命令路径。

启动是一个完整屏障：取得命令日志，加载并校验图与软件包 sidecar，解析适配器工厂，启动经过验证的 MuJoCo 子进程，等待其仅所有者可访问的套接字，构造相机/底盘/机械臂适配器，绑定控制与 gRPC 监听，最后报告就绪。就绪前失败会停止已创建适配器和子进程，并保留一条可处理的故障事件。EGIA 仅在 gRPC 健康、发现、资源、状态与能力服务通过现有类型校验后连接。Rust 网关把已发布消息映射到同一个准入注册表及 Dispatcher，保留 protobuf 字段号、JSON 名称和操作身份。

**该屏障的 R05 实现已通过独立审查及限定范围的资格验证：** 可连接的 MuJoCo 套接字不足以证明就绪。EOS 带 `--managed-strict-socket` 启动直接子进程，不启用历史 gRPC 或 benchmark 监听器，并探测每台声明设备的 `2.0.0` 健康响应，要求精确的 `strict-socket-v1`、回显请求 ID，以及正面的 `ok`/`accepted`。证明通过前，后端就绪事件、对外就绪行和 gRPC 健康保持不可用；失败时终止并回收未就绪子进程。见 [EOS MuJoCo 契约](../../interfaces/mujoco_cn.md)。EOS 之外独立启动的模拟器保留旧行为。

运行时拥有健康与类型化观察；Dispatcher 独自负责截止时间、边界、去重、持久日志、安全以及唯一的执行器路径。模拟器套接字或适配器 host 丢失时，受影响节点变为不可用。结果不确定的已释放命令绝不自动重试。R06 所有者路径现已在工作阻塞时执行关闭顺序：停止接收与排队工作，取得底盘和机械臂分别停止的确认或记录不确定结果，再终止并回收 MuJoCo，最后关闭监听与日志。只有两个 MuJoCo 套接字证明同一 generation 的 `strict-stop-v1` 后，受管理运动 primitive 才可用；仅有 R05 就绪不能证明后续物理安全停止。

R05 证明严格身份及同一子进程内的传输重发处理。R06 所有者实现提供停止准入栅栏、可抢占的有界模拟器通道、底盘/机械臂分别确认或持久未知，以及停止尝试之后才终止/回收。精确提交及最终负例跨所有者重放覆盖选定路径；完整 R06 资格验证仍需完成[停止设计](simulator-stop_cn.md)记录的实时故障。

内部适配器注册表包含 MuJoCo 工厂和套接字客户端。IsOS 仍安装并固定外部 `island/mujoco-sim@0.1.1` 程序与资源；EOS 接收已验证路径，不安装软件包也不编辑图。物理 ROS/厂商驱动属于后续狭窄适配器，必须测得停止行为后才可准入。见[实现路线图](../../maintenance/todo_cn.md)。

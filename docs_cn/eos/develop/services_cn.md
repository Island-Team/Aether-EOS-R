# Rust v2 服务主链

默认 EOS 构建与公开启动脚本使用这组精简的 v2 进程。`aether-runtime` 拥有 dispatcher 与 HAL 适配器。`aether-http-bridge` 通过仅所有者可访问的本地 Unix 套接字代理有界 JSON 控制请求，不直接访问适配器。`aether-tui` 基于同一套接字提供 REPL。`aether-config-server` 提供一个已验证的只读图快照；IsOS 通过重启其管理的进程应用新图。HTTP bridge 和配置服务只绑定回环地址。

| 进程 | 必需参数 | 端点或行为 |
| --- | --- | --- |
| `aether-profile` | `validate --file <graph>` | 在 IsOS 启动进程前验证 TOML 或 JSON 图；退出码 0 表示有效。 |
| `aether-runtime` | `--profile <graph> --journal-dir <dir> --socket <path>` | 有界 JSON 行控制套接字；拥有 HAL、dispatcher、持久化命令结果及关闭时的停止操作。 |
| `aether-http-bridge` | `--socket <path> --listen <loopback:port>` | `GET /api/eos/v2/health`、`POST /api/eos/v2/control`；POST 内容为含 `op` 的控制请求。 |
| `aether-config-server` | `--profile <graph> --listen <loopback:port>` | `GET /api/eos/v2/config` 返回已验证的源快照；`GET /api/eos/v2/config/validate` 返回版本、机器人 ID 和节点数；`GET /api/eos/v2/types` 返回已注册的节点类型目录。没有写入端点。 |
| `aether-tui` | `--socket <path>` | `time`、`discover`、`health`、`capture`、`observe`、`stop`、`result`、`cancel` 以及 JSON `dispatch` 命令。 |

`aether-control` 提供共享的有界控制客户端及简洁的回环 HTTP 读取器。请求上限为 64 KiB，响应上限为 16 MiB，套接字和 HTTP 读取超时为两秒。`capture` 使用 `rgb8_base64` 返回 RGB8，使用可选的 `depth_u16_le_base64` 返回深度；`observe` 返回类型化的底盘或机械臂数据。响应上限覆盖相机样本配置的 8 MiB 上限。简洁的依赖图避免 HAL 构建引入 Web 框架。上述进程不加载存储、技能或工作流模块。

运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings` 与 `cargo test --workspace`。仿真路径需要使用 `--v2-socket` 启动 `eos-mujoco-sim`，再使用 MuJoCo 图启动 `aether-runtime`，通过 dispatcher 和 HTTP bridge 检查发现、观察、移动和停止。完整输出保存至 `logs/test/<run-id>/`。

在配备 Intel i5-12490F 的 Linux 7.0 x86_64 环境中，2026-10-29 的 `logs/test/20261029T181500Z/` 记录了工作区测试、Clippy、实时 HAL 测试，以及通过 MuJoCo 执行的 HTTP 拍摄、观察、命令和停止检查，结果均通过。HTTP 返回 921,600 字节 RGB 帧和六个机械臂关节。固定 dispatcher 基准测试测得内存调度 448,801 次/秒（中位数 1.88 µs，p99 4.18 µs）；持久化调度为 126 次/秒（中位数 7.70 ms，p99 10.34 ms）。这些是本机基线，不是验收阈值。

2026-10-30 的完整进程测试使用包含 `ready_unix` 的 MuJoCo/IsOS 图，依次运行 `isos v2 apply`、`isos v2 run`、回环 HTTP 控制、`isos v2 status` 与 `isos v2 stop`。节点目录和发现、可选深度缺省的 RGB 拍摄、底盘及六关节观察、两个命令、安全停止和未知节点拒绝均通过。强制结束 EOS 后，IsOS 重启 EOS 及其 bridge/config 依赖进程；拍摄、底盘命令及停止再次通过，最终停止后子进程数量为零。输出见 `logs/test/20261029T183000Z/isos-e2e.log`。EOS 还单独测试了强制结束后遗留 Unix 套接字路径的恢复。

2026-10-30 的 EOS 链路专项检查先验证 IsOS MuJoCo 图模板，再用私有 Unix 套接字运行真实 `eos-mujoco-sim` 无界面进程、`aether-runtime` 和使用临时回环端口的 `aether-http-bridge`。HTTP 发现返回相机、底盘和机械臂节点；拍摄返回 921,600 字节 RGB8；两项观察、两项移动请求、两项停止及运行时正常关闭全部通过。仿真器确认接收移动请求，但 dispatcher 返回 `OutcomeUnknown`，因为传输确认不能证明物理动作已完成。相机初始化期间仿真器输出非致命的 OpenGL `0x502` 警告；RGB 图像通过解码与尺寸检查。完整输出保存在本地 `logs/test/20261030-eos-link/process-http-real.log`，本地运行脚本和程序路径保存在同一目录。HTTP bridge 还通过 `cargo test -p aether-http-bridge --test bridge` 运行了转发及 400、404、502 响应的 stub 集成测试。

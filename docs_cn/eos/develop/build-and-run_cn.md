# 构建和启动 Rust EOS v2 进程组

[English](../../../docs/eos/develop/build-and-run.md) · [服务结构](services_cn.md) · [节点图](../interfaces/hardware_cn.md)

Rust 负责默认 EOS 构建及公开的 `eosd`、`eos-north-http`、`eos-config`、`eos-cli` 启动脚本，分别对应 `aether-runtime`、`aether-http-bridge`、`aether-profile`、`aether-tui`。`aether-config-server` 作为独立的只读进程安装。IsOS 使用 `isos init` 创建配置，`isos serve` 运行 manager，`isos config apply <file>` 应用图，`isos run` 启动已应用部署。`isos deployment stop` 只停止图子进程；`isos stop` 关闭全部 IsOS 所属进程，包括 manager 和 UI。`eos` 脚本将部署生命周期命令转发给这些 IsOS 命令，通过 `enter` 打开 `eos-cli`。旧的 `island-launch` 和 `etc/runtime/stack.toml` 保留为未启动的侧链，不进入默认 v2 安装包。

虚拟 Cargo 工作区位于顶层 `crates/aether-*`，与 `../Island` 的 crate 布局一致。十个 crate 共用工作区的 `0.2.0` 版本。Rust 可执行文件采用 `aether-` 名称，EOS 启动脚本保留为产品入口。IsOS 使用所配置的 `aether_bin_dir` 或 `ISOS_AETHER_BIN_DIR` 查找 `aether-profile` 等 core 程序。源码目录中的 `bin/aether-profile` 会定位 release 程序。

在源码目录运行 `./build.sh`，或使用 CMake 的默认选项 `ISLAND_BUILD_EOS=ON` 和 `ISLAND_BUILD_EOS_CPP=OFF` 配置与构建。脚本将 release 程序放在 `target/release`；CMake 放在构建目录的 `cargo-target/release`，并将五个 Rust 程序安装到 `libexec/island`。源码启动脚本默认查找 `target/release`，也可通过 `CARGO_TARGET_DIR` 或 `ISLAND_BUILD_DIR` 指定位置。显式使用 `./build.sh --cpp-side-chain` 或 `-DISLAND_BUILD_EOS_CPP=ON` 可构建 C++ 运行时与硬件目标；C++ 进程目标不会覆盖安装的 Rust 进程名。默认安装在 `etc/island/eos/graphs` 中提供 mock 和 MuJoCo v2 节点图。

本地冒烟测试先建立私有目录，再运行 `bin/eosd --profile etc/eos/backends/mock-cxx.toml --journal-dir <private-dir>/journal --socket <private-dir>/eos.sock`。准入完成后进程输出 `1.0.0` 就绪 JSON。`bin/eos-north-http --socket <private-dir>/eos.sock --listen 127.0.0.1:16201` 提供有界控制 API；`bin/eos-cli --socket <private-dir>/eos.sock` 打开 REPL。`bin/eos-config validate --file <graph>` 在 IsOS 应用前校验共享图。`aether-config-server --profile <graph> --listen 127.0.0.1:16202` 提供已校验的只读快照与节点类型目录。路由与边界见[服务说明](services_cn.md)。

## Apply 前检查所选读取器

IsOS 从配置的 EOS 二进制目录选择精确的可执行文件。首次使用时可依次运行 `<eos-bin-dir>/aether-profile --capabilities`、`<eos-bin-dir>/aether-runtime --capabilities` 与 `<eos-bin-dir>/aether-config-server --capabilities`。每个探针都在不启动设备的情况下输出支持的图／类型版本及特性令牌。Profile 工具还响应 `printf '%s\n' '{"schema_version":1,"op":"identity","core":"eos"}' | <eos-bin-dir>/aether-profile setup --json`。[读取器契约](../interfaces/reader-features_cn.md)列出精确字段、内建 ID 与按条件要求的特性。`0.1.0` 图需要读取器显式声明 `graph-0.1-reader`；任何已验证的 MuJoCo v2 子进程（包括旧版 backend 图）还需要 runtime 的 `mujoco-child-v2`。启用的 HTTP bridge 需要 `vision-ws`。仅有产品版本不能证明兼容。HTTP bridge 报告传输特性，不报告图读取能力。IsOS 的 Validate 与 Apply 将比较实际图、已验证软件包 sidecar 与所选二进制文件；Run 与 Restart 在启动任何子进程前重查已选生成快照。如果预检拒绝不支持的类型或特性，先检查诊断指明的二进制文件和图指针，再使用精确且已验证的兼容发行版，或修改图后重新 Apply。旧 `1.0.0` 图只能通过其精确已知身份读取。

[MuJoCo IsOS 图样例](../../../examples/v2/mujoco-isos.toml)记录了采用 `@v1.0` 类型和 `scene = "default"` 的历史 `1.0.0` 部署；应将它保持为只读旧版兼容性测试样例。新 `0.1.1` 部署在 IsOS 中选择已验证的 `sim-hardware` 节点集及 `pick-place` 或 `navigation` 场景，再 Validate 和 Apply 生成的 `0.1.0` 图。已有图需要显式且按修订校验的迁移。已合并的 R05 路径启动已验证的仿真器子进程、连接其 Unix 套接字，并在报告就绪前逐一证明每台已声明设备的严格健康：每条响应都须包含协议 `2.0.0`、回显请求 ID、`ok:true`、`accepted:true`，且 `features` 包含 `strict-socket-v1`。之后 IsOS 才启动依赖的 bridge 和配置进程。模拟器 v2 套接字必须与图中每个 MuJoCo 适配器的 `socket_path` 一致。IsOS 先停止依赖进程，再停止 EOS；EOS 在终止并回收仿真器子进程前停止已准入的执行器。

v2 默认路径目前支持 CXX mock 与 MuJoCo 适配器。实体 ROS 2 和厂商控制器尚无等价适配器或实测硬件停止时限，因此这些节点会被拒绝准入。已验证的宿主技能 worker 可通过有界代理使用；任意模型权重、图内技能序列、Workflow/ActionList 及存储编排不属于此 0.x 目标。不要让 C++ 侧链与 Rust 分发层同时连接同一个实体执行器。旧 C++ 源码仍被可选侧链调用，删除时须遵循[最后调用者契约](../interfaces/cpp-last-caller_cn.md)。

## 验证

运行 `bash -n build.sh bin/aether-launch bin/eosd bin/eos-north-http bin/eos-config bin/eos-cli`、`./build.sh`、`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings` 和 `cargo test --workspace`。默认 CMake 配置、release 构建和临时前缀安装检查包内容。mock 冒烟测试须通过公开 `eosd` 脚本发现三个节点，并在 SIGTERM 后删除套接字。完整输出保存在 `logs/test/<run-id>/`；首次切换测试为 Linux x86_64 的 `logs/test/20261030-cutover/`。该机器上的 CMake release 构建用时 10.50 秒，源码 release 构建用时 12.80 秒；这些数字不代表硬件性能。

2026-10-31 的文档对齐检查了当前 IsOS CLI、EOS 脚本与 `aether-tui` 源码。运行 `python3 scripts/check_doc_links.py --all`、`git diff --check` 和 `bash -n bin/eos bin/eos-cli bin/eos-repl launch.sh` 可复现检查。220 个 Markdown 文件的相对链接均有效，diff 与脚本语法检查通过。完整本地输出保存在 `logs/test/20261031-docs-code-alignment/`。

2026-10-31 的 IsOS 生命周期脚本更新通过了 `bash -n bin/eos bin/eos-cli`、`python3 tests/eos/test_isos_lifecycle_wrapper.py`、`python3 scripts/check_doc_links.py --all`、`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace` 和 `git diff --check`。脚本测试使用替身程序检查每个生命周期命令、`enter`、帮助与未知命令。完整输出位于 `logs/test/20261031-isos-wrapper/`。

2026-10-31 的首次公开版本切换通过了 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace`、`cargo build --workspace`、`python3 scripts/check_doc_links.py --all` 与 `git diff --check`。冒烟检查查询了 EOS runtime、profile、HTTP bridge 和 config server 的 `--capabilities`，检查了 setup identity，并校验 demo 图：产品和图版本均为 `1.0.0`，setup schema 为 `1`。完整输出位于 `logs/test/20261031-first-public-version/`。

## Crate 布局验证（2026-10-30）

在 Linux x86_64、Intel i5-12490F 环境中，`./scripts/test_rust_workspace.sh --bench` 的格式、Clippy、全部工作区测试（含 CXX HAL）和固定调度基准测试均通过。`./build.sh` 构建了五个 release 程序。使用 `cmake -S . -B <build> -DCMAKE_INSTALL_PREFIX=<prefix> -DISLAND_INSTALL_ETCDIR=<prefix>/etc/island`、`cmake --build <build>`、`cmake --install <build>` 完成默认构建与安装。安装包含五个 `aether-*` 程序和 EOS 启动脚本；源码及安装的 `aether-profile`、安装的 `eos-config` 均验证了 demo 图。完整 IsOS → EOS → MuJoCo 图通过 `ISOS_AETHER_PROFILE_BIN=<source>/bin/aether-profile` 验证，再经 `isos v2 apply`、`run`、HTTP 控制和 `stop` 检查拍摄、观察、两种移动、安全停止、EOS/bridge 故障重启和无残留子进程。完整输出与环境信息见 `logs/test/20261030T002413Z-1121914/`。

固定基准测试结果：图准入 18,516 次/秒（中位数 51.74 µs，p99 85.01 µs）；内存调度 386,247 次/秒（中位数 2.13 µs，p99 4.54 µs）；持久化调度 126 次/秒（中位数 7,731.26 µs，p99 10,242.37 µs）；640×480 RGB8+深度样本传输 13,550 次/秒（中位数 59.85 µs，p99 304.87 µs）。这些仅是本机基线，不是验收阈值。

## 产品版本对齐（2026-10-31）

EOS 程序与工作区 crate 报告 `0.2.0`。图保持 `1.0.0`，支持的 MuJoCo 软件包为 `island/mujoco-sim@0.1.1`。格式、Clippy、工作区测试、程序构建和 222 个文档链接检查通过。复现命令：`cargo fmt --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace`、`cargo build --bins` 和 `python3 scripts/check_doc_links.py --all`。日志位于忽略 Git 的 `logs/test/20261031-version-alignment/`。

## Rust 格式基线（2026-11-01）

根目录 [`rustfmt.toml`](../../../rustfmt.toml) 采用相邻 Island 工作区的稳定格式选项，覆盖所有 Rust 工作区目标。一次性 `cargo fmt --all` 调整了现有 38 个 Rust 文件，不包含预期的行为变化。在 Linux x86_64、rustfmt 1.9.0-stable、rustc 1.98.1 环境中，`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace --all-targets`、`python3 scripts/check_doc_links.py --all` 与 `git diff --check` 均通过。测试运行通过 109 项；另有一项需要运行中模拟器的 MuJoCo 实时测试被忽略。完整输出位于忽略 Git 的 `logs/test/20261101-rust-format/`。

# Q02 最后调用方清单

[English](../../../docs/eos/maintenance/cpp-last-caller-inventory.md) · [契约](../interfaces/cpp-last-caller_cn.md) · [设计](../design/runtime/cpp-last-caller_cn.md)

基线：EOS 集成提交 `65475f56389d8fc6d0e21d15300fb3206dedbdc1`。这是实现前清单。删除每个候选项前，要重新检查反向依赖、运行指定等价测试并记录结果。“保留”表示仍有活动调用方或明确不属于 0.x，不代表未经审查的删除。

| 源或目标 | 已知最后调用方 | 等价或兼容门槛 | 建议处置 |
| --- | --- | --- | --- |
| `src/eos/bin/eos-2607.cpp`、`eos-config.cpp`、CMake 目标 `eosd`、`eos-config` | 旧 `eos_init`/`eos_init_config`，以及原生测试与历史服务调用者；不作为公开程序安装 | Rust 进程、profile 与已安装脚本冒烟通过，但旧 Workflow 和 JSON setup 契约尚无等价实现 | 保留两个原生入口；待准确契约等价或经审查弃用后再删除。保留公开 Rust 启动脚本。 |
| `src/eos/ext/north_http/`、`EOS_NORTH_HTTP` | 旧可选服务器实现已发布的 `/v1/eos/*` 和 `/v2/eos/*` REST/SSE/WebSocket 路由；`test_north_http_openapi.py` 读取源 | 路由级等价测试或明确的中英文 HTTP 契约弃用决定；OpenAPI 及安装/脚本审查 | 仍受支持时保留；Rust `/api/eos/v2/*` bridge 不构成等价。 |
| `src/eos/ext/repl/`、`EOS_REPL` | 旧可选 REPL 目标嵌在 `EOS_NORTH_HTTP` 下；公开 `eos-repl` 脚本选择 Rust TUI | 原生 REPL 测试及选项矩阵通过；Rust TUI 缺少已记录的 HTTP/SSE 命令 | 保留原生 REPL 与公开 Rust 脚本；将来删除须有命令等价或经审查弃用。 |
| `src/eos/runtime/{init,kernel,dispatch,northbound,actionlist,store,perception,models}`、`src/eos/ext/{decision,models}` | 旧 C++ 进程及集成测试 | 逐项 Rust Dispatcher/日志、runtime、gRPC、Vision 等价命令；Workflow/ActionList/模型引擎无 Rust 等价 | 保留有组件级契约或调用方的无等价组件。无契约实现细节仅可经审查例外删除。不得依据本行剪除整个子树。 |
| `src/eos/runtime/skills/{core,builtin,manager,native,external,worker}` | 旧进程图，但 `sdk_support.cpp` 和共享技能仍为活动 SDK | 逐项 Rust 主机 worker/SkillService 等价命令；安装 SDK/ABI 测试 | 先提取活动 SDK；保留无等价的组件级契约；按组件应用删除规则。 |
| `src/eos/runtime/hardware/`、`src/eos/ext/backends/{ros2,vendor}`、主机/vendor 设备 | 可选 C++ 实体支链及测试 | 0.x 没有实体 Rust 等价；缩减后的 C++ 构建/CTest | 保留。 |
| `src/eos/ext/simulator/`、`tests/eos/simulator_fixture.cpp` | C++ hardware/body/dispatch 测试使用 `eos_add_simulator_test` | 剩余 C++ 硬件测试图 | 使用期间保留；不能推断 Rust MuJoCo 等价。 |
| `src/eos/lib/`、`src/eos/api/` | C++ 共享库/原生 protobuf 图 | 缩减的 C++ 链接图；Python schema/清单生成 | 只剪除不可达原生部分；保留 protobuf/schema 源文件。 |
| `src/eos/include/eos/skill/*`、`src/eos/cmake/EOSSkill*`、`sdk_support.cpp`、`eos_skill_sdk` | 已安装的 `EOS::SkillABI`、`EOS::SkillCpp`、`EOS::SkillSDK`；Rust worker ABI | SDK 安装/样例、导出符号、ABI 布局及反例 | 保留并从旧进程组合中隔离。 |
| `crates/aether-hal/cxx/*`、`aether_adapter_v1` 头文件/主机 | 默认 Rust 模拟适配器及已验证加载器 | HAL CXX 和适配器主机失败/停止测试 | 保留。 |
| `share/eos/contracts/eos/*`、`share/eos/schemas/eos/*`、OpenAPI | Rust tonic、EGIA 类型化导入及兼容样例 | 冻结线协议样例、schema/清单/OpenAPI 生成、EGIA 导入 | 保留，安装不依赖旧 C++ 运行时。 |
| `bin/eosd`、`bin/eos-config`、`bin/eos-north-http`、`bin/eos-cli`、`bin/eos-repl`、`launch.sh` | 公开 Rust 启动命令 | 全新安装前缀的启动脚本冒烟 | 保留；`bin/island-launch` 仍有源码/测试，须单独记录调用方和处置。 |
| `tests/eos/CMakeLists.txt`、C++ 单元/集成测试 | 旧 C++ 目标与活动 SDK、schema、文档和脚本检查混合 | 拆分保留的注册，再运行缩减支链 CTest | 只随删除的目标删除专属测试。 |
| 根 `CMakeLists.txt` 安装列表 | 默认 Rust 安装列出不存在的 `*-v2.md` 页面 | 全新前缀配置/构建/安装及文件清单 | 替换成存在的中英文页面，保留公开数据资源。 |
| `etc/eos/backends/mock-cxx.toml`、`mujoco-socket.toml`、`scripts/test_rust_workspace.sh` | Rust `include_str!`、运行时测试、文档、CMake 和脚本 | 全引用扫描、样例字节、格式/静态检查/测试/安装 | 已在纯机械提交中按主题改名，图字节未变。 |
| `examples/v2/mujoco-isos.toml` | Rust 与 strict-socket 测试中的只读旧图样例 | 精确 `1.0.0` 内容/字节比对和迁移测试 | 作为兼容样例保留；可选主题命名别名。 |

此表是候选图，不批准按组删除。删除任何源文件或目录前，须增加逐组件记录：准确目标和源路径、编译/安装/运行时/测试/文档调用方、Rust 等价命令及结果，或没有调用方与组件级契约且独立审查批准的例外、处置结果，以及保留的资源或 ABI 依赖。整个目录内每个成员都有该记录后才可删除目录。须把 `EOS_NORTH_HTTP`/`EOS_REPL` 选项组合与 `bin/island-launch` 分别记录。使用 `rg` 和已配置的 CMake 目标图找出分支上新出现的调用方。清单与测试不一致时，以归属[契约](../interfaces/cpp-last-caller_cn.md)控制删除。

Q02 关闭前还须修正配对的中英文 `develop/README` 活动 proto/schema 归属、`develop/build-and-run` R05 严格就绪状态，以及 `design/runtime/foundation` 主机技能或带日期的 MVP 表述。检查中英文 `interfaces/external/http` 的已发布路由状态，并重新验证全部文档链接。
## 入口程序的最后调用者记录（2026-11-03）

| 组件 | 编译、安装、运行、测试与文档调用者 | Rust 等价证据 | 当前处置 |
| --- | --- | --- | --- |
| `src/eos/bin/eos-2607.cpp` 与 CMake `eosd` | `src/eos/bin/CMakeLists.txt` 在 `ISLAND_BUILD_EOS_CPP` 下编译；它是唯一调用 `eos::runtime::init::Run` 的原生程序，后者注册历史 WorkflowService。公开安装的 `bin/eosd` 是另一条 Rust 启动脚本。旧 `runtime/init` 测试及已发布 north HTTP Workflow 路由仍有调用。 | `cargo test -p aether-runtime --test process -- --test-threads=1`：20 项通过，见 `logs/test/20261103-q02-last-caller/rust-process-parity.log`。原生 `eos_init_tests`、`eos_northbound_tests` 及 HTTP 路由测试也通过。Rust 进程等价不包括 WorkflowService。 | **保留。** 已发布历史服务尚无 Rust 等价或获批准的弃用；`logs/test/20261103-q02-entrypoints/` 中的独立入口审查不批准删除。 |
| `src/eos/bin/eos-config.cpp` 与 CMake `eos-config` | `src/eos/bin/CMakeLists.txt` 链接 `eos_init_config`；`test_eos_config_interface.py` 直接测试其 JSON `describe/defaults/profiles/render/validate --json` 契约，包括 `eos.native` 与 `eos.uds`。安装的 `bin/eos-config` 是另一条 Rust 启动脚本。 | `cargo test -p aether-runtime --bin aether-profile -- --test-threads=1`：9 项通过，见 `logs/test/20261103-q02-last-caller/rust-profile-parity.log`。旧 JSON CLI 字段未映射到 Rust setup 协议，原生接口测试通过。 | **保留。** 直接测试调用者仍存在，且准确 setup 等价尚未证实。 |
| `src/eos/ext/north_http/` | `ISLAND_BUILD_EOS_CPP` 与 `EOS_NORTH_HTTP` 同时开启时编译；发布独立的 `/v1/eos/*`、`/v2/eos/*` REST、SSE 与 WebSocket 路由。 | Rust `/api/eos/v2/*` bridge 使用不同路由。 | 旧公开路由仍受支持时保留。 |
| `src/eos/ext/repl/` | 位于 `EOS_NORTH_HTTP` 之下；`EOS_REPL` 加入带 HTTP/SSE 命令的原生 REPL，且有活动 `eos_repl_tests`。公开 `bin/eos-repl` 独立启动 Rust TUI。 | Rust TUI 有控制套接字命令，但缺少原生客户端文档中的 workflow、skill、package、history 和 watch 命令。原生 REPL 测试及安装脚本冒烟通过；四种选项组合记录于 Q02 证据。 | **保留。** 所有原生 REPL 源仍有调用方，尚无命令级等价或获批准的弃用。 |
| `bin/island-launch` | 源码兼容启动器仍被 `scripts/eos_build_config.py`、`test_island_launch.py`、模拟器选择测试及调试指南调用；默认安装不包含它。 | 不作替代声明。 | 保留。 |

独立的精确调用者审计保存在本地 `/tmp/q02-exact-caller-audit.md`；为避免把 `/tmp` 当成发布文档，上表重述了源码路径和命令。Rust 进程测试通过并不会移除旧 Workflow/ActionList 服务契约，也不会使历史 C++ HTTP 路由自动过时。

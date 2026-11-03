# Q02 最后调用方验证证据

[English](../../../docs/eos/maintenance/q02-evidence.md) · [归属契约](../interfaces/cpp-last-caller_cn.md) · [清单](cpp-last-caller-inventory_cn.md) · [设计](../design/runtime/cpp-last-caller_cn.md)

Q02 从 Q01 集成提交 `65475f56389d8fc6d0e21d15300fb3206dedbdc1` 开始。本页记录 EOS 的最后调用方切片；跨仓库发布验证仍属于 Q03。环境为 Linux x86_64、Rust 1.98.1；可选原生构建使用 CMake、C++17、仓库配置的依赖及安装了 `protobuf==3.20.3` 的 `/tmp/eos-q02-python/bin/python`。完整本地输出位于 `logs/test/20261103-q02-install-sdk/`、`20261103-q02-last-caller/`、`20261103-q02-ctest-repair/` 与 `20261103-q02-entrypoints/`。

## 处置

默认安装现在无需启用 `ISLAND_BUILD_EOS_CPP` 即可在同级 `docs/` 与 `docs_cn/` 目录发布现有中英文指南、EOS 契约与 schema。旧进程关闭时仍可单独构建和安装原生技能 SDK，其目标名称、ABI 与示例保留。若没有系统 nlohmann JSON 软件包，安装的 SDK 包含获取的头文件；外部使用者测试仅使用该安装前缀。仍在使用的 `v2` 后端 profile 和工作区测试脚本改用主题名称，graph 字节不变。公开启动脚本仍选择五个 Rust 可执行文件。

准确的反向调用审查**不批准**删除原生 `eosd`、原生 `eos-config`、north HTTP、原生 REPL、实体适配器或整块 C++ 运行时目录。原生 `eosd` 是组合历史 WorkflowService 的唯一可执行程序；已发布的旧 HTTP workflow 路由仍调用它，Rust 尚无同路径等价。原生 `eos-config` 的 JSON setup 契约有直接测试，与 Rust profile 脚本不同。原生 REPL 文档中的 HTTP/SSE 命令超出 Rust TUI。实体 ROS/vendor 的等价验证不在本次 0.x 范围内。逐组件处置与保留的 ABI/schema 资源见清单。不能仅因默认构建选项关闭就删除源码。

## 可复现检查

| 边界 | 命令与结果 | 完整本地输出 |
| --- | --- | --- |
| Rust 格式与工作区 | 将 `/home/ashex/.cargo/bin` 加入 `PATH` 后运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace --all-targets -- --test-threads=1`，通过 | `20261103-q02-last-caller/fmt-final.log`、`clippy-final.log`、`workspace-test-final.log` |
| 进程与 profile 等价 | `cargo test -p aether-runtime --test process -- --test-threads=1`：20/20；`cargo test -p aether-runtime --bin aether-profile -- --test-threads=1`：9/9 | `20261103-q02-last-caller/rust-process-parity.log`、`rust-profile-parity.log` |
| 保留的 Rust 边界 | 上述串行工作区命令覆盖 HAL CXX 模拟适配器/adapter host 失败与停止、Dispatcher/日志、gRPC、浏览器 bridge 及运行时进程测试；结果保存在同一完整记录 | `20261103-q02-last-caller/workspace-test-final.log` |
| 选定 EGIA 类型化 schema 导入 | 使用 Q01 选定 EGIA checkout `d3868d2763c35d9f6bfed80d75edb7a1844ef54e`，其 venv Python 从 `src:libexec` 导入 `egia.core.link.client.EosClient` 与 `island.generated.eos.gateway_pb2[_grpc]`，通过 | `20261103-q02-last-caller/egia-typed-import.log` |
| 默认 Rust 安装 | `cmake -S . -B /tmp/eos-q02-default-rust -DISLAND_INSTALL_ETCDIR=/tmp/eos-q02-default-prefix/etc/island`；构建、安装并运行 CTest；最终含中英文安装指南链接检查 2/2 | `20261103-q02-last-caller/default-cmake-build.log`、`default-cmake-install-2.log`、`review-fix-default-configure.log`、`review-fix-default-ctest.log` |
| 独立原生 SDK | 使用 `ISLAND_BUILD_EOS=OFF`、`ISLAND_BUILD_EOS_CPP=OFF`、`ISLAND_BUILD_EOS_SKILL_SDK=ON`、`ISLAND_BUILD_TESTS=ON` 配置、构建及 CTest；含不额外暂存依赖的 SDK 使用者测试 3/3 | `20261103-q02-install-sdk/review-fix-configure.log`、`review-fix-build.log`、`review-fix-ctest.log` |
| 无 HTTP 的可选 C++ | 使用 `ISLAND_BUILD_EOS=OFF`、`ISLAND_BUILD_EOS_CPP=ON`、`EOS_NORTH_HTTP=OFF`、`EOS_REPL=OFF` 配置、构建及 CTest；修正过时测试 fixture/预期后 64/64 | `20261103-q02-ctest-repair/cpp-ctest-final-2.log` |
| 含 HTTP 和 REPL 的可选 C++ | 使用 `EOS_NORTH_HTTP=ON`、`EOS_REPL=ON` 在安装包审查修复后配置并完整构建及 CTest；含 REST/SSE/WebSocket、原生 REPL、已安装 OpenAPI/脚本检查，119/119 | `20261103-q02-last-caller/review-fix-full-configure.log`、`review-fix-full-build.log`、`review-fix-full-ctest.log` |
| 有 HTTP、无 REPL | 以 `EOS_NORTH_HTTP=ON`、`EOS_REPL=OFF` 重新配置；无 `eos_repl` 目标；路由 CTest 选择 `-R '^(NorthHttp|AuditTest|generated_openapi_check|openapi_contracts)'`，54/54 | `20261103-q02-last-caller/http-on-repl-off-configure.log`、`http-on-repl-off-targets.log`、`http-on-repl-off-ctest.log` |
| 要求 REPL、无 HTTP | 以 `EOS_NORTH_HTTP=OFF`、`EOS_REPL=ON` 重新配置；无原生 REPL 目标，也不获取 replxx | `20261103-q02-last-caller/cpp-repl-on-http-off-configure.log`、`cpp-repl-on-http-off-targets.log` |
| 契约与文档 | `python3 scripts/check_doc_links.py --all` 检查 344 个文件、零断链；旧 HTTP OpenAPI 与 Rust EOS 契约分别保留 | `20261103-q02-last-caller/doc-links-final-2.log`、`http-route-ctest.log` |

首次默认安装使用系统 `/etc/island`，因权限失败；改用临时安装目录后通过。最初原生 CTest 发现九项过时 fixture/版本预期；仅修复测试、不改 C++ 产品行为后，最终 64/64 通过。首次 HTTP+REPL 安装冒烟检查的 OpenAPI 嵌套路径预期错误；修正后 119/119 通过。这些失败尝试仍保存在证据中。

类型化导入命令为 `PYTHONPATH=/tmp/egia-q01-a03-d386/src:/tmp/egia-q01-a03-d386/libexec /home/ashex/projects/Island-egia/.venv/bin/python -c 'from egia.core.link.client import EosClient; from island.generated.eos import gateway_pb2, gateway_pb2_grpc; print("EGIA_TYPED_EOS_IMPORT_OK", gateway_pb2.DESCRIPTOR.name)'`。记录的输出为 `EGIA_TYPED_EOS_IMPORT_OK eos/gateway.proto`。

主题文件名改动前后的 graph 哈希清单相同。历史 `examples/v2/mujoco-isos.toml` 作为 `1.0.0` 读取 fixture 保持逐字节不变。可选原生入口审查的针对性构建与 CTest 3/3 通过；它不批准两个候选入口的删除。原生 REPL 审查也发现活动命令及测试调用者。Q02 关闭前须由独立实现审查接受这些保留决定。

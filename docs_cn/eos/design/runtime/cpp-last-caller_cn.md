# Q02 C++ 最后调用方设计

[English](../../../../docs/eos/design/runtime/cpp-last-caller.md) · [归属契约](../../interfaces/cpp-last-caller_cn.md) · [清单](../../maintenance/cpp-last-caller-inventory_cn.md) · [验证证据](../../maintenance/q02-evidence_cn.md)

Q01 已合并到 EOS 集成提交 `65475f56389d8fc6d0e21d15300fb3206dedbdc1`。Q02 从该提交建立独立主题分支。[跨仓库 TODO](../../../../docs/island/implementation-todo.md)规定 Q02 在 Q01 后由 EOS 负责；后续完整发布门槛属于 Q03。

删除顺序从公开目标沿 CMake 依赖确定。先修正默认安装中不存在的 `*-v2.md` 页面输入，并使契约与 schema 安装不依赖旧 C++ 进程开关。把当前技能 SDK 目标、头文件、软件包文件、共享技能示例及 SDK 测试提取到窄构建路径。再逐项检查旧的非安装进程入口（`adapters/eos-cxx/bin`）的 Rust 等价证据。已发布的 `/v1/eos/*` 与 `/v2/eos/*` REST、SSE 和 WebSocket 路由只要仍受支持，就保留可选的 C++ north HTTP 服务器；Rust `/api/eos/v2/*` 浏览器 bridge 是另一契约。删除旧服务器须有等价路由实现及测试，或经过另行审查的弃用决定，并同步更新中英文 HTTP 契约、启动及安装预期、OpenAPI 状态和测试。须明确检查 `EOS_NORTH_HTTP` 与 `EOS_REPL` CMake 选项的交互。实体 ROS/vendor 支链仍须可构建；Rust MuJoCo 子进程不能证明实体驱动等价。

旧的 `adapters/eos-cxx/runtime/skills/native/sdk_support.cpp` 虽处在也包含旧运行时代码的目录内，却仍是活动 SDK 源。提取时须保留准确的 SDK CMake 目标名称、`.so.2` SONAME、软件包路径和导出符号。旧的 `tests/eos/CMakeLists.txt` 将 C++ 集成测试与活动的契约、文档、启动脚本和 SDK 检查混合。删除旧进程库前必须拆分这两处。已发布的 `share/eos/contracts/eos/`、`share/eos/schemas/eos/` 和 OpenAPI 数据是产品资源，不依赖本机 C++ protobuf 生成。历史 Workflow/ActionList、模型和技能引擎没有 Rust 等价实现；若仍有组件级契约或活动调用方，就须保留。没有调用方和组件级契约的实现细节，只有清单明确证明且独立审查批准例外后才可删除。不能根据笼统的类别行删除整个目录。

在另一个纯机械提交中重命名仍在使用的 `v2` 示例及脚本文件名，之前须列全 `include_str!`、运行时路径、CMake 安装列表、fixture 和中英文链接。保持历史 `1.0.0` MuJoCo 示例的字节不变。不可重命名 `v2` 线协议路径、protobuf 身份、ABI 名称、套接字协议、生成快照布局或历史发布标签。

每次删除前，在清单中逐个目标及源文件写明最后一个编译、安装、运行时、测试和文档调用方、准确的 Rust 等价命令及结果或经审查的无契约例外，以及最终处置。整个目录内每个组件有记录前，不得删除该目录。另行处理仍有活动调用方的 `bin/island-launch`。测试默认 Rust 构建与临时前缀安装、已安装启动脚本、技能 SDK 软件包及符号表、schema/清单/OpenAPI 检查、EGIA 类型化导入、HAL CXX 模拟适配器、适配器主机失败与停止、Dispatcher/日志、gRPC、浏览器 bridge 和进程路径。若保留 north HTTP，须直接测试已发布的 REST/SSE/WebSocket 路由；仅检查 OpenAPI 资源不能证明服务器行为。配置、构建并用 CTest 测试缩减后的可选 C++ 支链。修正 `docs/eos/develop/README.md` 及中文页对活动 proto/schema 的归属说明、`docs/eos/develop/build-and-run.md` 及中文页对已合并 R05 严格就绪的说明，以及 `docs/eos/design/runtime/foundation.md` 及中文页对当前主机技能加载或带日期 MVP 历史的说明。检查中英文 HTTP 路由术语和全部链接。完整输出保存于 `logs/test/<run-id>/`；独立实现审查及门槛通过后，才将 Q02 压缩合并到 `refactor(v2)/runtime-main`。

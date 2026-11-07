# EOS 0.x C++ 最后调用方契约

[English](../../../docs/eos/interfaces/cpp-last-caller.md) · [接口目录](README_cn.md) · [Q02 设计](../design/runtime/cpp-last-caller_cn.md) · [最后调用方清单](../maintenance/cpp-last-caller-inventory_cn.md)

Q02 仅在确认旧 `adapters/eos-cxx/` 组件最后一个编译、安装、运行时、测试和文档调用方、Rust 等价测试通过、且缩减后的可选 C++ 支链仍可构建时，才删除该组件。WorkflowService、ActionList 或模型权重执行等明确排除的引擎不能宣称已有 Rust 等价实现；只要它仍有组件级契约或活动调用方，就必须保留。没有调用方且没有组件级契约的实现细节，只有在清单证明这些条件并经独立审查明确批准例外后，才可删除。仍有调用方的实体驱动必须保留。整个目录中的每个目标、源文件、测试和安装引用分别记录处置前，不得删除整个目录。

EOS 默认软件包通过公开的 `eosd`、`eos-config`、`eos-north-http`、`eos-cli` 和 `eos-repl` 启动脚本安装五个 Rust 可执行程序。当前 `crates/aether-hal/cxx` 模拟适配器和 `aether_adapter_v1` C ABI 仍属于 Rust 路径。C/C++ 主机技能 ABI 头文件、`eos_skill_sdk`、`EOS::SkillABI`、`EOS::SkillCpp`、`EOS::SkillSDK`、CMake 软件包文件及导出符号仍向技能作者和 Rust 已验证 worker 提供。旧 C++ 硬件与 ROS/vendor 支链继续可选，直到有实体硬件的等价验证。

若 SDK 构建获取了 nlohmann JSON，安装包须包含其头文件，并为同一安装前缀下的使用者定义导入依赖目标。若使用系统软件包，`EOSSkillConfig.cmake` 则解析该系统依赖。已安装 SDK 的使用者测试不能额外暂存获取的依赖。安装后的 `docs/` 和 `docs_cn/` 保持同级结构，使已编写的中英文相对链接可解析。

EOS protobuf 源、生成的 schema、契约清单和 HTTP OpenAPI 描述继续提供给 Rust 与 EGIA。旧 north HTTP 服务器已发布的 `/v1/eos/*` 与 `/v2/eos/*` REST、SSE 和 WebSocket 路由是独立的可选接口：只要仍受支持，就保留其实现，除非等价路由实现及测试通过，或有明确的契约弃用决定，并同步更新中英文 HTTP 契约、安装及启动脚本预期、OpenAPI 状态和测试。Rust 浏览器 bridge 的 `/api/eos/v2/*` 路由不能证明这种等价。Q02 不修改 protobuf 包、服务、方法、字段号、ProtoJSON 名称、套接字协议身份、技能 ABI 布局或符号。源文件名称仅在所有代码、测试、脚本、安装和中英文文档引用同步更新后才能更改。历史 `examples/v2/mujoco-isos.toml` 必须保留字节完全相同的只读 `1.0.0` 兼容性样例，即使新增按主题命名的别名。

每次删除都要在[清单](../maintenance/cpp-last-caller-inventory_cn.md)中逐项记录目标、最后调用方、Rust 等价证据或经审查的无契约例外、必需命令及结果和处置。缺少等价证据、仍有活动调用方或未经审查的例外均阻止删除。全新临时前缀安装必须包含真实存在且配对的中英文指南及必需契约资源。Q02 关闭前须修正 `docs/eos/develop/README.md`、`docs/eos/develop/build-and-run.md`、`docs/eos/design/runtime/foundation.md` 及对应中文页中过时的归属与就绪描述；给历史说法标注日期，检查路由术语并验证链接。必需资源缺失或变化时，安装、SDK、schema、ABI 和 EGIA 导入门槛必须失败。

# IsOS（Island OS）

[English](README.md) · [文档](docs_cn/README_cn.md)

IsOS 是 Island OS 管理器，也是部署图的唯一核心。**EI（Embodied Intelligence）** 负责认知和安全检查；**EOS（Embodied OS）** 负责具身执行、硬件适配器和 Dispatcher；**IsosUI** 是浏览器工作台。它们共用一个 `isos` 图核心，EI 和 EOS 仍运行在独立进程中，并通过显式传输通信。MuJoCo 仍是仓库外部的已验证软件包 `island/mujoco-sim@0.1.1`。

统一源码版本为 **0.2.1**。新图格式为 `0.2.0`，类型标识为 `isos.<layer>.<name>@v0.2`。带修订检查的迁移与 Apply 已启用；最终公开资格验证仍在进行。**尚未创建 v0.2.1 产品标签或发布。**参见[统一设计](docs_cn/island/unification-v0.2.1_cn.md)和[发布草稿](docs_cn/island/release-v0.2.1-draft_cn.md)。

## 仓库结构

| 路径 | 所属部分 |
| --- | --- |
| `crates/isos/` | 管理器、图迁移、软件包、进程监护和命令行。 |
| `crates/aether-*/`、`adapters/eos-cxx/`、`share/eos/` | EOS Rust 运行时、可选的 C++ 支线和契约。 |
| `ei/` | EI Python 项目，以及 EGIA 兼容命令和导入。 |
| `ui/` | 浏览器工作台及其独立测试夹具。 |
| `docs/{isos,ei,eos,isosui}/`、`docs_cn/{isos,ei,eos,isosui}/` | 中英文所属文档。 |

根 Cargo 工作区构建管理器和 EOS crate。根 CMake 组合 Rust 程序、EI 分发包、可选 UI 构建和显式启用的 EOS 原生支线。使用说明参见 [IsOS](docs_cn/isos/usage/README_cn.md)、[EI](docs_cn/ei/README_cn.md)、[EOS](docs_cn/eos/usage/README_cn.md)和 [IsosUI](docs_cn/isosui/README_cn.md)。[源码来源记录](docs/island/source-provenance-v0.2.1.md)列出导入历史和测试夹具归属。

## 开发

遵循[仓库约定](docs_cn/island/conventions.v1.3.1_cn.md)，在 `refactor(v2)/<topic>` 分支工作，并将完整检查输出保存到忽略的 `logs/test/<run-id>/`。集成 PR 前运行相关 Rust 格式、Clippy 与测试，EI Ruff、Pyright、导入规则与测试，UI 类型检查、测试和两种构建，CMake/CTest、夹具检查以及中英文链接检查。发布前还需在全新环境完成 MuJoCo 和安全 Stop 回放。历史公开 EOS 0.2.0 发布记录见 [EOS 发布证据](docs_cn/eos/maintenance/release_cn.md)。

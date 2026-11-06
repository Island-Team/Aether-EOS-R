# EGI and EGI Agent terminology（中文）

> 本文是 [EGI and EGI Agent terminology](egi.md) 的中文伴随文档。代码、命令、路径、接口名称和字段名保持与英文原文一致，以便直接用于开发与运维。

## 中文说明

本页覆盖 **EGI and EGI Agent terminology** 的当前实现、边界和使用约定。请结合英文原文阅读完整的命令示例、配置键、协议字段与代码片段；任何与实现不一致时，以仓库中的代码和规范契约为准。

## 原文

以下内容保留原文，便于逐段对照：

<details>
<summary>展开英文原文</summary>

# EGI and EGI Agent terminology

Island-egia uses two related names with deliberately different meanings.

## EGI

**EGI** expands to **Embodied General Intelligence**. It names the research direction associated with this runtime; the implementation status and future work are maintained in [todo.md](todo.md).

## EGI Agent

**EGI Agent** is the software runtime implemented by this repository. It hosts configured models and coordinates perception, context, planning, memory, missions, safety-gated primitives, and recovery.

**EGIA** is the compact acronym for **EGI Agent** and supplies the `egia` suffix in the repository name **Island-egia**. Use **EGI Agent** in prose because it is immediately understandable; use **EGIA** where a short identifier is useful. EGIA names the runtime.

The relationship is:

```text
configured model → EGI Agent → embodied runtime/backend → action
```

The model supplies learned intelligence. The EGI Agent supplies the agent loop and cognitive orchestration. The embodied runtime/backend owns device integration, execution, and final physical safety.

## Technical names

The repository and its technical interfaces use **EGIA** consistently:

- `egia` Python packages and executable
- `EGIA_*` environment variables
- `etc/egia`, `docs/egia`, and related installed paths
- `egia_ext` for bundled out-of-process extensions

This is a breaking rename from the former identifiers; deployments and downstream integrations must update imports, commands, configuration paths, and environment variables together. In prose, use **EGI Agent** for the runtime and expand **EGI** as **Embodied General Intelligence** on first mention.

</details>

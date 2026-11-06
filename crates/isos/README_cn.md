# IsOS

IsOS 是管理器、EI 认知、EOS 执行和 IsosUI 工作台共用的唯一部署图核心。它保存图、验证精确的软件包版本、提供本地工作台 API，并监管直接启动的子进程。软件包通过 `node_contributions` 声明注册节点类型、资源和进程。IsOS 验证声明与负载字节；EI 和 EOS 验证各自的进程投影和生命周期语义。读者优先的文档见 [IsOS 指南](../../docs_cn/isos/README_cn.md)。

## 首次使用

```sh
isos init --mode dev
isos status --json
isos logs all --current
```

安装或链接发布 `isos.package@v1` 的软件包，在图中选择其注册节点类型和声明的资源 ID，然后 Validate、依据当前修订 Apply，再 Run。[软件包指南](../../docs_cn/isos/usage/packages_cn.md)和[运行指南](../../docs_cn/isos/usage/run_cn.md)给出流程。新图格式为 `0.2.0`，只有 `isos` 核心，包含 `eos_*`、`ei_*` 和 `ui` 层。支持预览旧图迁移；完整的所属进程与启动检查通过前，迁移 Apply 仍拒绝写入。所选归属方或适配器不可用时，IsOS 拒绝准入，且不会替换活动运行。

`isos serve` 提供本地工作台；`isos open` 启动或打开它。在 [config/isos.toml](config/isos.toml) 中配置进程程序和已验证的 UI 包。`ISOS_HOME` 为测试选择隔离的 `config/`、`data/` 和 `run/` 目录。API 路径版本仍为 `/api/isos/v1/*`；迁移预览使用独立版本的 `schema_version: "0.2.0"`。

其他细节见[现行接口](../../docs_cn/isos/interfaces/README_cn.md)、[类型目录](../../docs_cn/isos/layers/README_cn.md)、[已实现设计](../../docs_cn/isos/design/README_cn.md)和[验证](../../docs_cn/isos/maintenance/verification_cn.md)。[English](README.md)

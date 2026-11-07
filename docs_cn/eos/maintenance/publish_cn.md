# 发布 EOS 源码

[English](../../../docs/eos/maintenance/publish.md) · [发布检查](release_cn.md)

在私有 `Island-eos` 仓库开发和审查修改。公开的 `Aether-EOS-R` 仓库拥有独立历史：从其当前目标分支建立新的公开分支，只复制准备发布且已审查的源码快照。不要将私有提交或标签推送、合并、cherry-pick 或镜像到公开仓库。

提交公开 PR 前，将快照与私有源码对照并执行[发布检查](release_cn.md)。确认部署配置、凭据、设备身份和被忽略的运行时状态不在快照中。从公开快照构建与测试，审查完整的公开 diff，并验证文档链接。

公开 PR 通过审查和检查后再合并。只从已合并的公开提交创建公开标签；产品版本由根目录 `CMakeLists.txt` 决定。primitive、schema 和 skill ABI 的版本独立于产品发布版本。

## 准备 0.2.0 源码候选版

选择已审查的私有 EOS 提交及其准确源码树。从 `Aether-EOS-R` 当前目标分支建立新的公开分支，只将该私有源码树中受 Git 跟踪的文件复制到公开工作树，但排除私有的 `examples/skills/site-specific-inspection/` 子树。该可选 skill 在私有仓库中保留其可调用身份，不属于 0.2.0 公开源码候选版。公开旧树中仍被跟踪但不在选定私有树中的文件也需要删除。公开仓库保留自己的提交和标签。将私有提交 ID、公开基线 ID、选定源码树 ID 和审查决定记录在两个源码树之外的发布收据中。

选定的私有源码树包含公开仓库 `LICENSE`、`CONTRIBUTORS.md` 和 `contributor.md` 的原始字节，以保留 Apache-2.0 条款及已有贡献者署名。请在受跟踪文件收据和公开差异中核对这三个路径。

复制后先用 `git add -A` 将完整公开源码树暂存；检查器会拒绝未跟踪且未忽略的文件。然后运行 `python3 scripts/check_public_snapshot.py "$EOS_PRIVATE" "$EOS_PUBLIC" --output "$RELEASE_LOG/eos-tracked-manifest.json"`。收据列出两个检出目录中每个受跟踪路径的 Git 模式和当前字节 SHA-256；未经审查的路径、模式或字节差异都会使命令失败。只传入下文列出的已审查例外；收据记录每个省略及覆盖路径和可用的两侧摘要。`RELEASE_LOG` 应指向两个检出目录之外、空间充足的文件系统。此比较证明快照内容一致，不能替代来源与泄露审查；公开 PR 前仍须完成[发布检查](release_cn.md)。

私有发布策略从公开树中排除 `examples/skills/site-specific-inspection/`，同时保留其私有可调用 ABI。公开覆盖层还须把 `.gitignore` 中指定名称的后端忽略规则替换为 `/etc/eos/backends/*.toml`，删除 `share/eos/skills/CMakeLists.txt` 中的可选引用，并从 `docs/eos/maintenance/publish.md` 和 `docs_cn/eos/maintenance/publish_cn.md` 删除私有名称。运行检查器时传入 `--exclude-private-prefix examples/skills/site-specific-inspection/`、上述四个覆盖文件各一个 `--allow-changed <path>`，以及 `--reject-public-text site-specific robot name` 与 `--reject-public-text site-specific robot name`。公开差异必须显示这些准确修改；收据必须列出准确的例外路径和摘要；公开干净构建与链接检查必须通过。此策略段落及其对应英文段落的公开副本必须删除私有名称。

公开 PR 应审查相对公开目标分支的完整差异，包括删除的文件。从干净的公开检出目录构建和测试。审查通过并合并后，确认远端公开目标分支，再仅从该已合并的公开提交创建 `0.2.0` 标签。在发布收据中记录公开提交和标签对象 ID。私有 `refactor(v2)/runtime-main` 集成历史不会复制到公开仓库。

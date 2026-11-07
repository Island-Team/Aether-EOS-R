# Q04 重放存储位置

[English](../../docs/island/q04-home-replay.md) · [Q01 验收](q01-first-use-design_cn.md)

选定的 Q04 重放通过 `tempfile.gettempdir()` 放置 M4 全新 home、相机用例 home 和 R06/A03 故障用例 home。启动重放前，把 `TMPDIR` 设为大容量文件系统上简短且可写的路径，例如 `/home/operator/q04t`。选定的源码检出、包归档、构建产物、浏览器采集结果和 `logs/test/<run-id>/` 归档均放在 `/home`。运行时和模拟器使用路径长度受限的 Unix 域套接字，因此临时目录路径须简短。

此 harness 修改和 MuJoCo 发布输入合入各自集成分支**之后**，再冻结选定输入。冻结文件中的 harness 摘要记录重放脚本和 `q01_selection.py` 的精确字节。确认每个选定的可执行文件和 UI 构建产物都位于选定源码检出下，且路径的上级目录没有符号链接。保留失败尝试的全部证据；重试时使用新的运行 ID。

2026-11-03 在 Linux x86-64 上运行 `TMPDIR=/home/operator/q04t PYTHONPATH=tests python3 -m unittest tests/test_q01_selection.py -v`，13 个测试全部通过。新增测试在新的 Python 进程中将 `TMPDIR` 设在 `/home` 下，并在产生的重放 home 内绑定 Unix 套接字；还检查路径越界会被拒绝。完整输出保存在忽略提交的 `logs/test/20261103-q04-home-harness/selection-unit.log`。

提交审核前，选定输入、harness 防护和归档路径的聚焦测试共 23 个通过。`cargo fmt --all -- --check`、禁止警告的整个 workspace Clippy、串行 workspace 测试（52 个通过的测试组）、`cargo build --workspace`、Python 字节码编译和 `python3 scripts/check_doc_links.py --all` 均通过。复现时设置 `TMPDIR=/home/operator/q04t`；完整输出位于忽略提交的 `logs/test/20261103-q04-home-harness/`。

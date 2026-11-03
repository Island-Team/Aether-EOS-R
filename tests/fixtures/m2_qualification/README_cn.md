# M2 验收用例

[English](README.md) · [关卡计划](../../../docs_cn/island/m2-qualification_cn.md)

`cases.json` 固定 28 条有顺序的 M2 边界。`check_cases.py` 检查编号、阶段覆盖、所有者依赖、延后到 R02/R03/R05 的边界、P07 已合并的 TODO 状态及英中说明链接。在 EOS 根目录执行 `python3 tests/fixtures/m2_qualification/check_cases.py`。该检查器验证夹具和文档；可执行行为由 `tests/m2_cross_repo_replay.py` 与六批次包装脚本 `tests/run_m2_qualification.sh` 核对，完整输出保存在被 Git 忽略的 `logs/test/<run-id>/`。

执行器应为每类修改建立独立主目录，固定仓库完整提交与所选程序，在私有日志中保存原始响应和状态快照，并比较拒绝操作前后的保存字节、活动修订与子进程 PID。M2-21 分两层取证：Apply 后将 `cursor-state.json` 换成目录，公开 Run 在启动子进程前返回 `log_scope_publish_failed`；IsOS 所有者单元测试分别注入保存前故障及成功重命名后的模拟不确定结果，再检查新运行仍存在且锚点不可用。公开修改是状态路径受阻，所有者模拟并未使真正的父目录 fsync 系统调用失败。M2-27 是公开 Apply/Run 用例：未安装的 0.1.0 锁定由 P01 在进程 `/package` 返回 `package_missing_file`。M2-28 是独立的 IsOS 单元谓词用例，可执行 `cargo test --no-default-features --bin isos contributed_type_needs_exact_catalog_pin`；它向锁定 0.1.1 的图注入 0.1.0 目录，得到 `reader_package_pin_mismatch`，而公开的按锁定组装目录路径无法构造这种不匹配。每条用例报告通过、失败或被所列依赖阻塞；不得把 R02/R03/R05 记为已通过的 M2 断言。

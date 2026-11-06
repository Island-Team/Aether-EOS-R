# M4 受影响边界重放

[English](../../docs/island/m4-affected-replay.md) · [M4 里程碑](implementation-milestones_cn.md) · [跨仓库 TODO](implementation-todo_cn.md)

本次全新 home 的选定重放检查已合并的 M4 归属方边界；它不是 M4 或 M5 发布门槛。IsOS 管理图原始字节、选定目录、Apply、子进程启动与 Stop；EOS 管理活动准入与日志；EGIA 管理受监督任务与检查点；IsosUI 经公共桥接调用一项已声明原语。此前本地 `logs/test/20261102-m3-exact-merged-replay/qualification.md` 记录物理停止和停止响应丢失的证据。

源码版本锁定 IsOS `2bdf6ea0ce8dbf72ee73e563d0151364c93ff0c0`、EOS `c3aec9247634ff3cbbf3b4ede687c508f2c0f645`、EGIA `475aa567247d6288c40160a2baafc04c8db65856`、IsosUI `6afd5bdc84a4a3aa2ca73a93342a09d7067d1cc1`。已验证 MuJoCo 包为 `island/mujoco-sim@0.1.1`。`selected-inputs.json` 记录源码及二进制哈希。EOS 二进制包来自 M3 精确版本重放；至本 M4 分支点 EOS 源码未变化。

下方命令保留本次运行的历史工作树路径。删除工作树后，以所记录提交及哈希的检出与二进制替换这些路径；`selected-inputs.json` 是输入的权威清单。

在 EOS 主题工作树中，用精确构建输入及全新输出目录运行：

```sh
python3 tests/m4_affected_replay.py \
  --isos-root /tmp/isos-m4-2bdf \
  --eos-root /home/operator/projects/Island-eos-m4-first-use \
  --egia-root /tmp/egia-m4-merged \
  --isos-bin /home/operator/projects/isos/logs/test/20261102-egia-state-root-exact-merge/isos \
  --eos-bin-dir /home/operator/projects/Island-eos/logs/test/20261102-m3-exact-merged-replay/bin \
  --bundle /home/operator/projects/Island-eos/logs/test/20261102-m3-exact-merged-replay/mujoco-bundle \
  --mujoco-root /home/operator/mujoco-3.16.0-linux-x86_64/mujoco-3.16.0 \
  --ui-dist /tmp/isosui-m4-6afd/dist/local \
  --egia-python /tmp/egia-m4-merged/.venv/bin/python \
  --output logs/test/20261102-m4-affected/exact-merged-full-browser-2 \
  --mission --browser-release-file /tmp/m4-exact-merged-browser-release-2 \
  --browser-pause-seconds 1800
python3 tests/verify_m4_affected.py logs/test/20261102-m4-affected/exact-merged-full-browser-2
```

写出 `browser-ready.json` 后，用其中端口设置 `ISOSUI_API_ORIGIN=http://127.0.0.1:<api_port>` 和 `ISOSUI_EOS_API_ORIGIN=http://127.0.0.1:<bridge_port>`，重新构建锁定版本的 IsosUI 本地包。打开临时 URL，检查选定 EGIA 节点卡与活动修订，仅点击一次已启用的底盘绑定，保存 `browser-dom-observation.json` 及公共 `op=result` 响应 `browser-result.json`。随后创建释放文件，让脚本执行机器人 Stop、IsOS Stop 并记录子进程回收状态。不要记录控制授权文件内容。

精确合并版本运行以零状态退出，严格核对在 `logs/test/20261102-m4-affected/exact-merged-full-browser-2/verifier-full.log` 返回 `M4_AFFECTED_VERIFIED_PASS`：两条获准的路由证据边、166 条归属方事件、保存/活动/覆盖修订 `a5ca3bf58cb0f4cfca477e78eb4d9b9137ed59e99b0c8ebb612b7578812d3a37`、受监督 EGIA 任务操作 `b1e1efe9-691a-41a4-9c1d-2fb68ee4b49f` 与一条对应 EOS gRPC 日志记录、独立浏览器命令 `ui-581a75cb-9b5a-4581-a3d6-c0b3744872d7` 经 `private_bridge` 完成，以及已确认的选定 Stop。运行目录保存原始图、覆盖文件、实际服务的 UI 包、选定目录、归属方 JSONL、状态与日志哈希。本次已完成运行的 `reap-proof.json` 明确标为运行后的 PID 消失观察；后续脚本在 Stop 后立即记录。此前一次浏览器尝试在点击前超时，失败证据保存在 `exact-merged-full-browser/`。

独立的全新 home 重放 `logs/test/20261102-m4-affected/exact-merged-a03-negative-2/` 退出为 `M4_A03_SELECTED_NEGATIVE_PASS`，其基线亦通过 `verify_m4_affected.py --scope a03`，且 `python3 tests/verify_m4_recovery.py logs/test/20261102-m4-affected/exact-merged-a03-negative-2` 返回 `M4_A03_SELECTED_NEGATIVE_VERIFIED_PASS`。它使用受监督任务操作 `327e855b-d365-4919-b7c7-61e0becd8e2d`：下次 IsOS Run 的授权查询该旧 ID 返回 404，且对应 EOS 日志记录未变化。EGIA 停止后，测试注入另一条不存在的待处理操作及私有 0600 安全锁存。下一选定子进程报告操作未知及重置拒绝，保留锁存，未为该 ID 再次提交运动。受控 EGIA 子进程故障随后触发结果为 unknown 的依赖停止及子进程回收。`a03-recovery/` 明确将注入和跨运行查询标为负向证据，不将其当作普通首次使用流程。

本次证明受监督任务与选定 Stop。该选定 IsOS 边界仍未验证 EGIA **进程重启后同一命令恢复** 的正向路径：受管子进程退出后 IsOS 按设计停止依赖进程，新 Run 更换运行 ID 与授权。EGIA 归属方的[进程重启证明 #98](retired-repository-references_cn.md#ref-022af3ceccb3) 覆盖其稳定运行恢复契约。Q01 完整注入故障/重启及 M4 发布门槛仍开放。

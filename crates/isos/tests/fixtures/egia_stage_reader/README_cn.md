# A01 EGIA 阶段读取器夹具

`valid_egia.json` 固定 EGIA 归属方 A01 实现中 `tests/egia/test_stages.py::_graph` 的七阶段 core 与七条本地证据边。`tests/reader_features_cli.py` 将其复制到全新 IsOS starter 图，并在 Validate、Apply 前把 link gateway 端口设为选中 EOS gRPC 缺省值 `16200`。阶段描述符及全部 23 个数据 schema 均来自选中归属方目录。

重放验证目录调用前拒绝缺失的选中类型、仅两组 A04 归属方端点路由、固定端口拒绝、真实归属方 Validate/Apply 成功及 `egia/stages.json` 的精确修订字节，以及 Run/Restart 在启动子进程前拒绝篡改的所选覆盖文件。测试使用全新 `ISOS_HOME`，不修改源码工作树。构建 EOS 与 IsOS 后运行：`ISOS_REAL_EGIA_ROOT=/absolute/path/to/EGIA-A04 ISOS_BIN=/absolute/path/to/isos PYTHONPATH=tests python3 -m unittest -q reader_features_cli.ReaderFeaturesReplay.test_real_egia_stage_catalog_routes_only_evidence_and_denies_fixed_edges reader_features_cli.ReaderFeaturesReplay.test_real_egia_stage_apply_pins_exact_owner_overlay reader_features_cli.ReaderFeaturesReplay.test_real_egia_scheduler_loss_rejects_restart_before_old_child_stop`。

A04 在不改变此样例保存字节的情况下扩展准入：仅 resource 与 observations 两组是选定的证据路由。真实归属方重放接受 resource 上的优先级，拒绝 context snapshot 优先级与图编写的固定 admission 连线；所选 `egia-evidence-scheduler-v1` 缺失时，Run 和 Restart 均拒绝且不改变旧进程 PID 或保存字节。使用已合并的 EGIA A04 归属方构建，而非此前的 A01 构建。

A01 兼容性重放使用没有 `routed_edges` 的已合并 EGIA `d7d342e`：设置 `ISOS_REAL_EGIA_A01_ROOT` 并运行 `reader_features_cli.ReaderFeaturesReplay.test_real_a01_owner_stays_design_only_and_can_run`。它证明未选择 EGIA 运行时进程时仅用于设计的 Validate/Apply/Run，以及精确 QoS 拒绝且保存字节不变。

所选目录重放扩展 `test_real_egia_stage_apply_pins_exact_owner_overlay`：Apply 后 HTTP `GET /node-types/selected` 必须匹配保存字节修订，包含精确七个归属方阶段和 23 个 EGIA schema，并为编辑器暴露 `settings[].data_type`。`GET /node-graph` 须报告相同修订且可编辑。所选端点不写入图。

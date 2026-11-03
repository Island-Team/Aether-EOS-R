# R02 gRPC 设计用例

[English](README.md) · [所有者契约](../../../docs_cn/eos/interfaces/grpc_cn.md#r02-冻结的-rust-子集) · [设计](../../../docs_cn/eos/design/runtime/grpc-gateway_cn.md)

`cases.json` 冻结 Rust gRPC 网关的 35 条正向、反向和恢复边界。它是设计夹具，不是运行时通过报告。后续每条用例都须对应聚焦单元或集成测试，并在被忽略的 `logs/test/<run-id>/` 留有完整记录。`R02-07` 固定当前 EGIA 客户端的可选 `ListSkills` 回退；`R02-17`–`R02-21` 要求统一的持久 Dispatcher 身份和恢复；`R02-23` 只涉及停机健康，物理 stop 顺序仍属于 R06。`R02-26`–`R02-28` 区分 journal 化的设备停止、全机器人受控停止和紧急停止；`R02-12` 指明当前 EGIA 的慢 watch 错误边界；`R02-29` 固定不使用虚构硬件节点的 typed journal 目标迁移；`R02-30` 固定 RobotStop 部分扇出后的恢复及结果上限；`R02-31`–`R02-34` 覆盖并发运动 inhibit、两个溢出阶段和显式新 ID 恢复；`R02-35` 固定 adapter 前暂停的释放竞态和未决调用恢复。

设计检查：`python3 tests/fixtures/r02_grpc_gateway/check_cases.py`、`python3 scripts/check_doc_links.py --all`、`git diff --check`。完整输出保存在 `logs/test/20261101-r02-grpc-design/`。本设计切片不宣称 gRPC 实现或集成结果。

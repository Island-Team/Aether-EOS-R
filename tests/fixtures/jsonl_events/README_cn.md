# P06 EOS 事件设计夹具

[English](README.md) · [归属契约](../../../docs_cn/eos/interfaces/events_cn.md) · [运行时设计](../../../docs_cn/eos/design/runtime/events_cn.md)

`examples.jsonl` 包含三条中英文契约完全相同的示例。`check_events.py` 校验 UTF-8 JSONL 封装、字段上限、包摘要、共享文档链接、中英文一致性以及十三种非法变体。`cases.json` 以稳定 ID、变更和期望结果冻结 30 个实现与边界重放用例。Rust 生产者由 `cargo test -p aether-common`、`cargo test -p aether-runtime --test events` 和 `python3 tests/jsonl_events_process.py` 验证。

EOS 测试用伪单调时钟验证五秒抑制窗口，捕获三个常驻程序的 stderr，保留 runtime 既有 stdout ready 行，并启动桩 MuJoCo 子进程验证启动、backend 失联及先停后 reap。用同一个 `run_id` 配对 IsOS P06 捕获重放，并验证 spawn 前包／Apply 故障只具有 IsOS 管理器会话 ID。检查解析字段和原始字节，确保密钥、提示词、图像帧、参数、路径及子进程输出不泄露。`eos_ready` 是本地 runtime 转移；IsOS status 是部署就绪的权威。

在仓库根目录运行 `python3 tests/fixtures/jsonl_events/check_events.py`。完整实现测试输出保存于忽略的 `logs/test/<run-id>/`。
`browser_result_cases.json` 固定 Q01 EOS 桥接器生产端边界，包括确定性的 128 字节 UTF-8 关联令牌、响应不匹配、重复事件抑制、有界缓存和写入失败。桥接器 Rust 单元测试及私有运行时桩测试覆盖这些用例；IsOS 选定子进程接纳及 P07 回放使用独立的归属方证据。

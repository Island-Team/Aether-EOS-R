# Q01 浏览器命令事件设计

现有 P06 写入器发生非阻塞错误后会永久停止写入。同一桥接进程不重试丢失的诊断行，也不改变命令响应；所选关联日志缺失时，完整 Q01 验证失败。

[English](../../../../docs/eos/design/runtime/browser-result-events.md) · [归属方事件契约](../../interfaces/events_cn.md#q01-浏览器命令关联eos-生产端已实现) · [浏览器结果契约](../../interfaces/control_cn.md#r07-浏览器授权结果后续查询) · [跨仓任务](../../../../docs/island/implementation-todo.md)

**EOS 生产端已实现；IsOS 接纳及 Q01 回放待完成。**桥接器已将类型化浏览器控制和经授权的结果后续查询转发至私有运行时 IPC，验证成功结果的身份和状态，并发出脱敏事件。当前远端版本的 IsOS 封闭 P06 桥接角色仍只接收生命周期事件，因此 Q01 回放尚无法在 P07 当前运行日志中找到 UI 命令 ID。

桥接器保持已发布的 128 字节 UTF-8 浏览器 ID 接受规则。安全 ID 原样用作 P06 关联值；其他有效 ID 使用确定性不透明令牌，因此既保留同一运行的后续查询，也不记录任意文本。收到私有响应后，仅当 `ok` 为 true、结果 ID 与请求 ID 相同、状态属于已发布的 Dispatcher 枚举时，才发出结果事件。不能从 HTTP 200 或请求正文推断状态。有界状态缓存抑制重复的 `InProgress` 轮询及终态读取，同时保留提交结果和首次观察到的终态转换。共享的非阻塞事件写入器对客户线程的有界写入进行串行化；写入失败是诊断损失，不改变命令结果，也不能成为重放运动的理由。[归属方事件契约](../../interfaces/events_cn.md#q01-浏览器命令关联eos-生产端已实现)定义令牌映射、全部字段和安全响应边界。

边界测试使用私有运行时桩，覆盖立即终态、`InProgress` 后 `Completed`、错误 ID 或状态、缺失授权、同一运行中 128 字节或非 ASCII ID、重复后续查询、持续轮询、缓存逐出、并发调用及事件写入失败。所选版本集成验证对照原始桥接事件、IsOS 规范化日志行、P07 页面、UI 显示 ID 和 EOS 单条日志结果。日志证明脱敏归属事件的关联和接纳；Dispatcher 日志仍是结果权威。

## EOS 生产端验证，2026-11-03

在本主题工作树运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test -p aether-common -p aether-http-bridge -- --test-threads=1`、`cargo test --workspace -- --test-threads=1`、`cargo build --workspace`、`python3 scripts/check_doc_links.py --all` 和 `python3 tests/fixtures/jsonl_events/check_events.py`，全部通过。完整输出保存在本地 `logs/test/20261103-q01-browser-events/`。桥接器测试覆盖 UTF-8 命令在私有提交与后续查询间的流转、安全 ID 原样关联、缓存并发与逐出、矛盾响应拒绝和写入失败。该证据只验证 EOS 生产端；IsOS 日志接纳与 Q01 回放仍未完成。

独立审查发现一个并发顺序边界：终态后续查询可能先于原提交线程记录其响应。缓存现在分别跟踪首次提交与首次终态观察，即使顺序倒置也各发出一次。确定性的倒序测试固定这一边界。写入失败测试统计写入次数，并验证首次诊断写入失败后，两次成功的私有结果仍返回 HTTP 200。复测输出位于 `logs/test/20261103-q01-browser-events-rereview/`。

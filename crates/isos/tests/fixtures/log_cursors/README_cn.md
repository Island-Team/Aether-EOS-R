# P07 当前运行日志游标固定样例

[English](README.md) · [归属接口](../../../../../docs_cn/isos/develop/owner/log-cursors_cn.md) · [设计](../../../../../docs_cn/isos/design/server_cn.md)

`cases.json` 固定显式启用的端点、响应键、范围和锚点字段、筛选顺序、诊断顺序、边界及 22 个正反线路案例与 12 个可执行状态场景，包括之后同码事件、状态损坏时双运行历史不能证明最新准入运行、密钥轮换时保留经过独立证明的运行标识并拒绝旧游标，以及篡改 `r812` → `r999` 范围时封闭失败。运行 `python3 tests/fixtures/log_cursors/check_cases.py` 执行状态场景的参考准入／范围选择器并校验线路样例自身一致性；这仅是契约证据，并非运行时测试。实现测试需调用真实 LogStore／HTTP／CLI 路径，将可观察的日志页和状态与案例比较。查询中的 `lc1.*` 值只是案例标识，不是序列化 token。

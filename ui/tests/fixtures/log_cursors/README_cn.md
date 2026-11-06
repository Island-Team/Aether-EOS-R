# P07 IsosUI 日志游标夹具

[English](README.md) · [UI 契约](../../../../docs_cn/isosui/interfaces/log-cursors_cn.md) · [设计](../../../../docs_cn/isosui/design-log-cursors_cn.md)

`cases.json` 冻结 31 个 UI 状态与服务边界用例。`check_log_cursors.py` 校验稳定 ID、上限、精确 P07 查询／响应字段、必要安全断言和中英文文档链接。这些夹具规定目标行为，不表示 UI 已实现。实现测试应将每个用例转为规约器或桩 HTTP 重放，并包含实际 IsOS 轮转及收集器恢复重放。完整输出保存在忽略的 `logs/test/<run-id>/`。

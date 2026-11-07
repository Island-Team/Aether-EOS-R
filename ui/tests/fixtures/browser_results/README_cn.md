# R07 UI 浏览器结果设计用例

[English](README.md) · [UI 契约](../../../../docs_cn/isosui/interfaces/browser-results_cn.md) · [设计](../../../../docs_cn/isosui/design-browser-results_cn.md)

`cases.json` 固定 46 项 IsosUI 对 EOS R07 类型化发送及授权浏览器结果路由的设计用例。`send` 用例要求操作员点击后只发送一次活动绑定 `primitive_typed` 请求，图目标和载荷长度精确匹配；`request` 用例要求结果读取的公开请求体只有 `op`、`command_id`、`active_revision`；`owner_result` 覆盖 EOS 的六种精确状态；`fence` 将旧运行或旧修订与当前命令隔离；`owner_error`、`transport` 和 `protocol` 用例在证据缺失时保持明确不可用。`log_link` 用例要求只有所有者提供有效关联 ID 才筛选日志；Stop 用例要求 EOS 离线时仍能使用独立的 IsOS 部署 Stop。这是设计证据，不是实现测试。

在仓库根目录执行 `python3 tests/fixtures/browser_results/check.py`。关闭 R07 或 U04 前，实现测试须用模拟 IsOS／EOS 服务、两个 UI 构建以及真实跨所有者首次使用重放覆盖这些用例。

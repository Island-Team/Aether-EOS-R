# 故障排查

本页帮助操作人员定位软件包或运行失败的阶段。读取结构化错误的 `code`、`stage` 和图 `path`；[HTTP 信封](../develop/http/envelope_cn.md)定义其含义。重试前先用 `isos status --json` 和 `isos logs all --current` 检查当前状态。

## 软件包拒绝

不支持的契约、已更改的负载、不安全路径、未知资源、归属方或生命周期适配器都会导致准入失败。使用精确版本引用和规范路径定位声明。字节更改后需显式重新安装或链接；重试不会刷新回执。参见[软件包验证](../interfaces/packages/verification_cn.md)。

## 启动与就绪

从日志检查所选归属方、进程声明和有界就绪结果。失败的候选运行不会替换活动运行。[进程契约](../interfaces/packages/process_cn.md)和[交接契约](../interfaces/cores/handoff_cn.md)分别说明 IsOS 已验证的内容和归属方需要接受的内容。

## 日志与不确定停止

使用当前运行 ID 和[日志游标契约](../develop/owner/log-cursors_cn.md)区分不同运行的事件。如果终止停止证明缺失、过期、迟到或矛盾，状态保持 `unknown`。归属方和进程状态核对完成前不要启动替代运行；参见[停止证明](../interfaces/cores/dependent-stop_cn.md)。

[English](../../../docs/isos/debug/troubleshooting.md)

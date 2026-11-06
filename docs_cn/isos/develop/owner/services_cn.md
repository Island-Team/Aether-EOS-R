# 独立服务定义

[English](../../../../docs/isos/develop/owner/services.md) · [管理器 IPC](manager_cn.md)

本页定义仅所有者可用的管理器使用的服务定义，**不是 HTTP 路由**。服务在部署图外，只通过显式 CLI 或管理器操作启动。`isos serve` 和 `isos run` 不隐式启动它们。管理器关闭时停止其服务。当前定义为 TOML `version="1.0.0"`；该版本独立于 IsOS HTTP 模式。

```toml
version = "1.0.0"
id = "example"
command = ["/absolute/verified/launcher"]
cwd = "/absolute/verified"
depends_on = []
restart = "on_failure"
max_restarts = 2
start_timeout_ms = 30000
stop_timeout_ms = 5000

[artifact]
path = "/absolute/verified/launcher"
sha256 = "<64 lowercase hexadecimal digits>"

[readiness]
kind = "tcp"
target = "127.0.0.1:18003"
```

| 字段 | 类型 / 单位 | 用途与验证 | 缺省值 |
| --- | --- | --- | --- |
| `version`, `id` | 字符串 / 定义版本与稳定 ID | 精确解析版本与服务身份。 | 必填。 |
| `command`, `cwd` | 字符串数组与绝对路径 / 进程输入 | 显式启动输入，不从软件包 ID 推断。 | 必填。 |
| `depends_on` | 字符串数组 / 服务 ID | 独立服务间排序；循环拒绝。 | 无依赖时为空。 |
| `restart`, `max_restarts` | 枚举与整数 / 策略、次数 | 限制自动恢复。 | 必填。 |
| `start_timeout_ms`, `stop_timeout_ms` | 正整数 / 毫秒 | 限制就绪与终止时间。 | 必填。 |
| `artifact.path`, `.sha256` | 绝对路径与 64 位十六进制 / 字节 | 绑定选中可执行文件。 | 必填。 |
| `readiness.kind`, `.target` | 字符串 / 探测 | 所有者在成功前检查声明就绪。 | 必填。 |

服务定义不提供软件包特定解析器，也不绕过图节点的通用软件包贡献路径。

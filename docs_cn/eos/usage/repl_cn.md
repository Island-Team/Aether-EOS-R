# 使用 EOS v2 CLI

[English](../../../docs/eos/usage/repl.md) · [使用 EOS](README_cn.md) · [v2 服务进程](../develop/services_cn.md)

`eos-cli` 启动已安装的 Rust `aether-tui` 客户端。它通过仅所有者可访问的 Unix 控制套接字连接 `aether-runtime`；可选的 HTTP 桥接是另一条客户端路径。C++ HTTP/SSE 诊断客户端仅属于显式启用的侧链，见[旧版 REPL 指南](../develop/ext/tools/repl_cn.md)。

在一个终端运行 `isos serve` 启动 IsOS manager，在另一个终端运行 `isos run` 启动已应用的部署。运行 `./bin/eos-cli --socket <runtime-socket>`（安装后使用 `eos-cli --socket <runtime-socket>`）打开客户端。没有传参时，启动脚本使用 `EOS_V2_SOCKET` 或 `/run/island/eos-v2.sock`。部署图中的 runtime `--socket` 参数是准确路径；客户端须使用同一路径。`./launch.sh` 和 `./bin/eos-repl` 是只打开该客户端的兼容入口。

| 命令 | 请求 |
| --- | --- |
| `time`、`discover` | 读取运行时时间或已准入节点。 |
| `health <path>`、`capture <path>`、`observe <path>` | 读取指定图节点。 |
| `stop <path>` | 请求安全停止指定节点。 |
| `result <id>`、`cancel <id>` | 查看或取消命令。 |
| `dispatch <json>` | 发送包含 `"op":"dispatch"` 的 JSON 请求。 |
| `quit`、`exit`、Ctrl-D | 退出客户端；EOS 继续运行。 |

例如，`discover` 返回规范底盘路径后：

```text
discover
health cores/eos/layers/hardware/groups/base/nodes/drive
stop cores/eos/layers/hardware/groups/base/nodes/drive
quit
```

客户端对每个请求输出 JSON 响应或错误。它不提供 stack、软件包、技能、工作流、历史记录或 `shutdown` 命令。使用 `isos status` 查看 manager 和部署状态，`isos deployment stop` 只停止已应用部署，`isos stop` 关闭 manager、UI、API、独立服务和部署子进程。显式构建 C++ 侧链时，另见[旧版 REPL 指南](../develop/ext/tools/repl_cn.md)。

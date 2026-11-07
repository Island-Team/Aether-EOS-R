# 开发 SSH 隧道

[English](../../docs/island/ssh-tunnel.md)

`scripts/ssh-tunnel.sh` 用于维护开发环境中的 SSH 本地端口转发，使工作站能够访问无法直接连接的服务。脚本加载仓库根目录的 `.env`，校验隧道记录，然后委托给 `scripts/ssh-tunnel.py`。该辅助工具不会安装，也不会随正式版本发布。

## 配置

将 `.env.example` 复制为 `.env`，然后在 `SSH_TUNNELS` 中按顺序定义隧道名称。每个名称不区分大小写地展开为 `SSH_TUNNEL_<NAME>` 和 `SSH_TUNNEL_<NAME>_PASSWORD_ENV`：

```sh
SSH_TUNNELS="lab simulator"

SSH_TUNNEL_LAB="developer@robot.example.com:22 50051 8000"
SSH_TUNNEL_LAB_PASSWORD_ENV=LAB_SSH_PASSWORD
LAB_SSH_PASSWORD='replace-me'

SSH_TUNNEL_SIMULATOR="operator@simulator.example.com:2222 50052"
SSH_TUNNEL_SIMULATOR_PASSWORD_ENV=SIMULATOR_SSH_PASSWORD
SIMULATOR_SSH_PASSWORD='replace-me'
```

每条隧道记录以 `user@host:ssh_port` 开头，后跟一个或多个 TCP 端口。对于每个端口，辅助工具在本地绑定 `127.0.0.1:<port>`，并将其转发到目标主机的 `127.0.0.1:<port>`。所有目标使用的本地端口必须唯一。隧道名称可以包含字母、数字和下划线，但必须以字母开头。

密码采用间接配置：`SSH_TUNNEL_LAB_PASSWORD_ENV=LAB_SSH_PASSWORD` 指定哪个环境变量保存该目标的密码。`.env` 会由 Bash 加载，因此包含 shell 特殊字符的值必须加引号。切勿提交 `.env`；目标支持时应优先使用 SSH 密钥和 SSH agent。首次连接时，OpenSSH 会自动记录以前未见的主机密钥；主机密钥如发生变更，仍会拒绝连接。

设置 `SSH_TUNNEL_PROXY` 或小写 `http_proxy` 后，辅助工具会通过该 HTTP CONNECT 代理连接 SSH。代理 URL 保留在子进程环境中，不会展开到进程参数里。

## 运行

在前台启动 worker：

```sh
./scripts/ssh-tunnel.sh
```

worker 为每个目标启动一个 OpenSSH 进程。连接尝试限制为五秒；SSH keepalive 在连续两个五秒探测无响应后终止会话。连接失败后，各目标会独立等待五秒并重试。按 `Ctrl-C` 可终止前台 worker 及其活动 SSH 进程。

使用以下命令在后台启动：

```sh
./scripts/ssh-tunnel.sh --backend
```

命令会输出 worker PID，并将日志写入 `var/ssh-tunnel.log`。不再需要转发时，应显式终止该 PID。辅助工具通过 `SSH_ASKPASS` 将密码传给 OpenSSH，不会把密码写入日志。

## 故障排查

任何配置错误都会在启动 SSH 进程前报告。请确认仓库根目录存在 `.env`，`SSH_TUNNELS` 中的每个名称都有两个派生设置，其引用的密码变量非空，并且所有 SSH 端口和转发端口都是 1 到 65535 之间的整数。OpenSSH 的主机密钥检查和认证策略仍然有效。

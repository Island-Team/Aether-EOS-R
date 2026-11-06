# 为软件包发布登录

本页面向通过 IsOS CLI 使用 Island-port 的发布者。`isos auth login` 使用 GitHub OAuth Device Flow，并将临时 GitHub 用户令牌换成 Island-port 访问及刷新令牌。它只请求 `user:email`；GitHub 令牌不写入磁盘。IsOS 将 Island-port 凭据保存到配置目录中的 `auth.json`，权限仅限文件所有者。此登录与 IsOS 的[本地 HTTP 请求准入](../develop/http/request-admission_cn.md)相互独立。

运行 `isos auth login` 启动设备流程，`isos auth whoami` 查看登录账号，`isos auth logout` 撤销刷新会话并移除本地凭据。Island-port 服务必须启用 Device Flow。API 返回 401 后访问凭据会刷新，替换的刷新令牌会保存。发布资格由 Island-port 决定；IsOS 不获取 GitHub 仓库写入凭据。

[English](../../../docs/isos/usage/authentication.md)

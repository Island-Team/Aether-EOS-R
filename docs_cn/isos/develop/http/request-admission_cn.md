# 本地 HTTP 请求准入

本页面向 IsOS 回环 API 的实现者。当前监听器绑定 `127.0.0.1`，端口由 `ISOS_HTTP_PORT` 指定（默认 `16000`）。路由执行前，传输层检查 `Host` 是否与配置的回环主机及端口完全一致。提供的 `Origin` 必须是本地 IsOS 来源、配置的 IsosUI 来源或允许的托管 UI 来源。跨站 `sec-fetch-site` 请求缺少 `Origin` 时会被拒绝。当前本地请求不携带归属方 bearer 令牌。

CORS 预检需要 `Origin`；允许的请求方法为 `GET`、`POST`、`PUT`，允许的请求头只有 `Content-Type`。Host、Origin 或预检被拒绝时，在图或运行时变更前返回 403。每条路由仍需验证负载、运行围栏和图权限。[节点事件契约](../../interfaces/ui/node-events_cn.md)给出首个 UI 控制请求。

[English](../../../../docs/isos/develop/http/request-admission.md)

# Local HTTP request admission

This page is for implementers of the IsOS loopback API. The current listener binds to `127.0.0.1` on `ISOS_HTTP_PORT` (default `16000`). Before a route runs, the transport checks that `Host` equals the configured loopback host and port. A supplied `Origin` must be the local IsOS origin, the configured IsosUI origin, or one of the hosted UI origins. A cross-site `sec-fetch-site` request without `Origin` is rejected. Current local requests do not carry an owner bearer token.

CORS preflight requires `Origin`; allowed requested methods are `GET`, `POST`, and `PUT`, and the only permitted requested header is `Content-Type`. A rejected Host, Origin, or preflight returns 403 before graph or runtime mutation. Individual routes still validate payload, run fence, and graph authority. The [node-event contract](../../interfaces/ui/node-events.md) gives the first UI control request.

[中文](../../../../docs_cn/isos/develop/http/request-admission_cn.md)

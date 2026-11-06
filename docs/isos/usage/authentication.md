# Sign in for package publication

This page is for publishers using the IsOS CLI with Island-port. `isos auth login` uses GitHub OAuth Device Flow and exchanges a temporary GitHub user token for Island-port access and refresh tokens. It requests only `user:email`; the GitHub token is not written to disk. IsOS stores Island-port credentials in its config directory's `auth.json` with owner-only permissions. This login is separate from IsOS's [local HTTP request admission](../develop/http/request-admission.md).

Run `isos auth login` to start the device flow, `isos auth whoami` to inspect the signed-in account, and `isos auth logout` to revoke the refresh session and remove local credentials. Device Flow must be enabled by the Island-port service. Access credentials refresh after an API 401 and the replacement refresh token is persisted. Publication eligibility is decided by Island-port; IsOS does not acquire GitHub repository write credentials.

[中文](../../../docs_cn/isos/usage/authentication_cn.md)

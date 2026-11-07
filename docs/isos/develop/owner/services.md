# Independent service definition

[中文](../../../../docs_cn/isos/develop/owner/services_cn.md) · [Manager IPC](manager.md)

This page describes the service definition used by the owner-only manager, **not an HTTP route**. Services are outside the deployment graph and start only through an explicit CLI or manager operation. `isos serve` and `isos run` do not implicitly start them. Manager shutdown stops its services. The current definition is TOML `version="1.0.0"`; its version is independent of the IsOS HTTP schema.

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

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `version`, `id` | strings / definition version and stable ID | Exact parser version and service identity. | Required. |
| `command`, `cwd` | string array and absolute path / process input | Explicit launch input, never inferred from package ID. | Required. |
| `depends_on` | string array / service IDs | Order among independent services; cycles reject. | Empty if none. |
| `restart`, `max_restarts` | enum and integer / policy, attempts | Bound automatic recovery. | Required. |
| `start_timeout_ms`, `stop_timeout_ms` | positive integers / milliseconds | Bound readiness and termination. | Required. |
| `artifact.path`, `.sha256` | absolute path and 64-hex / bytes | Bind the selected executable. | Required. |
| `readiness.kind`, `.target` | strings / probe | Owner checks declared readiness before success. | Required. |

A service definition does not provide a package-specific parser or bypass the generic package contribution path for graph nodes.

# Selected core setup adapter

[中文](../../../../docs_cn/isos/interfaces/cores/setup_cn.md) · [Cores HTTP](../../develop/http/cores.md)

The implemented external owner setup exchange uses **schema 2**. IsOS selects the configured owner executable; a client cannot supply a path. Input is bounded to 1 MiB, output to 4 MiB, and the deadline is 10 seconds. `identity` and `catalog` are read-only; `validate` checks a draft; `render` previews bounded generated files. IsOS rejects wrong core, incompatible identity, malformed or oversized reply, escaping path, and timeout before publication. This external wire is independent of the IsOS-owned HTTP `"0.1.0"` schema.

```json
{"schema_version":2,"ok":true,"diagnostics":[],"files":[],"identity":{"core":"eos","setup_schema_version":2,"product_version":"0.2.0","graph_version":"0.1.0","node_type_version":"0.1"}}
```

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `schema_version` | integer / external owner schema | Exactly `2` for request and reply. | Required. |
| `op` | `identity`, `catalog`, `validate`, `render` / operation | Selects one bounded operation. | Required in request. |
| `core` | string / owner role | Must match selected configured owner. | Required in request. |
| `graph`, `revision` | JSON object and optional 64-hex / graph and SHA-256 | Validate/render draft and optional saved-byte fence. | Operation-specific. |
| `ok`, `diagnostics`, `files` | boolean and arrays / result | Closed reply; diagnostic entries have `path`, `code`, `message`; file entries have safe relative `path`, `content`. | Required in reply. |
| `identity`, `catalog` | JSON or null / owner metadata | Present according to operation; identity has `setup_schema_version:2`. | Optional. |

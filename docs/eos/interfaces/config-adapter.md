# EOS configuration adapter

`eos-config` is the EOS-owned configuration boundary used by IsOS. It does not
start `eosd`, open a listener, inspect hardware, or write deployment files.
The native executable is installed as `libexec/island/eos-config`; the public
`bin/eos-config` wrapper locates it in source, portable, and system layouts.

Run `eos-config describe|defaults|profiles|render|validate --json`. Every
response is one JSON object with `schema_version: 1`, `ok`, `data`, and
`diagnostics`. Diagnostics contain `path`, `code`, and `message`. A rejected
request exits with status 1. `render` and `validate` read one JSON object on
standard input:

```json
{"schema_version":1,"values":{"grpc_uds_path":"/run/user/1000/isos/eos.sock"}}
```

`defaults` returns the editable values: `host`, `grpc_port`, `grpc_uds_path`,
`grpc_uds_mode` (decimal permission bits), `north_http_port`,
`binding_backend`, and `binding_profile`. `render` returns relative paths and
contents for `eos/eos.toml` and `ext/north-http/config.toml`; IsOS writes them
under its managed configuration directory. Rendered EOS TOML passes the same
parser used by the runtime. EOS requires exactly one mode binding per process.
The default binding is `bindings/isos-demo.toml`, relative to the rendered
`eos/eos.toml`. With this default, `render` also returns
`eos/bindings/isos-demo.toml`, so the generated tree is self-contained. EOS
can boot and serve discovery with no skill package or simulator. Demo device
operations become ready only when a separately managed simulator IPC service
is running and `EOS_SIMULATOR_SOCKET` points to its socket.

`profiles` exposes `eos.native` (one EOS process with a TCP gRPC output) and
`eos.uds` (a logical gRPC endpoint on that process). The UDS profile is ready
only after its parent EOS process binds the socket. Both advertise the actual
Gateway v1, Skill v1, Capability v2, and Workflow v1 services. IsOS must
create a private, process-owned `0700` socket parent and wait for readiness
before starting a UDS consumer. The adapter does not create that directory.

Each profile has an ordered `fields` array for graph editors. Entries contain
`name`, `type`, `label`, `description`, and applicable `default`, `required`,
`min`, `max`, or `options` constraints. `eos.native` describes listener,
binding, and socket settings; its `grpc_uds_path` is derived from the endpoint
when IsOS manages both nodes. `eos.uds` exposes the required graph setting
`socket_path`, which also names its UDS readiness field. `render` and
`validate` accept `socket_path` as an alias of `grpc_uds_path` and reject
requests containing different values for both. The existing
`grpc_uds_path` input remains supported.

Managed defaults are loopback gRPC port `16200` and north HTTP port `16201`.
`grpc_uds_path` is empty until IsOS supplies a private per-deployment path.
Both listener ports must be integers in `1..65535` and must differ. The UDS
mode is a decimal integer in `1..511`; EOS renders it as octal TOML.

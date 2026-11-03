# Use the EOS v2 CLI

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Usage](README.md) / **repl**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [usage/](README.md)
      - [arm64.md](arm64.md)
      - [backends.md](backends.md)
      - [configuration-reference.md](configuration-reference.md)
      - [configure.md](configure.md)
      - [mujoco.md](mujoco.md)
      - [packages.md](packages.md)
      - **repl.md**

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/usage/repl_cn.md) · [Use EOS](README.md) · [v2 service processes](../develop/services.md)

`eos-cli` launches the installed Rust `aether-tui` client. It talks to `aether-runtime` over the owner-only Unix control socket; the optional HTTP bridge is a separate client path. The C++ HTTP/SSE diagnostic client is available only in the opt-in side chain and is documented in the [legacy REPL guide](../develop/ext/tools/repl.md).

Start the IsOS manager in one terminal with `isos serve`, then run the applied deployment with `isos run` in another. Open the EOS client with `./bin/eos-cli --socket <runtime-socket>` (or installed `eos-cli --socket <runtime-socket>`). The wrapper uses `EOS_V2_SOCKET` or `/run/island/eos-v2.sock` when no arguments are supplied. The graph's runtime `--socket` argument is authoritative; pass that same path to the client. `./launch.sh` and `./bin/eos-repl` are compatibility names that open this client only.

The client supports these commands:

| Command | Request |
| --- | --- |
| `time`, `discover` | Read runtime time or admitted nodes. |
| `health <path>`, `capture <path>`, `observe <path>` | Read the named graph node. |
| `stop <path>` | Request a safe stop for the named node. |
| `result <id>`, `cancel <id>` | Inspect or cancel a command. |
| `dispatch <json>` | Send a JSON request with `"op":"dispatch"`. |
| `quit`, `exit`, Ctrl-D | Leave the client; EOS keeps running. |

For example, after `discover` returns a canonical base path:

```text
discover
health cores/eos/layers/hardware/groups/base/nodes/drive
stop cores/eos/layers/hardware/groups/base/nodes/drive
quit
```

The client prints the JSON response or an error for each request. It has no built-in stack, package, skill, workflow, history, or `shutdown` command. Use `isos status` for manager and deployment state, `isos deployment stop` for the applied deployment, and `isos stop` to close the manager, UI, API, independent services, and deployment children. The [legacy REPL guide](../develop/ext/tools/repl.md) covers the separate C++ client when that side chain is explicitly built.

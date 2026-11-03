# Launch and manage local EOS

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Debug](README.md) / **launch**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [debug/](README.md)
      - **launch.md**

</details>
<!-- docs-tree:end -->

[Debugging runbook](README.md) · [EOS configuration](../usage/configuration-reference.md) · [Package releases](../usage/packages.md)

This page documents the opt-in C++ checkout launcher and its historical stack format. The default Rust v2 deployment uses `isos serve` for the manager, `isos run` for graph children, and `isos deployment stop` for those children. Independent model-like services use separate IsOS service files and are never started by the graph. `launch.sh` opens only the Rust `eos-cli` client, whose [current command set](../usage/repl.md) differs from the legacy C++ REPL. The legacy launcher below remains available only when the C++ side chain is explicitly built.

## Commands

| Command | Effect |
| --- | --- |
| `./bin/island-launch eos start` | Start missing drivers and managed services. |
| `./bin/island-launch eos stop` | Stop managed services; retain drivers. |
| `./bin/island-launch eos restart` | Restart managed services; retain drivers. |
| `./bin/island-launch backend stop` | Stop the whole graph, including drivers. |
| `./bin/island-launch backend restart` | Restart the whole graph in dependency order. |
| `./bin/island-launch eos status --json` | Show selected stack, active stack, units, and north HTTP URL. |
| `./bin/island-launch eos logs` | Show recent journal entries for declared units. |
| `./bin/island-launch profile use PATH` | Select a `stack.toml` file or a directory containing one. `default` selects the repository stack. |

Selection does not alter running units. Restart to apply it. When changing drivers, use `backend restart`; `eos restart` rejects switching to a different stack while drivers from the old stack run. `profile use` calls its selection a *profile* in CLI output, but the selected object is a runtime stack.

## Stack and module files

`etc/runtime/stack.toml` names the EOS config, core services, and every external module to load. A package installed on disk does not enter the graph until its module path is listed. A selected custom stack replaces the default stack; service tables are not merged with it.

The optional `package_root` points to a nondefault `island-pkg` store (absolute or relative to the stack). Packaged modules require their exact package ID and version in that store's `isos.lock` and an unchanged installed release.

```toml
version = 1
eos_config = "../eos/eos.toml"
modules = ["../ext/simulator/module.toml", "../ext/north-http/module.toml"]

[[service]]
name = "eosd"
role = "managed"
command = ["{root}/bin/eosd", "--config", "{eos_config}"]
ready_tcp = "127.0.0.1:16200"
```

Each referenced `etc/ext/<name>/module.toml` has a matching module ID. An external release can also record its package ID and version. Disabling a module removes its services and binding declarations from the graph.

```toml
version = 1

[module]
id = "mujoco"
enabled = true

[[service]]
name = "mujoco-sim"
role = "managed"
command = ["{package_file:bin/eos-mujoco-sim}", "--socket", "{run}/simulator.sock"]
ready_unix = "{run}/simulator.sock"

[[binding]]
backend = "backends/mujoco.toml"
profile = "benchmark"
requires = ["mujoco-sim"]
eos_after = ["mujoco-sim"]
```

The `backend` path in `[[binding]]` resolves relative to the selected `eos_config` directory and must match an EOS `[[mode.binding]]` path and profile exactly. Every selected EOS binding needs one runtime declaration. `requires` names enabled services the binding needs; `eos_after` adds dependencies to `eosd`. The launcher does not infer a simulator or ROS bridge from backend filenames or device types. Declare those processes and relationships explicitly.

Service names must be unique across the stack and enabled modules. `role` is `managed` or `driver`; drivers may depend only on drivers. `command` is an argument array beginning with an absolute executable path and is run without a shell. `after` lists services to start first. A service can have one `ready_tcp` or `ready_unix` endpoint. Optional `timeout_ms` is 1–300000, `env` is a string table, and `enabled` defaults to true. The launcher rejects cycles, missing or disabled dependencies, duplicate endpoints, occupied endpoints, and failed readiness. Placeholders in commands and endpoints include `{root}`, `{run}`, `{bundle}`, `{eos_config}`, `{sim_config}`, and `{north_http_config}`.

The launcher checks the process graph and selected binding reference. For a packaged module it also verifies the exact `isos.lock` selection and installed release digest; `eosd` repeats those checks before admitting the binding. A changed graph applies on restart. Old `launch.toml` bundles fail with a migration error.

## Process ownership and diagnosis

`driver` units survive `eos stop`; `managed` units do not. `backend stop` stops both. The launcher confirms the stopped unit's cgroup and readiness endpoint have cleared. It refuses to adopt an unmanaged process at a declared endpoint and never kills processes by name. `eosd` owns device admission and hardware dispatch, while declared bridges and drivers remain separate processes.

To diagnose a failure, inspect `eos status --json`, `eos logs`, the selected `stack.toml`, its referenced modules, and the selected EOS binding. Verify the executable and readiness endpoint for each service. A failed EOS-only start rolls back managed services but can leave drivers running; clear them with `backend stop`. A failed full backend start rolls back its driver units too.
